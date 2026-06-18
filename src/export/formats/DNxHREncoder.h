/*
 * DNxHREncoder — Avid DNxHR encoding via FFmpeg dnxhd.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

// DNxHRProfile is defined in Encoder.h (beside ProResProfile) so EncoderConfig
// can carry it.

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
