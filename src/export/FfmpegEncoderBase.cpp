/*
 * FfmpegEncoderBase.cpp — shared FFmpeg encoder machinery.
 * See FfmpegEncoderBase.h for the subclass contract.
 */

#include "FfmpegEncoderBase.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

FfmpegEncoderBase::~FfmpegEncoderBase()
{
    // shutdown() is idempotent; subclasses that override it should still
    // end up here with everything already null.
    FfmpegEncoderBase::shutdown();
}

void FfmpegEncoderBase::beginInit(const EncoderConfig& config)
{
    if (m_initialized) shutdown();
    m_config = config;
    m_framesEncoded = 0;
    m_hwAccel = false;
}

void FfmpegEncoderBase::applyCommonParams(AVCodecContext* ctx,
                                          const EncoderConfig& config,
                                          int pixFmt, bool tagColors,
                                          bool setRateControl) const
{
    ctx->width     = static_cast<int>(config.width);
    ctx->height    = static_cast<int>(config.height);
    ctx->time_base = {config.fpsDen, config.fpsNum};
    ctx->framerate = {config.fpsNum, config.fpsDen};
    ctx->pix_fmt   = static_cast<AVPixelFormat>(pixFmt);

    // Codec params (SPS/PPS/VPS, AV1 sequence header, ProRes profile…)
    // belong in the container header (avcC/hvcC box), not inline.  Without
    // this the file plays in lenient demuxers (VLC) but breaks in strict
    // editors (Premiere, Resolve, QuickTime, browsers).
    ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (setRateControl) {
        ctx->gop_size = config.gopSize > 0 ? config.gopSize : config.fpsNum * 2;
        if (config.bitrateMbps > 0)
            ctx->bit_rate = static_cast<int64_t>(config.bitrateMbps) * 1000000;
    }

    if (tagColors && config.bt709) {
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc       = AVCOL_TRC_BT709;
        ctx->colorspace      = AVCOL_SPC_BT709;
    }
}

bool FfmpegEncoderBase::initNvencThenCpu(const EncoderConfig& config,
                                         const char* nvencName,
                                         const char* cpuName,
                                         int cpuFallbackId)
{
    // ── Phase 1: NVENC with CUDA hardware frames (zero-copy input) ──────
    if (config.hwAccel == HardwareAccel::NVENC) {
        const AVCodec* nvenc = avcodec_find_encoder_by_name(nvencName);
        if (nvenc) {
            m_hwDeviceCtx = nullptr;
            int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx,
                                             AV_HWDEVICE_TYPE_CUDA,
                                             nullptr, nullptr, 0);
            if (ret >= 0) {
                spdlog::info("{}: CUDA device created, trying HW frames", logName());
                m_codecCtx = avcodec_alloc_context3(nvenc);
                if (m_codecCtx) {
                    applyCommonParams(m_codecCtx, config, AV_PIX_FMT_CUDA,
                                      /*tagColors=*/true, /*setRateControl=*/true);
                    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                    configureCodecOptions(m_codecCtx, config, /*hwPath=*/true);

                    // NVENC with CUDA input needs an explicit hw_frames_ctx
                    // BEFORE open; without it open returns EINVAL (-22) and
                    // we silently fall to the slow software-upload path.
                    int hwfErr = attachCudaHwFrames(
                        m_codecCtx, m_hwDeviceCtx,
                        static_cast<int>(config.width),
                        static_cast<int>(config.height));
                    ret = (hwfErr < 0)
                              ? hwfErr
                              : avcodec_open2(m_codecCtx, nvenc, nullptr);
                    if (ret >= 0 && m_codecCtx->hw_frames_ctx) {
                        m_hwAccel = true;
                        spdlog::info("{}: NVENC with CUDA HW frames", logName());
                    } else {
                        spdlog::warn("{}: CUDA HW frames failed (ret={}), "
                                     "trying NVENC software input", logName(), ret);
                        avcodec_free_context(&m_codecCtx);
                        m_codecCtx = nullptr;
                    }
                }
            }

            // ── Phase 2: NVENC with software input (YUV420P) ────────────
            if (!m_codecCtx) {
                if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); m_hwDeviceCtx = nullptr; }
                m_codecCtx = avcodec_alloc_context3(nvenc);
                if (m_codecCtx) {
                    applyCommonParams(m_codecCtx, config, AV_PIX_FMT_YUV420P,
                                      /*tagColors=*/true, /*setRateControl=*/true);
                    configureCodecOptions(m_codecCtx, config, /*hwPath=*/true);

                    int openRet = avcodec_open2(m_codecCtx, nvenc, nullptr);
                    if (openRet >= 0 && m_codecCtx->pix_fmt == AV_PIX_FMT_YUV420P) {
                        m_hwAccel = true;
                        spdlog::info("{}: NVENC with software input (YUV420P)", logName());
                    } else {
                        spdlog::warn("{}: NVENC unusable, falling back to CPU", logName());
                        avcodec_free_context(&m_codecCtx);
                        m_codecCtx = nullptr;
                        m_hwAccel = false;
                    }
                }
            }
        }
    }

    // ── Phase 3: CPU fallback ────────────────────────────────────────────
    if (!m_codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name(cpuName);
        if (!codec) codec = avcodec_find_encoder(static_cast<AVCodecID>(cpuFallbackId));
        if (!codec) {
            m_lastError = std::string(logName()) + ": no encoder found";
            spdlog::error("{}", m_lastError);
            return false;
        }
        m_codecCtx = avcodec_alloc_context3(codec);
        if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }

        applyCommonParams(m_codecCtx, config, AV_PIX_FMT_YUV420P,
                          /*tagColors=*/true, /*setRateControl=*/true);
        configureCodecOptions(m_codecCtx, config, /*hwPath=*/false);

        if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
            m_lastError = std::string(logName()) + ": failed to open " + cpuName;
            spdlog::error("{}", m_lastError);
            avcodec_free_context(&m_codecCtx);
            return false;
        }
        m_hwAccel = false;
        spdlog::info("{}: Using CPU ({})", logName(), cpuName);
    }

    return finishInit(AV_PIX_FMT_YUV420P);
}

