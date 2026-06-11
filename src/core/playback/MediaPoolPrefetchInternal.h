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
#include <chrono>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
}
#endif

namespace rt {

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
