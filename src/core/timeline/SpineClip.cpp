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

    // Base clip properties
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_offline    = m_offline;
    copy->m_renderStatus = m_renderStatus;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
    copy->m_maintainPitch = m_maintainPitch;
    copy->m_timeInterpolation = m_timeInterpolation;
    copy->m_speedRamp  = m_speedRamp;
    copy->m_blendMode  = m_blendMode;
    copy->m_opacity    = m_opacity;
    copy->m_posX       = m_posX;
    copy->m_posY       = m_posY;
    copy->m_scaleX     = m_scaleX;
    copy->m_scaleY     = m_scaleY;
    copy->m_rotation   = m_rotation;
    copy->m_shutterAngle = m_shutterAngle;
    copy->m_anchorX    = m_anchorX;
    copy->m_anchorY    = m_anchorY;

    // Shot group / layer metadata
    copy->m_groupId    = m_groupId;
    copy->m_syncLine   = m_syncLine;
    copy->m_linkId     = m_linkId;
    copy->m_shotName   = m_shotName;
    copy->m_layerId    = m_layerId;

    // Effect stack
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

    // Opacity masks and markers (deep copy vectors)
    copy->m_masks    = m_masks;
    copy->m_markers  = m_markers;

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
