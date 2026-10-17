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

std::unique_ptr<Clip> AdjustmentClip::createEmpty() const
{
    return std::make_unique<AdjustmentClip>();
}

void AdjustmentClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const AdjustmentClip&>(src);
    // AdjustmentClip-specific
    m_singleTrack = o.m_singleTrack;
}

} // namespace rt
