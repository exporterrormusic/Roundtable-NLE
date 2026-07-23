/*
 * GraphicLayer.h — Internal layers for a GraphicClip container.
 *
 * A GraphicClip holds a stack of GraphicLayers rendered bottom-to-top.
 * Each layer has its own transform, appearance, and type-specific data.
 *
 * Layer types:
 *   - TextLayer:  Rich text with font, tracking, outline, shadow
 *   - ShapeLayer: Rectangle, ellipse with fill/stroke
 *
 * Modelled after Adobe Premiere Pro 2024-2026 Essential Graphics.
 */

#pragma once

#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Enums
// ═════════════════════════════════════════════════════════════════════════════

enum class GraphicLayerType : uint8_t
{
    Text,
    Shape
};

/// Stroke position relative to the shape/text edge.
enum class StrokePosition : uint8_t
{
    Center,
    Inner,
    Outer
};

/// Shape type for ShapeLayers.
enum class ShapeType : uint8_t
{
    Rectangle,
    Ellipse,
    RoundedRect
};

/// Text alignment (reused from TitleClip but duplicated here for decoupling).
enum class GTextAlign : uint8_t
{
    Left,
    Center,
    Right,
    Justify
};

enum class GTextVAlign : uint8_t
{
    Top,
    Middle,
    Bottom
};

/// Appearance attached to one character range. A run only uses a component
/// when the corresponding TextStyleOverride bit is set; otherwise it follows
/// the text layer/caption defaults.
struct TextRunAppearance
{
    bool           fillEnabled{true};
    uint32_t       fillColor{0xFFFFFFFF};
    bool           strokeEnabled{false};
    uint32_t       strokeColor{0xFF000000};
    float          strokeWidth{2.0f};
    StrokePosition strokePosition{StrokePosition::Center};
    bool           shadowEnabled{false};
    uint32_t       shadowColor{0x80000000};
    float          shadowDistance{4.0f};
    float          shadowAngle{135.0f};
    float          shadowSoftness{4.0f};
    float          shadowOpacity{0.6f};
    bool           backgroundEnabled{false};
    uint32_t       backgroundColor{0x00000000};
    float          backgroundPadding{4.0f};

    bool operator==(const TextRunAppearance&) const = default;
};

/// Character formatting for a UTF-16 range in a TextLayer.  Qt's text
/// editor/layout APIs use UTF-16 cursor positions, so keeping the same unit in
/// the model avoids corrupting ranges around emoji and other surrogate pairs.
/// Font fields contain resolved values. Extended character fields use explicit
/// override bits so unmodified text can keep following animated layer values.
struct TextStyleRun
{
    uint32_t    start{0};
    uint32_t    length{0};
    std::string fontFamily{"Arial"};
    std::string fontStyle;
    float       fontSize{72.0f};
    int         fontWeight{400};
    bool        italic{false};
    bool        allCaps{false};
    bool        smallCaps{false};
    float       tracking{0.0f};
    float       baselineShift{0.0f};
    float       leading{0.0f};
    float       kerning{0.0f};
    float       tabWidth{48.0f};
    float       tsume{0.0f};
    bool        fauxBold{false};
    bool        fauxItalic{false};
    bool        underline{false};
    bool        superscript{false};
    bool        subscript{false};
    TextRunAppearance appearance;
    /// Extended properties only override the layer when their bit is set.
    /// This lets ordinary font-only runs continue following keyframed layer
    /// tracking/baseline values.
    uint32_t    overrideMask{0};

    bool operator==(const TextStyleRun&) const = default;
};

enum TextStyleOverride : uint32_t
{
    TextOverrideCapitalization = 1u << 0,
    TextOverrideTracking       = 1u << 1,
    TextOverrideBaseline       = 1u << 2,
    TextOverrideLeading        = 1u << 3,
    TextOverrideKerning        = 1u << 4,
    TextOverrideDecoration     = 1u << 5,
    TextOverrideScript         = 1u << 6,
    TextOverrideTabWidth       = 1u << 7,
    TextOverrideTsume          = 1u << 8,
    TextOverrideFill           = 1u << 9,
    TextOverrideStroke         = 1u << 10,
    TextOverrideShadow         = 1u << 11,
    TextOverrideBackground     = 1u << 12,
    TextOverrideFontStyle      = 1u << 13,
    TextOverrideFauxStyle      = 1u << 14
};

