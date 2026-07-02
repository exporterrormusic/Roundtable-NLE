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

    // Base clip properties
    copy->m_label        = m_label;
    copy->m_color        = m_color;
    copy->m_enabled      = m_enabled;
    copy->m_offline      = m_offline;
    copy->m_renderStatus = m_renderStatus;
    copy->m_timelineIn   = m_timelineIn;
    copy->m_duration     = m_duration;
    copy->m_sourceIn     = m_sourceIn;
    copy->m_speed        = m_speed;
    copy->m_maintainPitch = m_maintainPitch;
    copy->m_speedRamp    = m_speedRamp;
    copy->m_blendMode    = m_blendMode;
    copy->m_opacity      = m_opacity;
    copy->m_posX         = m_posX;
    copy->m_posY         = m_posY;
    copy->m_scaleX       = m_scaleX;
    copy->m_scaleY       = m_scaleY;
    copy->m_rotation     = m_rotation;
    copy->m_anchorX      = m_anchorX;
    copy->m_anchorY      = m_anchorY;

    // Shot group / layer metadata
    copy->m_groupId      = m_groupId;
    copy->m_syncLine     = m_syncLine;
    copy->m_linkId       = m_linkId;
    copy->m_shotName     = m_shotName;
    copy->m_layerId      = m_layerId;

    // Effect stack
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

    // Opacity masks and markers (deep copy vectors)
    copy->m_masks   = m_masks;
    copy->m_markers = m_markers;

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
