/*
 * ProResAlphaEncoder.cpp — ProRes 4444 encoder with native alpha channel.
 *
 * Encodes RGBA → YUVA444P10LE → ProRes 4444 and muxes into a QuickTime
 * MOV container.  This is the same codec/container combination used by
 * Premiere Pro and DaVinci Resolve for alpha-bearing media.
 *
 * ProRes 4444 is an intra-frame codec — every frame is independently
 * decodable, which eliminates all B-frame drain issues and enables
 * instant random-access seeking (O(1) per frame).
 */

#include "ProResEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

// ═════════════════════════════════════════════════════════════════════════════
//  Static probe — is ProRes encoding available?
// ═════════════════════════════════════════════════════════════════════════════

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

// ═════════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════

ProResAlphaEncoder::ProResAlphaEncoder() = default;

ProResAlphaEncoder::~ProResAlphaEncoder()
{
    if (m_isOpen) finalize();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Open
// ═════════════════════════════════════════════════════════════════════════════

bool ProResAlphaEncoder::open(const std::filesystem::path& path,
                          uint32_t width, uint32_t height,
                          int fps, int quality)
{
    if (m_isOpen) finalize();

    // ProRes requires even dimensions
    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;

    // ── Create output format context (MOV) ──────────────────────────────
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mov",
                                              path.string().c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = "ProRes: Failed to create MOV output context";
        spdlog::error("{}", m_lastError);
        return false;
    }

    // ── Find ProRes encoder ─────────────────────────────────────────────
    const AVCodec* codec = avcodec_find_encoder_by_name("prores_ks");
    if (!codec) {
        m_lastError = "ProRes: prores_ks encoder not found";
        spdlog::error("{}", m_lastError);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Create video stream ─────────────────────────────────────────────
    m_stream = avformat_new_stream(m_fmtCtx, codec);
    if (!m_stream) {
        m_lastError = "ProRes: Failed to create video stream";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_stream->id = 0;
    m_stream->time_base = {1, fps};

    // ── Configure codec context ─────────────────────────────────────────
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "ProRes: Failed to allocate codec context";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx->width     = static_cast<int>(m_width);
    m_codecCtx->height    = static_cast<int>(m_height);
    m_codecCtx->time_base = {1, fps};
    m_codecCtx->framerate = {fps, 1};

    // ProRes 4444 uses YUVA444P10LE — 4:4:4 chroma + alpha, 10-bit.
    // This is the native pixel format for profiles 4 (4444) and 5 (4444XQ).
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUVA444P10LE;

    // ProRes is all-intra — every frame is a keyframe.
    m_codecCtx->gop_size = 1;

    // Set the ProRes profile.
    // Profile 4 = "4444" (with alpha), Profile 5 = "4444xq" (with alpha, highest quality)
    int profile = std::clamp(quality, 0, 5);
    av_opt_set_int(m_codecCtx->priv_data, "profile", profile, 0);

    // Use all available cores for encoding — ProRes encoding is fast
    // (much faster than VP9/HEVC) so threading overhead is minimal.
    int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
    m_codecCtx->thread_count = std::max(1, hwThreads / 2);

    // Vendor tag: 'apl0' indicates Apple-compatible ProRes
    av_opt_set(m_codecCtx->priv_data, "vendor", "apl0", 0);

    // Request global header if container needs it
    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        m_lastError = std::string("ProRes: Failed to open codec: ") + errBuf;
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // Copy codec params to stream
    avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);

    // ── Open output file ────────────────────────────────────────────────
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmtCtx->pb, pathToUtf8(path).c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_lastError = "ProRes: Failed to open output file";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    // Write header
    ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        m_lastError = "ProRes: Failed to write container header";
        spdlog::error("{}", m_lastError);
        avio_closep(&m_fmtCtx->pb);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Allocate frame + packet ─────────────────────────────────────────
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_YUVA444P10LE;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // ── sws_scale: RGBA (8-bit) → YUVA444P10LE ─────────────────────────
    m_swsCtx = sws_getContext(
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_RGBA,
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_YUVA444P10LE,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = "ProRes: Failed to create sws_scale context";
        spdlog::error("{}", m_lastError);
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("ProRes: Opened {}x{} @ {}fps profile={} → {}",
                 m_width, m_height, fps, profile, path.string());
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Write Frame
// ═════════════════════════════════════════════════════════════════════════════

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

    // Send frame to encoder
    int ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) {
        m_lastError = "ProRes: Error sending frame to encoder";
        return false;
    }

    // Receive encoded packets — ProRes is intra-frame so we typically
    // get exactly one packet per frame with no buffering delay.
    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = "ProRes: Error receiving packet from encoder";
            return false;
        }

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            m_lastError = "ProRes: Error writing packet to container";
            return false;
        }
    }

    ++m_framesWritten;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Flush / Finalize
// ═════════════════════════════════════════════════════════════════════════════

bool ProResAlphaEncoder::flushEncoder()
{
    if (!m_codecCtx) return false;

    avcodec_send_frame(m_codecCtx, nullptr);

    while (true) {
        int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return false;

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        av_interleaved_write_frame(m_fmtCtx, m_packet);
    }
    return true;
}

bool ProResAlphaEncoder::finalize()
{
    if (!m_isOpen) return false;

    flushEncoder();

    if (m_fmtCtx) {
        av_write_trailer(m_fmtCtx);
    }

    // Cleanup
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_frame)  { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }

    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }

    m_stream = nullptr;
    m_isOpen = false;

    spdlog::info("ProRes: Finalized — {} frames written", m_framesWritten);
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool ProResAlphaEncoder::isAvailable() { return false; }

ProResAlphaEncoder::ProResAlphaEncoder() = default;
ProResAlphaEncoder::~ProResAlphaEncoder() = default;

bool ProResAlphaEncoder::open(const std::filesystem::path&,
                          uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool ProResAlphaEncoder::writeFrame(const uint8_t*) { return false; }
bool ProResAlphaEncoder::finalize() { return false; }
bool ProResAlphaEncoder::flushEncoder() { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