bool FfmpegEncoderBase::initCpuCodec(const EncoderConfig& config,
                                     const char* codecName,
                                     int codecFallbackId,
                                     int pixFmt)
{
    const AVCodec* codec =
        codecName ? avcodec_find_encoder_by_name(codecName) : nullptr;
    if (!codec) codec = avcodec_find_encoder(static_cast<AVCodecID>(codecFallbackId));
    if (!codec) {
        m_lastError = std::string(logName()) + ": no encoder found";
        spdlog::error("{}", m_lastError);
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { m_lastError = "Failed to alloc context"; return false; }

    applyCommonParams(m_codecCtx, config, pixFmt,
                      /*tagColors=*/false, /*setRateControl=*/false);
    configureCodecOptions(m_codecCtx, config, /*hwPath=*/false);

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_lastError = std::string(logName()) + ": failed to open codec";
        spdlog::error("{}", m_lastError);
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    m_hwAccel = false;

    return finishInit(pixFmt);
}

bool FfmpegEncoderBase::finishInit(int swFrameFormat)
{
    // If using CUDA HW frames, allocate with codec-context dimensions
    // (NVENC may align them); the HW upload path transfers the full frame.
    m_frame = av_frame_alloc();
    m_frame->format = swFrameFormat;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    av_frame_get_buffer(m_frame, 0);
    m_packet = av_packet_alloc();

    m_swsCtx = sws_getContext(
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_BGRA,
        m_codecCtx->width, m_codecCtx->height,
        static_cast<AVPixelFormat>(swFrameFormat),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        m_lastError = std::string(logName()) + ": failed to create sws context";
        spdlog::error("{} pix_fmt={} wxh={}x{}", m_lastError,
                      swFrameFormat, m_codecCtx->width, m_codecCtx->height);
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_initialized = true;
    spdlog::info("{}: {}x{} @ {}/{} fps ({}) pix_fmt={}", logName(),
                 m_config.width, m_config.height,
                 m_config.fpsNum, m_config.fpsDen,
                 m_hwAccel ? "HW" : "CPU",
                 static_cast<int>(m_codecCtx->pix_fmt));
    return true;
}

bool FfmpegEncoderBase::encodeFrame(const uint8_t* bgraPixels, int64_t frameIndex)
{
    if (!m_initialized) { spdlog::error("{}: !initialized", logName()); return false; }
    av_frame_make_writable(m_frame);

    const uint8_t* srcSlice[] = { bgraPixels };
    int srcStride[] = { static_cast<int>(m_config.width * 4) };

    // min(config.height, m_frame->height) so we never read past the
    // caller's BGRA buffer (config-sized).  m_frame may be taller if NVENC
    // aligned the dimensions — extra lines stay zero (black).
    int srcLines = std::min(static_cast<int>(m_config.height), m_frame->height);
    sws_scale(m_swsCtx, srcSlice, srcStride, 0, srcLines,
              m_frame->data, m_frame->linesize);
    m_frame->pts = frameIndex;

    // ── HW path: upload the software frame to a CUDA frame ──────────────
    // hw_frames_ctx may be null if avcodec_open2 rejected our frames ctx
    // and reverted to software input — fall through to the SW path then.
    if (m_hwAccel && m_codecCtx->hw_frames_ctx) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            spdlog::error("{}: failed to get HW frame buffer", logName());
            av_frame_free(&hwFrame);
            return false;
        }
        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            spdlog::error("{}: failed to upload frame to GPU", logName());
            av_frame_free(&hwFrame);
            return false;
        }
        hwFrame->pts = m_frame->pts;
        bool ok = sendFrame(hwFrame);
        av_frame_free(&hwFrame);
        return ok;
    }

    return sendFrame(m_frame);
}

