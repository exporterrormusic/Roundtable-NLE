/*
 * Adjustment-layer render-boundary construction.
 * Kept separate from the media-heavy layer builder so the invariant can be
 * regression-tested without pulling every CPU clip renderer into the test.
 */

#include "CompositeServiceLayerBuild.h"
#include "timeline/AdjustmentClip.h"

#include <utility>

namespace rt {

void appendAdjustmentLayerBoundary(std::vector<LayerInfo>& layers,
                                   AdjustmentClip& adjustment,
                                   int64_t localTick)
{
    if (!adjustment.effects().hasActiveEffects())
        return;

    LayerInfo marker;
    marker.isAdjustmentLayer = true;
    marker.clipId = adjustment.id();
    marker.clipPtr = &adjustment;
    marker.effects = adjustment.effects().evaluate(localTick);
    layers.push_back(std::move(marker));
}

} // namespace rt
