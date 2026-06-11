/*
 * DNxHREncoder — Avid DNxHR encoding via FFmpeg dnxhd.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

/// DNxHR quality profile
enum class DNxHRProfile : uint8_t
{
    LB,     // Low Bandwidth (8-bit 4:2:2, ~36 Mbps @ 1080p30)
    SQ,     // Standard Quality (8-bit 4:2:2, ~145 Mbps)
    HQ,     // High Quality (8-bit 4:2:2, ~220 Mbps)
    HQX,    // High Quality 10-bit (10-bit 4:2:2, ~220 Mbps)
    _444,   // 4:4:4 10-bit (~350 Mbps)
    Count
};

class DNxHREncoder : public FfmpegEncoderBase
{
public:
    DNxHREncoder() = default;
    ~DNxHREncoder() override { shutdown(); }

    bool init(const EncoderConfig& config) override;

    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::DNxHR; }
    [[nodiscard]] int avCodecId() const noexcept override;
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return false; }

    void setProfile(DNxHRProfile p) noexcept { m_profile = p; }
    [[nodiscard]] DNxHRProfile profile() const noexcept { return m_profile; }

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "DNxHREncoder"; }
    void configureCodecOptions(AVCodecContext* ctx,
                               const EncoderConfig& config,
                               bool hwPath) override;

private:
    DNxHRProfile m_profile{DNxHRProfile::HQ};
};

} // namespace rt
