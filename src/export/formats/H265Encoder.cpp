/*
 * H265Encoder.cpp — H.265/HEVC encoding via FFmpeg.
 *
 * Tries hevc_nvenc first (NVIDIA), then libx265 (CPU).
 * All shared machinery lives in FfmpegEncoderBase.
 */

#include "formats/H265Encoder.h"

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

bool H265Encoder::init(const EncoderConfig& config)
{
    beginInit(config);
    return initNvencThenCpu(config, "hevc_nvenc", "libx265", AV_CODEC_ID_HEVC);
}

void H265Encoder::configureCodecOptions(AVCodecContext* ctx,
                                        const EncoderConfig& config,
                                        bool hwPath)
{
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
}

int H265Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_HEVC;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool H265Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
void H265Encoder::configureCodecOptions(AVCodecContext*, const EncoderConfig&, bool) {}
int  H265Encoder::avCodecId() const noexcept { return 0; }

#endif

} // namespace rt
