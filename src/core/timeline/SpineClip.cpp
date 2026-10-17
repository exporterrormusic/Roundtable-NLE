/*
 * SpineClip.cpp — Spine character clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/SpineClip.h"

namespace rt {

SpineClip::SpineClip()
    : Clip(ClipType::Spine)
{
    m_label = "Spine Clip";
    // Color left at sentinel — TimelineTrackWidget applies the spine theme tint.
}

SpineClip::SpineClip(const std::string& characterName, const std::string& outfit)
    : SpineClip()
{
    m_characterName = characterName;
    m_outfit = outfit;
}

std::unique_ptr<Clip> SpineClip::createEmpty() const
{
    return std::make_unique<SpineClip>();
}

void SpineClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const SpineClip&>(src);
    // SpineClip-specific properties
    m_characterName = o.m_characterName;
    m_outfit        = o.m_outfit;
    m_stance        = o.m_stance;
    m_animation     = o.m_animation;
    m_looping       = o.m_looping;
    m_talking       = o.m_talking;
    m_animSpeed     = o.m_animSpeed;
    m_useGlobalTime = o.m_useGlobalTime;
    m_cropL         = o.m_cropL;
    m_cropR         = o.m_cropR;
    m_cropT         = o.m_cropT;
    m_cropB         = o.m_cropB;
}

} // namespace rt
