/*
 * AV1Encoder — AV1 encoding via FFmpeg libsvtav1 or NVENC av1_nvenc.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

class AV1Encoder : public FfmpegEncoderBase
{
public:
    AV1Encoder() = default;
    ~AV1Encoder() override { shutdown(); }

    bool init(const EncoderConfig& config) override;

    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::AV1; }
    [[nodiscard]] int avCodecId() const noexcept override;

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "AV1Encoder"; }
    void configureCodecOptions(AVCodecContext* ctx,
                               const EncoderConfig& config,
                               bool hwPath) override;
};

} // namespace rt
