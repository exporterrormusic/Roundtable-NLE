/*
 * ClipBindingController — which clip the clip panels show.
 *
 * Effect Controls, Graphics Editor, Color Correction and Properties (plus
 * Tier List and Effects) each hold the Clip* they edit. This is the one
 * place that points them at a clip, or lets go:
 *   - selectClip(): the timeline selected or focused one clip. SelectionState
 *     is set first (GraphicsEditorPanel::setClip emits layerSelected, whose
 *     handler reads it), then every clip panel binds, open or closed (as in
 *     Premiere, a panel opened later shows the current clip), the overlay
 *     refreshes and the dock that edits the clip's kind comes to the front;
 *   - selectClips(): several clips; Properties edits them together;
 *   - clearSelection(): nothing selected;
 *   - bindClipPanels() / clearClipPanels(): the four main panels, for edits
 *     that create or delete the selected clip;
 *   - releaseClipsNotOnTimeline(): after any history change, a panel bound
 *     to a clip that left the timeline lets go before the clip is freed.
 */

#pragma once

#include <QString>

#include <cstddef>
#include <functional>
#include <vector>

class QDockWidget;

namespace rt {

class Clip;
class ColorGradingPanel;
class EffectControlsPanel;
class EffectsPanel;
class GraphicsEditorPanel;
class PropertiesPanel;
class TierListPanel;
class Timeline;
class Track;
struct SelectionState;

class ClipBindingController {
public:
    struct Panels {
        EffectControlsPanel* effectControls{nullptr};
        GraphicsEditorPanel* graphicsEditor{nullptr};
        ColorGradingPanel*   colorGrading{nullptr};
        PropertiesPanel*     properties{nullptr};
        TierListPanel*       tierList{nullptr};
        EffectsPanel*        effects{nullptr};
    };

    struct Config {
        SelectionState* selection{nullptr};   // the workspace's, outlives us
        std::function<Timeline*()> timeline;
        std::function<QDockWidget*(const QString&)> dockForPanel;
        std::function<void()> scheduleOverlayRefresh;
        /// Hide the Program Monitor's transform overlay.
        std::function<void()> clearTransformOverlays;
    };

    explicit ClipBindingController(Config cfg);

    /// The panels once built (all null before: every call is then a no-op
    /// for panels).
    void setPanels(const Panels& panels) { m_panels = panels; }

    /// Binds the four clip panels and Tier List, then raises the clip's dock.
    void selectClip(Clip* clip, Track* track, size_t trackIdx, size_t clipIdx);
    void selectClips(const std::vector<Clip*>& clips, bool editable);
    /// Clears the clip panels, then SelectionState, then the overlay.
    void clearSelection();

    /// Bind Effect Controls, Graphics Editor, Color and Properties.
    void bindClipPanels(Clip* clip, Track* track);
    /// Release those four panels (SelectionState is left to the caller).
    void clearClipPanels();

    /// Show and front the dock that edits `clip`'s kind: Graphics Editor
    /// for Graphic clips, Properties for Spine and video characters,
    /// Effect Controls for everything else.
    void raiseDockFor(const Clip& clip);
    void raiseDock(const QString& name);

    /// Pointers are only compared, never dereferenced: a stale one may
    /// already be freed.
    void releaseClipsNotOnTimeline();

private:
    Config m_cfg;
    Panels m_panels;
};

} // namespace rt
