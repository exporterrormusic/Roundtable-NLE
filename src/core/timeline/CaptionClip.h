/*
 * CaptionClip â€” a subtitle / closed-caption entry on the timeline.
 *
 * Lives on a TrackType::Caption track. Each clip represents one
 * subtitle cue with text, optional speaker label, and style overrides.
 */

#pragma once

#include "timeline/Clip.h"
#include <string>

namespace rt {

/// Vertical position preset for captions.
enum class CaptionPosition : uint8_t
{
 Bottom, // Default subtitle position (lower third)
 Top,
 Middle
};

class CaptionClip : public Clip
{
public:
 CaptionClip();
 ~CaptionClip() override = default;

 // â”€â”€ Text â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] const std::string& text() const noexcept { return m_text; }
 [[nodiscard]] const std::string& speaker() const noexcept { return m_speaker; }
 // Keep the base-clip label in sync with the caption text so the timeline
 // clip widget shows the caption content directly.
 void setText(const std::string& t) { m_text = t; m_label = t.empty() ? "Caption" : t; }
 void setSpeaker(const std::string& s) { m_speaker = s; }

 // â”€â”€ Style â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] const std::string& fontFamily() const noexcept { return m_fontFamily; }
 [[nodiscard]] float fontSize() const noexcept { return m_fontSize; }
 [[nodiscard]] uint32_t textColor() const noexcept { return m_textColor; }
 [[nodiscard]] uint32_t bgColor() const noexcept { return m_bgColor; }
 [[nodiscard]] CaptionPosition position() const noexcept { return m_position; }
 [[nodiscard]] bool isBold() const noexcept { return m_bold; }
 [[nodiscard]] uint32_t outlineColor() const noexcept { return m_outlineColor; }
 [[nodiscard]] float outlineWidth() const noexcept { return m_outlineWidth; }
 [[nodiscard]] bool showSpeaker() const noexcept { return m_showSpeaker; }

 void setFontFamily(const std::string& f) { m_fontFamily = f; }
 void setFontSize(float s) noexcept { m_fontSize = s; }
 void setTextColor(uint32_t c) noexcept { m_textColor = c; }
 void setBgColor(uint32_t c) noexcept { m_bgColor = c; }
 void setPosition(CaptionPosition p) noexcept { m_position = p; }
 void setBold(bool b) noexcept { m_bold = b; }
 void setOutlineColor(uint32_t c) noexcept { m_outlineColor = c; }
 void setOutlineWidth(float w) noexcept { m_outlineWidth = w; }
 void setShowSpeaker(bool v) noexcept { m_showSpeaker = v; }

 // â”€â”€ Clone â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
 std::string m_text;
 std::string m_speaker;
 std::string m_fontFamily{"Arial"};
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
};

} // namespace rt
