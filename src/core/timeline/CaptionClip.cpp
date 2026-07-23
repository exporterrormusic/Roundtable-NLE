/*
 * CaptionClip.cpp â€” Subtitle / closed-caption clip implementation.
 */

#include "timeline/CaptionClip.h"

#include <algorithm>

namespace rt {

CaptionClip::CaptionClip()
 : Clip(ClipType::Caption)
{
 m_label = "Caption";
 // Color left at sentinel — theme tint applies.
}

void CaptionClip::replaceTextPreservingStyles(const std::string& text)
{
 auto mapped = remapTextStyleRuns(m_text, text, m_styleRuns);
 auto mappedParagraphs = remapTextParagraphStyles(
     m_text, text, m_paragraphStyles);
 m_text = text;
 m_label = text.empty() ? "Caption" : text;
 m_styleRuns = std::move(mapped);
 m_paragraphStyles = std::move(mappedParagraphs);
}

CaptionStyle CaptionClip::captionStyle() const
{
 return {m_fontFamily, m_fontStyle, m_fontSize, m_textColor, m_bgColor,
         m_position, m_bold, m_outlineColor, m_outlineWidth, m_showSpeaker,
         m_italic, m_allCaps, m_smallCaps, m_underline, m_superscript,
         m_subscript, m_fauxBold, m_fauxItalic, m_tracking, m_leading,
         m_alignment, m_trackStyleName};
}

void CaptionClip::setCaptionStyle(const CaptionStyle& style)
{
 setFontFamily(style.fontFamily);
 setFontStyle(style.fontStyle);
 setFontSize(style.fontSize);
 setTextColor(style.textColor);
 setBgColor(style.bgColor);
 setPosition(style.position);
 setBold(style.bold);
 setOutlineColor(style.outlineColor);
 setOutlineWidth(style.outlineWidth);
 setShowSpeaker(style.showSpeaker);
 setItalic(style.italic);
 setAllCaps(style.allCaps);
 setSmallCaps(style.smallCaps);
 setUnderline(style.underline);
 setScript(style.superscript, style.subscript);
 setFauxStyles(style.fauxBold, style.fauxItalic);
 setTracking(style.tracking);
 setLeading(style.leading);
 setAlignment(style.alignment);
 setTrackStyleName(style.name);
}

void CaptionClip::setStyleRuns(std::vector<TextStyleRun> runs)
{
 runs.erase(std::remove_if(runs.begin(), runs.end(),
                           [](const TextStyleRun& run) {
                               return run.length == 0;
                           }),
            runs.end());
 std::stable_sort(runs.begin(), runs.end(),
                  [](const TextStyleRun& a, const TextStyleRun& b) {
                      return a.start < b.start;
                  });
 std::vector<TextStyleRun> normalized;
 for (auto run : runs) {
     if (!normalized.empty()) {
         const uint64_t previousEnd =
             static_cast<uint64_t>(normalized.back().start)
             + normalized.back().length;
         if (run.start < previousEnd) {
             const uint64_t overlap = previousEnd - run.start;
             if (overlap >= run.length) continue;
             run.start = static_cast<uint32_t>(previousEnd);
             run.length -= static_cast<uint32_t>(overlap);
         }
         const auto& previous = normalized.back();
         const bool same = previous.fontFamily == run.fontFamily
             && previous.fontStyle == run.fontStyle
             && previous.fontSize == run.fontSize
             && previous.fontWeight == run.fontWeight
             && previous.italic == run.italic
             && previous.allCaps == run.allCaps
             && previous.smallCaps == run.smallCaps
             && previous.tracking == run.tracking
             && previous.baselineShift == run.baselineShift
             && previous.leading == run.leading
             && previous.kerning == run.kerning
             && previous.tabWidth == run.tabWidth
             && previous.tsume == run.tsume
             && previous.fauxBold == run.fauxBold
             && previous.fauxItalic == run.fauxItalic
             && previous.underline == run.underline
             && previous.superscript == run.superscript
             && previous.subscript == run.subscript
             && previous.appearance == run.appearance
             && previous.overrideMask == run.overrideMask;
         if (same && previousEnd == run.start) {
             normalized.back().length += run.length;
             continue;
         }
     }
     normalized.push_back(std::move(run));
 }
 m_styleRuns = std::move(normalized);
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
 copy->m_timeInterpolation = m_timeInterpolation;
 copy->m_speedRamp = m_speedRamp;
 copy->m_opacity = m_opacity;
 copy->m_posX = m_posX;
 copy->m_posY = m_posY;
 copy->m_scaleX = m_scaleX;
 copy->m_scaleY = m_scaleY;
 copy->m_rotation = m_rotation;
 copy->m_shutterAngle = m_shutterAngle;
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
 copy->m_fontStyle = m_fontStyle;
 copy->m_fontSize = m_fontSize;
 copy->m_textColor = m_textColor;
 copy->m_bgColor = m_bgColor;
 copy->m_position = m_position;
 copy->m_bold = m_bold;
 copy->m_outlineColor = m_outlineColor;
 copy->m_outlineWidth = m_outlineWidth;
 copy->m_showSpeaker = m_showSpeaker;
 copy->m_italic = m_italic;
 copy->m_allCaps = m_allCaps;
 copy->m_smallCaps = m_smallCaps;
 copy->m_underline = m_underline;
 copy->m_superscript = m_superscript;
 copy->m_subscript = m_subscript;
 copy->m_fauxBold = m_fauxBold;
 copy->m_fauxItalic = m_fauxItalic;
 copy->m_tracking = m_tracking;
 copy->m_leading = m_leading;
 copy->m_alignment = m_alignment;
 copy->m_trackStyleName = m_trackStyleName;
 copy->m_styleRuns = m_styleRuns;
 copy->m_paragraphStyles = m_paragraphStyles;

 return copy;
}

} // namespace rt
