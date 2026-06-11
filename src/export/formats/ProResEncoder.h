/*
 * ProResEncoder — Apple ProRes encoding via FFmpeg prores_ks.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

class ProResEncoder : public FfmpegEncoderBase
{
public:
    ProResEncoder() = default;
    ~ProResEncoder() override { shutdown(); }

    bool init(const EncoderConfig& config) override;

    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::ProRes; }
    [[nodiscard]] int avCodecId() const noexcept override;
    [[nodiscard]] bool isHardwareAccelerated() const noexcept override { return false; }

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "ProResEncoder"; }
    void configureCodecOptions(AVCodecContext* ctx,
                               const EncoderConfig& config,
                               bool hwPath) override;
};

} // namespace rt
