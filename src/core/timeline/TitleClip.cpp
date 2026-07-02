/*
 * TitleClip.cpp — Title/text clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/TitleClip.h"

namespace rt {

TitleClip::TitleClip()
    : Clip(ClipType::Title)
{
    m_label = "Title";
    // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> TitleClip::clone() const
{
    auto copy = std::make_unique<TitleClip>();

    // Base clip properties
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
    copy->m_speedRamp  = m_speedRamp;
    copy->m_blendMode  = m_blendMode;
    copy->m_opacity    = m_opacity;
    copy->m_posX       = m_posX;
    copy->m_posY       = m_posY;
    copy->m_scaleX     = m_scaleX;
    copy->m_scaleY     = m_scaleY;
    copy->m_rotation   = m_rotation;
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

    // TitleClip-specific
    copy->m_text         = m_text;
    copy->m_fontFamily   = m_fontFamily;
    copy->m_fontSize     = m_fontSize;
    copy->m_bold         = m_bold;
    copy->m_italic       = m_italic;
    copy->m_align        = m_align;
    copy->m_valign       = m_valign;
    copy->m_textColor    = m_textColor;
    copy->m_bgColor      = m_bgColor;
    copy->m_outlineColor = m_outlineColor;
    copy->m_outlineWidth = m_outlineWidth;
    copy->m_tracking     = m_tracking;
    copy->m_lineHeight   = m_lineHeight;

    return copy;
}

} // namespace rt
