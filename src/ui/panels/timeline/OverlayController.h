/*
 * OverlayController.h — Program Monitor transform-overlay binder.
 *
 * Owns everything between the selected clip/layer and the two overlay
 * widgets (GPU TransformOverlayWidget + software Viewport):
 *   - overlay sync (updateTransformOverlay / scheduleOverlayRefresh /
 *     graphicCanvasRes)
 *   - the shared drag/click handlers (onOverlay*) both wiring sites
 *     connect to, plus the drag-session state they track (group-move
 *     snapshots, scale-drag flip capture, inline-text-edit snapshot)
 *   - the overlay/viewport/tool signal wiring itself
 *
 * Extracted from TimelineWorkspace (god-class decomposition,
 * cleanup audit §3.1).  TRANSITIONAL DESIGN: the controller keeps a
 * back-pointer to the workspace and is a friend, so the moved code could
 * stay byte-identical (m_X → m_ws->m_X).  Narrowing this to injected
 * collaborators (like MediaWatchController::Config) is a follow-up —
 * the win here is that ~2k lines of overlay logic and all drag-session
 * state are out of the workspace class.
 *
 * Definitions: OverlayController.cpp (overlay sync + wiring),
 * OverlayControllerHandlers.cpp (the onOverlay* drag/click handlers).
 */
#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "timeline/GraphicLayer.h"

namespace rt {

class TimelineWorkspace;
class TextLayer;
struct OpacityMask;
struct TransformOverlayInfo;

class OverlayController : public QObject {
public:
    explicit OverlayController(TimelineWorkspace* ws) : m_ws(ws) {}

    // ── Signal wiring (called from the workspace's wire* phase) ────────
    void wireTransformOverlaySignals();   // GPU TransformOverlayWidget
    void wireViewportTransformSignals();  // software Viewport
    void wireOverlayToolSignals();        // tool changes + inline text edit

    // ── Overlay sync ────────────────────────────────────────────────────
    /// Rebuild the Program Monitor handle overlay for the selected
    /// clip/layer (both the software viewport and the GPU widget).
    void updateTransformOverlay();
    /// Deferred overlay re-sync via QTimer (0/50/150 ms, generation-guarded)
    /// plus a composite flush so the displayed frame is current.
    void scheduleOverlayRefresh();
    /// Reference canvas resolution for GraphicClip layout (the project/
    /// sequence resolution renderGraphicClip() composites at — NOT the
    /// monitor preview resolution).  Overlay, text-tool click→posX
    /// conversion, and layer hit-testing must all agree on this space.
    void graphicCanvasRes(uint32_t& w, uint32_t& h) const;

    /// Select which mask list the Program Monitor edits: 0 = the clip's
    /// opacity masks; otherwise the id of an effect on the selected clip
    /// whose effect masks should be shown/edited (Premiere: clicking a
    /// mask under an effect activates it in the monitor).
    void setActiveMaskContext(uint64_t effectId);
    [[nodiscard]] uint64_t activeMaskEffectId() const noexcept { return m_activeMaskEffectId; }

private:
    /// Program-monitor editing is read-only while the selected clip's track
    /// is padlocked. Playback and rendering remain unaffected.
    [[nodiscard]] bool selectedTrackIsEditable() const noexcept;

    /// Measure a text layer's rendered glyph bounds and fill the overlay's
    /// content-rect fields (useContentRect + contentL/T/R/B + canvas dims) so
    /// the selection box hugs the actual text. Mirrors renderGraphicClip's
    /// font/alignment so the box matches the rendered pixels. Used for both a
    /// focused text layer and the whole-clip case (so the box always tracks
    /// the text, not the canvas). The caller sets posX/scale/rotation.
    void measureTextContentRect(TextLayer* tl, int64_t relTick,
                                TransformOverlayInfo& info) const;

    // ── Shared drag/click handlers ──────────────────────────────────────
    // Connected from BOTH wiring sites (GPU overlay + software viewport).
    void onOverlayPositionChanged(float posX, float posY);
    void onOverlayScaleChanged(float scX, float scY);
    void onOverlayRotationChanged(float rot);
    void onOverlayDragFinished(float oldPosX, float oldPosY,
                               float oldScX, float oldScY, float oldRot,
                               float newPosX, float newPosY,
                               float newScX, float newScY, float newRot);
    void onOverlayAnchorChanged(float ax, float ay);
    void onOverlayAnchorDragFinished(float oldX, float oldY,
                                     float newX, float newY);
    void onOverlayMaskDragFinished(int maskIndex, const OpacityMask& oldMask,
                                   const OpacityMask& newMask);
    void onOverlayMaskCreated(int maskIndex, const OpacityMask& mask);
    void onOverlayEmptyAreaClicked(float frameX, float frameY,
                                   Qt::KeyboardModifiers mods);
    void onOverlayCropChanged(float l, float r, float t, float b);
    void onOverlayCropDragFinished(float oldL, float oldR, float oldT, float oldB,
                                   float newL, float newR, float newT, float newB);

    TimelineWorkspace* m_ws{nullptr};

    // ── Drag-session state ──────────────────────────────────────────────
    /// Per-layer snapshot of (posX, posY) at the moment a group-move
    /// drag begins. Used to apply the focused-layer Δ to siblings
    /// without accumulating per-tick error. Cleared on drag finished.
    struct GroupMoveStart { int idx; float posX; float posY; };
    std::vector<GroupMoveStart> m_groupMoveStart;
    bool  m_groupMoveActive{false};
    float m_groupMoveFocusStartX{0.0f};
    float m_groupMoveFocusStartY{0.0f};

    bool   m_scaleDragActive{false};              ///< True while a program-monitor scale drag is in progress
    bool   m_scaleXWasStaticAtDragStart{true};    ///< Saved scaleX static state at drag start
    bool   m_scaleYWasStaticAtDragStart{true};    ///< Saved scaleY static state at drag start
    float  m_scaleXAtDragStart{1.0f};             ///< True SIGNED scaleX from the clip at drag start (preserves flip)
    float  m_scaleYAtDragStart{1.0f};             ///< True SIGNED scaleY from the clip at drag start (preserves flip)

    /// Text snapshot taken when in-place text editing begins. The layer's
    /// text is temporarily cleared during editing so the rendered text
    /// doesn't show through behind the editor box (Premiere Pro behavior).
    /// Restored on cancel, replaced with new text on commit.
    std::string m_preEditOriginalText;
    std::vector<TextStyleRun> m_preEditOriginalStyles;
    std::vector<TextParagraphStyle> m_preEditOriginalParagraphStyles;
    uint64_t    m_preEditClipId{0};
    uint64_t    m_preEditLayerId{0};
    bool        m_preEditWasCaption{false};
    bool        m_inlineTextEditActive{false};

    uint32_t m_overlayRefreshGen{0};   ///< Generation counter for deferred overlay updates

    /// Which mask list the monitor edits: 0 = clip opacity masks, else the
    /// id of an effect on the selected clip (effect masks). Reset to 0 when
    /// the effect no longer exists.
    uint64_t m_activeMaskEffectId{0};

    /// Re-entrancy guard for the Text tool empty-area click handler.
    /// Prevents duplicate clip creation when the overlay click signal
    /// fires twice on the first interaction after project open.
    std::atomic<bool> m_textToolBusy{false};
};

} // namespace rt
