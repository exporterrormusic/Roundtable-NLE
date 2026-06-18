/*
 * PassthroughEligibility.cpp — see PassthroughEligibility.h for the contract.
 */

#include "timeline/PassthroughEligibility.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/Transition.h"

#include <cmath>

namespace rt {

namespace {

// Identity thresholds — match the existing single-layer fast path in
// CompositeServiceFrame (opacity>=0.999, |pos|<0.5, |scale-1|<0.001,
// |rot|<0.01, crop<0.01) so passthrough never fires where the compositor
// would actually do transform work.
constexpr float kPosEps   = 0.5f;     // sub-pixel position
constexpr float kScaleEps = 0.001f;
constexpr float kRotEps   = 0.01f;
constexpr float kCropEps  = 0.01f;
constexpr float kOpaque   = 0.999f;

/// A clip is "active" at `tick` on the half-open interval [in, out).
bool isActiveAt(const Clip& c, int64_t tick) noexcept
{
    return tick >= c.timelineIn() && tick < c.timelineOut();
}

/// The single active video clip has an identity transform / no compositing
/// extras at `localTick`.
bool isIdentityVideoClip(VideoClip& vc, int64_t localTick) noexcept
{
    if (vc.isVideoCharacter()) return false;            // always 0.85× contain-fit
    if (vc.opacity().evaluate(localTick) < kOpaque) return false;
    if (std::abs(vc.positionX().evaluate(localTick)) > kPosEps) return false;
    if (std::abs(vc.positionY().evaluate(localTick)) > kPosEps) return false;
    if (std::abs(vc.scaleX().evaluate(localTick) - 1.0f) > kScaleEps) return false;
    if (std::abs(vc.scaleY().evaluate(localTick) - 1.0f) > kScaleEps) return false;
    if (std::abs(vc.rotation().evaluate(localTick)) > kRotEps) return false;
    if (vc.cropLeft()  > kCropEps || vc.cropRight()  > kCropEps ||
        vc.cropTop()   > kCropEps || vc.cropBottom() > kCropEps) return false;
    if (vc.effects().hasActiveEffects()) return false;
    if (vc.blendMode() != 0) return false;              // 0 = Normal
    if (vc.maskCount() > 0) return false;
    // Retime: only an un-r*ramped*, 1.0× clip is a 1:1 passthrough of
    // contiguous source frames.
    if (std::abs(vc.speed() - 1.0) > 1e-9) return false;
    if (!vc.speedRamp().isStatic()) return false;
    return true;
}

} // namespace

PassthroughTarget evaluatePassthroughAt(Timeline& timeline, int64_t tick)
{
    PassthroughTarget result;

    VideoClip* candidate = nullptr;
    int        visualLayers = 0;

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        Track* tr = timeline.track(ti);
        if (!tr || tr->isDivider()) continue;
        if (tr->type() != TrackType::Video) continue;   // audio tracks don't composite

        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            Clip* c = tr->clip(ci);
            if (!c || !isActiveAt(*c, tick)) continue;

            const ClipType ct = c->clipType();

            // A caption overlay (on the caption track) is a second composited
            // layer — RenderComplexity ignores it, but passthrough must not.
            if (ct == ClipType::Caption || tr->isCaptionTrack())
                return result;   // ineligible

            // Adjustment layers carry no pixels but inject effects into the
            // layers below.  One with active effects forces a render pass; a
            // no-op adjustment is invisible and ignored.
            if (ct == ClipType::Adjustment) {
                if (c->effects().hasActiveEffects()) return result;
                continue;
            }

            if (!c->isVisual()) continue;

            ++visualLayers;
            if (visualLayers > 1) return result;          // more than one layer
            // The single visual layer must be a plain video clip.
            if (ct != ClipType::Video) return result;
            candidate = static_cast<VideoClip*>(c);
        }

        // Any active transition (even a single-sided fade) means a blend pass.
        for (const Transition& t : tr->transitions()) {
            int64_t ts = 0, te = 0;
            t.getRange(ts, te);
            if (tick >= ts && tick < te) return result;
        }
    }

    if (visualLayers != 1 || !candidate) return result;

    const int64_t localTick = tick - candidate->timelineIn();
    if (!isIdentityVideoClip(*candidate, localTick)) return result;

    result.eligible  = true;
    result.clip      = candidate;
    result.localTick = localTick;
    return result;
}

} // namespace rt
