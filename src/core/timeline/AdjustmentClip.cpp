/*
 * AdjustmentClip.cpp — Adjustment layer clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/AdjustmentClip.h"

namespace rt {

AdjustmentClip::AdjustmentClip()
    : Clip(ClipType::Adjustment)
{
    m_label = "Adjustment Layer";
    // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> AdjustmentClip::clone() const
{
    auto copy = std::make_unique<AdjustmentClip>();
    copyBaseStateTo(*copy);

    // AdjustmentClip-specific. m_blendMode here is AdjustmentClip's own
    // field (it shadows Clip::m_blendMode), so copyBaseStateTo can't reach it.
    copy->m_blendMode   = m_blendMode;
    copy->m_singleTrack = m_singleTrack;

    return copy;
}

} // namespace rt