bool FfmpegEncoderBase::sendFrame(AVFrame* frame)
{
    m_pendingPackets.clear();
    m_pktStore.clear();  // invalidates prior m_lastPacket/pending (already consumed)
    int ret = avcodec_send_frame(m_codecCtx, frame);
    if (ret < 0) {
        m_lastError = std::string(logName()) + ": error sending frame";
        return false;
    }

    // Drain ALL available packets.  One send may produce several packets
    // (B-frame reordering) or none (EAGAIN — encoder needs more frames).
    // Every payload is copied into m_pktStore because m_packet's buffer is
    // recycled on the next receive.
    bool gotOne = false;
    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = std::string(logName()) + ": receive error";
            return gotOne;
        }

        EncodedPacket ep;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        retainPacketData(ep, m_packet->data, m_packet->size);
        av_packet_unref(m_packet);

        if (!gotOne) { m_lastPacket = ep; gotOne = true; }
        else         { m_pendingPackets.push_back(ep); }
        ++m_framesEncoded;
    }
    return gotOne;
}

int FfmpegEncoderBase::flush()
{
    if (!m_initialized) return 0;
    m_flushedPackets.clear();
    m_pktStore.clear();  // prior packets already consumed by the caller
    avcodec_send_frame(m_codecCtx, nullptr);

    int count = 0;
    while (avcodec_receive_packet(m_codecCtx, m_packet) == 0) {
        EncodedPacket ep;
        ep.pts        = m_packet->pts;
        ep.dts        = m_packet->dts;
        ep.duration   = (m_packet->duration > 0) ? m_packet->duration : 1;
        ep.isKeyframe = (m_packet->flags & AV_PKT_FLAG_KEY) != 0;
        retainPacketData(ep, m_packet->data, m_packet->size);
        av_packet_unref(m_packet);
        m_flushedPackets.push_back(ep);
        ++count; ++m_framesEncoded;
    }
    return count;
}

void FfmpegEncoderBase::shutdown()
{
    if (m_swsCtx)      { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_packet)      av_packet_free(&m_packet);
    if (m_frame)       av_frame_free(&m_frame);
    if (m_codecCtx)    avcodec_free_context(&m_codecCtx);
    if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
    if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
    m_packet = nullptr; m_frame = nullptr; m_codecCtx = nullptr;
    m_hwFramesCtx = nullptr; m_hwDeviceCtx = nullptr;
    m_initialized = false;
    m_flushedPackets.clear();
}

#else // !ROUNDTABLE_HAS_FFMPEG

FfmpegEncoderBase::~FfmpegEncoderBase() = default;
bool FfmpegEncoderBase::encodeFrame(const uint8_t*, int64_t) { return false; }
int  FfmpegEncoderBase::flush() { return 0; }
void FfmpegEncoderBase::shutdown() {}

#endif

} // namespace rt
