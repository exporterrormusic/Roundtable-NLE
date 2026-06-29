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
    [[nodiscard]] float              fontSize()   const noexcept { return m_fontSize; }
    [[nodiscard]] int                fontWeight()  const noexcept { return m_fontWeight; }   // 100-900
    [[nodiscard]] bool               isItalic()   const noexcept { return m_italic; }
    [[nodiscard]] bool               allCaps()    const noexcept { return m_allCaps; }
    [[nodiscard]] bool               smallCaps()  const noexcept { return m_smallCaps; }
    [[nodiscard]] GTextAlign         alignment()  const noexcept { return m_align; }
    [[nodiscard]] GTextVAlign        vAlignment() const noexcept { return m_valign; }

    void setText(const std::string& t)      { m_text = t; }
    void setFontFamily(const std::string& f) { m_fontFamily = f; }
    void setFontSize(float s) noexcept      { m_fontSize = s; }
    void setFontWeight(int w) noexcept      { m_fontWeight = w; }
    void setItalic(bool v) noexcept         { m_italic = v; }
    void setAllCaps(bool v) noexcept        { m_allCaps = v; }
    void setSmallCaps(bool v) noexcept      { m_smallCaps = v; }
    void setAlignment(GTextAlign a) noexcept   { m_align = a; }
    void setVAlignment(GTextVAlign a) noexcept { m_valign = a; }

    // ── Keyframeable text properties ────────────────────────────────
    KeyframeTrack<float>& tracking()      noexcept { return m_tracking; }
    KeyframeTrack<float>& leading()       noexcept { return m_leading; }
    KeyframeTrack<float>& baselineShift() noexcept { return m_baselineShift; }

    const KeyframeTrack<float>& tracking()      const noexcept { return m_tracking; }
    const KeyframeTrack<float>& leading()       const noexcept { return m_leading; }
    const KeyframeTrack<float>& baselineShift() const noexcept { return m_baselineShift; }

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
    float          m_fontSize{72.0f};
    int            m_fontWeight{400};      // 400 = normal, 700 = bold
    bool           m_italic{false};
    bool           m_allCaps{false};
    bool           m_smallCaps{false};
    GTextAlign     m_align{GTextAlign::Center};
    GTextVAlign    m_valign{GTextVAlign::Middle};

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
