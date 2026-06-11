/*
 * MediaFileEncoderBase.cpp — shared file-writing encoder machinery.
 * See MediaFileEncoderBase.h for the subclass contract.
 */

#include "convert/MediaFileEncoderBase.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

MediaFileEncoderBase::~MediaFileEncoderBase()
{
    // Subclass destructors call finalize() themselves (so their virtual
    // overrides are still alive); by the time we get here everything
    // should already be released — this is belt-and-braces only.
    releaseAll();
}

bool MediaFileEncoderBase::probeNvencH264(const char* logTag)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        spdlog::debug("{}: h264_nvenc codec not found", logTag);
        return false;
    }

    // Attempt to init a CUDA hw device — fails if no NVIDIA GPU.
    AVBufferRef* hwCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_CUDA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        spdlog::debug("{}: CUDA device init failed (no NVIDIA GPU?)", logTag);
        return false;
    }

    // Open a tiny test encoder context to confirm NVENC actually works.
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
    const bool available = (ret >= 0);

    avcodec_free_context(&testCtx);
    av_buffer_unref(&hwCtx);

    if (available)
        spdlog::info("{}: NVENC H.264 is AVAILABLE", logTag);
    else
        spdlog::info("{}: NVENC not available — using CPU fallback", logTag);

    return available;
}

bool MediaFileEncoderBase::createOutputContainer(const std::filesystem::path& path,
                                                 const char* formatName)
{
    int ret = avformat_alloc_output_context2(&m_fmtCtx, nullptr, formatName,
                                             pathToUtf8(path).c_str());
    if (ret < 0 || !m_fmtCtx) {
        m_lastError = std::string(logName()) + ": Failed to create output context";
        spdlog::error("{}", m_lastError);
        return false;
    }
    return true;
}

bool MediaFileEncoderBase::tryOpenNvencH264(int encodeW, int encodeH,
                                            int fps, int crf, int colorRange)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec)
        return false;

    int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                                     nullptr, nullptr, 0);
    if (ret < 0) {
        spdlog::debug("{}: CUDA device init failed", logName());
        return false;
    }

    bool nvencOk = false;
    m_codecCtx = avcodec_alloc_context3(codec);
    if (m_codecCtx) {
        m_codecCtx->width     = encodeW;
        m_codecCtx->height    = encodeH;
        m_codecCtx->time_base = {1, fps};
        m_codecCtx->framerate = {fps, 1};
        m_codecCtx->pix_fmt   = AV_PIX_FMT_CUDA;
        // h264_nvenc rejects gop_size=1 with bf=0 ("Gop Length should be
        // greater than number of B frames + 1").  Use gop_size=2 with
        // forced-idr so every output frame is still an IDR and seeking
        // is O(1).
        m_codecCtx->gop_size     = 2;
        m_codecCtx->max_b_frames = 0;
        m_codecCtx->color_range     = static_cast<AVColorRange>(colorRange);
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
            framesCtx->format    = AV_PIX_FMT_CUDA;
            framesCtx->sw_format = AV_PIX_FMT_YUV420P;
            framesCtx->width     = encodeW;
            framesCtx->height    = encodeH;
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
                    spdlog::info("{}: NVENC H.264 opened ({}x{} YUV420P, QP={})",
                                 logName(), encodeW, encodeH, crf);
                } else {
                    char errBuf[256];
                    av_strerror(ret, errBuf, sizeof(errBuf));
                    spdlog::warn("{}: avcodec_open2 h264_nvenc failed: {}",
                                 logName(), errBuf);
                }
            } else {
                spdlog::warn("{}: hw_frames_ctx init failed", logName());
            }
        }

        if (!nvencOk) {
            avcodec_free_context(&m_codecCtx);
            if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
            av_buffer_unref(&m_hwDeviceCtx);
            m_hwDeviceCtx = nullptr;
        }
    }

    return nvencOk;
}

bool MediaFileEncoderBase::openSoftwareIntraFallback(const char* primaryName,
                                                     const char* secondaryName,
                                                     int encodeW, int encodeH,
                                                     int fps, int crf)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(primaryName);
    if (!codec)
        codec = avcodec_find_encoder_by_name(secondaryName);
    if (!codec) {
        m_lastError = std::string(logName()) + ": No suitable encoder found ("
                    + primaryName + "/" + secondaryName + ")";
        spdlog::error("{}", m_lastError);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    m_codecCtx->width     = encodeW;
    m_codecCtx->height    = encodeH;
    m_codecCtx->time_base = {1, fps};
    m_codecCtx->framerate = {fps, 1};
    m_codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
    m_codecCtx->gop_size  = 1;   // all-intra: every frame is a keyframe
    m_codecCtx->max_b_frames = 0;

    av_opt_set_int(m_codecCtx->priv_data, "crf", crf, 0);

    const int swThreads = static_cast<int>(std::thread::hardware_concurrency());
    m_codecCtx->thread_count = std::max(1, swThreads / 2);

    if (std::string(codec->name) == "libx264")
        av_opt_set(m_codecCtx->priv_data, "preset", "veryfast", 0);
    else
        av_opt_set(m_codecCtx->priv_data, "preset", "fast", 0);

    if (m_fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        m_lastError = std::string(logName())
                    + ": Failed to open software encoder: " + errBuf;
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }

    spdlog::info("{}: Using software encoder '{}' ({}x{}, CRF={})",
                 logName(), codec->name, encodeW, encodeH, crf);
    return true;
}