/// Block/paragraph formatting uses UTF-16 ranges for the same reason as
/// TextStyleRun. Newline separators belong to the preceding paragraph.
struct TextParagraphStyle
{
    uint32_t   start{0};
    uint32_t   length{0};
    GTextAlign alignment{GTextAlign::Center};
    bool       rightToLeft{false};

    bool operator==(const TextParagraphStyle&) const = default;
};

/// Re-map UTF-16 character-format ranges after a plain-text edit.  The
/// unchanged prefix/suffix retain their formatting and inserted text inherits
/// the format at the edit point, matching a normal rich-text editor.
[[nodiscard]] std::vector<TextStyleRun> remapTextStyleRuns(
    const std::string& oldText,
    const std::string& newText,
    const std::vector<TextStyleRun>& runs);

[[nodiscard]] std::vector<TextParagraphStyle> remapTextParagraphStyles(
    const std::string& oldText,
    const std::string& newText,
    const std::vector<TextParagraphStyle>& styles);

// ═════════════════════════════════════════════════════════════════════════════
//  Appearance sub-objects (stackable fills, strokes, shadows)
// ═════════════════════════════════════════════════════════════════════════════

struct FillEntry
{
    uint32_t color{0xFFFFFFFF};   // ARGB
    float    opacity{1.0f};
    bool     enabled{true};
};

struct StrokeEntry
{
    uint32_t       color{0xFF000000};
    float          width{2.0f};
    StrokePosition position{StrokePosition::Center};
    float          opacity{1.0f};
    bool           enabled{true};
};

struct ShadowEntry
{
    uint32_t color{0x80000000};
    float    distance{4.0f};
    float    angle{135.0f};      // degrees, 0 = right
    float    softness{4.0f};     // blur radius
    float    opacity{0.6f};
    bool     enabled{true};
};

/// Stacked appearance model (Premiere Pro Essential Graphics style).
struct Appearance
{
    std::vector<FillEntry>   fills;
    std::vector<StrokeEntry> strokes;
    std::vector<ShadowEntry> shadows;
};

// ═════════════════════════════════════════════════════════════════════════════
//  Layer transform (per-layer, separate from clip transform)
// ═════════════════════════════════════════════════════════════════════════════

struct LayerTransform
{
    KeyframeTrack<float> posX{0.0f};
    KeyframeTrack<float> posY{0.0f};
    KeyframeTrack<float> scaleX{1.0f};
    KeyframeTrack<float> scaleY{1.0f};
    KeyframeTrack<float> rotation{0.0f};
    KeyframeTrack<float> anchorX{0.0f};
    KeyframeTrack<float> anchorY{0.0f};
    KeyframeTrack<float> opacity{1.0f};
};

// ═════════════════════════════════════════════════════════════════════════════
//  GraphicLayer — base class
// ═════════════════════════════════════════════════════════════════════════════

class GraphicLayer
{
public:
    explicit GraphicLayer(GraphicLayerType type);
    virtual ~GraphicLayer();

    GraphicLayer(const GraphicLayer&) = delete;
    GraphicLayer& operator=(const GraphicLayer&) = delete;

    [[nodiscard]] GraphicLayerType layerType() const noexcept { return m_type; }
    [[nodiscard]] uint64_t         layerId()   const noexcept { return m_id; }
    [[nodiscard]] const std::string& name()    const noexcept { return m_name; }
    [[nodiscard]] bool             isVisible() const noexcept { return m_visible; }
    [[nodiscard]] bool             isLocked()  const noexcept { return m_locked; }

    void setName(const std::string& name) { m_name = name; }
    void setVisible(bool v) noexcept { m_visible = v; }
    void setLocked(bool v)  noexcept { m_locked = v; }

    LayerTransform&       transform()       noexcept { return m_transform; }
    const LayerTransform& transform() const noexcept { return m_transform; }

    Appearance&       appearance()       noexcept { return m_appearance; }
    const Appearance& appearance() const noexcept { return m_appearance; }

