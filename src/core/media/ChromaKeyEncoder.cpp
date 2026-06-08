/*
 * ChromaKeyEncoder.cpp — Standard H.264 encoder for chroma-key output.
 *
 * Encodes opaque RGBA frames as YUV420P H.264 MP4.  The alpha channel
 * is discarded — only RGB colour is encoded.  Uses NVENC for GPU
 * acceleration when available, libx264/libx265 as software fallback.
 */

#include "ChromaKeyEncoder.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

// ═════════════════════════════════════════════════════════════════════════════
//  Static probe — can we use NVENC?
// ═════════════════════════════════════════════════════════════════════════════

bool ChromaKeyEncoder::isNvencAvailable()
{
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        spdlog::debug("ChromaKeyEncoder: h264_nvenc codec not found");
        return false;
    }

    AVBufferRef* hwCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_CUDA,
                                      nullptr, nullptr, 0);
    if (ret < 0) {
        spdlog::debug("ChromaKeyEncoder: CUDA device init failed");
        return false;
    }

    AVCodecContext* testCtx = avcodec_alloc_context3(codec);
    if (!testCtx) {
        av_buffer_unref(&hwCtx);
        return false;
    }

    testCtx->width  = 64;
    testCtx->height = 64;
    testCtx->time_base = {1, 30};
    testCtx->pix_fmt   = AV_PIX_FMT_CUDA;
    testCtx->hw_device_ctx = av_buffer_ref(hwCtx);

    AVBufferRef* hwFramesRef = av_hwframe_ctx_alloc(hwCtx);
    if (hwFramesRef) {
        auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hwFramesRef->data);
        framesCtx->format    = AV_PIX_FMT_CUDA;
        framesCtx->sw_format = AV_PIX_FMT_YUV420P;
        framesCtx->width     = 64;
        framesCtx->height    = 64;
        framesCtx->initial_pool_size = 4;
        av_hwframe_ctx_init(hwFramesRef);
        testCtx->hw_frames_ctx = av_buffer_ref(hwFramesRef);
        av_buffer_unref(&hwFramesRef);
    }

    ret = avcodec_open2(testCtx, codec, nullptr);
    bool available = (ret >= 0);

    avcodec_free_context(&testCtx);
    av_buffer_unref(&hwCtx);

    if (available)
        spdlog::info("ChromaKeyEncoder: NVENC H.264 is AVAILABLE");
    else
        spdlog::info("ChromaKeyEncoder: NVENC not available — using CPU fallback");

    return available;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═════════════════════════════════════════════════════════════════════════════

ChromaKeyEncoder::ChromaKeyEncoder() = default;

