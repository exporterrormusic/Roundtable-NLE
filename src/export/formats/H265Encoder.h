/*
 * H265Encoder — H.265/HEVC encoding via FFmpeg libx265 or NVENC hevc_nvenc.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

class H265Encoder : public FfmpegEncoderBase
{
public:
    H265Encoder() = default;
    ~H265Encoder() override { shutdown(); }

    bool init(const EncoderConfig& config) override;

    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::H265; }
    [[nodiscard]] int avCodecId() const noexcept override;

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "H265Encoder"; }
    void configureCodecOptions(AVCodecContext* ctx,
                               const EncoderConfig& config,
                               bool hwPath) override;
};

} // namespace rt
