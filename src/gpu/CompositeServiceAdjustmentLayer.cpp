/*
 * Adjustment-layer render-boundary construction.
 * Kept separate from the media-heavy layer builder so the invariant can be
 * regression-tested without pulling every CPU clip renderer into the test.
 */

#include "CompositeServiceLayerBuild.h"
#include "timeline/AdjustmentClip.h"

#include <algorithm>
#include <utility>

namespace rt {

void appendAdjustmentLayerBoundary(std::vector<LayerInfo>& layers,
                                   AdjustmentClip& adjustment,
                                   int64_t localTick,
                                   float effectStrength)
{
    if (!adjustment.effects().hasActiveEffects())
        return;

    LayerInfo marker;
    marker.isAdjustmentLayer = true;
    marker.clipId = adjustment.id();
    marker.clipPtr = &adjustment;
    marker.opacity = std::clamp(effectStrength, 0.0f, 1.0f);
    marker.effects = adjustment.effects().evaluate(localTick);
    layers.push_back(std::move(marker));
}

float adjustmentLayerStrengthAtTick(
    AdjustmentClip& adjustment,
    int64_t localTick,
    int64_t timelineTick,
    const std::vector<Transition>& transitions)
{
    float strength = 1.0f;
    try {
        strength = adjustment.opacity().evaluate(localTick);
    } catch (...) {
        // Match ordinary clip evaluation: an invalid keyframe track should
        // degrade to the neutral/default opacity rather than lose the frame.
        strength = 1.0f;
    }
    strength = std::clamp(strength, 0.0f, 1.0f);

    for (const auto& transition : transitions) {
        if (transition.type != TransitionType::CrossDissolve)
            continue;

        const bool incoming = transition.rightClipId == adjustment.id();
        const bool outgoing = transition.leftClipId == adjustment.id();
        if (!incoming && !outgoing)
            continue;

        float progress = transition.progress(timelineTick);
        if (progress < 0.0f) {
            // Preserve the same sub-frame boundary guard used by ordinary
            // clips. A rounded head/tail edit point must not flash the effect
            // at full strength for one frame just outside the nominal range.
            int64_t rangeStart = 0;
            int64_t rangeEnd = 0;
            transition.getRange(rangeStart, rangeEnd);
            const bool singleIncoming = incoming && transition.leftClipId == 0;
            const bool singleOutgoing = outgoing && transition.rightClipId == 0;
            if (singleIncoming && timelineTick < rangeStart)
                progress = 0.0f;
            else if (singleOutgoing && timelineTick >= rangeEnd)
                progress = 1.0f;
            else
                continue;
        }

        // An adjustment has no source pixels to fade. Its dissolve is the
        // strength of the transformation from the pre-effect frame to the
        // processed frame.
        if (incoming)
            strength *= progress;
        if (outgoing)
            strength *= (1.0f - progress);
    }

    return std::clamp(strength, 0.0f, 1.0f);
}

} // namespace rt
