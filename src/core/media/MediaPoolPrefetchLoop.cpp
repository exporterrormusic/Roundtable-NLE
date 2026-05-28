/*
 * MediaPoolPrefetchLoop.cpp — Loop pre-decode logic extracted from
 * MediaPoolPrefetch.cpp.
 *
 * Contains: loopPreDecodeDispatcher(), startLoopPreDecode(),
 * loopPreDecodeWorker().
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

void MediaPool::startLoopPreDecode(MediaHandle handle, ResolutionTier tier, int64_t priority)
{
    std::filesystem::path path;
    VideoStreamInfo info;
    bool packed = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry) return;
        path   = entry->path;
        info   = entry->info;
        packed = entry->packedAlpha;
    }

    // Skip clips too long to be a meaningful loop.
    if (info.frameCount > LOOP_PREDECODE_MAX_FRAMES) {
        spdlog::info("[PERF] Loop pre-decode: SKIP handle={} '{}' ({} frames > cap {})",
                     handle, path.filename().string(),
                     info.frameCount, LOOP_PREDECODE_MAX_FRAMES);
        return;
    }

    // Skip codecs that decode catastrophically slowly in SW (ProRes, DNxHD).
    {
        const std::string& cn = info.codecName;
        if (cn == "prores" || cn == "prores_ks" || cn == "prores_aw" ||
            cn == "dnxhd"  || cn == "dnxhr") {
            spdlog::info("[PERF] Loop pre-decode: SKIP handle={} '{}' (codec={} too slow for SW pre-decode)",
                         handle, path.filename().string(), cn);
            return;
        }
    }

    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        if (m_loopPreDecodeActive.count(handle) ||
            m_loopPreDecodeDone.count(handle))
            return;

        m_loopPreDecodeActive.insert(handle);
        LoopPreDecodeTask task;
        task.priority    = priority;
        task.seq         = ++m_loopPreDecodeSeq;
        task.handle      = handle;
        task.path        = path;
        task.info        = info;
        task.packedAlpha = packed;
        task.tier        = tier;
        m_loopPreDecodeQueue.push(std::move(task));
    }
    m_loopPreDecodeCv.notify_one();

    spdlog::info("[PERF] Loop pre-decode: enqueued handle={} '{}' ({} frames, priority={})",
                 handle, path.filename().string(), info.frameCount, priority);
}

void MediaPool::loopPreDecodeDispatcher()
{
#ifdef _WIN32
    // Background work — never preempt playback/composite/prefetch threads.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        LoopPreDecodeTask task;
        {
            std::unique_lock lock(m_loopPreDecodeMutex);
            m_loopPreDecodeCv.wait(lock, [this] {
                return !m_loopPreDecodeRunning.load() || !m_loopPreDecodeQueue.empty();
            });
            if (!m_loopPreDecodeRunning.load() && m_loopPreDecodeQueue.empty())
                return;
            task = m_loopPreDecodeQueue.top();
            m_loopPreDecodeQueue.pop();
        }

        spdlog::info("[PERF] Loop pre-decode: handle={} '{}' starting decode (priority={})",
                     task.handle, task.path.filename().string(), task.priority);
        loopPreDecodeWorker(task.handle, std::move(task.path), task.info,
                            task.packedAlpha, task.tier);
    }
}

void MediaPool::loopPreDecodeWorker(
    MediaHandle handle, std::filesystem::path path,
    VideoStreamInfo info, bool packedAlpha, ResolutionTier tier)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    // Use hardware decode (NVDEC) when available.  The RTX 4090 supports
    // 5+ concurrent NVDEC sessions; the 4 prefetch workers + loop workers
    // stay well within that budget.  FFmpeg auto-falls back to software
    // if the session limit is hit, so there is zero crash risk.
    VideoDecoder decoder;
    if (!decoder.open(path, /*forceSoftware=*/false, /*maxThreads=*/2)) {
        spdlog::warn("Loop pre-decode: failed to open decoder for handle={} '{}'",
                     handle, path.filename().string());
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.erase(handle);
        return;
    }

    const int64_t frameCount = info.frameCount;
    // Stop 2 frames before EOF to avoid inaccurate container metadata.
    const int64_t safeFrameCount = std::max<int64_t>(1, frameCount - 2);
    int64_t decoded_count = 0;
    int64_t skipped_count = 0;
    int consecutive_failures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 3;

    // Start at playhead % frameCount so playback doesn't wait for frame 0.
    int64_t startFrame = 0;
    {
        const int64_t ph = m_playheadFrame.load(std::memory_order_relaxed);
        if (ph > 0 && frameCount > 1) {
            startFrame = ((ph % frameCount) + frameCount) % frameCount;
        }
    }
    if (startFrame > 0 && info.fps > 0.0) {
        decoder.seek(static_cast<double>(startFrame) / info.fps, SeekMode::Precise);
    } else {
        decoder.seek(0.0, SeekMode::Precise);
    }

    SwsContext* sws = nullptr;
    int swsSrcW = 0, swsSrcH = 0, swsSrcFmt = -1;
    int swsDstW = 0, swsDstH = 0;

    for (int64_t fi = 0; fi < safeFrameCount; ++fi) {
        const int64_t f = (startFrame + fi) % std::max<int64_t>(1, frameCount);
        if (!m_prefetchRunning) break;

        // Yield to playback during interactive periods.
        while (isInteractivePlaybackActive(m_interactivePlaybackUntilMs)) {
            if (!m_prefetchRunning) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (!m_prefetchRunning) break;

        // Re-seek on wraparound transition.
        if (fi > 0 && f == 0 && startFrame > 0) {
            decoder.seek(0.0, SeekMode::Precise);
        }

        if (m_cache->contains(handle, f, tier)) {
            ++skipped_count;
            DecodedFrame discard;
            decoder.decodeNext(discard);
            continue;
        }

        DecodedFrame raw;
        if (!decoder.decodeNext(raw)) {
            spdlog::debug("Loop pre-decode: decode failed at frame {} for handle={}",
                          f, handle);
            ++skipped_count;
            if (++consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
                spdlog::debug("Loop pre-decode: {} consecutive failures at EOF, stopping for handle={}",
                              consecutive_failures, handle);
                break;
            }
            continue;
        }
        consecutive_failures = 0;

        if (raw.isHardware) {
            DecodedFrame cpuFrame;
            if (!decoder.transferHardwareFrame(raw, cpuFrame)) continue;
            raw = std::move(cpuFrame);
        }

        if (!raw.data[0] || raw.width == 0 || raw.height == 0) continue;

        auto cached = std::make_shared<CachedFrame>();
        cached->mediaId     = handle;
        cached->frameNumber = f;
        cached->width       = raw.width;
        cached->height      = raw.height;
        cached->tier        = tier;
        cached->isKeyframe  = raw.isKeyframe;
        cached->timestamp   = raw.timestamp;
        cached->pinned      = (info.frameCount <= 1);
        cached->isLoopFrame = true;

#ifdef ROUNDTABLE_HAS_FFMPEG
        AVPixelFormat srcFmt = AV_PIX_FMT_YUV420P;
        if (raw.rawFormat >= 0) {
            srcFmt = static_cast<AVPixelFormat>(raw.rawFormat);
        } else {
            switch (raw.format) {
                case PixelFormat::YUV420P: srcFmt = AV_PIX_FMT_YUV420P; break;
                case PixelFormat::NV12:    srcFmt = AV_PIX_FMT_NV12;    break;
                case PixelFormat::BGRA:    srcFmt = AV_PIX_FMT_BGRA;    break;
                case PixelFormat::RGBA:    srcFmt = AV_PIX_FMT_RGBA;    break;
                default:                   srcFmt = AV_PIX_FMT_YUV420P; break;
            }
        }

        const int w = static_cast<int>(raw.width);
        const int h = static_cast<int>(raw.height);

        int maxDim = (tier == ResolutionTier::Quarter) ? 480
                   : (tier == ResolutionTier::Half)    ? 960
                   :                                     1920;
        const int contentH = (packedAlpha && h > 1) ? h / 2 : h;
        int dW = w, dH = h;
        if (w > maxDim || contentH > maxDim) {
            const float scale = std::min(
                static_cast<float>(maxDim) / w,
                static_cast<float>(maxDim) / contentH);
            dW = std::max(2, static_cast<int>(w * scale) & ~1);
            dH = std::max(2, static_cast<int>(h * scale) & ~1);
        }

        const bool needsResize = (dW != w || dH != h);

        if (srcFmt == AV_PIX_FMT_BGRA && !needsResize) {
            cached->stride = w * 4;
            cached->pixels.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                std::memcpy(cached->pixels.data() + y * cached->stride,
                            raw.data[0] + y * raw.linesize[0],
                            static_cast<size_t>(w) * 4);
            }
        } else {
            if (!sws || swsSrcW != w || swsSrcH != h ||
                swsSrcFmt != static_cast<int>(srcFmt) ||
                swsDstW != dW || swsDstH != dH) {
                if (sws) sws_freeContext(sws);
                sws = sws_getContext(w, h, srcFmt,
                                     dW, dH, AV_PIX_FMT_BGRA,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                swsSrcW = w;  swsSrcH = h;
                swsSrcFmt = static_cast<int>(srcFmt);
                swsDstW = dW; swsDstH = dH;
            }
            if (!sws) continue;

            cached->width  = static_cast<uint32_t>(dW);
            cached->height = static_cast<uint32_t>(dH);
            cached->stride = static_cast<uint32_t>(dW) * 4;
            cached->pixels.resize(static_cast<size_t>(dW) * dH * 4);

            uint8_t* dstData[1] = { cached->pixels.data() };
            int dstLinesize[1] = { static_cast<int>(cached->stride) };

            sws_scale(sws, raw.data, raw.linesize, 0, h,
                      dstData, dstLinesize);
        }

        if (info.hasAlpha && !packedAlpha && !cached->pixels.empty()) {
            clearTransparentPixelRGB(cached->pixels.data(),
                                     cached->pixels.size() / 4);
        }

        // ── Chroma-key green-screen media (#18FF00) ───────────────────
        if (!cached->pixels.empty()) {
            std::string fn = path.filename().string();
            std::transform(fn.begin(), fn.end(), fn.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (fn.find("GREEN") != std::string::npos) {
                chromaKeyInPlace(cached->pixels.data(),
                                 cached->pixels.size() / 4);
            }
        }
#endif

        m_cache->put(cached);
        if (m_diskCache) m_diskCache->putAsync(cached);
        m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
        ++decoded_count;

        if (decoded_count % 50 == 0) {
            spdlog::info("[PERF] Loop pre-decode: handle={} progress {}/{} frames",
                         handle, f + 1, frameCount);
        }
    }

    if (sws) sws_freeContext(sws);

    auto t1 = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    spdlog::info("[PERF] Loop pre-decode DONE: handle={} '{}' decoded={} skipped={} "
                 "total={:.0f}ms ({:.1f}ms/frame)",
                 handle, path.filename().string(),
                 decoded_count, skipped_count, totalMs,
                 decoded_count > 0 ? totalMs / decoded_count : 0.0);

    m_cache->removePlayhead(handle);

    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        m_loopPreDecodeActive.erase(handle);
        m_loopPreDecodeDone.insert(handle);
    }
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
