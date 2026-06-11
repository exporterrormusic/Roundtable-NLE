/*
 * H264Encoder — H.264/AVC encoding via FFmpeg libx264 or NVENC h264_nvenc.
 */

#pragma once

#include "FfmpegEncoderBase.h"

namespace rt {

class H264Encoder : public FfmpegEncoderBase
{
public:
    H264Encoder() = default;
    ~H264Encoder() override { shutdown(); }

    bool init(const EncoderConfig& config) override;

    [[nodiscard]] EncoderCodec codec() const noexcept override { return EncoderCodec::H264; }
    [[nodiscard]] int avCodecId() const noexcept override;

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "H264Encoder"; }
    void configureCodecOptions(AVCodecContext* ctx,
                               const EncoderConfig& config,
                               bool hwPath) override;
};

} // namespace rt
