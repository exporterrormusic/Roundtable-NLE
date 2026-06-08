/*
 * VP9AlphaEncoder.cpp — VP9+alpha WebM writer using FFmpeg libvpx-vp9.
 *
 * Encodes RGBA → YUVA420P → VP9 and muxes into a WebM container with
 * alpha support.  Uses libvpx-vp9's native YUVA420P alpha encoding.
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
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

VP9AlphaEncoder::VP9AlphaEncoder() = default;

VP9AlphaEncoder::~VP9AlphaEncoder()
{
    if (m_isOpen) finalize();
}

bool VP9AlphaEncoder::open(const std::filesystem::path& path,
                            uint32_t width, uint32_t height,
                            int fps, int crf)
{
    if (m_isOpen) finalize();

    // Dimensions must be even for YUV subsampling
    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;

    // ── Create output format context (WebM) ─────────────────────────────
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "webm",
                                              path.string().c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = "VP9Alpha: Failed to create WebM output context";
        spdlog::error("{}", m_lastError);
        return false;
    }

    // ── Find VP9 encoder ────────────────────────────────────────────────
    const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9");
    if (!codec) {
        m_lastError = "VP9Alpha: libvpx-vp9 encoder not found";
        spdlog::error("{}", m_lastError);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Create video stream ─────────────────────────────────────────────
    m_stream = avformat_new_stream(m_fmtCtx, codec);
    if (!m_stream) {
        m_lastError = "VP9Alpha: Failed to create video stream";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_stream->id = 0;
    m_stream->time_base = {1, fps};

    // ── Configure codec context ─────────────────────────────────────────
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "VP9Alpha: Failed to allocate codec context";
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx->width     = static_cast<int>(m_width);
    m_codecCtx->height    = static_cast<int>(m_height);
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

    // Request global header if container needs it
    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        m_lastError = std::string("VP9Alpha: Failed to open codec: ") + errBuf;
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
            m_lastError = "VP9Alpha: Failed to open output file";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    // Write header
    ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        m_lastError = "VP9Alpha: Failed to write container header";
        spdlog::error("{}", m_lastError);
        avio_closep(&m_fmtCtx->pb);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Allocate frame + packet ─────────────────────────────────────────
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_YUVA420P;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // ── sws_scale: RGBA → YUVA420P ──────────────────────────────────────
    m_swsCtx = sws_getContext(
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_RGBA,
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_YUVA420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = "VP9Alpha: Failed to create sws_scale context";
        spdlog::error("{}", m_lastError);
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("VP9Alpha: Opened {}x{} @ {}fps CRF={} → {}",
                 m_width, m_height, fps, crf, path.string());
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

    // Send frame to encoder
    int ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) {
        m_lastError = "VP9Alpha: Error sending frame to encoder";
        return false;
    }

    // Receive any available packets
    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = "VP9Alpha: Error receiving packet from encoder";
            return false;
        }

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            m_lastError = "VP9Alpha: Error writing packet to container";
            return false;
        }
    }

    ++m_framesWritten;
    return true;
}

bool VP9AlphaEncoder::flushEncoder()
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

bool VP9AlphaEncoder::finalize()
{
    if (!m_isOpen) return false;

    flushEncoder();

    // Write trailer
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

    spdlog::info("VP9Alpha: Finalized — {} frames written", m_framesWritten);
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

VP9AlphaEncoder::VP9AlphaEncoder() = default;
VP9AlphaEncoder::~VP9AlphaEncoder() = default;

bool VP9AlphaEncoder::open(const std::filesystem::path&,
                            uint32_t, uint32_t, int, int)
{
    m_lastError = "FFmpeg not available";
    return false;
}

bool VP9AlphaEncoder::writeFrame(const uint8_t*) { return false; }
bool VP9AlphaEncoder::finalize() { return false; }
bool VP9AlphaEncoder::flushEncoder() { return false; }

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
