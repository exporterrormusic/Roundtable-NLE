// MediaPoolDecode.cpp - Frame decoding (extracted from MediaPool.cpp).

#include "MediaPool.h"
#include "GpuContext.h"
#include "Nv12Converter.h"
#include "PathUtils.h"
#include "decode/ConvertDecodedFrame.h"
#include "cache/FrameContentBounds.h"
#include "cuda/CudaVulkanInterop.h"
#include <volk.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>
#include <chrono>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

namespace {

constexpr double kSlowHardwarePreviewFallbackMs = 180.0;
constexpr uint64_t kSlowHardwarePreviewMinPixels = 1920ull * 1080ull;

bool shouldFallbackToSoftwarePreview(const VideoDecoder& decoder,
                                     ResolutionTier tier,
                                     uint32_t sourceWidth,
                                     uint32_t sourceHeight,
                                     double totalMs,
                                     bool packedAlpha)
{
    if (!decoder.isHardwareAccelerated() || tier == ResolutionTier::Full) {
        return false;
    }

    // Packed-alpha cache files are large and SW decode is ~200ms/frame --
    // strictly worse than the occasional NVDEC hiccup.  Never fall back
    // for packed-alpha sources.
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
void resetMediaEntryConversionState(MediaEntry& entry)
{
    if (entry.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(entry.swsCtx));
        entry.swsCtx = nullptr;
    }
    entry.swsSrcW = 0;
    entry.swsSrcH = 0;
    entry.swsSrcFmt = -1;
    entry.swsDstW = 0;
    entry.swsDstH = 0;
}
#endif

bool reopenMediaEntryAsSoftware(MediaEntry& entry)
{
    auto softwareDecoder = std::make_unique<VideoDecoder>();
    if (!softwareDecoder->open(entry.path, /*forceSoftware=*/true)) {
        spdlog::warn("MediaPool: failed to reopen '{}' in software mode for preview fallback",
                     pathToUtf8(entry.path.filename()));
        return false;
    }

    entry.decoder = std::move(softwareDecoder);
    entry.lastDecodedFrame = -1;
    entry.decodePathLogged = 0;
#ifdef ROUNDTABLE_HAS_FFMPEG
    resetMediaEntryConversionState(entry);
#endif
    return true;
}

} // namespace


// â”€â”€â”€ FFmpeg CLI helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€