    [[nodiscard]] virtual std::unique_ptr<GraphicLayer> clone() const = 0;

    /// Copy all editable state (transform, appearance, type-specific fields)
    /// from another layer of the same type, WITHOUT changing this layer's id
    /// or type. Used by undo/redo to restore a snapshot in place so the layer's
    /// stable id (and the panel's selection) survives the round-trip.
    virtual void assignStateFrom(const GraphicLayer& other);

protected:
    GraphicLayerType  m_type;
    uint64_t          m_id;
    std::string       m_name;
    bool              m_visible{true};
    bool              m_locked{false};
    LayerTransform    m_transform;
    Appearance        m_appearance;

    static std::atomic<uint64_t> s_nextLayerId;
};

// ═════════════════════════════════════════════════════════════════════════════
//  TextLayer
// ═════════════════════════════════════════════════════════════════════════════

class TextLayer : public GraphicLayer
{
public:
    TextLayer();
    ~TextLayer() override = default;

    // ── Text content ────────────────────────────────────────────────
    [[nodiscard]] const std::string& text()       const noexcept { return m_text; }
    [[nodiscard]] const std::string& fontFamily() const noexcept { return m_fontFamily; }
    [[nodiscard]] const std::string& fontStyle() const noexcept { return m_fontStyle; }
    [[nodiscard]] float              fontSize()   const noexcept { return m_fontSize; }
    [[nodiscard]] int                fontWeight()  const noexcept { return m_fontWeight; }   // 100-900
    [[nodiscard]] bool               isItalic()   const noexcept { return m_italic; }
    [[nodiscard]] bool               allCaps()    const noexcept { return m_allCaps; }
    [[nodiscard]] bool               smallCaps()  const noexcept { return m_smallCaps; }
    [[nodiscard]] GTextAlign         alignment()  const noexcept { return m_align; }
    [[nodiscard]] GTextVAlign        vAlignment() const noexcept { return m_valign; }
    [[nodiscard]] float kerning() const noexcept { return m_kerning; }
    [[nodiscard]] float tabWidth() const noexcept { return m_tabWidth; }
    [[nodiscard]] float tsume() const noexcept { return m_tsume; }
    [[nodiscard]] bool fauxBold() const noexcept { return m_fauxBold; }
    [[nodiscard]] bool fauxItalic() const noexcept { return m_fauxItalic; }
    [[nodiscard]] bool underline() const noexcept { return m_underline; }
    [[nodiscard]] bool superscript() const noexcept { return m_superscript; }
    [[nodiscard]] bool subscript() const noexcept { return m_subscript; }
    [[nodiscard]] bool rightToLeft() const noexcept { return m_rightToLeft; }
    [[nodiscard]] bool backgroundEnabled() const noexcept { return m_backgroundEnabled; }
    [[nodiscard]] uint32_t backgroundColor() const noexcept { return m_backgroundColor; }
    [[nodiscard]] float backgroundPadding() const noexcept { return m_backgroundPadding; }
    [[nodiscard]] bool maskWithText() const noexcept { return m_maskWithText; }
    [[nodiscard]] const std::string& linkedStyleName() const noexcept {
        return m_linkedStyleName;
    }

