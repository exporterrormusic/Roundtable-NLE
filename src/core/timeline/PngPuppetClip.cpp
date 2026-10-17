/*
 * PngPuppetClip.cpp — Veadotube-style PNG puppet clip implementation.
 */

#include "timeline/PngPuppetClip.h"

namespace rt {

PngPuppetClip::PngPuppetClip()
    : Clip(ClipType::PngPuppet)
{
    m_label = "PNG Puppet";
    // Color left at sentinel — TimelineTrackWidget applies the theme tint.
}

PngPuppetClip::PngPuppetClip(const std::string& characterName, const std::string& variant)
    : PngPuppetClip()
{
    m_characterName = characterName;
    if (!variant.empty())
        m_variant = variant;
    if (!characterName.empty())
        m_label = characterName;
}

std::unique_ptr<Clip> PngPuppetClip::clone() const
{
    auto copy = std::make_unique<PngPuppetClip>();
    copyBaseStateTo(*copy);

    // PngPuppetClip-specific properties
    copy->m_characterName        = m_characterName;
    copy->m_variant              = m_variant;
    copy->m_facePaths            = m_facePaths;
    copy->m_talking              = m_talking;
    copy->m_talkSwapSeconds      = m_talkSwapSeconds;
    copy->m_blinkIntervalSeconds = m_blinkIntervalSeconds;
    copy->m_blinkDurationSeconds = m_blinkDurationSeconds;
    copy->m_breathAmplitude      = m_breathAmplitude;
    copy->m_breathSpeed          = m_breathSpeed;
    copy->m_swayAmplitude        = m_swayAmplitude;
    copy->m_seed                 = m_seed;

    return copy;
}

} // namespace rt