bool MediaFileEncoderBase::createStreamOpenFileWriteHeader(
    const std::filesystem::path& path, int fps, AVDictionary** muxOpts)
{
    m_stream = avformat_new_stream(m_fmtCtx, nullptr);
    if (!m_stream) {
        m_lastError = std::string(logName()) + ": Failed to create video stream";
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    m_stream->id = 0;
    m_stream->time_base = {1, fps};
    avcodec_parameters_from_context(m_stream->codecpar, m_codecCtx);

    if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&m_fmtCtx->pb, pathToUtf8(path).c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            m_lastError = std::string(logName()) + ": Failed to open output file";
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
            return false;
        }
    }

    int ret = avformat_write_header(m_fmtCtx, muxOpts);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": Failed to write container header";
        spdlog::error("{}", m_lastError);
        avio_closep(&m_fmtCtx->pb);
        avcodec_free_context(&m_codecCtx);
        avformat_free_context(m_fmtCtx); m_fmtCtx = nullptr;
        return false;
    }
    return true;
}

bool MediaFileEncoderBase::allocFrameAndPacket(int pixFmt, int encodeW, int encodeH)
{
    m_frame = av_frame_alloc();
    m_frame->format = pixFmt;
    m_frame->width  = encodeW;
    m_frame->height = encodeH;
    av_frame_get_buffer(m_frame, 0);

    m_packet = av_packet_alloc();
    return m_frame && m_packet;
}

bool MediaFileEncoderBase::createSwsFromRgba(int dstPixFmt, int encodeW, int encodeH)
{
    m_swsCtx = sws_getContext(
        encodeW, encodeH, AV_PIX_FMT_RGBA,
        encodeW, encodeH, static_cast<AVPixelFormat>(dstPixFmt),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = std::string(logName()) + ": Failed to create sws_scale context";
        spdlog::error("{}", m_lastError);
        return false;
    }
    return true;
}

bool MediaFileEncoderBase::sendFrameToEncoder(AVFrame* frame)
{
    int ret = avcodec_send_frame(m_codecCtx, frame);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": Error sending frame to encoder";
        return false;
    }
    return receiveAndWritePackets();
}

bool MediaFileEncoderBase::sendFrameWithCudaUpload()
{
    AVFrame* hwFrame = av_frame_alloc();
    hwFrame->format = AV_PIX_FMT_CUDA;
    hwFrame->width  = m_frame->width;
    hwFrame->height = m_frame->height;

    int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": Failed to get HW frame buffer";
        av_frame_free(&hwFrame);
        return false;
    }

    ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": Failed to upload frame to GPU";
        av_frame_free(&hwFrame);
        return false;
    }

    hwFrame->pts       = m_frame->pts;
    hwFrame->pict_type = AV_PICTURE_TYPE_I;
    hwFrame->key_frame = 1;

    ret = avcodec_send_frame(m_codecCtx, hwFrame);
    av_frame_free(&hwFrame);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": Error sending HW frame to encoder";
        return false;
    }
    return receiveAndWritePackets();
}

bool MediaFileEncoderBase::receiveAndWritePackets()
{
    while (true) {
        int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = std::string(logName())
                        + ": Error receiving packet from encoder";
            return false;
        }

        av_packet_rescale_ts(m_packet, m_codecCtx->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmtCtx, m_packet);
        if (ret < 0) {
            m_lastError = std::string(logName())
                        + ": Error writing packet to container";
            return false;
        }
    }
    return true;
}

bool MediaFileEncoderBase::flushEncoder()
{
    if (!m_codecCtx) return false;
    avcodec_send_frame(m_codecCtx, nullptr);
    return receiveAndWritePackets();
}

void MediaFileEncoderBase::releaseAll()
{
    if (m_swsCtx)   { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_packet)   { av_packet_free(&m_packet); }
    if (m_frame)    { av_frame_free(&m_frame); }
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }

    if (m_hwFramesCtx) { av_buffer_unref(&m_hwFramesCtx); m_hwFramesCtx = nullptr; }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }

    if (m_fmtCtx) {
        if (!(m_fmtCtx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_fmtCtx->pb);
        avformat_free_context(m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_stream = nullptr;
}

bool MediaFileEncoderBase::finalize()
{
    if (!m_isOpen) return false;

    flushEncoder();

    if (m_fmtCtx)
        av_write_trailer(m_fmtCtx);

    releaseAll();

    m_isOpen = false;
    spdlog::info("{}: finalized — {} frames written", logName(), m_framesWritten);
    return true;
}

#else // !ROUNDTABLE_HAS_FFMPEG

MediaFileEncoderBase::~MediaFileEncoderBase() = default;

bool MediaFileEncoderBase::finalize()
{
    m_isOpen = false;
    return false;
}

#endif // ROUNDTABLE_HAS_FFMPEG

} // namespace rt