std::shared_ptr<CachedFrame> MediaPool::decodeFrame(
    MediaEntry& entry, int64_t frameNumber, ResolutionTier tier, bool scrubMode)
{
    auto perfDecodeT0 = std::chrono::high_resolution_clock::now();
    auto& decoder = *entry.decoder;
    double fps = entry.info.fps > 0 ? entry.info.fps : 30.0;
    double targetTime = frameNumber / fps;

    bool isStillImage = (entry.info.duration <= 0.0 || entry.info.frameCount <= 1);
    // Still images are timeless — every "frame" produces the same pixels.
    // Pin them under index 0 so producers and consumers all look up the
    // single canonical entry instead of caching N copies under arbitrary
    // playhead frame numbers (which then expire from the LRU and trigger
    // another full software PNG decode — visible in the log as repeated
    // SLOW DECODE warnings for the same handle at frame=0, 20, 31, etc).
    if (isStillImage) frameNumber = 0;

    // â”€â”€ Sequential-playback fast path â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // During forward playback the frames come in order (N, N+1, N+2â€¦).
    // Seeking for each one is catastrophically slow: for ProRes the seek
    // goes back to a keyframe then decode-forwards through every frame in
    // between â†’ 5-12Ã— the work.  Instead, if the requested frame is the
    // next sequential one, just call decodeNext() directly.
    bool needSeek = true;
    if (!isStillImage && entry.lastDecodedFrame >= 0) {
        int64_t delta = frameNumber - entry.lastDecodedFrame;
        if (delta == 1) {
            needSeek = false;  // next frame â€” just decode
        } else if (delta == 0) {
            // Same frame requested again (shouldn't happen, cache
            // should have caught it) â€” seek as fallback
            needSeek = true;
        } else if (delta > 1 && delta <= 180) {
            // Forward skip within a reasonable window.  For HEVC / H.264
            // with long GOPs (e.g. the 44s CROWN mp4: ~60-frame GOP on
            // NVDEC) a fresh seek costs 100-180ms because NVDEC must
            // flush, rewind to the keyframe, and re-decode the GOP.
            // Decoding forward through up to ~180 inter-frames on NVDEC
            // (~3-5ms/frame ≈ 540-900ms worst-case) is often still better
            // than a fresh seek because the frames in between are also
            // emitted into the prefetch cache, so subsequent nearby
            // requests become free cache hits.  This matters when the
            // prefetch worker briefly falls behind the playhead and
            // needs to catch up over 1-3 seconds of video.
            needSeek = false;
            DecodedFrame skip;
            for (int64_t i = 0; i < delta - 1; ++i) {
                if (!decoder.decodeNext(skip)) {
                    needSeek = true;  // ran out of packets â€” fall back to seek
                    break;
                }
            }
        }
        // delta < 0 or > 30 â†’ need full seek
    }

    if (isStillImage) {
        // Still images (PNG, single-frame): reopen to reset demuxer state,
        // then decode the one frame.  Avoids seek issues with image2 demuxer.
        decoder.close();
        if (!decoder.open(entry.path)) {
            spdlog::warn("MediaPool: reopen failed for still image handle {}",
                         entry.handle);
            return nullptr;
        }
    } else if (needSeek) {
        // Video: seek to the target frame.
        // During scrubbing use Keyframe mode â€” seeks to the nearest keyframe
        // and decodes ONE frame.  Precise mode decodes forward from the
        // keyframe to the exact target, which can mean 60-128 frames of
        // decode work per seek â€” catastrophically slow for scrubbing.
        const SeekMode mode = scrubMode ? SeekMode::Keyframe : SeekMode::Precise;
        if (!decoder.seek(targetTime, mode)) {
            spdlog::warn("MediaPool: seek failed for handle {} frame {}",
                         entry.handle, frameNumber);
            return nullptr;
        }
    }

    // Decode the frame
    DecodedFrame decoded;
    if (!decoder.decodeNext(decoded)) {
        spdlog::warn("MediaPool: decode failed for handle {} frame {}",
                     entry.handle, frameNumber);
        entry.lastDecodedFrame = -1;  // reset on failure
        return nullptr;
    }

    // Guard: NVDEC / D3D11VA can return success with null data pointers
    // (stale surface, driver hiccup).  Reject early so the frame-drop
    // fallback in getFrame() can show a nearby cached frame instead of
    // passing an empty CachedFrame to the compositor (-> grey flicker).
    if (!decoded.data[0] && !decoded.isHardware) {
        spdlog::warn("MediaPool: decoded frame has null data — handle={} frame={}",
                     entry.handle, frameNumber);
        entry.lastDecodedFrame = -1;
        return nullptr;
    }

    entry.lastDecodedFrame = frameNumber;

    // â”€â”€ Diagnostic: log decode path selection (once per handle) â”€â”€â”€â”€â”€â”€
    if (entry.decodePathLogged == 0) {
        spdlog::info("[PERF] MediaPool: handle={} first decode: {}x{} hw={} packedAlpha={} "
                     "fmt={} rawFmt={} data[0]={} data[1]={}",
                     entry.handle, decoded.width, decoded.height,
                     decoded.isHardware, entry.packedAlpha,
                     static_cast<int>(decoded.format), decoded.rawFormat,
                     (void*)decoded.data[0], (void*)decoded.data[1]);
        entry.decodePathLogged = 1;
    }

    // Decode timing (before path selection)
    auto perfAfterDecode = std::chrono::high_resolution_clock::now();
    double decodeOnlyMs = std::chrono::duration<double, std::milli>(perfAfterDecode - perfDecodeT0).count();

    // â”€â”€ CUDAâ†’Vulkan zero-copy path â€” DISABLED â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // The CUDAâ†”Vulkan interop path allocated a full-resolution GPU texture
    // per frame and called vkWaitForFences(UINT64_MAX) inline, blocking
    // the render thread for 72-300ms/frame.  Real NLEs (Kdenlive, Shotcut,
    // Olive, DaVinci Resolve) use the simple path: NVDEC â†’ CPU transfer â†’
    // sws_scale downscale â†’ compositor batch-uploads small textures.
    //
    // The CPU path below respects ResolutionTier (downscales 1080Ã—3776 â†’
    // ~274Ã—960 for Half tier), so the FrameCache stores tiny frames (~1MB
    // vs 16MB), prefetch workers produce compatible frames, and the
    // GpuTexCache compositor upload is trivially fast.
    //
    // To re-enable CUDA zero-copy in the future, it needs pipelining
    // (ring buffer of shared allocations, deferred fence waits) instead
    // of the current synchronous-per-frame design.

    // If hardware frame, transfer to CPU for sws_scale + downscale
    if (decoded.isHardware) {
        if (entry.decodePathLogged == 1) {
            spdlog::info("[PERF] MediaPool: handle={} -> NVDEC+CPU path ({}x{} packedAlpha={} tier={})",
                         entry.handle, decoded.width, decoded.height,
                         entry.packedAlpha, static_cast<int>(tier));
            entry.decodePathLogged = 2;
        }
        DecodedFrame cpuFrame;
        if (!decoder.transferHardwareFrame(decoded, cpuFrame)) {
            spdlog::warn("MediaPool: hw transfer failed for handle {} frame {}",
                         entry.handle, frameNumber);
            return nullptr;
        }
        decoded = std::move(cpuFrame);
    }

    // After hardware transfer (or for software decode), verify data pointer.
    // NVDEC/D3D11VA can produce frames where avcodec_receive_frame succeeds
    // but the surface has no usable data — reject early.
    if (!decoded.data[0] || decoded.width == 0 || decoded.height == 0) {
        spdlog::warn("MediaPool: decoded frame has null/zero data — "
                     "handle={} frame={} data[0]={} {}x{}",
                     entry.handle, frameNumber, (void*)decoded.data[0],
                     decoded.width, decoded.height);
        entry.lastDecodedFrame = -1;
        return nullptr;
    }

    // Build CachedFrame with BGRA pixel data
    auto cached = std::make_shared<CachedFrame>();
    cached->mediaId     = entry.handle;
    cached->frameNumber = frameNumber;
    cached->width       = decoded.width;
    cached->height      = decoded.height;
    cached->tier        = tier;
    cached->isKeyframe  = decoded.isKeyframe;
    cached->timestamp   = decoded.timestamp;
    // Default to CPU origin; the GPU NV12/YUV420P fast paths below override
    // to GpuShader before jumping to nv12_done.
    cached->origin      = ConverterOrigin::CpuSws;
    // Pin static images (PNGs / single-frame media) so the LRU never
    // evicts them.  Re-decoding mid-playback shows up as a "missing layer"
    // skip in the compositor (e.g. backgrounds disappearing for a frame).
    cached->pinned      = isStillImage;

    // Copy pixel data from decoded frame and convert to BGRA
    if (decoded.data[0] && decoded.width > 0 && decoded.height > 0) {

#ifdef ROUNDTABLE_HAS_FFMPEG
        // Convert any pixel format → BGRA (shared core; see
        // ConvertDecodedFrame.h for everything the helper covers).
        const AVPixelFormat srcFmt =
            static_cast<AVPixelFormat>(resolveDecodedAvFormat(decoded));

        const int w = static_cast<int>(decoded.width);
        const int h = static_cast<int>(decoded.height);


        // â”€â”€ Preview downscale â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Full preserves native dimensions even for unusually tall/wide
        // artwork. Half and Quarter are true source-relative preview tiers.
        const auto tierSize = resolutionTierDimensions(w, h, tier);
        const int dstW = tierSize.width;
        const int dstH = tierSize.height;

        const bool needsResize = (dstW != w || dstH != h);

        // Colourspace gate (Phase 4.1): the GPU NV12/YUV420P shaders hardcode
        // BT.709 + limited range + SDR.  Only take the GPU fast-paths below
        // when the source matches; otherwise fall through to the CPU sws core,
        // which honours the source's matrix/range (resolveColorConversion).
        // Mirrors the gate in convertDecodedToCacheGpu so the urgent/scrub
        // and prefetch paths agree on which sources are GPU-eligible.
        const bool gpuColorOk = resolveColorConversion(entry.info).isGpuShaderDefault;

        // ── GPU NV12 → BGRA with integrated downscale ──────────────────
        // Uses Nv12Converter compute shader to convert AND downscale in a
        // single GPU dispatch.  Input uploaded at srcW×srcH, shader writes
        // output at dstW×dstH using bilinear-filtered texture sampling.
        // No per-frame persistent texture creation — just CPU readback.
        // 4-tile luma-packed alpha needs special GPU unpack (handled above);
        // 2-tile stacked-alpha (e.g. Wells HEVC) works fine with normal
        // NV12→BGRA conversion — the compositor splits via isPacked.
        if (gpuColorOk && entry.info.packedTiles != 4 &&
            (srcFmt == AV_PIX_FMT_NV12 || decoded.format == PixelFormat::NV12)
            && w <= 16384 && h <= 16384) {
            if (entry.decodePathLogged < 4) {
                spdlog::info("[PERF] MediaPool: handle={} -> GPU NV12 path ({}x{} -> {}x{})",
                             entry.handle, w, h, dstW, dstH);
                entry.decodePathLogged = 4;
            }
            auto& gpu = GpuContext::get();
            if (gpu.isInitialized()) {
                Nv12Converter* conv = gpu.nv12Converter(
                    static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH));
                // convertAndReadback* (NOT convertSyncScaled+readbackOutput):
                // the converter is SHARED per (dstW,dstH) and only the
                // convertAndReadback wrappers hold m_apiMutex across the
                // convert + readback pair.  Split calls let another thread's
                // convert overwrite the output texture between our dispatch
                // and our readback (wrong-content frames).
                if (conv && conv->convertAndReadbackNV12Scaled(
                        decoded.data[0], decoded.linesize[0],
                        decoded.data[1], decoded.linesize[1],
                        static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                        static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
                        cached->pixels))
                {
                    cached->width  = static_cast<uint32_t>(dstW);
                    cached->height = static_cast<uint32_t>(dstH);
                    cached->stride = static_cast<uint32_t>(dstW) * 4;
                    cached->origin = ConverterOrigin::GpuShader;
                    goto nv12_done;   // skip sws_scale path
                }
            }
        }

        // ── GPU YUV420P → BGRA with integrated downscale ───────────────
        // 4-tile luma-packed alpha needs special GPU unpack (handled above);
        // 2-tile stacked-alpha works fine with normal YUV420P→BGRA conversion.
        if (gpuColorOk && entry.info.packedTiles != 4 &&
            (srcFmt == AV_PIX_FMT_YUV420P || decoded.format == PixelFormat::YUV420P) &&
            w <= 16384 && h <= 16384 && decoded.data[0] && decoded.data[1] && decoded.data[2])
        {
            if (entry.decodePathLogged < 5) {
                spdlog::info("[PERF] MediaPool: handle={} -> GPU YUV420P path ({}x{} -> {}x{})",
                             entry.handle, w, h, dstW, dstH);
                entry.decodePathLogged = 5;
            }
            auto& gpu = GpuContext::get();
            if (gpu.isInitialized()) {
                Nv12Converter* conv = gpu.nv12Converter(
                    static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH));
                // See NV12 branch above: must use the locked
                // convert-and-readback wrapper on the shared converter.
                if (conv && conv->convertAndReadbackYuv420pScaled(
                        decoded.data[0], decoded.linesize[0],
                        decoded.data[1], decoded.linesize[1],
                        decoded.data[2], decoded.linesize[2],
                        static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                        static_cast<uint32_t>(dstW), static_cast<uint32_t>(dstH),
                        cached->pixels))
                {
                    cached->width  = static_cast<uint32_t>(dstW);
                    cached->height = static_cast<uint32_t>(dstH);
                    cached->stride = static_cast<uint32_t>(dstW) * 4;
                    cached->origin = ConverterOrigin::GpuShader;
                    goto nv12_done;   // skip sws_scale path
                }
            }
        }

        // NOTE: ProRes 4444 GPU convert intentionally not wired yet — the
        // shared Nv12Converter command pool is not safe across the threads
        // that reach this path.  ProRes uses the CPU core below until the
        // per-worker GPU path lands.
        if (srcFmt != AV_PIX_FMT_BGRA || needsResize) {
            if (entry.decodePathLogged < 6) {
                spdlog::info("MediaPool: handle={} -> CPU sws_scale ({}x{} -> {}x{} fmt={} packedAlpha={})",
                             entry.handle, w, h, dstW, dstH,
                             static_cast<int>(srcFmt), entry.packedAlpha);
                entry.decodePathLogged = 6;
            }
        }
        if (!convertDecodedToBgra(
                decoded, static_cast<int>(srcFmt), dstW, dstH,
                entry.info, entry.path,
                SwsCacheRef{entry.swsCtx, entry.swsSrcW, entry.swsSrcH,
                            entry.swsSrcFmt, entry.swsDstW, entry.swsDstH},
                /*pool=*/nullptr, *cached))
        {
            spdlog::warn("MediaPool: sws_getContext failed for handle {} frame {}",
                         entry.handle, frameNumber);
        }

