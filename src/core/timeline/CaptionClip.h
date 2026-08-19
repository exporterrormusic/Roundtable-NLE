/*
 * CaptionClip â€” a subtitle / closed-caption entry on the timeline.
 *
 * Lives on a TrackType::Caption track. Each clip represents one
 * subtitle cue with text, optional speaker label, and style overrides.
 */

#pragma once

#include "timeline/Clip.h"
#include "timeline/GraphicLayer.h"
#include <string>
#include <utility>

namespace rt {

/// Vertical position preset for captions.
enum class CaptionPosition : uint8_t
{
 Bottom, // Default subtitle position (lower third)
 Top,
 Middle
};

/// The reusable, track-level part of a caption's formatting. Character and
/// paragraph ranges remain local overrides because their offsets are tied to
/// one caption's text.
struct CaptionStyle {
 std::string fontFamily{"Arial"};
 std::string fontStyle;
 float fontSize{32.0f};
 uint32_t textColor{0xFFFFFFFFu};
 uint32_t bgColor{0xCC000000u};
 CaptionPosition position{CaptionPosition::Bottom};
 bool bold{true};
 uint32_t outlineColor{0xFF000000u};
 float outlineWidth{0.0f};
 bool showSpeaker{false};
 bool italic{false};
 bool allCaps{false};
 bool smallCaps{false};
 bool underline{false};
 bool superscript{false};
 bool subscript{false};
 bool fauxBold{false};
 bool fauxItalic{false};
 float tracking{0.0f};
 float leading{0.0f};
 GTextAlign alignment{GTextAlign::Center};
 std::string name;

 bool operator==(const CaptionStyle&) const = default;
};

class CaptionClip : public Clip
{
public:
 CaptionClip();
 ~CaptionClip() override = default;

 // â”€â”€ Text â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] const std::string& text() const noexcept { return m_text; }
 [[nodiscard]] const std::string& speaker() const noexcept { return m_speaker; }
 [[nodiscard]] float confidence() const noexcept { return m_confidence; }
 // Keep the base-clip label in sync with the caption text so the timeline
 // clip widget shows the caption content directly.
 void setText(const std::string& t) {
     if (m_text != t) {
         m_text = t;
         m_styleRuns.clear();
         m_paragraphStyles.clear();
     }
     m_label = t.empty() ? "Caption" : t;
 }
 void replaceTextPreservingStyles(const std::string& t);
 void setSpeaker(const std::string& s) { m_speaker = s; }
 void setConfidence(float value) noexcept { m_confidence = value; }

 // â”€â”€ Style â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] const std::string& fontFamily() const noexcept { return m_fontFamily; }
 [[nodiscard]] const std::string& fontStyle() const noexcept { return m_fontStyle; }
 [[nodiscard]] float fontSize() const noexcept { return m_fontSize; }
 [[nodiscard]] uint32_t textColor() const noexcept { return m_textColor; }
 [[nodiscard]] uint32_t bgColor() const noexcept { return m_bgColor; }
 [[nodiscard]] CaptionPosition position() const noexcept { return m_position; }
 [[nodiscard]] bool isBold() const noexcept { return m_bold; }
 [[nodiscard]] uint32_t outlineColor() const noexcept { return m_outlineColor; }
 [[nodiscard]] float outlineWidth() const noexcept { return m_outlineWidth; }
 [[nodiscard]] bool showSpeaker() const noexcept { return m_showSpeaker; }
 [[nodiscard]] bool isItalic() const noexcept { return m_italic; }
 [[nodiscard]] bool allCaps() const noexcept { return m_allCaps; }
 [[nodiscard]] bool smallCaps() const noexcept { return m_smallCaps; }
 [[nodiscard]] bool underline() const noexcept { return m_underline; }
 [[nodiscard]] bool superscript() const noexcept { return m_superscript; }
 [[nodiscard]] bool subscript() const noexcept { return m_subscript; }
 [[nodiscard]] bool fauxBold() const noexcept { return m_fauxBold; }
 [[nodiscard]] bool fauxItalic() const noexcept { return m_fauxItalic; }
 [[nodiscard]] float tracking() const noexcept { return m_tracking; }
 [[nodiscard]] float leading() const noexcept { return m_leading; }
 [[nodiscard]] GTextAlign alignment() const noexcept { return m_alignment; }
 [[nodiscard]] const std::string& trackStyleName() const noexcept {
     return m_trackStyleName;
 }
 [[nodiscard]] CaptionStyle captionStyle() const;
 void setCaptionStyle(const CaptionStyle& style);

