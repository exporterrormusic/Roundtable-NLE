/*
 * SequenceClip.cpp — Nested sequence clip implementation.
 */

#include "timeline/SequenceClip.h"

namespace rt {

SequenceClip::SequenceClip()
    : Clip(ClipType::Sequence)
{
    setLabel("Nested Sequence");
    // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> SequenceClip::clone() const
{
    auto copy = std::make_unique<SequenceClip>();

    // Base Clip fields
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
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
    copy->m_groupId    = m_groupId;
    copy->m_syncLine   = m_syncLine;
    copy->m_linkId     = m_linkId;
    copy->m_shotName   = m_shotName;
    copy->m_layerId    = m_layerId;
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

    // SequenceClip fields
    copy->m_sequenceIndex = m_sequenceIndex;
    copy->m_sequenceName  = m_sequenceName;

    return copy;
}

} // namespace rt
