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
#include <deque>
#include <iterator>
#include <utility>

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

inline bool replaceableByDecodeAheadRebuild(const PrefetchTask& task,
                                             MediaHandle handle,
                                             int64_t playhead) noexcept
{
    if (task.exactRequest) return false;
    return task.handle == handle || task.frameNumber < playhead - 4;
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

inline bool shouldFallbackToSoftwarePreview(const VideoDecoder& decoder,
                                     ResolutionTier tier,
                                     uint32_t sourceWidth,
                                     uint32_t sourceHeight,
                                     double totalMs,
                                     bool packedAlpha)
{
    if (!decoder.isHardwareAccelerated() || tier == ResolutionTier::Full) {
        return false;
    }
    if (packedAlpha) {
        return false;
    }
    if (totalMs < kSlowHardwarePreviewFallbackMs) {
        return false;
    }
    const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) *
                                  static_cast<uint64_t>(sourceHeight);
    return sourcePixels >= kSlowHardwarePreviewMinPixels;
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
