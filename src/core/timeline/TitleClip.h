/*
 * TitleClip — a timeline clip for text overlays.
 *
 * Contains rich text content, font selection, and text-specific
 * animated properties (tracking, line height, etc.).
 * Rendering is handled by TextRenderer (gpu/).
 */

#pragma once

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"
#include <string>

namespace rt {

/// Text alignment
enum class TextAlign : uint8_t
{
    Left,
    Center,
    Right
};

/// Text vertical alignment
enum class TextVAlign : uint8_t
{
    Top,
    Middle,
    Bottom
};

class TitleClip : public Clip
{
public:
    TitleClip();
    ~TitleClip() override = default;

    // ── Text content ────────────────────────────────────────────────────
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }
    void setText(const std::string& text) { m_text = text; }

    // ── Font properties ─────────────────────────────────────────────────
    [[nodiscard]] const std::string& fontFamily() const noexcept { return m_fontFamily; }
    void setFontFamily(const std::string& family) { m_fontFamily = family; }

    [[nodiscard]] float fontSize() const noexcept { return m_fontSize; }
    void setFontSize(float s) noexcept { m_fontSize = s; }

    [[nodiscard]] bool isBold()   const noexcept { return m_bold; }
    [[nodiscard]] bool isItalic() const noexcept { return m_italic; }
    void setBold(bool v)   noexcept { m_bold = v; }
    void setItalic(bool v) noexcept { m_italic = v; }

    // ── Layout ──────────────────────────────────────────────────────────
    [[nodiscard]] TextAlign  alignment()  const noexcept { return m_align; }
    [[nodiscard]] TextVAlign verticalAlignment() const noexcept { return m_valign; }
    void setAlignment(TextAlign a)          noexcept { m_align = a; }
    void setVerticalAlignment(TextVAlign a) noexcept { m_valign = a; }

    // ── Color ───────────────────────────────────────────────────────────
    [[nodiscard]] uint32_t textColor()    const noexcept { return m_textColor; }
    [[nodiscard]] uint32_t bgColor()      const noexcept { return m_bgColor; }
    [[nodiscard]] uint32_t outlineColor() const noexcept { return m_outlineColor; }
    void setTextColor(uint32_t c)    noexcept { m_textColor = c; }
    void setBgColor(uint32_t c)      noexcept { m_bgColor = c; }
    void setOutlineColor(uint32_t c) noexcept { m_outlineColor = c; }

    [[nodiscard]] float outlineWidth() const noexcept { return m_outlineWidth; }
    void setOutlineWidth(float w) noexcept { m_outlineWidth = w; }

    // ── Keyframeable text properties ────────────────────────────────────
    KeyframeTrack<float>& tracking()   noexcept { return m_tracking; }
    KeyframeTrack<float>& lineHeight() noexcept { return m_lineHeight; }

    // ── Clone ───────────────────────────────────────────────────────────
protected:
    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;
    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    std::string m_text{"Title"};
    std::string m_fontFamily{"Arial"};
    float       m_fontSize{72.0f};
    bool        m_bold{false};
    bool        m_italic{false};
    TextAlign   m_align{TextAlign::Center};
    TextVAlign  m_valign{TextVAlign::Middle};
    uint32_t    m_textColor{0xFFFFFFFF};    // White
    uint32_t    m_bgColor{0x00000000};      // Transparent
    uint32_t    m_outlineColor{0xFF000000}; // Black
    float       m_outlineWidth{0.0f};

    KeyframeTrack<float> m_tracking{0.0f};   // Letter spacing
    KeyframeTrack<float> m_lineHeight{1.2f}; // Line height multiplier
};

} // namespace rt
