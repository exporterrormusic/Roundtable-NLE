/*
 * ProResEncoder.cpp — ProRes encoding via FFmpeg prores_ks.
 * All shared machinery lives in FfmpegEncoderBase.
 */

#include "formats/ProResEncoder.h"

#include <spdlog/spdlog.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}
#endif

namespace rt {

#ifdef ROUNDTABLE_HAS_FFMPEG

namespace {
int proresProfileIndex(ProResProfile p) noexcept
{
    switch (p) {
        case ProResProfile::Proxy:    return 0;
        case ProResProfile::LT:       return 1;
        case ProResProfile::Standard: return 2;
        case ProResProfile::HQ:       return 3;
        case ProResProfile::_4444:    return 4;
        case ProResProfile::_4444XQ:  return 5;
        default:                      return 2;
    }
}
} // namespace

bool ProResEncoder::init(const EncoderConfig& config)
{
    beginInit(config);

    // ProRes uses YUV422P10LE, or YUVA444P10LE for the alpha profiles.
    const bool alpha = config.proresProfile >= ProResProfile::_4444;
    const int pixFmt = alpha ? AV_PIX_FMT_YUVA444P10LE : AV_PIX_FMT_YUV422P10LE;

    if (!initCpuCodec(config, "prores_ks", AV_CODEC_ID_PRORES, pixFmt))
        return false;

    spdlog::info("ProResEncoder: {}x{} profile={}", config.width, config.height,
                 proresProfileIndex(config.proresProfile));
    return true;
}

void ProResEncoder::configureCodecOptions(AVCodecContext* ctx,
                                          const EncoderConfig& config,
                                          bool /*hwPath*/)
{
    av_opt_set_int(ctx->priv_data, "profile",
                   proresProfileIndex(config.proresProfile), 0);
}

int ProResEncoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_PRORES;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool ProResEncoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
void ProResEncoder::configureCodecOptions(AVCodecContext*, const EncoderConfig&, bool) {}
int  ProResEncoder::avCodecId() const noexcept { return 0; }

#endif

} // namespace rt
