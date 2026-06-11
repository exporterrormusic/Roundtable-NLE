/*
 * AV1Encoder.cpp — AV1 encoding via FFmpeg.
 *
 * Tries av1_nvenc first (RTX 40-series native AV1), then libsvtav1 (CPU).
 * All shared machinery lives in FfmpegEncoderBase.
 */

#include "formats/AV1Encoder.h"

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

bool AV1Encoder::init(const EncoderConfig& config)
{
    beginInit(config);
    return initNvencThenCpu(config, "av1_nvenc", "libsvtav1", AV_CODEC_ID_AV1);
}

void AV1Encoder::configureCodecOptions(AVCodecContext* ctx,
                                       const EncoderConfig& config,
                                       bool hwPath)
{
    if (hwPath) {
        av_opt_set(ctx->priv_data, "preset", "p5", 0);
        av_opt_set(ctx->priv_data, "tune", "hq", 0);
        av_opt_set(ctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(ctx->priv_data, "qp", config.crf, 0);
    } else {
        // SVT-AV1 uses a numeric 0-13 preset scale (13 = fastest).
        int svtPreset = 8;
        switch (config.preset) {
            case EncoderPreset::Ultrafast: svtPreset = 12; break;
            case EncoderPreset::Superfast: svtPreset = 11; break;
            case EncoderPreset::Veryfast:  svtPreset = 10; break;
            case EncoderPreset::Faster:    svtPreset = 9; break;
            case EncoderPreset::Fast:      svtPreset = 8; break;
            case EncoderPreset::Medium:    svtPreset = 6; break;
            case EncoderPreset::Slow:      svtPreset = 4; break;
            case EncoderPreset::Slower:    svtPreset = 2; break;
            case EncoderPreset::Veryslow:  svtPreset = 0; break;
            default: break;
        }
        av_opt_set_int(ctx->priv_data, "preset", svtPreset, 0);
        if (config.bitrateMbps == 0)
            av_opt_set_int(ctx->priv_data, "crf", config.crf, 0);
    }
}

int AV1Encoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_AV1;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool AV1Encoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
void AV1Encoder::configureCodecOptions(AVCodecContext*, const EncoderConfig&, bool) {}
int  AV1Encoder::avCodecId() const noexcept { return 0; }

#endif

} // namespace rt
