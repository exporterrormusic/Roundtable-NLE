/*
 * Encoder.cpp — Base encoder utilities and factory.
 */

#include "Encoder.h"
#include "formats/H264Encoder.h"
#include "formats/H265Encoder.h"
#include "formats/AV1Encoder.h"
#include "formats/ProResEncoder.h"
#include "formats/DNxHREncoder.h"
#include "formats/ImageSequence.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#endif

namespace rt {

int Encoder::attachCudaHwFrames(AVCodecContext* codecCtx,
                                AVBufferRef* hwDeviceCtx,
                                int width, int height) noexcept
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (!codecCtx || !hwDeviceCtx || width <= 0 || height <= 0)
        return AVERROR(EINVAL);

    AVBufferRef* framesRef = av_hwframe_ctx_alloc(hwDeviceCtx);
    if (!framesRef)
        return AVERROR(ENOMEM);

    auto* fctx = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    fctx->format            = AV_PIX_FMT_CUDA;
    // NV12 is NVENC's native input: the GPU export converter writes it, and
    // the BGRA path's swscale converts to it as cheaply as to YUV420P.
    fctx->sw_format         = AV_PIX_FMT_NV12;
    fctx->width             = width;
    fctx->height            = height;
    fctx->initial_pool_size = 20;  // headroom for lookahead + B-frame reorder

    int err = av_hwframe_ctx_init(framesRef);
    if (err < 0) {
        av_buffer_unref(&framesRef);
        return err;
    }

    codecCtx->hw_frames_ctx = av_buffer_ref(framesRef);
    av_buffer_unref(&framesRef);
    return codecCtx->hw_frames_ctx ? 0 : AVERROR(ENOMEM);
#else
    (void)codecCtx; (void)hwDeviceCtx; (void)width; (void)height;
    return -1;
#endif
}

HardwareAccel Encoder::detectBestHardware(EncoderCodec codec) noexcept
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    const char* nvenc = nullptr;
    const char* qsv   = nullptr;
    const char* amf   = nullptr;
    switch (codec) {
        case EncoderCodec::H264: nvenc = "h264_nvenc"; qsv = "h264_qsv"; amf = "h264_amf"; break;
        case EncoderCodec::H265: nvenc = "hevc_nvenc"; qsv = "hevc_qsv"; amf = "hevc_amf"; break;
        case EncoderCodec::AV1:  nvenc = "av1_nvenc";  qsv = "av1_qsv";  amf = "av1_amf";  break;
        default:
            // ProRes / DNxHR / ImageSequence have no hardware path.
            return HardwareAccel::None;
    }
    auto have = [](const char* n) {
        return n && avcodec_find_encoder_by_name(n) != nullptr;
    };
    if (have(nvenc)) { spdlog::info("detectBestHardware: NVENC ({})", nvenc); return HardwareAccel::NVENC; }
    if (have(qsv))   { spdlog::info("detectBestHardware: QSV ({})", qsv);   return HardwareAccel::QSV;   }
    if (have(amf))   { spdlog::info("detectBestHardware: AMF ({})", amf);   return HardwareAccel::AMF;   }
    spdlog::info("detectBestHardware: no hardware encoder for codec {} — using CPU",
                 static_cast<int>(codec));
#else
    (void)codec;
#endif
    return HardwareAccel::None;
}

void Encoder::retainPacketData(EncodedPacket& ep, const uint8_t* data, int size)
{
    auto& buf = m_pktStore.emplace_back();
    if (data && size > 0) buf.assign(data, data + size);
    ep.data     = buf.data();
    ep.size     = size;
    ep.ownsData = false;  // owned by m_pktStore, freed on next send/flush
}

const char* encoderCodecName(EncoderCodec codec) noexcept
{
    switch (codec) {
        case EncoderCodec::H264:          return "H.264";
        case EncoderCodec::H265:          return "H.265";
        case EncoderCodec::AV1:           return "AV1";
        case EncoderCodec::ProRes:        return "ProRes";
        case EncoderCodec::DNxHR:          return "DNxHR";
        case EncoderCodec::ImageSequence: return "Image Sequence";
        default:                          return "Unknown";
    }
}

std::unique_ptr<Encoder> Encoder::create(EncoderCodec codec, HardwareAccel hwAccel)
{
    spdlog::info("Encoder: Creating {} encoder (hw={})",
                 encoderCodecName(codec), static_cast<int>(hwAccel));

    switch (codec) {
        case EncoderCodec::H264:
            return std::make_unique<H264Encoder>();
        case EncoderCodec::H265:
            return std::make_unique<H265Encoder>();
        case EncoderCodec::AV1:
            return std::make_unique<AV1Encoder>();
        case EncoderCodec::ProRes:
            return std::make_unique<ProResEncoder>();
        case EncoderCodec::DNxHR:
            return std::make_unique<DNxHREncoder>();
        case EncoderCodec::ImageSequence:
            return std::make_unique<ImageSequence>();
        default:
            spdlog::error("Encoder: Unknown codec {}", static_cast<int>(codec));
            return nullptr;
    }
}

} // namespace rt
