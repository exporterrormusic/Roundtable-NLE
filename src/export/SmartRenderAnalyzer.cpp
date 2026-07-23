/*
 * SmartRenderAnalyzer.cpp — implementation of smart render analysis.
 */

#include "SmartRenderAnalyzer.h"
#include "Encoder.h"

#include "Constants.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"

#include <spdlog/spdlog.h>
#include <cmath>

namespace rt {

const char* encoderCodecToSourceName(EncoderCodec codec) noexcept
{
    switch (codec) {
        case EncoderCodec::H264:  return "h264";
        case EncoderCodec::H265:  return "hevc";
        case EncoderCodec::AV1:   return "av1";
        case EncoderCodec::ProRes: return "prores";
        case EncoderCodec::DNxHR:  return "dnxhd";
        default:                  return "";
    }
}

/// Check if a VideoClip is an "identity" clip with no modifications.
static bool isIdentityClip(Clip& clip, const VideoClip& vc,
                           const EncoderConfig& enc,
                           uint32_t outW, uint32_t outH)
{
    // Blending and optical flow synthesize pixels even at 1.0x, so they can
    // never use packet passthrough / smart render.
    if (clip.timeInterpolation() != TimeInterpolation::FrameSampling)
        return false;

    // Speed must be exactly 1.0
    if (clip.speed() != 1.0)
        return false;

    // Speed ramp must be static at 1.0
    if (!clip.speedRamp().isStatic() || clip.speedRamp().defaultValue() != 1.0f)
        return false;

    // Opacity must be static at 1.0
    if (!clip.opacity().isStatic() || clip.opacity().defaultValue() != 1.0f)
        return false;

    // Transform must be identity
    if (!clip.positionX().isStatic() || clip.positionX().defaultValue() != 0.0f)
        return false;
    if (!clip.positionY().isStatic() || clip.positionY().defaultValue() != 0.0f)
        return false;
    if (!clip.scaleX().isStatic() || clip.scaleX().defaultValue() != 1.0f)
        return false;
    if (!clip.scaleY().isStatic() || clip.scaleY().defaultValue() != 1.0f)
        return false;
    if (!clip.rotation().isStatic() || clip.rotation().defaultValue() != 0.0f)
        return false;
    if (!clip.shutterAngle().isStatic() ||
        clip.shutterAngle().defaultValue() != 0.0f)
        return false;

    // No effects
    if (!clip.effects().isEmpty())
        return false;

    // No masks
    if (!clip.masks().empty())
        return false;

    // Blend mode must be Normal (0)
    if (clip.blendMode() != 0)
        return false;

    // No crop
    if (vc.cropLeft() != 0.0f || vc.cropRight() != 0.0f ||
        vc.cropTop() != 0.0f || vc.cropBottom() != 0.0f)
        return false;

    // Source resolution must match export resolution
    if (vc.sourceWidth() != outW || vc.sourceHeight() != outH)
        return false;

    // Source codec must match export codec
    const char* expectedCodec = encoderCodecToSourceName(enc.codec);
    if (!expectedCodec[0] || vc.sourceCodecName() != expectedCodec)
        return false;

    // Source FPS must match export FPS (within 0.01)
    double exportFps = static_cast<double>(enc.fpsNum) / enc.fpsDen;
    if (std::abs(vc.sourceFps() - exportFps) > 0.01)
        return false;

    return true;
}

SmartRenderPlan analyzeSmartRender(
    const Timeline& timeline,
    const EncoderConfig& encoderConfig,
    uint32_t outputWidth,
    uint32_t outputHeight,
    int64_t startFrame,
    int64_t endFrame)
{
    SmartRenderPlan plan;
    plan.totalFrames = endFrame - startFrame;

    const double exportFps = static_cast<double>(encoderConfig.fpsNum) / encoderConfig.fpsDen;

    // Image sequences can't do passthrough
    if (encoderConfig.codec == EncoderCodec::ImageSequence) {
        plan.reEncodeCount = plan.totalFrames;
        spdlog::info("SmartRender: Image sequence export — no passthrough");
        return plan;
    }

    for (int64_t f = startFrame; f < endFrame; ++f) {
        // Convert frame number to timeline tick
        int64_t tick = static_cast<int64_t>(
            static_cast<double>(f) * kTicksPerSecond / exportFps);

        // Collect all active video clips at this tick across all video tracks
        std::vector<std::pair<Clip*, const VideoClip*>> activeClips;

        for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
            const auto* trk = timeline.track(ti);
            if (!trk || trk->type() != TrackType::Video || trk->isMuted())
                continue;

            auto clips = trk->clipsAtTime(tick);
            for (auto* c : clips) {
                if (!c || !c->isEnabled() || c->clipType() != ClipType::Video)
                    continue;
                activeClips.emplace_back(c, static_cast<const VideoClip*>(c));
            }
        }

        // Passthrough requires exactly one active video clip
        if (activeClips.size() != 1) {
            ++plan.reEncodeCount;
            continue;
        }

        const auto& [clip, vc] = activeClips[0];

        // Check if the clip is identity (no modifications)
        if (!isIdentityClip(*clip, *vc, encoderConfig, outputWidth, outputHeight)) {
            ++plan.reEncodeCount;
            continue;
        }

        // Compute source frame number:
        // clip local offset in ticks = tick - timelineIn
        // source tick = sourceIn + localOffset (speed==1.0 guaranteed)
        int64_t localOffset = tick - clip->timelineIn();
        int64_t sourceTick = clip->sourceIn() + localOffset;
        double sourceSeconds = static_cast<double>(sourceTick) / kTicksPerSecond;
        int64_t sourceFrame = std::llround(sourceSeconds * vc->sourceFps());

        PassthroughFrame pf;
        pf.mediaPath = vc->mediaPath();
        pf.sourceFrame = sourceFrame;
        plan.passthroughFrames[f - startFrame] = std::move(pf);
        ++plan.passthroughCount;
    }

    spdlog::info("SmartRender: {}/{} frames passthrough ({:.1f}%), {} re-encode",
                 plan.passthroughCount, plan.totalFrames,
                 100.0 * plan.passthroughCount / std::max<int64_t>(plan.totalFrames, 1),
                 plan.reEncodeCount);

    return plan;
}

} // namespace rt