 void setFontFamily(const std::string& f) {
     m_fontFamily = f;
     for (auto& run : m_styleRuns) run.fontFamily = f;
 }
 void setFontStyle(const std::string& style) {
     m_fontStyle = style;
     for (auto& run : m_styleRuns) {
         run.fontStyle = style;
         run.overrideMask &= ~TextOverrideFontStyle;
     }
 }
 void setFontSize(float s) noexcept {
     m_fontSize = s;
     for (auto& run : m_styleRuns) run.fontSize = s;
 }
 void setTextColor(uint32_t c) noexcept { m_textColor = c; }
 void setBgColor(uint32_t c) noexcept { m_bgColor = c; }
 void setPosition(CaptionPosition p) noexcept { m_position = p; }
 void setBold(bool b) noexcept {
     m_bold = b;
     for (auto& run : m_styleRuns) run.fontWeight = b ? 700 : 400;
 }
 void setItalic(bool value) noexcept {
     m_italic = value;
     for (auto& run : m_styleRuns) run.italic = value;
 }
 void setAllCaps(bool value) noexcept {
     m_allCaps = value;
     if (value) m_smallCaps = false;
     for (auto& run : m_styleRuns) {
         run.allCaps = value;
         if (value) run.smallCaps = false;
         run.overrideMask &= ~TextOverrideCapitalization;
     }
 }
 void setSmallCaps(bool value) noexcept {
     m_smallCaps = value;
     if (value) m_allCaps = false;
     for (auto& run : m_styleRuns) {
         run.smallCaps = value;
         if (value) run.allCaps = false;
         run.overrideMask &= ~TextOverrideCapitalization;
     }
 }
 void setUnderline(bool value) noexcept {
     m_underline = value;
     for (auto& run : m_styleRuns) {
         run.underline = value;
         run.overrideMask &= ~TextOverrideDecoration;
     }
 }
 void setScript(bool super, bool sub) noexcept {
     m_superscript = super; m_subscript = sub;
     for (auto& run : m_styleRuns) {
         run.superscript = super; run.subscript = sub;
         run.overrideMask &= ~TextOverrideScript;
     }
 }
 void setFauxStyles(bool bold, bool italic) noexcept {
     m_fauxBold = bold; m_fauxItalic = italic;
     for (auto& run : m_styleRuns) {
         run.fauxBold = bold; run.fauxItalic = italic;
         run.overrideMask &= ~TextOverrideFauxStyle;
     }
 }
 void setTracking(float value) noexcept {
     m_tracking = value;
     for (auto& run : m_styleRuns) {
         run.tracking = value;
         run.overrideMask &= ~TextOverrideTracking;
     }
 }
 void setLeading(float value) noexcept {
     m_leading = value;
     for (auto& run : m_styleRuns) {
         run.leading = value;
         run.overrideMask &= ~TextOverrideLeading;
     }
 }
 void setAlignment(GTextAlign value) noexcept {
     m_alignment = value;
     m_paragraphStyles.clear();
 }
 void setTrackStyleName(std::string name) { m_trackStyleName = std::move(name); }
 void setOutlineColor(uint32_t c) noexcept { m_outlineColor = c; }
 void setOutlineWidth(float w) noexcept { m_outlineWidth = w; }
 void setShowSpeaker(bool v) noexcept { m_showSpeaker = v; }

 [[nodiscard]] const std::vector<TextStyleRun>& styleRuns() const noexcept {
     return m_styleRuns;
 }
 void setStyleRuns(std::vector<TextStyleRun> runs);
 void clearStyleRuns() noexcept { m_styleRuns.clear(); }
 [[nodiscard]] const std::vector<TextParagraphStyle>& paragraphStyles()
     const noexcept { return m_paragraphStyles; }
 void setParagraphStyles(std::vector<TextParagraphStyle> styles) {
     m_paragraphStyles = std::move(styles);
 }

 // â”€â”€ Clone â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
 std::string m_text;
 std::string m_speaker;
 float m_confidence{1.0f};
 std::string m_fontFamily{"Arial"};
 std::string m_fontStyle;
 float m_fontSize{32.0f};
 uint32_t m_textColor{0xFFFFFFFF}; // White
 uint32_t m_bgColor{0xCC000000}; // Semi-transparent black
 CaptionPosition m_position{CaptionPosition::Bottom};
 // v31 style fields. Defaults preserve the pre-v31 rendered look exactly
 // (bold was hard-coded on, no outline, speaker never burned in).
 bool m_bold{true};
 uint32_t m_outlineColor{0xFF000000}; // Black
 float m_outlineWidth{0.0f}; // 0 = no outline
 bool m_showSpeaker{false};
 bool m_italic{false};
 bool m_allCaps{false};
 bool m_smallCaps{false};
 bool m_underline{false};
 bool m_superscript{false};
 bool m_subscript{false};
 bool m_fauxBold{false};
 bool m_fauxItalic{false};
 float m_tracking{0.0f};
 float m_leading{0.0f};
 GTextAlign m_alignment{GTextAlign::Center};
 std::string m_trackStyleName;
 std::vector<TextStyleRun> m_styleRuns;
 std::vector<TextParagraphStyle> m_paragraphStyles;
};

} // namespace rt