ChromaKeyEncoder::~ChromaKeyEncoder()
{
    if (m_isOpen) finalize();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Open
// ═════════════════════════════════════════════════════════════════════════════

bool ChromaKeyEncoder::open(const std::filesystem::path& path,
                             uint32_t width, uint32_t height,
                             int fps, int crf)
{
    if (m_isOpen) finalize();

    m_width  = (width  + 1) & ~1u;
    m_height = (height + 1) & ~1u;
    m_framesWritten = 0;
    m_usingNvenc = false;

    // ── Create output format context (MP4) ──────────────────────────────
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, "mp4",
                                              pathToUtf8(path).c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = "ChromaKey: Failed to create MP4 output context";
        spdlog::error("{}", m_lastError);
        return false;
    }

    // ── Try NVENC first ─────────────────────────────────────────────────
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    bool nvencOk = false;

    if (codec) {
        ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                                      nullptr, nullptr, 0);
        if (ret >= 0) {
            m_codecCtx = avcodec_alloc_context3(codec);
            if (m_codecCtx) {
                m_codecCtx->width     = static_cast<int>(m_width);
                m_codecCtx->height    = static_cast<int>(m_height);
                m_codecCtx->time_base = {1, fps};
                m_codecCtx->framerate = {fps, 1};
                m_codecCtx->pix_fmt   = AV_PIX_FMT_CUDA;
                m_codecCtx->gop_size  = 2;
                m_codecCtx->max_b_frames = 0;
                m_codecCtx->color_range     = AVCOL_RANGE_MPEG;
                m_codecCtx->color_primaries = AVCOL_PRI_BT709;
                m_codecCtx->color_trc       = AVCOL_TRC_BT709;
                m_codecCtx->colorspace      = AVCOL_SPC_BT709;

                av_opt_set(m_codecCtx->priv_data, "preset", "p4", 0);
                av_opt_set(m_codecCtx->priv_data, "tune",   "hq", 0);
                av_opt_set(m_codecCtx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(m_codecCtx->priv_data, "qp", crf, 0);
                av_opt_set(m_codecCtx->priv_data, "profile", "high", 0);
                av_opt_set_int(m_codecCtx->priv_data, "forced-idr", 1, 0);

                m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

                m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
                if (m_hwFramesCtx) {
                    auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(m_hwFramesCtx->data);
                    framesCtx->format         = AV_PIX_FMT_CUDA;
                    framesCtx->sw_format      = AV_PIX_FMT_YUV420P;
                    framesCtx->width          = static_cast<int>(m_width);
                    framesCtx->height         = static_cast<int>(m_height);
                    framesCtx->initial_pool_size = 8;

                    ret = av_hwframe_ctx_init(m_hwFramesCtx);
                    if (ret >= 0) {
                        m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);

                        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                            m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                        ret = avcodec_open2(m_codecCtx, codec, nullptr);
                        if (ret >= 0) {
                            nvencOk = true;
                            m_usingNvenc = true;
                            spdlog::info("ChromaKey: NVENC H.264 opened "
                                         "({}x{} YUV420P, QP={})",
                                         m_width, m_height, crf);
                        } else {
                            char errBuf[256];
                            av_strerror(ret, errBuf, sizeof(errBuf));
                            spdlog::warn("ChromaKey: avcodec_open2 h264_nvenc failed: {}", errBuf);
                        }
                    } else {
                        spdlog::warn("ChromaKey: hw_frames_ctx init failed");
                    }
                }

                if (!nvencOk) {
                    avcodec_free_context(&m_codecCtx);
                    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
                    av_buffer_unref(&m_hwDeviceCtx);
                    m_hwDeviceCtx = nullptr;
                }
            }
        } else {
            spdlog::debug("ChromaKey: CUDA device init failed");
        }
    }

    // ── Fallback: software encoder ──────────────────────────────────────
    if (!nvencOk) {
        // Try libx264 first (widely available), then libx265
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec)
            codec = avcodec_find_encoder_by_name("libx265");
        if (!codec) {
            m_lastError = "ChromaKey: No suitable encoder found (h264_nvenc/libx264/libx265)";
            spdlog::error("{}", m_lastError);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }

        m_codecCtx = avcodec_alloc_context3(codec);
        m_codecCtx->width     = static_cast<int>(m_width);
        m_codecCtx->height    = static_cast<int>(m_height);
        m_codecCtx->time_base = {1, fps};
        m_codecCtx->framerate = {fps, 1};
        m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
        m_codecCtx->gop_size  = 1;   // all-intra
        m_codecCtx->max_b_frames = 0;

        av_opt_set_int(m_codecCtx->priv_data, "crf", crf, 0);

        int swThreads = static_cast<int>(std::thread::hardware_concurrency());
        m_codecCtx->thread_count = std::max(1, swThreads / 2);

        if (std::string(codec->name) == "libx264") {
            av_opt_set(m_codecCtx->priv_data, "preset", "veryfast", 0);
        } else {
            av_opt_set(m_codecCtx->priv_data, "preset", "fast", 0);
        }

        if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
            m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        ret = avcodec_open2(m_codecCtx, codec, nullptr);
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            m_lastError = std::string("ChromaKey: Failed to open software encoder: ") + errBuf;
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }

        spdlog::info("ChromaKey: Using software encoder '{}' ({}x{}, CRF={})",
                      codec->name, m_width, m_height, crf);
    }

    // ── Create video stream ─────────────────────────────────────────────
    m_stream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_stream) {
        m_lastError = "ChromaKey: Failed to create video stream";
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_stream->id = 0;
    m_stream->time_base = {1, fps};
    avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);

    // ── Open output file ────────────────────────────────────────────────
    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmtCtx->pb, pathToUtf8(path).c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_lastError = "ChromaKey: Failed to open output file";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    // Write header
    ret = avformat_write_header(m_fmtCtx, nullptr);
    if (ret < 0) {
        m_lastError = "ChromaKey: Failed to write container header";
        spdlog::error("{}", m_lastError);
        avio_closep(&m_fmtCtx->pb);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    // ── Allocate frame + packet ─────────────────────────────────────────
    m_frame = av_frame_alloc();
    m_frame->format = AV_PIX_FMT_YUV420P;
    m_frame->width  = static_cast<int>(m_width);
    m_frame->height = static_cast<int>(m_height);
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();

    // ── sws_scale: RGBA → YUV420P ──────────────────────────────────────
    m_swsCtx = sws_getContext(
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_RGBA,
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = "ChromaKey: Failed to create sws_scale context";
        spdlog::error("{}", m_lastError);
        finalize();
        return false;
    }

    m_isOpen = true;
    spdlog::info("ChromaKey: Opened {}x{} @ {}fps → {} [{}]",
                 m_width, m_height, fps, pathToUtf8(path),
                 m_usingNvenc ? "NVENC" : "CPU");
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Write Frame
// ═════════════════════════════════════════════════════════════════════════════

bool ChromaKeyEncoder::writeFrame(const uint8_t* rgbaPixels)
{
    if (!m_isOpen) return false;

    // ── Convert RGBA → YUV420P ─────────────────────────────────────────
    // The alpha channel is simply ignored by sws_scale.
    av_frame_make_writable(m_frame);

    const uint8_t* srcSlice[] = { rgbaPixels };
    int srcStride[] = { static_cast<int>(m_width * 4) };

    sws_scale(m_swsCtx, srcSlice, srcStride, 0, static_cast<int>(m_height),
              m_frame->data, m_frame->linesize);

    m_frame->pts = m_framesWritten;
    m_frame->pict_type = AV_PICTURE_TYPE_I;
    m_frame->key_frame = 1;

    // ── If NVENC, upload CPU frame to GPU ────────────────────────────────
    if (m_usingNvenc) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            m_lastError = "ChromaKey: Failed to get HW frame buffer";
            av_frame_free(&hwFrame);
            return false;
        }

        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            m_lastError = "ChromaKey: Failed to upload frame to GPU";
            av_frame_free(&hwFrame);
            return false;
        }

        hwFrame->pts = m_frame->pts;
        hwFrame->pict_type = AV_PICTURE_TYPE_I;
        hwFrame->key_frame = 1;

        ret = avcodec_send_frame(m_codecCtx, hwFrame);
        av_frame_free(&hwFrame);
        if (ret < 0) {
            m_lastError = "ChromaKey: Error sending HW frame to encoder";
            return false;
        }
    } else {
        int ret = avcodec_send_frame(m_codecCtx, m_frame);
        if (ret < 0) {
            m_lastError = "ChromaKey: Error sending frame to encoder";
            return false;
        }
    }

    if (!receiveAndWritePackets())
        return false;

    ++m_framesWritten;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Receive & Write Packets
