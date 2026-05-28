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
#include "media/FrameCache.h"         // chromaKeyInPlace, clearTransparentPixelRGB

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace rt {

namespace {

// Premiere-style live file replacement: a still image (single frame /
// zero duration) is decoded exactly once and pinned in the cache, so the
// scrub/prefetch decoder that just produced it is never needed again.
// Fully release it (reset(), not just close()) so the underlying OS file
// HANDLE is dropped and the image can be overwritten/deleted live in
// Explorer.  getScrubDecoder() recreates a fresh decoder on demand if the
// pinned frame is ever evicted and the still must be re-decoded.
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
                 task.filePath.filename().string(), task.handle);
}

} // namespace

std::shared_ptr<CachedFrame> MediaPool::decodePrefetchFrame(
    PrefetchDecoderState& state, const PrefetchTask& task,
    WorkerGpuState* wgs)
{
    auto perfDecodeT0 = std::chrono::high_resolution_clock::now();
    auto& decoder = *state.decoder;
    double targetTime = task.frameNumber / task.fps;

    // GREEN-suffixed chroma-key files MUST go through the CPU path
    // so chromaKeyInPlace() is applied; the GPU-resident path has no
    // chroma-key shader and would leave the green background visible.
    bool chromaKeyFile = false;
    {
        std::string fn = task.filePath.filename().string();
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        chromaKeyFile = (fn.find("GREEN") != std::string::npos);
    }

    // Sequential fast path: delta==1 or delta<=150 → advance decoder instead of seeking.
    bool needSeek = true;
    if (state.lastDecodedFrame >= 0) {
        int64_t delta = task.frameNumber - state.lastDecodedFrame;
        if (delta == 1) {
            needSeek = false;
        } else if (delta > 1 && delta <= 150) {
            needSeek = false;
            DecodedFrame skip;
            for (int64_t i = 0; i < delta - 1; ++i) {
                if (!decoder.decodeNext(skip)) {
                    needSeek = true;
                    break;
                }
            }
        }
    }

    if (needSeek) {
        // Keyframe seek + forward decode (avoids Precise seek B-frame bug).
        if (!decoder.seek(targetTime, SeekMode::Keyframe)) {
            spdlog::debug("MediaPool prefetch: seek failed handle={} frame={}",
                          task.handle, task.frameNumber);
            state.lastDecodedFrame = -1;
            return nullptr;
        }
        const double halfFrame = 0.5 / task.fps;
        DecodedFrame fwd;
        int fwdCount = 0;
        constexpr int kMaxForwardDecode = 600;
        while (fwdCount < kMaxForwardDecode) {
            if (!decoder.decodeNext(fwd)) {
                state.lastDecodedFrame = -1;
                return nullptr;
            }
            ++fwdCount;
            if (fwd.timestamp >= targetTime - halfFrame)
                break;
        }
        state.lastDecodedFrame = task.frameNumber;

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

    // NVDEC→SW fallback only on sequential decodes (seek frames are inherently slow).
    if (!needSeek &&
        shouldFallbackToSoftwarePreview(*state.decoder,
                                        task.tier,
                                        decoded.width,
                                        decoded.height,
                                        totalMs,
                                        task.packedAlpha)) {
        ++state.consecutiveSlowHwFrames;
        constexpr int kFallbackSlowFrameThreshold = 3;
        if (state.consecutiveSlowHwFrames >= kFallbackSlowFrameThreshold) {
            spdlog::warn("[PERF] MediaPool prefetch: handle={} '{}' switching to software after {} consecutive slow hw frames (last={:.1f}ms total, src={}x{}, dst={}x{}, tier={})",
                         task.handle,
                         task.filePath.filename().string(),
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
    } else {
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
