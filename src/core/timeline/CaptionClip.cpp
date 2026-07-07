/*
 * CaptionClip.cpp â€” Subtitle / closed-caption clip implementation.
 */

#include "timeline/CaptionClip.h"

namespace rt {

CaptionClip::CaptionClip()
 : Clip(ClipType::Caption)
{
 m_label = "Caption";
 // Color left at sentinel — theme tint applies.
}

std::unique_ptr<Clip> CaptionClip::clone() const
{
 auto copy = std::make_unique<CaptionClip>();

 // Base clip properties
 copy->m_label = m_label;
 copy->m_color = m_color;
 copy->m_enabled = m_enabled;
 copy->m_timelineIn = m_timelineIn;
 copy->m_duration = m_duration;
 copy->m_sourceIn = m_sourceIn;
 copy->m_speed = m_speed;
 copy->m_speedRamp = m_speedRamp;
 copy->m_opacity = m_opacity;
 copy->m_posX = m_posX;
 copy->m_posY = m_posY;
 copy->m_scaleX = m_scaleX;
 copy->m_scaleY = m_scaleY;
 copy->m_rotation = m_rotation;
 copy->m_anchorX = m_anchorX;
 copy->m_anchorY = m_anchorY;

 // Shot group / layer metadata
 copy->m_groupId = m_groupId;
 copy->m_syncLine = m_syncLine;
 copy->m_linkId = m_linkId;
 copy->m_shotName = m_shotName;
 copy->m_layerId = m_layerId;

 // Effect stack
 if (!m_effects.isEmpty()) {
 auto clonedEffects = m_effects.clone();
 if (clonedEffects)
 copy->m_effects = std::move(*clonedEffects);
 }

 // CaptionClip-specific
 copy->m_text = m_text;
 copy->m_speaker = m_speaker;
 copy->m_fontFamily = m_fontFamily;
 copy->m_fontSize = m_fontSize;
 copy->m_textColor = m_textColor;
 copy->m_bgColor = m_bgColor;
 copy->m_position = m_position;
 copy->m_bold = m_bold;
 copy->m_outlineColor = m_outlineColor;
 copy->m_outlineWidth = m_outlineWidth;
 copy->m_showSpeaker = m_showSpeaker;

 return copy;
}

} // namespace rt
