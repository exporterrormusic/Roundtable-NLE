/*
 * VP9AlphaEncoder.cpp — VP9+alpha WebM writer.
 * Shared machinery lives in MediaFileEncoderBase.
 */

#include "VP9AlphaEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

VP9AlphaEncoder::~VP9AlphaEncoder()
{
    if (m_isOpen) finalize();
}

#ifdef ROUNDTABLE_HAS_FFMPEG

bool VP9AlphaEncoder::open(const std::filesystem::path& path,
                           uint32_t width, uint32_t height,
                           int fps, int crf)
{
    if (m_isOpen) finalize();

    // Dimensions must be even for YUV subsampling
    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;

    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);

    if (!createOutputContainer(path, "webm"))
        return false;

    const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9");
    if (!codec) {
        m_lastError = "VP9Alpha: libvpx-vp9 encoder not found";
        spdlog::error("{}", m_lastError);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "VP9Alpha: Failed to allocate codec context";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx->width     = w;
    m_codecCtx->height    = h;
    m_codecCtx->time_base = {1, fps};
    m_codecCtx->framerate = {fps, 1};
    m_codecCtx->pix_fmt   = AV_PIX_FMT_YUVA420P;  // Alpha!
    m_codecCtx->gop_size  = fps * 2;               // Keyframe every 2 seconds

    // CRF mode (quality-based, no bitrate cap)
    av_opt_set_int(m_codecCtx->priv_data, "crf", crf, 0);
    av_opt_set(m_codecCtx->priv_data, "b", "0", 0);  // Required for CRF mode

    // ── Threading: use all available CPU cores ────────────────
    // Divide threads among concurrent encoders to prevent oversubscription.
    // AnimationVideoCache runs hw_concurrency/4 workers, each with its own
    // encoder, so each encoder should use roughly 4 threads.
    int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
    int numWorkers = std::max(1, hwThreads / 4);
    m_codecCtx->thread_count = std::max(1, hwThreads / numWorkers);

    // Speed preset: 4 gives ~3-4x faster encoding vs 2 with
    // negligible quality loss at CRF-based encoding.
    av_opt_set_int(m_codecCtx->priv_data, "speed", 4, 0);
    av_opt_set_int(m_codecCtx->priv_data, "lag-in-frames", 16, 0);
    av_opt_set(m_codecCtx->priv_data, "row-mt", "1", 0);  // Multi-threaded rows

    // Tile-based parallelism (2^2 = 4 tile columns, 2^1 = 2 tile rows)
    // Allows the encoder to process frame regions in parallel.
    av_opt_set_int(m_codecCtx->priv_data, "tile-columns", 2, 0);
    av_opt_set_int(m_codecCtx->priv_data, "tile-rows", 1, 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        m_lastError = std::string("VP9Alpha: Failed to open codec: ") + errBuf;
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    if (!createStreamOpenFileWriteHeader(path, fps))
        return false;

    if (!allocFrameAndPacket(AV_PIX_FMT_YUVA420P, w, h))
        return false;

    if (!createSwsFromRgba(AV_PIX_FMT_YUVA420P, w, h)) {
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("VP9Alpha: Opened {}x{} @ {}fps CRF={} → {}",
                 m_width, m_height, fps, crf, pathToUtf8(path));
    return true;
}

bool VP9AlphaEncoder::writeFrame(const uint8_t* rgbaPixels)
{
    if (!m_isOpen) return false;

    av_frame_make_writable(m_frame);

    // Convert RGBA → YUVA420P
    const uint8_t* srcSlice[] = { rgbaPixels };
    int srcStride[] = { static_cast<int>(m_width * 4) };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, static_cast<int>(m_height),
              m_frame->data, m_frame->linesize);

    m_frame->pts = m_framesWritten;

    if (!sendFrameToEncoder(m_frame))
        return false;

    ++m_framesWritten;
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool VP9AlphaEncoder::open(const std::filesystem::path&,
                           uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool VP9AlphaEncoder::writeFrame(const uint8_t*) { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
