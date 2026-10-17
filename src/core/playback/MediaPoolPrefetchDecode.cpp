/*
 * MediaPoolPrefetchDecode.cpp — Core prefetch decode logic extracted from
 * MediaPoolPrefetch.cpp.
 *
 * Contains: decodePrefetchFrame() — the main decode → convert → cache path
 * used by both prefetch workers and scrub-frame misses.
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"
#include "MediaPoolPrefetchGpu.h"     // tryConvertDecodedToCacheGpu
#include "cache/FrameCache.h"
#include "decode/ConvertDecodedFrame.h"
#include "decode/FrameLabel.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace rt {

namespace {

// Premiere-style live file replacement: a still image (single frame /
// zero duration) is decoded exactly once and pinned in the cache, so the
// scrub/prefetch decoder that just produced it is never needed again.
// Fully release it (reset(), not just close()) so the underlying OS file
// HANDLE is dropped and the image can be overwritten/deleted live in
// Explorer.  The next scrub/exact request reopens a fresh decoder (under
// the slot lock, in getFrame) if the still must be decoded again.
void releaseStillDecoder(PrefetchDecoderState& state, const PrefetchTask& task)
{
    if (task.info.frameCount > 1 && task.info.duration > 0.0)
        return;  // real video — keep the streaming decoder open
    if (!state.decoder)
        return;
    state.decoder.reset();
    state.lastDecodedFrame = -1;
    spdlog::warn("MediaPool: released scrub/prefetch handle for still image "
                 "'{}' (handle={}) — live replace enabled",
                 pathToUtf8(task.filePath.filename()), task.handle);
}

/// Whether this decode's picture should be checked against its label.
bool shouldCheckLabel(const PrefetchDecoderState& state, const PrefetchTask& task,
                      const DecodedFrame& picture)
{
    return !state.timestampsUnreliable && !task.info.isVFR &&
           task.info.frameCount > 1 && task.info.duration > 0.0 &&
           picture.pts != kNoPts;
}

} // namespace

std::shared_ptr<CachedFrame> MediaPool::decodePrefetchFrame(
    PrefetchDecoderState& state, const PrefetchTask& task,
    WorkerGpuState* wgs)
{
    auto perfDecodeT0 = std::chrono::high_resolution_clock::now();
    double targetTime = task.frameNumber / task.fps;

    // GREEN-suffixed chroma-key files MUST go through the CPU path
    // so chromaKeyInPlace() is applied; the GPU-resident path has no
    // chroma-key shader and would leave the green background visible.
    const bool chromaKeyFile = isGreenScreenMediaPath(task.filePath);

    // Sequential fast path: delta==1 or delta<=150 → advance decoder instead of seeking.
    // A fresh still decoder is already positioned at its only frame. Seeking
    // to timestamp zero makes FFmpeg's image demuxer reopen and re-probe the
    // PNG before decoding it, adding another cold open to every miss.
    const bool freshSingleFrame =
        state.lastDecodedFrame < 0 && task.frameNumber == 0 &&
        (task.info.duration <= 0.0 || task.info.frameCount <= 1);
    bool needSeek = !freshSingleFrame;
    bool singleFrameStep = false;  // only these measure the hw decoder
    const bool forceReseek = std::exchange(state.forceReseek, false);
    if (state.lastDecodedFrame >= 0 && !forceReseek) {
        int64_t delta = task.frameNumber - state.lastDecodedFrame;
        if (delta == 1) {
            needSeek = false;
            singleFrameStep = true;
        } else if (delta > 1 && delta <= 150) {
            // Short forward jump (scrub step, prefetch catching up): decode
            // on from here, skipping pictures nothing references, instead of
            // fully decoding every frame in between.
            needSeek = !state.decoder->advanceTo(targetTime);
        }
    }

    // A frame-threaded software decoder is reopened rather than flushed:
    // frame threads and avcodec_flush_buffers corrupted H.264 references in
    // the past (see openScrubDecoder). Opening a software decoder costs a
    // few ms and an export seeks about once per clip. A decoder that keeps
    // seeking (a reversed clip seeks every frame) goes slice-threaded for
    // good instead of reopening every frame.
    if (needSeek && state.frameThreaded && state.lastDecodedFrame >= 0 &&
        !state.decoder->isHardwareAccelerated()) {
        constexpr int kFrameThreadedSeekLimit = 8;
        const bool keepFrameThreads =
            ++state.frameThreadedSeeks < kFrameThreadedSeekLimit;
        auto fresh = std::make_unique<VideoDecoder>();
        if (!fresh->open(task.filePath, /*forceSoftware=*/false,
                         keepFrameThreads ? exportDecodeThreads() : 2,
                         /*sliceOnlyThreading=*/!keepFrameThreads)) {
            spdlog::warn("MediaPool: reopening export decoder failed for handle={}",
                         task.handle);
            state.decoder.reset();
            state.lastDecodedFrame = -1;
            return nullptr;
        }
        state.decoder = std::move(fresh);
        state.frameThreaded = keepFrameThreads;
    }
    auto& decoder = *state.decoder;

    if (needSeek) {
        // Precise seek: back to the keyframe, then forward to the exact
        // frame, skipping unreferenced pictures and backing off further when
        // an open-GOP keyframe lands AFTER the target. (This used to seek to
        // the keyframe and decode forward here by hand, which on open-GOP
        // footage stopped at the keyframe and cached it as the target.)
        if (!decoder.seek(targetTime, SeekMode::Precise)) {
            spdlog::debug("MediaPool prefetch: seek failed handle={} frame={}",
                          task.handle, task.frameNumber);
            state.lastDecodedFrame = -1;
            return nullptr;
        }
        DecodedFrame fwd;
        const int fwdCount = 1;
        if (!decoder.decodeNext(fwd)) {
            state.lastDecodedFrame = -1;
            return nullptr;
        }
        state.lastDecodedFrame = task.frameNumber;
        // A precise seek is what defines frame N. Landing off the target
        // means this file's timestamps don't map onto frame numbers (a start
        // offset, a gap), so stop checking them rather than seek every frame.
        if (shouldCheckLabel(state, task, fwd) &&
            !timestampMatchesFrame(fwd.timestamp, task.frameNumber, task.fps)) {
            state.timestampsUnreliable = true;
            m_perf.labelMismatches.fetch_add(1, std::memory_order_relaxed);
            spdlog::warn("[FRAME-LABEL] handle={} '{}': seek to frame {} gave a "
                         "picture at {:.4f}s (frame {}) - not checking this "
                         "file's timestamps",
                         task.handle, pathToUtf8(task.filePath.filename()),
                         task.frameNumber, fwd.timestamp,
                         frameFromTimestamp(fwd.timestamp, task.fps));
        }

        auto perfDecodeT1a = std::chrono::high_resolution_clock::now();
        // UPGRADE_PLAN Phase 4: try GPU-resident path first; fall back to
        // CPU on any eligibility failure (feature flag off, headless,
        // packed-alpha, unsupported pixel format, device-lost, etc.).
        // GREEN chroma-key files skip GPU — chroma key runs on CPU only.
        auto cached = chromaKeyFile ? nullptr
            : tryConvertDecodedToCacheGpu(
                *this, state, task, fwd, task.frameNumber, wgs);
        if (!cached)
            cached = convertDecodedToCache(state, task, fwd, task.frameNumber);
        if (!cached) return nullptr;

        auto perfDecodeT1 = std::chrono::high_resolution_clock::now();
        const double totalMs = std::chrono::duration<double, std::milli>(perfDecodeT1 - perfDecodeT0).count();
        const double decodeOnlyMs = std::chrono::duration<double, std::milli>(perfDecodeT1a - perfDecodeT0).count();
        const double convertMs = totalMs - decodeOnlyMs;

        logDecodePerf(task, true, fwdCount, decodeOnlyMs, convertMs, totalMs,
                      fwd.width, fwd.height);
        releaseStillDecoder(state, task);
        return cached;
    }

    DecodedFrame decoded;
    if (!decoder.decodeNext(decoded)) {
        state.lastDecodedFrame = -1;
        return nullptr;
    }
    // Decoding on from the last picture assumes the stream yields exactly
    // the pictures in between. When the picture says otherwise, decode this
    // frame again through a precise seek (once: the seek path never comes
    // back here) instead of caching the wrong picture under its number.
    if (shouldCheckLabel(state, task, decoded) &&
        !timestampMatchesFrame(decoded.timestamp, task.frameNumber, task.fps)) {
        m_perf.labelReseeks.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<int> s_reseekLog{0};
        const int n = ++s_reseekLog;
        if (n <= 20 || n % 100 == 0) {
            spdlog::warn("[FRAME-LABEL] handle={} '{}': decoding on to frame {} "
                         "gave a picture at {:.4f}s (frame {}) - re-seeking",
                         task.handle, pathToUtf8(task.filePath.filename()),
                         task.frameNumber, decoded.timestamp,
                         frameFromTimestamp(decoded.timestamp, task.fps));
        }
        state.forceReseek = true;
        return decodePrefetchFrame(state, task, wgs);
    }
    state.lastDecodedFrame = task.frameNumber;

    auto perfDecodeT1a = std::chrono::high_resolution_clock::now();
    // UPGRADE_PLAN Phase 4: GPU-resident path first; CPU fallback.
    // GREEN chroma-key files skip GPU (see seek-path comment above).
    auto cached = chromaKeyFile ? nullptr
        : tryConvertDecodedToCacheGpu(
            *this, state, task, decoded, task.frameNumber, wgs);
    if (!cached)
        cached = convertDecodedToCache(state, task, decoded, task.frameNumber);
    if (!cached)
        return nullptr;

    auto perfDecodeT1 = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(perfDecodeT1 - perfDecodeT0).count();
    const double decodeOnlyMs = std::chrono::duration<double, std::milli>(perfDecodeT1a - perfDecodeT0).count();
    const double convertMs = totalMs - decodeOnlyMs;

    logDecodePerf(task, false, 0, decodeOnlyMs, convertMs, totalMs,
                  decoded.width, decoded.height);

    // NVDEC→SW fallback only on single next-frame decodes: seeks and
    // forward jumps decode many pictures in one call.
    // Not for export: its CPU transfer + convert makes every hardware frame
    // look "slow", and the software fallback is far slower still.
    if (!state.exportDecoder &&
        shouldFallbackToSoftwarePreview(*state.decoder,
                                        singleFrameStep,
                                        task.tier,
                                        decoded.width,
                                        decoded.height,
                                        totalMs,
                                        task.packedAlpha)) {
        ++state.consecutiveSlowHwFrames;
        if (state.consecutiveSlowHwFrames >= kSlowHardwareFramesBeforeFallback) {
            spdlog::warn("[PERF] MediaPool prefetch: handle={} '{}' switching to software after {} consecutive slow hw frames (last={:.1f}ms total, src={}x{}, dst={}x{}, tier={})",
                         task.handle,
                         pathToUtf8(task.filePath.filename()),
                         state.consecutiveSlowHwFrames,
                         totalMs,
                         decoded.width,
                         decoded.height,
                         cached->width,
                         cached->height,
                         static_cast<int>(task.tier));
            reopenPrefetchDecoderAsSoftware(state, task);
            state.consecutiveSlowHwFrames = 0;
        }
    } else if (singleFrameStep) {
        state.consecutiveSlowHwFrames = 0;
    }

    // Rolling average decode time (EMA: 0.875*old + 0.125*sample)
    {
        uint64_t us = static_cast<uint64_t>(totalMs * 1000.0);
        uint64_t prev = m_perf.avgDecodeUs.load(std::memory_order_relaxed);
        uint64_t avg = (prev * 7 + us) / 8;
        m_perf.avgDecodeUs.store(avg, std::memory_order_relaxed);
    }

    releaseStillDecoder(state, task);
    return cached;
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
