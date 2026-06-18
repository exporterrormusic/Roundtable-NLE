/*
 * DNxHREncoder.cpp — Avid DNxHR encoding via FFmpeg dnxhd codec.
 * All shared machinery lives in FfmpegEncoderBase.
 */

#include "formats/DNxHREncoder.h"

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
const char* dnxhrProfileName(DNxHRProfile p) noexcept
{
    switch (p) {
        case DNxHRProfile::LB:   return "dnxhr_lb";
        case DNxHRProfile::SQ:   return "dnxhr_sq";
        case DNxHRProfile::HQ:   return "dnxhr_hq";
        case DNxHRProfile::HQX:  return "dnxhr_hqx";
        case DNxHRProfile::_444: return "dnxhr_444";
        default:                 return "dnxhr_hq";
    }
}
} // namespace

bool DNxHREncoder::init(const EncoderConfig& config)
{
    beginInit(config);

    // Honour the UI-selected profile (defaults to HQ).
    m_profile = config.dnxhrProfile;

    // DNxHR uses YUV422P (8-bit), YUV422P10LE (10-bit) or YUV444P10LE (444).
    const bool is10bit = (m_profile == DNxHRProfile::HQX || m_profile == DNxHRProfile::_444);
    const bool is444   = (m_profile == DNxHRProfile::_444);
    const int pixFmt = is444   ? AV_PIX_FMT_YUV444P10LE
                     : is10bit ? AV_PIX_FMT_YUV422P10LE
                               : AV_PIX_FMT_YUV422P;

    if (!initCpuCodec(config, nullptr, AV_CODEC_ID_DNXHD, pixFmt))
        return false;

    spdlog::info("DNxHREncoder: {}x{} profile={}", config.width, config.height,
                 dnxhrProfileName(m_profile));
    return true;
}

void DNxHREncoder::configureCodecOptions(AVCodecContext* ctx,
                                         const EncoderConfig& /*config*/,
                                         bool /*hwPath*/)
{
    av_opt_set(ctx->priv_data, "profile", dnxhrProfileName(m_profile), 0);
}

int DNxHREncoder::avCodecId() const noexcept
{
    return AV_CODEC_ID_DNXHD;
}

#else // !ROUNDTABLE_HAS_FFMPEG

bool DNxHREncoder::init(const EncoderConfig&) { m_lastError = "FFmpeg not available"; return false; }
void DNxHREncoder::configureCodecOptions(AVCodecContext*, const EncoderConfig&, bool) {}
int  DNxHREncoder::avCodecId() const noexcept { return 0; }

#endif

} // namespace rt
