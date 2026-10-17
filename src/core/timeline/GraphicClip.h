/*
 * GraphicClip.h — A timeline clip container for text & shape layers.
 *
 * Modelled after Adobe Premiere Pro's Essential Graphics "Graphic Clip":
 *   - A single timeline clip that contains a layer stack
 *   - Each layer has its own transform (separate from clip transform)
 *   - Layers render bottom-to-top
 *   - Supports TextLayer and ShapeLayer
 *
 * This is the new home for text on the timeline, superseding TitleClip
 * for new content. TitleClip remains supported for backwards compat.
 */

#pragma once

#include "timeline/Clip.h"
#include "timeline/GraphicLayer.h"

#include <memory>
#include <vector>

namespace rt {

class GraphicClip : public Clip
{
public:
    GraphicClip();
    ~GraphicClip() override = default;

    // ── Layer stack ─────────────────────────────────────────────────────

    [[nodiscard]] size_t layerCount() const noexcept { return m_layers.size(); }

    /// Access layer by index (0 = bottom of stack / rendered first).
    [[nodiscard]] GraphicLayer* layer(size_t index) const;

    /// Add a layer to the top of the stack. Returns raw pointer.
    GraphicLayer* addLayer(std::unique_ptr<GraphicLayer> layer);

    /// Insert a layer at a specific position.
    GraphicLayer* insertLayer(size_t index, std::unique_ptr<GraphicLayer> layer);

    /// Remove a layer by index. Returns the removed layer.
    std::unique_ptr<GraphicLayer> removeLayer(size_t index);

    /// Move a layer from one position to another (reorder).
    void moveLayer(size_t fromIndex, size_t toIndex);

    /// Find layer by ID. Returns nullptr if not found.
    [[nodiscard]] GraphicLayer* findLayerById(uint64_t id) const;

    /// Find layer index by ID. Returns SIZE_MAX if not found.
    [[nodiscard]] size_t findLayerIndex(uint64_t id) const;

    /// Direct access to layer vector (for iteration / rendering).
    [[nodiscard]] const std::vector<std::unique_ptr<GraphicLayer>>& layers() const noexcept
    { return m_layers; }

    // ── Convenience: add typed layers ───────────────────────────────────

    /// Add a new TextLayer with given text, returns pointer.
    TextLayer* addTextLayer(const std::string& text = "Title");

    /// Add a new ShapeLayer, returns pointer.
    ShapeLayer* addShapeLayer(ShapeType shape = ShapeType::Rectangle);

    // ── Clone ───────────────────────────────────────────────────────────
protected:
    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;
    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    /// Layer stack: index 0 = bottom (rendered first), last = top.
    std::vector<std::unique_ptr<GraphicLayer>> m_layers;
};

} // namespace rt