// ═════════════════════════════════════════════════════════════════════════════

bool ChromaKeyEncoder::receiveAndWritePackets()
{
    while (true) {
        int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = "ChromaKey: Error receiving packet from encoder";
            return false;
        }

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            m_lastError = "ChromaKey: Error writing packet to container";
            return false;
        }
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Flush / Finalize
// ═════════════════════════════════════════════════════════════════════════════

bool ChromaKeyEncoder::flushEncoder()
{
    if (!m_codecCtx) return false;
    avcodec_send_frame(m_codecCtx, nullptr);
    return receiveAndWritePackets();
}

bool ChromaKeyEncoder::finalize()
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

    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }

    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }

    m_isOpen = false;
    spdlog::info("ChromaKey: finalized — {} frames written", m_framesWritten);
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool ChromaKeyEncoder::isNvencAvailable() { return false; }

ChromaKeyEncoder::ChromaKeyEncoder() = default;
ChromaKeyEncoder::~ChromaKeyEncoder() = default;

bool ChromaKeyEncoder::open(const std::filesystem::path&,
                             uint32_t, uint32_t, int, int)
{
    m_lastError = "ChromaKey: FFmpeg not available in this build";
    return false;
}

bool ChromaKeyEncoder::writeFrame(const uint8_t*)
{
    return false;
}

bool ChromaKeyEncoder::finalize()
{
    m_isOpen = false;
    return false;
}

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
