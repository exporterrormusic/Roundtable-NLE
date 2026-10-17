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

std::unique_ptr<Clip> SpineClip::clone() const
{
    auto copy = std::make_unique<SpineClip>();
    copyBaseStateTo(*copy);

    // SpineClip-specific properties
    copy->m_characterName = m_characterName;
    copy->m_outfit        = m_outfit;
    copy->m_stance        = m_stance;
    copy->m_animation     = m_animation;
    copy->m_looping       = m_looping;
    copy->m_talking       = m_talking;
    copy->m_animSpeed     = m_animSpeed;
    copy->m_useGlobalTime = m_useGlobalTime;
    copy->m_cropL         = m_cropL;
    copy->m_cropR         = m_cropR;
    copy->m_cropT         = m_cropT;
    copy->m_cropB         = m_cropB;

    return copy;
}

} // namespace rt
