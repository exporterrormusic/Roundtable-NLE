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

std::unique_ptr<Clip> GraphicClip::createEmpty() const
{
    return std::make_unique<GraphicClip>();
}

void GraphicClip::copyOwnStateFrom(const Clip& src, CloneMode mode)
{
    const auto& o = static_cast<const GraphicClip&>(src);
    std::vector<std::unique_ptr<GraphicLayer>> layers;
    layers.reserve(o.m_layers.size());
    for (const auto& want : o.m_layers) {
        if (!want) continue;
        if (mode == CloneMode::Assign) {
            // Keep the live layer object (the Graphics panel holds pointers
            // to it) when the same id and type still exist.
            auto it = std::find_if(m_layers.begin(), m_layers.end(),
                [&](const std::unique_ptr<GraphicLayer>& have) {
                    return have && have->layerId() == want->layerId()
                        && have->layerType() == want->layerType();
                });
            if (it != m_layers.end()) {
                (*it)->assignStateFrom(*want);
                layers.push_back(std::move(*it));
                continue;
            }
        }
        layers.push_back(mode == CloneMode::Duplicate ? want->clone()
                                                      : want->cloneKeepingId());
    }
    m_layers = std::move(layers);
}

} // namespace rt