nv12_done:  // GPU NV12 fast-path jumps here after successful conversion
        (void)0;
#else
        // No FFmpeg â€” copy raw plane data as-is (fallback)
        int planeCount = 0;
        switch (decoded.format) {
            case PixelFormat::YUV420P: planeCount = 3; break;
            case PixelFormat::NV12:    planeCount = 2; break;
            default:                   planeCount = 1; break;
        }

        size_t totalSize = 0;
        size_t planeSizes[4] = {};
        for (int i = 0; i < planeCount; ++i) {
            if (!decoded.data[i]) break;
            uint32_t planeHeight = (i == 0) ? decoded.height : decoded.height / 2;
            planeSizes[i] = static_cast<size_t>(decoded.linesize[i]) * planeHeight;
            totalSize += planeSizes[i];
        }

        cached->pixels.resize(totalSize);
        cached->stride = decoded.linesize[0];

        size_t offset = 0;
        for (int i = 0; i < planeCount; ++i) {
            if (!decoded.data[i] || planeSizes[i] == 0) break;
            std::memcpy(cached->pixels.data() + offset, decoded.data[i], planeSizes[i]);
            offset += planeSizes[i];
        }
#endif
    }


    // Safety: if the frame ended up with no usable pixel data (e.g. NVDEC
    // surface was stale, sws_getContext failed, etc.), return nullptr so
    // getFrame()'s frame-drop fallback can serve a nearby valid frame
    // instead of an empty CachedFrame that the compositor would skip.
    if (cached->pixels.empty() && !cached->gpuReady && !cached->lazyReadback) {
        spdlog::warn("MediaPool: empty frame after decode — handle={} frame={} "
                     "data[0]={} hw={} {}x{}",
                     entry.handle, frameNumber, (void*)decoded.data[0],
                     decoded.isHardware, cached->width, cached->height);
        entry.lastDecodedFrame = -1;
        return nullptr;
    }

    if (cached->pinned && !cached->pixels.empty())
        computeBgraContentBounds(*cached);

    // Insert into cache (after empty-pixels check so bad frames aren't cached)
    m_cache->put(cached);
    if (m_diskCache) m_diskCache->putAsync(cached);

    // Total decode path timing (always-on)
    {
        auto perfDecodeT1 = std::chrono::high_resolution_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(perfDecodeT1 - perfDecodeT0).count();
        static int s_decodeLog = 0;
        if (++s_decodeLog % 30 == 0 || totalMs > 15.0) {
            const char* pathName = "UNKNOWN";
            if (cached->gpuReady) pathName = "GPU-resident";
            else if (cached->pixels.empty()) pathName = "EMPTY";
            else pathName = "CPU sws_scale";
            spdlog::info("[PERF] decodeFrame: handle={} frame={} -> {} ({}x{} gpuReady={}) "
                         "decode={:.1f}ms total={:.1f}ms",
                         entry.handle, frameNumber, pathName,
                         cached->width, cached->height, cached->gpuReady,
                         decodeOnlyMs, totalMs);
        }

        if (shouldFallbackToSoftwarePreview(*entry.decoder,
                                            tier,
                                            decoded.width,
                                            decoded.height,
                                            totalMs,
                                            entry.packedAlpha)) {
            spdlog::warn("[PERF] MediaPool: handle={} '{}' switching preview decode to software after slow hw path ({:.1f}ms total, src={}x{}, dst={}x{}, tier={})",
                         entry.handle,
                         pathToUtf8(entry.path.filename()),
                         totalMs,
                         decoded.width,
                         decoded.height,
                         cached->width,
                         cached->height,
                         static_cast<int>(tier));
            reopenMediaEntryAsSoftware(entry);
        }
    }

    // ── Premiere-style live file replacement for still images ───────────
    // A still (PNG/JPG single frame) is decoded exactly once: its pixels are
    // now copied into `cached`, inserted into the LRU as `pinned` (line ~266)
    // and into the disk cache, so the decoder is never consulted again for
    // this media.  Keeping entry.decoder open would hold an OS file HANDLE on
    // the image for the whole session — even though it carries
    // FILE_SHARE_DELETE|WRITE, an external image editor's "Save" opens the
    // file *exclusively* and is refused, and a delete-pending file cannot be
    // recreated until we close.  Releasing the handle here lets the user
    // overwrite/replace the image live in Explorer (the next decode, if the
    // pinned cache is ever evicted, simply reopens via the isStillImage
    // close()+open() branch above).
    if (isStillImage) {
        decoder.close();
        spdlog::warn("MediaPool: released file handle for still image '{}' "
                     "(handle={}) — live replace enabled",
                     pathToUtf8(entry.path.filename()), entry.handle);
    }

    return cached;
}

// â”€â”€â”€ Prefetch background worker â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

} // namespace rt
