#pragma once
/*
 * MediaPoolPrefetchInternal.h — Internal helpers shared across MediaPool
 * prefetch translation units.
 *
 * Contains anonymous-namespace helpers lifted from MediaPoolPrefetch.cpp
 * so they are accessible from all prefetch TUs.
 */

#include "MediaPool.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
}
#endif

namespace rt {

namespace prefetch_detail {

constexpr size_t kMaxExactQueueDepth = 64;

inline bool sameTaskKey(const PrefetchTask& lhs,
                        const PrefetchTask& rhs) noexcept
{
    return lhs.handle == rhs.handle &&
           lhs.frameNumber == rhs.frameNumber && lhs.tier == rhs.tier;
}

/// Promote or insert one exact endpoint without rebuilding unrelated
/// decode-ahead work. Returns true only when a new queue entry was added.
inline bool mergeExactTask(std::deque<PrefetchTask>& queue,
                           PrefetchTask task,
                           size_t maxDepth = kMaxExactQueueDepth)
{
    task.urgent = true;
    task.exactRequest = true;

    const auto insertInEndpointOrder = [&](PrefetchTask exact) {
        auto position = queue.begin();
        while (position != queue.end() && position->urgent &&
               position->exactRequest && position->handle == exact.handle &&
               position->frameNumber < exact.frameNumber) {
            ++position;
        }
        queue.insert(position, std::move(exact));
    };

    const auto existing = std::find_if(
        queue.begin(), queue.end(),
        [&](const PrefetchTask& queued) { return sameTaskKey(queued, task); });
    if (existing != queue.end()) {
        task.exportFullRes = task.exportFullRes || existing->exportFullRes;
        queue.erase(existing);
        insertInEndpointOrder(std::move(task));
        return false;
    }

    // Exact requests are bounded independently of the normal interactive
    // queue cap. Prefer evicting ordinary lookahead from the back; if all
    // entries are exact, discard the oldest exact request.
    if (maxDepth == 0) return false;
    while (queue.size() >= maxDepth) {
        const auto replaceable = std::find_if(
            queue.rbegin(), queue.rend(),
            [](const PrefetchTask& queued) { return !queued.exactRequest; });
        if (replaceable != queue.rend()) {
            queue.erase(std::next(replaceable).base());
        } else {
            queue.pop_back();
        }
    }
    insertInEndpointOrder(std::move(task));
    return true;
}

/// A prefetch worker releases a decoder idle this long (filmcraft MEM2: an
/// NVDEC decoder that has decoded holds its surface pool, tens to hundreds
/// of MB of VRAM, for as long as it lives)...
inline constexpr auto kIdleDecoderRelease = std::chrono::seconds(5);
/// ...but keeps this many warm, so the files it just served (a cut back to
/// the previous shot) don't pay a fresh open + seek.
inline constexpr size_t kWarmDecodersKept = 2;

/// Handles whose decoders to release, oldest first: those idle at least
/// `idleAfter`, while more than `keepWarm` decoders would remain.
inline std::vector<MediaHandle> idleDecodersToRelease(
    std::vector<std::pair<MediaHandle, std::chrono::steady_clock::time_point>> lastUsed,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration idleAfter,
    size_t keepWarm)
{
    std::sort(lastUsed.begin(), lastUsed.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<MediaHandle> release;
    size_t remaining = lastUsed.size();
    for (const auto& [handle, used] : lastUsed) {
        if (remaining <= keepWarm || now - used < idleAfter) break;
        release.push_back(handle);
        --remaining;
    }
    return release;
}

/// Queue-ordering distance of a frame from its file's playhead. A file with
/// no recorded playhead sorts last instead of overflowing.
inline int64_t frameDistanceFromPlayhead(int64_t frameNumber,
                                         int64_t filePlayhead) noexcept
{
    if (filePlayhead <= INT64_MIN + 4) return INT64_MAX;
    const int64_t d = frameNumber - filePlayhead;
    return d < 0 ? -d : d;
}

/// `taskFilePlayhead` is the last frame requested from the TASK's own file.
/// Frame numbers of different files are unrelated, so another file's tasks
/// are stale only once that file's own playhead has moved past them.
inline bool replaceableByDecodeAheadRebuild(const PrefetchTask& task,
                                             MediaHandle handle,
                                             int64_t taskFilePlayhead) noexcept
{
    if (task.exactRequest) return false;
    return task.handle == handle || task.frameNumber < taskFilePlayhead - 4;
}

inline bool containsTaskKey(const std::deque<PrefetchTask>& queue,
                            const PrefetchTask& task)
{
    return std::any_of(queue.begin(), queue.end(),
        [&](const PrefetchTask& queued) { return sameTaskKey(queued, task); });
}

} // namespace prefetch_detail

// ── Constants ──────────────────────────────────────────────────────────────

constexpr double kSlowHardwarePreviewFallbackMs = 180.0;
constexpr uint64_t kSlowHardwarePreviewMinPixels = 1920ull * 1080ull;

// ── Helpers ────────────────────────────────────────────────────────────────

inline int64_t steadyClockMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline bool isInteractivePlaybackActive(const std::atomic<int64_t>& untilMs)
{
    return untilMs.load(std::memory_order_acquire) > steadyClockMillis();
}

/// Consecutive slow single-frame hardware decodes before a reduced-tier
/// preview decoder is reopened in software.
constexpr int kSlowHardwareFramesBeforeFallback = 3;

namespace prefetch_detail {

/// Whether one decode call counts as a slow hardware frame. Only a
/// single next-frame decode measures the decoder: a seek or a forward jump
/// decodes many pictures in one call, so a 2.4 s catch-up across 150 frames
/// used to count as one "slow frame" and pushed ordinary MP4s onto the
/// (slower, CPU-converted) software decoder for the rest of the session.
inline bool isSlowHardwareStep(bool hardware, bool singleFrameStep,
                               ResolutionTier tier, uint32_t sourceWidth,
                               uint32_t sourceHeight, double totalMs,
                               bool packedAlpha) noexcept
{
    if (!hardware || !singleFrameStep || tier == ResolutionTier::Full)
        return false;
    // Packed-alpha cache files are large and SW decode is ~200ms/frame --
    // strictly worse than the occasional NVDEC hiccup.
    if (packedAlpha) return false;
    if (totalMs < kSlowHardwarePreviewFallbackMs) return false;
    const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) *
                                  static_cast<uint64_t>(sourceHeight);
    return sourcePixels >= kSlowHardwarePreviewMinPixels;
}

} // namespace prefetch_detail

inline bool shouldFallbackToSoftwarePreview(const VideoDecoder& decoder,
                                     bool singleFrameStep,
                                     ResolutionTier tier,
                                     uint32_t sourceWidth,
                                     uint32_t sourceHeight,
                                     double totalMs,
                                     bool packedAlpha)
{
    return prefetch_detail::isSlowHardwareStep(
        decoder.isHardwareAccelerated(), singleFrameStep, tier,
        sourceWidth, sourceHeight, totalMs, packedAlpha);
}

#ifdef ROUNDTABLE_HAS_FFMPEG
inline void resetPrefetchConversionState(PrefetchDecoderState& state)
{
    if (state.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
        state.swsCtx = nullptr;
    }
    state.swsSrcW = 0;
    state.swsSrcH = 0;
    state.swsSrcFmt = -1;
    state.swsDstW = 0;
    state.swsDstH = 0;
}
#endif

// Consolidated: reopen decoder with specified mode (software or hardware).
bool reopenPrefetchDecoder(PrefetchDecoderState& state,
                            const PrefetchTask& task,
                            bool forceSoftware);

/// Threads for a prefetch worker's software decoder (ProRes, DNxHR,
/// CineForm, alpha formats). One worker owns each file, so this is the whole
/// CPU budget a software-decoded clip gets during playback; two threads held
/// 4K ProRes 4444 to roughly 15 fps. Several such files can decode at once,
/// hence a quarter of the logical cores rather than all of them.
inline int prefetchSoftwareDecodeThreads() noexcept
{
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(cores / 4, 2u, 8u));
}

