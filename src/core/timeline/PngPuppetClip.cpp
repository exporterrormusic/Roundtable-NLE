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

std::unique_ptr<Clip> PngPuppetClip::createEmpty() const
{
    return std::make_unique<PngPuppetClip>();
}

void PngPuppetClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const PngPuppetClip&>(src);
    // PngPuppetClip-specific properties
    m_characterName        = o.m_characterName;
    m_variant              = o.m_variant;
    m_facePaths            = o.m_facePaths;
    m_talking              = o.m_talking;
    m_talkSwapSeconds      = o.m_talkSwapSeconds;
    m_blinkIntervalSeconds = o.m_blinkIntervalSeconds;
    m_blinkDurationSeconds = o.m_blinkDurationSeconds;
    m_breathAmplitude      = o.m_breathAmplitude;
    m_breathSpeed          = o.m_breathSpeed;
    m_swayAmplitude        = o.m_swayAmplitude;
    m_seed                 = o.m_seed;
}

} // namespace rt
