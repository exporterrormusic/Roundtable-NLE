/*
 * GraphicLayer.cpp — GraphicLayer base + TextLayer + ShapeLayer implementation.
 */

#include "timeline/GraphicLayer.h"

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  GraphicLayer base
// ═════════════════════════════════════════════════════════════════════════════

std::atomic<uint64_t> GraphicLayer::s_nextLayerId{1};

GraphicLayer::GraphicLayer(GraphicLayerType type)
    : m_type(type)
    , m_id(s_nextLayerId.fetch_add(1, std::memory_order_relaxed))
    , m_name("Layer")
{
    // Default appearance: one white fill, no stroke, no shadow
    m_appearance.fills.push_back(FillEntry{0xFFFFFFFF, 1.0f, true});
}

GraphicLayer::~GraphicLayer() = default;

void GraphicLayer::assignStateFrom(const GraphicLayer& other)
{
    // Copies shared (non-identity) state. Subclasses chain to this then copy
    // their own fields. m_id and m_type are deliberately NOT copied.
    m_name       = other.m_name;
    m_visible    = other.m_visible;
    m_locked     = other.m_locked;
    m_transform  = other.m_transform;   // LayerTransform is copy-assignable
    m_appearance = other.m_appearance;
}

// ═════════════════════════════════════════════════════════════════════════════
//  TextLayer
// ═════════════════════════════════════════════════════════════════════════════

TextLayer::TextLayer()
    : GraphicLayer(GraphicLayerType::Text)
{
    m_name = "Text";
}

std::unique_ptr<GraphicLayer> TextLayer::clone() const
{
    auto copy = std::make_unique<TextLayer>();

    // Base
    copy->m_name    = m_name;
    copy->m_visible = m_visible;
    copy->m_locked  = m_locked;
    copy->m_transform.posX     = m_transform.posX;
    copy->m_transform.posY     = m_transform.posY;
    copy->m_transform.scaleX   = m_transform.scaleX;
    copy->m_transform.scaleY   = m_transform.scaleY;
    copy->m_transform.rotation = m_transform.rotation;
    copy->m_transform.anchorX  = m_transform.anchorX;
    copy->m_transform.anchorY  = m_transform.anchorY;
    copy->m_transform.opacity  = m_transform.opacity;
    copy->m_appearance = m_appearance;

    // Text-specific
    copy->m_text        = m_text;
    copy->m_fontFamily  = m_fontFamily;
    copy->m_fontSize    = m_fontSize;
    copy->m_fontWeight  = m_fontWeight;
    copy->m_italic      = m_italic;
    copy->m_allCaps     = m_allCaps;
    copy->m_smallCaps   = m_smallCaps;
    copy->m_align       = m_align;
    copy->m_valign      = m_valign;
    copy->m_tracking    = m_tracking;
    copy->m_leading     = m_leading;
    copy->m_baselineShift = m_baselineShift;
    copy->m_boxWidth    = m_boxWidth;
    copy->m_boxHeight   = m_boxHeight;
    copy->m_useParagraphBox = m_useParagraphBox;

    return copy;
}

void TextLayer::assignStateFrom(const GraphicLayer& other)
{
    GraphicLayer::assignStateFrom(other);
    const auto* o = dynamic_cast<const TextLayer*>(&other);
    if (!o) return;
    m_text            = o->m_text;
    m_fontFamily      = o->m_fontFamily;
    m_fontSize        = o->m_fontSize;
    m_fontWeight      = o->m_fontWeight;
    m_italic          = o->m_italic;
    m_allCaps         = o->m_allCaps;
    m_smallCaps       = o->m_smallCaps;
    m_align           = o->m_align;
    m_valign          = o->m_valign;
    m_tracking        = o->m_tracking;
    m_leading         = o->m_leading;
    m_baselineShift   = o->m_baselineShift;
    m_boxWidth        = o->m_boxWidth;
    m_boxHeight       = o->m_boxHeight;
    m_useParagraphBox = o->m_useParagraphBox;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ShapeLayer
// ═════════════════════════════════════════════════════════════════════════════

ShapeLayer::ShapeLayer()
    : GraphicLayer(GraphicLayerType::Shape)
{
    m_name = "Shape";
}

std::unique_ptr<GraphicLayer> ShapeLayer::clone() const
{
    auto copy = std::make_unique<ShapeLayer>();

    // Base
    copy->m_name    = m_name;
    copy->m_visible = m_visible;
    copy->m_locked  = m_locked;
    copy->m_transform.posX     = m_transform.posX;
    copy->m_transform.posY     = m_transform.posY;
    copy->m_transform.scaleX   = m_transform.scaleX;
    copy->m_transform.scaleY   = m_transform.scaleY;
    copy->m_transform.rotation = m_transform.rotation;
    copy->m_transform.anchorX  = m_transform.anchorX;
    copy->m_transform.anchorY  = m_transform.anchorY;
    copy->m_transform.opacity  = m_transform.opacity;
    copy->m_appearance = m_appearance;

    // Shape-specific
    copy->m_shape        = m_shape;
    copy->m_width        = m_width;
    copy->m_height       = m_height;
    copy->m_cornerRadius = m_cornerRadius;
    copy->m_fillColor    = m_fillColor;

    return copy;
}

void ShapeLayer::assignStateFrom(const GraphicLayer& other)
{
    GraphicLayer::assignStateFrom(other);
    const auto* o = dynamic_cast<const ShapeLayer*>(&other);
    if (!o) return;
    m_shape        = o->m_shape;
    m_width        = o->m_width;
    m_height       = o->m_height;
    m_cornerRadius = o->m_cornerRadius;
    m_fillColor    = o->m_fillColor;
}

} // namespace rt
