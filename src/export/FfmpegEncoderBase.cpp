/*
 * FfmpegEncoderBase.cpp — shared FFmpeg encoder machinery.
 * See FfmpegEncoderBase.h for the subclass contract.
 */

#include "FfmpegEncoderBase.h"
#include "Rgba16fPack.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>

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
    m_framesSubmitted = 0;
    m_hwAccel = false;
    m_failed = false;
    m_lastError.clear();
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
        // gop_size is measured in FRAMES, not time-base ticks.  For fractional
        // rates fpsNum is commonly 30000 or 60000 with fpsDen=1001; using the
        // numerator alone produced 60,000/120,000-frame GOPs (roughly 33
        // minutes at 59.94 fps), leaving normal MP4 seeks with no nearby IDR.
        const int64_t fpsNum = std::max<int64_t>(1, config.fpsNum);
        const int64_t fpsDen = std::max<int64_t>(1, config.fpsDen);
        const int64_t autoGop = std::max<int64_t>(
            1, (fpsNum * 2 + fpsDen / 2) / fpsDen); // nearest 2 seconds
        ctx->gop_size = config.gopSize > 0
            ? config.gopSize
            : static_cast<int>(std::min<int64_t>(
                  autoGop, std::numeric_limits<int>::max()));
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

    // Multi-thread the intra encode — running on ONE core is the single
    // biggest reason ProRes/DNxHR export feels glacial.  thread_count = 0 lets
    // libavcodec pick the logical-core count; the thread_type default
    // (FRAME|SLICE) lets each encoder use its best mode: prores_ks / dnxhd
    // SLICE-thread (no reorder latency, bit-identical), while prores_aw
    // FRAME-threads.  For a batch export the frame-thread reorder latency is
    // irrelevant (the drain loop already collects delayed packets).  No-op for
    // codecs that don't thread; safe to set before open.
    m_codecCtx->thread_count = 0;            // 0 = auto (all logical cores)

    // Phase 4.2: for the 10-bit intra targets the export passthrough authors
    // real BT.709 limited-range YCbCr (and the BGRA→YUV swscale for composited
    // frames also yields BT.709 limited), so tag the stream accordingly —
    // otherwise Resolve/Premiere may misread the levels.  Gated on the 10-bit
    // pixel formats only, so 8-bit ProRes/DNxHR output stays byte-identical.
    // These are pure metadata fields (unlike bit_rate/gop) that prores_ks and
    // dnxhd accept at open.
    const bool tenBit = (pixFmt == AV_PIX_FMT_YUV422P10LE ||
                         pixFmt == AV_PIX_FMT_YUV444P10LE ||
                         pixFmt == AV_PIX_FMT_YUVA444P10LE ||
                         pixFmt == AV_PIX_FMT_P010LE);
    if (tenBit && config.bt709) {
        m_codecCtx->color_primaries = AVCOL_PRI_BT709;
        m_codecCtx->color_trc       = AVCOL_TRC_BT709;
        m_codecCtx->colorspace      = AVCOL_SPC_BT709;
        m_codecCtx->color_range     = AVCOL_RANGE_MPEG;
    }

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
    auto failInit = [&](std::string message) {
        m_lastError = std::move(message);
        m_failed = true;
        spdlog::error("{}", m_lastError);
        shutdown();
        return false;
    };

    // If using CUDA HW frames, allocate with codec-context dimensions
    // (NVENC may align them); the HW upload path transfers the full frame.
    m_frame = av_frame_alloc();
    if (!m_frame)
        return failInit(std::string(logName()) + ": failed to allocate frame");
    m_frame->format = swFrameFormat;
    m_frame->width  = m_codecCtx->width;
    m_frame->height = m_codecCtx->height;
    if (av_frame_get_buffer(m_frame, 0) < 0)
        return failInit(std::string(logName()) + ": failed to allocate frame buffer");
    m_packet = av_packet_alloc();
    if (!m_packet)
        return failInit(std::string(logName()) + ": failed to allocate packet");

    m_swsCtx = sws_getContext(
        m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_BGRA,
        m_codecCtx->width, m_codecCtx->height,
        static_cast<AVPixelFormat>(swFrameFormat),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
        return failInit(std::string(logName()) +
                        ": failed to create pixel conversion context");
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
    if (!m_initialized || !bgraPixels) {
        m_lastError = std::string(logName()) + ": invalid BGRA frame input";
        m_failed = true;
        spdlog::error("{}", m_lastError);
        return false;
    }
    if (av_frame_make_writable(m_frame) < 0) {
        m_lastError = std::string(logName()) + ": frame buffer is not writable";
        m_failed = true;
        return false;
    }

    const uint8_t* srcSlice[] = { bgraPixels };
    int srcStride[] = { static_cast<int>(m_config.width * 4) };

    // min(config.height, m_frame->height) so we never read past the
    // caller's BGRA buffer (config-sized).  m_frame may be taller if NVENC
    // aligned the dimensions — extra lines stay zero (black).
    int srcLines = std::min(static_cast<int>(m_config.height), m_frame->height);
    const int convertedRows = sws_scale(
        m_swsCtx, srcSlice, srcStride, 0, srcLines,
        m_frame->data, m_frame->linesize);
    if (convertedRows != m_frame->height) {
        m_lastError = std::string(logName()) +
                      ": BGRA conversion returned an incomplete frame";
        m_failed = true;
        return false;
    }

    return finishFrame(frameIndex);
}