    void setText(const std::string& t) {
        if (m_text != t) {
            m_text = t;
            m_styleRuns.clear();
            m_paragraphStyles.clear();
        }
    }
    void replaceTextPreservingStyles(const std::string& t);
    void setFontFamily(const std::string& f) {
        m_fontFamily = f;
        for (auto& run : m_styleRuns) run.fontFamily = f;
    }
    void setFontStyleForAll(const std::string& style) {
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
    void setFontWeight(int w) noexcept {
        m_fontWeight = w;
        for (auto& run : m_styleRuns) run.fontWeight = w;
    }
    void setItalic(bool v) noexcept {
        m_italic = v;
        for (auto& run : m_styleRuns) run.italic = v;
    }
    void setAllCaps(bool v) noexcept {
        m_allCaps = v;
        for (auto& run : m_styleRuns) {
            run.allCaps = v;
            run.smallCaps = false;
            run.overrideMask &= ~TextOverrideCapitalization;
        }
    }
    void setSmallCaps(bool v) noexcept {
        m_smallCaps = v;
        for (auto& run : m_styleRuns) {
            run.smallCaps = v;
            run.allCaps = false;
            run.overrideMask &= ~TextOverrideCapitalization;
        }
    }
    void setAlignment(GTextAlign a) noexcept {
        m_align = a;
        m_paragraphStyles.clear();
    }
    void setVAlignment(GTextVAlign a) noexcept { m_valign = a; }
    void setKerningForAll(float value) noexcept {
        m_kerning = value;
        for (auto& run : m_styleRuns) {
            run.kerning = value;
            run.overrideMask &= ~TextOverrideKerning;
        }
    }
    void setTabWidthForAll(float value) noexcept {
        m_tabWidth = value;
        for (auto& run : m_styleRuns) {
            run.tabWidth = value;
            run.overrideMask &= ~TextOverrideTabWidth;
        }
    }
    void setTsumeForAll(float value) noexcept {
        m_tsume = value;
        for (auto& run : m_styleRuns) {
            run.tsume = value;
            run.overrideMask &= ~TextOverrideTsume;
        }
    }
    void setFauxStylesForAll(bool bold, bool italic) noexcept {
        m_fauxBold = bold;
        m_fauxItalic = italic;
        for (auto& run : m_styleRuns) {
            run.fauxBold = bold;
            run.fauxItalic = italic;
            run.overrideMask &= ~TextOverrideFauxStyle;
        }
    }
    void setUnderlineForAll(bool value) noexcept {
        m_underline = value;
        for (auto& run : m_styleRuns) {
            run.underline = value;
            run.overrideMask &= ~TextOverrideDecoration;
        }
    }
    void setScriptForAll(bool super, bool sub) noexcept {
        m_superscript = super;
        m_subscript = sub;
        for (auto& run : m_styleRuns) {
            run.superscript = super;
            run.subscript = sub;
            run.overrideMask &= ~TextOverrideScript;
        }
    }
    void setRightToLeft(bool value) noexcept {
        m_rightToLeft = value;
        m_paragraphStyles.clear();
    }
    void setBackgroundForAll(bool enabled, uint32_t color, float padding);
    void setFillForAll(bool enabled, uint32_t color);
    void setStrokeForAll(bool enabled, uint32_t color, float width,
                         StrokePosition position);
    void setShadowForAll(bool enabled, uint32_t color, float distance = 4.0f,
                         float angle = 135.0f, float softness = 4.0f,
                         float opacity = 0.6f);
    void setMaskWithText(bool value) noexcept { m_maskWithText = value; }
    void setLinkedStyleName(std::string name) {
        m_linkedStyleName = std::move(name);
    }

    /// Per-character formatting. Empty means the layer-wide defaults apply to
    /// the whole string. Positions and lengths are UTF-16 code units.
    [[nodiscard]] const std::vector<TextStyleRun>& styleRuns() const noexcept {
        return m_styleRuns;
    }
    void setStyleRuns(std::vector<TextStyleRun> runs);
    void clearStyleRuns() noexcept { m_styleRuns.clear(); }
    [[nodiscard]] const std::vector<TextParagraphStyle>& paragraphStyles()
        const noexcept { return m_paragraphStyles; }
    void setParagraphStyles(std::vector<TextParagraphStyle> styles);
    void clearParagraphStyles() noexcept { m_paragraphStyles.clear(); }

    // ── Keyframeable text properties ────────────────────────────────
    KeyframeTrack<float>& tracking()      noexcept { return m_tracking; }
    KeyframeTrack<float>& leading()       noexcept { return m_leading; }
    KeyframeTrack<float>& baselineShift() noexcept { return m_baselineShift; }

    const KeyframeTrack<float>& tracking()      const noexcept { return m_tracking; }
    const KeyframeTrack<float>& leading()       const noexcept { return m_leading; }
    const KeyframeTrack<float>& baselineShift() const noexcept { return m_baselineShift; }
    void setTrackingForAll(float value) {
        m_tracking.addKeyframe(0, value);
        for (auto& run : m_styleRuns) {
            run.tracking = value;
            run.overrideMask &= ~TextOverrideTracking;
        }
    }
    void setBaselineShiftForAll(float value) {
        m_baselineShift.addKeyframe(0, value);
        for (auto& run : m_styleRuns) {
            run.baselineShift = value;
            run.overrideMask &= ~TextOverrideBaseline;
        }
    }
    void setLeadingForAll(float value) {
        m_leading.addKeyframe(0, value);
        for (auto& run : m_styleRuns) {
            run.leading = value;
            run.overrideMask &= ~TextOverrideLeading;
        }
    }

    // ── Text box (paragraph mode) ───────────────────────────────────
    [[nodiscard]] float boxWidth()  const noexcept { return m_boxWidth; }
    [[nodiscard]] float boxHeight() const noexcept { return m_boxHeight; }
    [[nodiscard]] bool  useParagraphBox() const noexcept { return m_useParagraphBox; }

    void setBoxWidth(float w)  noexcept { m_boxWidth = w; }
    void setBoxHeight(float h) noexcept { m_boxHeight = h; }
    void setUseParagraphBox(bool v) noexcept { m_useParagraphBox = v; }

    [[nodiscard]] std::unique_ptr<GraphicLayer> clone() const override;
    void assignStateFrom(const GraphicLayer& other) override;

private:
    std::string    m_text{"Title"};
    std::string    m_fontFamily{"Arial"};
    std::string    m_fontStyle;
    float          m_fontSize{72.0f};
    int            m_fontWeight{400};      // 400 = normal, 700 = bold
    bool           m_italic{false};
    bool           m_allCaps{false};
    bool           m_smallCaps{false};
    GTextAlign     m_align{GTextAlign::Center};
    GTextVAlign    m_valign{GTextVAlign::Middle};
    std::vector<TextStyleRun> m_styleRuns;
    std::vector<TextParagraphStyle> m_paragraphStyles;

    float m_kerning{0.0f};
    float m_tabWidth{48.0f};
    float m_tsume{0.0f};
    bool m_fauxBold{false};
    bool m_fauxItalic{false};
    bool m_underline{false};
    bool m_superscript{false};
    bool m_subscript{false};
    bool m_rightToLeft{false};
    bool m_backgroundEnabled{false};
    uint32_t m_backgroundColor{0x00000000};
    float m_backgroundPadding{4.0f};
    bool m_maskWithText{false};
    std::string m_linkedStyleName;

    KeyframeTrack<float> m_tracking{0.0f};
    KeyframeTrack<float> m_leading{1.2f};       // line-height multiplier
    KeyframeTrack<float> m_baselineShift{0.0f};

    // Paragraph text box (0 = auto-size / point text)
    float m_boxWidth{0.0f};
    float m_boxHeight{0.0f};
    bool  m_useParagraphBox{false};
};

// ═════════════════════════════════════════════════════════════════════════════
//  ShapeLayer
// ═════════════════════════════════════════════════════════════════════════════

class ShapeLayer : public GraphicLayer
{
public:
    ShapeLayer();
    ~ShapeLayer() override = default;

    [[nodiscard]] ShapeType shapeType()    const noexcept { return m_shape; }
    [[nodiscard]] float     shapeWidth()   const noexcept { return m_width; }
    [[nodiscard]] float     shapeHeight()  const noexcept { return m_height; }
    [[nodiscard]] float     cornerRadius() const noexcept { return m_cornerRadius; }
    [[nodiscard]] uint32_t  fillColor()    const noexcept { return m_fillColor; }

    void setShapeType(ShapeType t) noexcept   { m_shape = t; }
    void setShapeWidth(float w)    noexcept   { m_width = w; }
    void setShapeHeight(float h)   noexcept   { m_height = h; }
    void setCornerRadius(float r)  noexcept   { m_cornerRadius = r; }
    void setFillColor(uint32_t c)  noexcept   { m_fillColor = c; }

    [[nodiscard]] std::unique_ptr<GraphicLayer> clone() const override;
    void assignStateFrom(const GraphicLayer& other) override;

private:
    ShapeType m_shape{ShapeType::Rectangle};
    float     m_width{200.0f};
    float     m_height{100.0f};
    float     m_cornerRadius{0.0f};
    uint32_t  m_fillColor{0xFF333333};
};

} // namespace rt
