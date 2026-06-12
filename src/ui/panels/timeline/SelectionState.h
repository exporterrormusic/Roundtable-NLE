/*
 * SelectionState.h — the timeline workspace's current clip/layer selection.
 *
 * Extracted from five loose TimelineWorkspace members (god-class
 * decomposition, fable_cleanup.txt §3.1) so binder controllers (overlay,
 * selection bindings) can take a SelectionState* instead of the whole
 * workspace.  Plain value struct — all mutation policy stays with the
 * code that owns the workspace.
 */
#pragma once

#include <cstddef>
#include <vector>

namespace rt {

class Clip;

/// Tracks the selected clip (and graphic sub-layer) for the Program
/// Monitor transform overlay, the properties/effect panels, and the
/// edit shortcuts.
struct SelectionState {
    Clip*  clip{nullptr};      ///< Selected clip (non-owning; null = none)
    size_t trackIdx{0};        ///< Track index of `clip`
    size_t clipIdx{0};         ///< Clip index within its track

    /// Selected layer within a GraphicClip (-1 = whole clip).
    int graphicLayerIdx{-1};

    /// Stack indices of every layer selected in the Essential Graphics
    /// layer list (Shift / Ctrl multi-select). Includes the focused
    /// layer. Empty when nothing is selected. Drives group-move on the
    /// program monitor: a single body drag applies the same Δ to every
    /// layer in this set.
    std::vector<int> graphicLayerIdxs;
};

} // namespace rt