bool FfmpegEncoderBase::is10BitTarget() const noexcept
{
    if (!m_codecCtx) return false;
    switch (m_codecCtx->pix_fmt) {
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE:
        case AV_PIX_FMT_YUVA444P10LE:
        case AV_PIX_FMT_P010LE:
            return true;
        default:
            return false;
    }
}

bool FfmpegEncoderBase::encodeFrame16f(const uint16_t* rgba16f, int srcStrideBytes,
                                       int64_t frameIndex)
{
    if (!m_initialized || !rgba16f || !is10BitTarget() || srcStrideBytes <= 0) {
        m_lastError = std::string(logName()) + ": invalid RGBA16F frame input";
        m_failed = true;
        spdlog::error("{}", m_lastError);
        return false;
    }

    PackTarget target;
    switch (m_codecCtx->pix_fmt) {
        case AV_PIX_FMT_YUV422P10LE:  target = PackTarget::YUV422P10LE;  break;
        case AV_PIX_FMT_YUV444P10LE:  target = PackTarget::YUV444P10LE;  break;
        case AV_PIX_FMT_YUVA444P10LE: target = PackTarget::YUVA444P10LE; break;
        case AV_PIX_FMT_P010LE:       target = PackTarget::P010LE;       break;
        default:
            m_lastError = std::string(logName()) + ": unsupported 10-bit pixel format";
            m_failed = true;
            return false;
    }

    if (av_frame_make_writable(m_frame) < 0) {
        m_lastError = std::string(logName()) + ": frame buffer is not writable";
        m_failed = true;
        return false;
    }

    // Pack RGBA16F directly into the AVFrame's native 10-bit planes — no
    // swscale, no 8-bit intermediate.  linesizes are in BYTES (FFmpeg may pad
    // them past width*2), which packRgba16fToYuv honours.
    PackPlanes planes;
    planes.y = reinterpret_cast<uint16_t*>(m_frame->data[0]); planes.yStride = m_frame->linesize[0];
    planes.u = reinterpret_cast<uint16_t*>(m_frame->data[1]); planes.uStride = m_frame->linesize[1];
    planes.v = reinterpret_cast<uint16_t*>(m_frame->data[2]); planes.vStride = m_frame->linesize[2];
    if (target == PackTarget::YUVA444P10LE) {
        planes.a = reinterpret_cast<uint16_t*>(m_frame->data[3]);
        planes.aStride = m_frame->linesize[3];
    }
    packRgba16fToYuv(rgba16f, srcStrideBytes,
                     static_cast<int>(m_config.width), static_cast<int>(m_config.height),
                     target, planes);

    return finishFrame(frameIndex);
}

bool FfmpegEncoderBase::finishFrame(int64_t frameIndex)
{
    m_frame->pts = frameIndex;

    // ── HW path: upload the software frame to a CUDA frame ──────────────
    // hw_frames_ctx may be null if avcodec_open2 rejected our frames ctx
    // and reverted to software input — fall through to the SW path then.
    // (ProRes/DNxHR are CPU intra codecs with m_hwAccel=false, so the 16F
    // passthrough always takes the direct sendFrame path below.)
    if (m_hwAccel && m_codecCtx->hw_frames_ctx) {
        AVFrame* hwFrame = av_frame_alloc();
        hwFrame->format = AV_PIX_FMT_CUDA;
        hwFrame->width  = m_frame->width;
        hwFrame->height = m_frame->height;

        int ret = av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, hwFrame, 0);
        if (ret < 0) {
            m_lastError = std::string(logName()) + ": failed to get HW frame buffer";
            m_failed = true;
            spdlog::error("{}", m_lastError);
            av_frame_free(&hwFrame);
            return false;
        }
        ret = av_hwframe_transfer_data(hwFrame, m_frame, 0);
        if (ret < 0) {
            m_lastError = std::string(logName()) + ": failed to upload frame to GPU";
            m_failed = true;
            spdlog::error("{}", m_lastError);
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
        m_failed = true;
        return false;
    }
    if (frame)
        ++m_framesSubmitted;

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
            m_failed = true;
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
    int ret = avcodec_send_frame(m_codecCtx, nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        m_lastError = std::string(logName()) + ": error flushing encoder";
        m_failed = true;
        return 0;
    }

    int count = 0;
    while (true) {
        ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            m_lastError = std::string(logName()) + ": receive error while flushing";
            m_failed = true;
            break;
        }
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
bool FfmpegEncoderBase::encodeFrame16f(const uint16_t*, int, int64_t) { return false; }
bool FfmpegEncoderBase::is10BitTarget() const noexcept { return false; }
int  FfmpegEncoderBase::flush() { return 0; }
void FfmpegEncoderBase::shutdown() {}

#endif

} // namespace rt
