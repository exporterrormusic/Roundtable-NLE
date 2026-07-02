/*
 * GraphicClip.cpp — Graphic clip container implementation.
 */

#include "timeline/GraphicClip.h"

#include <algorithm>

namespace rt {

GraphicClip::GraphicClip()
    : Clip(ClipType::Graphic)
{
    m_label = "Graphic";
    // Color left at sentinel — theme tint applies.
}

// ═════════════════════════════════════════════════════════════════════════════
//  Layer stack operations
// ═════════════════════════════════════════════════════════════════════════════

GraphicLayer* GraphicClip::layer(size_t index) const
{
    if (index >= m_layers.size()) return nullptr;
    return m_layers[index].get();
}

GraphicLayer* GraphicClip::addLayer(std::unique_ptr<GraphicLayer> layer)
{
    if (!layer) return nullptr;
    auto* ptr = layer.get();
    m_layers.push_back(std::move(layer));
    return ptr;
}

GraphicLayer* GraphicClip::insertLayer(size_t index, std::unique_ptr<GraphicLayer> layer)
{
    if (!layer) return nullptr;
    if (index > m_layers.size()) index = m_layers.size();
    auto* ptr = layer.get();
    m_layers.insert(m_layers.begin() + static_cast<ptrdiff_t>(index), std::move(layer));
    return ptr;
}

std::unique_ptr<GraphicLayer> GraphicClip::removeLayer(size_t index)
{
    if (index >= m_layers.size()) return nullptr;
    auto layer = std::move(m_layers[index]);
    m_layers.erase(m_layers.begin() + static_cast<ptrdiff_t>(index));
    return layer;
}

void GraphicClip::moveLayer(size_t fromIndex, size_t toIndex)
{
    if (fromIndex >= m_layers.size() || toIndex >= m_layers.size()) return;
    if (fromIndex == toIndex) return;

    auto layer = std::move(m_layers[fromIndex]);
    m_layers.erase(m_layers.begin() + static_cast<ptrdiff_t>(fromIndex));
    m_layers.insert(m_layers.begin() + static_cast<ptrdiff_t>(toIndex), std::move(layer));
}

GraphicLayer* GraphicClip::findLayerById(uint64_t id) const
{
    for (auto& l : m_layers)
        if (l && l->layerId() == id) return l.get();
    return nullptr;
}

size_t GraphicClip::findLayerIndex(uint64_t id) const
{
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i] && m_layers[i]->layerId() == id) return i;
    return SIZE_MAX;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Convenience: typed layer creation
// ═════════════════════════════════════════════════════════════════════════════

TextLayer* GraphicClip::addTextLayer(const std::string& text)
{
    auto layer = std::make_unique<TextLayer>();
    layer->setText(text);
    layer->setName("Text");
    auto* ptr = layer.get();
    m_layers.push_back(std::move(layer));
    return ptr;
}

ShapeLayer* GraphicClip::addShapeLayer(ShapeType shape)
{
    auto layer = std::make_unique<ShapeLayer>();
    layer->setShapeType(shape);
    layer->setName("Shape");
    auto* ptr = layer.get();
    m_layers.push_back(std::move(layer));
    return ptr;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clone (deep copy)
// ═════════════════════════════════════════════════════════════════════════════

std::unique_ptr<Clip> GraphicClip::clone() const
{
    auto copy = std::make_unique<GraphicClip>();

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

    // Clone all graphic layers
    for (const auto& layer : m_layers) {
        if (layer)
            copy->m_layers.push_back(layer->clone());
    }

    return copy;
}

} // namespace rt
