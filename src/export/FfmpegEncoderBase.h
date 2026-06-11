/*
 * FfmpegEncoderBase — shared implementation for all FFmpeg-backed encoders.
 *
 * The five codec encoders (H264/H265/AV1/ProRes/DNxHR) previously each
 * carried a verbatim copy of the same machinery: codec-context setup,
 * BGRA→YUV swscale conversion, the NVENC hw-frames → NVENC sw-input → CPU
 * fallback ladder, the send/receive packet-drain loop, flush, and shutdown.
 * This base owns all of that once; subclasses provide only codec identity
 * and per-codec option setup (configureCodecOptions).
 *
 * Subclass contract:
 *   - init() calls one of initNvencThenCpu() / initCpuCodec(), then
 *     finishInit().
 *   - configureCodecOptions(ctx, config, hwPath) sets priv_data options and
 *     any extra ctx fields (max_b_frames, rc caps, profile).  It is called
 *     AFTER the common fields (size/timebase/framerate/gop/global-header/
 *     bitrate/color tags) and BEFORE avcodec_open2, for every open attempt
 *     (hwPath=true for the two NVENC phases, false for CPU).
 *
 * Behavioral notes preserved from the originals:
 *   - The packet store discipline (m_pktStore cleared at the start of each
 *     send/flush; every payload copied via retainPacketData) — see the
 *     comment on Encoder::m_pktStore.
 *   - encodeFrame converts with srcLines = min(config.height, frame->height)
 *     so an NVENC-aligned (taller) frame never reads past the caller's
 *     buffer; the extra lines stay black.
 *   - The sws context is a per-instance member (the old per-TU thread_local
 *     meant two live encoders on one thread would have shared one context).
 */

#pragma once

#include "Encoder.h"

#ifdef ROUNDTABLE_HAS_FFMPEG
struct SwsContext;
#endif

namespace rt {

class FfmpegEncoderBase : public Encoder
{
public:
    ~FfmpegEncoderBase() override;

    bool encodeFrame(const uint8_t* bgraPixels, int64_t frameIndex) override;
    int  flush() override;
    void shutdown() override;

    [[nodiscard]] const EncodedPacket& lastPacket() const override { return m_lastPacket; }
    [[nodiscard]] const std::vector<EncodedPacket>& flushedPackets() const override { return m_flushedPackets; }
    [[nodiscard]] const std::vector<EncodedPacket>& pendingPackets() const override { return m_pendingPackets; }
    [[nodiscard]] AVCodecContext* avCodecContext() const noexcept override { return m_codecCtx; }
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized; }
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return m_hwAccel; }
    [[nodiscard]] const std::string& lastError() const noexcept override { return m_lastError; }
    [[nodiscard]] int64_t framesEncoded() const noexcept override { return m_framesEncoded; }

protected:
    /// Short name used in log/error messages ("H264Encoder", ...).
    [[nodiscard]] virtual const char* logName() const noexcept = 0;

    /// Per-codec option hook — see the class comment for the contract.
    virtual void configureCodecOptions(AVCodecContext* ctx,
                                       const EncoderConfig& config,
                                       bool hwPath) = 0;

#ifdef ROUNDTABLE_HAS_FFMPEG
    /// Reset state and remember the config.  Call first in init().
    void beginInit(const EncoderConfig& config);

    /// The NVENC ladder shared by H264/H265/AV1:
    ///   Phase 1: nvencName with CUDA hw frames (zero-copy encode input)
    ///   Phase 2: nvencName with software YUV420P input
    ///   Phase 3: cpuName (falling back to cpuFallbackId) on the CPU
    /// On success m_codecCtx is open and m_hwAccel reflects the path taken.
    /// `tagColors` applies the BT.709 colr tags when config.bt709 is set.
    bool initNvencThenCpu(const EncoderConfig& config,
                          const char* nvencName,
                          const char* cpuName,
                          int cpuFallbackId);

    /// Open a CPU-only intra codec (ProRes/DNxHR) with the given pixel
    /// format.  Preserves the historical intra-codec behavior: no colr
    /// tags, no bit_rate, no gop_size (prores_ks is qscale-driven and
    /// dnxhd validates bit_rate against its CBR table — setting it could
    /// make avcodec_open2 fail).
    bool initCpuCodec(const EncoderConfig& config,
                      const char* codecName,
                      int codecFallbackId,
                      int pixFmt);

    /// Allocate the software frame (swFrameFormat), the packet, and the
    /// BGRA→swFrameFormat sws context; set m_initialized on success.
    bool finishInit(int swFrameFormat);

    /// Common ctx fields: size, timebase, framerate, GLOBAL_HEADER, and —
    /// when setRateControl — gop_size + bit_rate; when tagColors &&
    /// config.bt709 — the BT.709 colr tags.
    void applyCommonParams(AVCodecContext* ctx, const EncoderConfig& config,
                           int pixFmt, bool tagColors,
                           bool setRateControl) const;

    /// avcodec_send_frame + drain loop into m_lastPacket/m_pendingPackets.
    bool sendFrame(AVFrame* frame);
#endif

    AVCodecContext* m_codecCtx{nullptr};
    AVFrame*        m_frame{nullptr};
    AVPacket*       m_packet{nullptr};
    AVBufferRef*    m_hwDeviceCtx{nullptr};
    AVBufferRef*    m_hwFramesCtx{nullptr};
#ifdef ROUNDTABLE_HAS_FFMPEG
    SwsContext*     m_swsCtx{nullptr};
#endif

    EncoderConfig              m_config;
    EncodedPacket              m_lastPacket;
    std::vector<EncodedPacket> m_flushedPackets;
    std::vector<EncodedPacket> m_pendingPackets;  // extra packets from draining
    std::string                m_lastError;
    int64_t                    m_framesEncoded{0};
    bool                       m_initialized{false};
    bool                       m_hwAccel{false};
};

} // namespace rt
