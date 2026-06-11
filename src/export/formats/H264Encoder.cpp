/*
 * H264Encoder.cpp — H.264/AVC encoding via FFmpeg.
 *
 * Tries h264_nvenc first (NVIDIA), then libx264 (CPU).
 * All shared machinery lives in FfmpegEncoderBase.
 */

#include "formats/H264Encoder.h"

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

bool H264Encoder::init(const EncoderConfig& config)
{
    beginInit(config);
    return initNvencThenCpu(config, "h264_nvenc", "libx264", AV_CODEC_ID_H264);
}

void H264Encoder::configureCodecOptions(AVCodecContext* ctx,
                                        const EncoderConfig& config,
                                        bool hwPath)
{
    ctx->max_b_frames = 2;

    // VBR cap: max rate defaults to 2x target when uncapped.
    if (config.bitrateMbps > 0) {
        ctx->rc_max_rate  = config.maxBitrateMbps > 0
            ? static_cast<int64_t>(config.maxBitrateMbps) * 1000000
            : ctx->bit_rate * 2;
        ctx->rc_buffer_size = static_cast<int>(ctx->rc_max_rate);
    }

    if (hwPath) {
        av_opt_set(ctx->priv_data, "preset", "p5", 0);
        av_opt_set(ctx->priv_data, "tune", "hq", 0);
        av_opt_set(ctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(ctx->priv_data, "qp", config.crf, 0);
    } else {
        static const char* presetNames[] = {
            "ultrafast", "superfast", "veryfast", "faster", "fast",
            "medium", "slow", "slower", "veryslow"
        };
        int idx = static_cast<int>(config.preset);
        if (idx < 9) av_opt_set(ctx->priv_data, "preset", presetNames[idx], 0);
        if (config.bitrateMbps == 0)
            av_opt_set_int(ctx->priv_data, "crf", config.crf, 0);
    }

    // High profile: required for 4:2:0 8-bit at 4K/high resolutions.
    // Main profile (the prior default) is non-standard for 4K and makes
    // editors/browsers conform or reject the clip.
    av_opt_set(ctx->priv_data, "profile", "high", 0);
}

int H264Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_H264;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool H264Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
void H264Encoder::configureCodecOptions(AVCodecContext*, const EncoderConfig&, bool) {}
int  H264Encoder::avCodecId() const noexcept { return 0; }

#endif

} // namespace rt