/// Frame threads for an export decoder's software decode: the logical core
/// count, capped at 8 — each frame thread holds a decoded frame, and a
/// frame rarely has more than a few video layers decoding at once.
inline int exportDecodeThreads() noexcept
{
    const unsigned cores = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(cores, 2u, 8u));
}

/// Free a scrub/export slot's decoder state; call with slot.mutex held.
/// `retired` stops a caller that still holds the slot from reopening it.
inline void retireSlotLocked(ScrubDecoderSlot& slot)
{
    slot.retired = true;
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (slot.state.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(slot.state.swsCtx));
        slot.state.swsCtx = nullptr;
    }
#endif
    slot.state.decoder.reset();
    slot.state.lastDecodedFrame = -1;
    slot.lastExactFrame.reset();
    slot.exportGpu.reset();   // waits for its pending GPU work
}

inline bool reopenPrefetchDecoderAsSoftware(PrefetchDecoderState& state,
                                     const PrefetchTask& task)
{
    return reopenPrefetchDecoder(state, task, /*forceSoftware=*/true);
}

inline void logDecodePerf(const PrefetchTask& task, bool needSeek, int fwdFrames,
                    double decodeMs, double convertMs, double totalMs,
                    uint32_t width, uint32_t height)
{
    static thread_local int s_decLog = 0;
    if (++s_decLog % 10 != 1) return;

    if (needSeek) {
        spdlog::info("[PERF] prefetch decode: handle={} frame={} tier={} "
                     "needSeek=true fwdFrames={} decode={:.1f}ms convert={:.1f}ms total={:.1f}ms "
                     "{}x{}",
                     task.handle, task.frameNumber,
                     static_cast<int>(task.tier),
                     fwdFrames, decodeMs, convertMs, totalMs,
                     width, height);
    } else {
        spdlog::info("[PERF] prefetch decode: handle={} frame={} tier={} "
                     "needSeek=false decode={:.1f}ms convert={:.1f}ms total={:.1f}ms "
                     "{}x{}",
                     task.handle, task.frameNumber,
                     static_cast<int>(task.tier),
                     decodeMs, convertMs, totalMs,
                     width, height);
    }
}

} // namespace rt
