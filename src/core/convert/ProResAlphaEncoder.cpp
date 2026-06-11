/*
 * ProResAlphaEncoder.cpp — ProRes 4444 encoding with native alpha.
 * Shared machinery lives in MediaFileEncoderBase.
 */

#include "ProResAlphaEncoder.h"
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

ProResAlphaEncoder::~ProResAlphaEncoder()
{
    if (m_isOpen) finalize();
}

#ifdef ROUNDTABLE_HAS_FFMPEG

bool ProResAlphaEncoder::isAvailable()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) {
        spdlog::debug("ProResAlphaEncoder: prores_ks codec not found in FFmpeg build");
        return false;
    }
    spdlog::info("ProResAlphaEncoder: prores_ks encoder is available");
    return true;
}

bool ProResAlphaEncoder::open(const std::filesystem::path& path,
                              uint32_t width, uint32_t height,
                              int fps, int quality)
{
    if (m_isOpen) finalize();

    // ProRes requires even dimensions
    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;

    const int w = static_cast<int>(m_width);
    const int h = static_cast<int>(m_height);

    if (!createOutputContainer(path, "mov"))
        return false;

    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) {
        m_lastError = "ProRes: prores_ks encoder not found";
        spdlog::error("{}", m_lastError);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "ProRes: Failed to allocate codec context";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx->width     = w;
    m_codecCtx->height    = h;
    m_codecCtx->time_base = {1, fps};
    m_codecCtx->framerate = {fps, 1};

    // ProRes 4444 uses YUVA444P10LE — 4:4:4 chroma + alpha, 10-bit.
    // This is the native pixel format for profiles 4 (4444) and 5 (4444XQ).
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUVA444P10LE;

    // ProRes is all-intra — every frame is a keyframe.
    m_codecCtx->gop_size = 1;

    // Set the ProRes profile.
    // Profile 4 = "4444" (with alpha), Profile 5 = "4444xq" (highest quality)
    const int profile = std::clamp(quality, 0, 5);
    av_opt_set_int(m_codecCtx->priv_data, "profile", profile, 0);

    // Use half the cores — ProRes encoding is fast, threading overhead
    // is minimal.
    const int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
    m_codecCtx->thread_count = std::max(1, hwThreads / 2);

    // Vendor tag: 'apl0' indicates Apple-compatible ProRes
    av_opt_set(m_codecCtx->priv_data, "vendor", "apl0", 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        m_lastError = std::string("ProRes: Failed to open codec: ") + errBuf;
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    if (!createStreamOpenFileWriteHeader(path, fps))
        return false;

    if (!allocFrameAndPacket(AV_PIX_FMT_YUVA444P10LE, w, h))
        return false;

    if (!createSwsFromRgba(AV_PIX_FMT_YUVA444P10LE, w, h)) {
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("ProRes: Opened {}x{} @ {}fps profile={} → {}",
                 m_width, m_height, fps, profile, pathToUtf8(path));
    return true;
}

bool ProResAlphaEncoder::writeFrame(const uint8_t* rgbaPixels)
{
    if (!m_isOpen) return false;

    av_frame_make_writable(m_frame);

    // Convert RGBA (8-bit) → YUVA444P10LE
    const uint8_t* srcSlice[] = { rgbaPixels };
    int srcStride[] = { static_cast<int>(m_width * 4) };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, static_cast<int>(m_height),
              m_frame->data, m_frame->linesize);

    m_frame->pts = m_framesWritten;

    // ProRes is intra-frame — typically exactly one packet per frame
    // with no buffering delay.
    if (!sendFrameToEncoder(m_frame))
        return false;

    ++m_framesWritten;
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool ProResAlphaEncoder::isAvailable() { return false; }

bool ProResAlphaEncoder::open(const std::filesystem::path&,
                              uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool ProResAlphaEncoder::writeFrame(const uint8_t*) { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
