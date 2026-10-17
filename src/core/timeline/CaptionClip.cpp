/*
 * CaptionClip.cpp — Subtitle / closed-caption clip implementation.
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

std::unique_ptr<Clip> CaptionClip::createEmpty() const
{
 return std::make_unique<CaptionClip>();
}

void CaptionClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
 const auto& o = static_cast<const CaptionClip&>(src);
 // CaptionClip-specific
 m_text = o.m_text;
 m_speaker = o.m_speaker;
 m_confidence = o.m_confidence;
 m_fontFamily = o.m_fontFamily;
 m_fontStyle = o.m_fontStyle;
 m_fontSize = o.m_fontSize;
 m_textColor = o.m_textColor;
 m_bgColor = o.m_bgColor;
 m_position = o.m_position;
 m_bold = o.m_bold;
 m_outlineColor = o.m_outlineColor;
 m_outlineWidth = o.m_outlineWidth;
 m_showSpeaker = o.m_showSpeaker;
 m_italic = o.m_italic;
 m_allCaps = o.m_allCaps;
 m_smallCaps = o.m_smallCaps;
 m_underline = o.m_underline;
 m_superscript = o.m_superscript;
 m_subscript = o.m_subscript;
 m_fauxBold = o.m_fauxBold;
 m_fauxItalic = o.m_fauxItalic;
 m_tracking = o.m_tracking;
 m_leading = o.m_leading;
 m_alignment = o.m_alignment;
 m_trackStyleName = o.m_trackStyleName;
 m_styleRuns = o.m_styleRuns;
 m_paragraphStyles = o.m_paragraphStyles;
}

} // namespace rt
