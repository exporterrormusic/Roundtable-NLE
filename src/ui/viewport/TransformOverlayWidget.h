/*
 * TransformOverlayWidget — Transparent overlay for transform handles + pan.
 *
 * Sits on top of VulkanViewport (which cannot use QPainter) and provides:
 *   - Bounding box + corner handles for selected clip transform / scale
 *   - Middle-mouse-button panning (forwarded to VulkanViewport)
 *   - Cursor hints (resize arrows, move hand, etc.)
 *
 * This widget is fully transparent except for the painted overlay elements.
 * Mouse events that don't hit a handle or the body are ignored so they can
 * fall through to the VulkanViewport/native-window below.
 */

#pragma once

#include <QWidget>
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QPlainTextEdit>

#include <cstdint>
#include <cmath>
#include <vector>

#include "viewport/OverlayMath.h"
#include "timeline/OpacityMask.h"

namespace rt {

class VulkanViewport;
class CommandStack;
template <typename T> class KeyframeTrack;

/// Transparent overlay widget for transform gizmo + middle-mouse pan.
class TransformOverlayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TransformOverlayWidget(VulkanViewport* viewport, QWidget* parent = nullptr);
    ~TransformOverlayWidget() override;

    /// Show / update the transform overlay.
    void setTransformOverlay(const TransformOverlayInfo& info);

    /// Set the outline-only secondary boxes drawn alongside the primary
    /// transform overlay — one per non-focused multi-selected layer in
    /// the Essential Graphics list. Secondary boxes have no scale /
    /// rotate / anchor handles and are not hit-tested; they exist purely
    /// to show the user which layers participate in a group-move drag.
    void setSecondaryOverlays(const std::vector<TransformOverlayInfo>& extras);

    /// Hide the overlay.
    void clearTransformOverlay();

    /// Toggle safe area guides overlay.
    void setSafeAreasVisible(bool visible);

    /// Toggle rule-of-thirds grid overlay.
    void setGridVisible(bool visible);

    /// Set the active editing tool (so overlay knows when Text tool is active).
    void setEditTool(uint8_t tool) noexcept;

    /// Begin in-place text editing: shows an editable box over the selected
    /// layer's bounding rect, prefilled with `initial`, styled with the
    /// layer's actual font/color so the user sees what the rendered text
    /// will look like (Premiere-style WYSIWYG). `fontSizeRef` is the layer's
    /// font size in 1920×1080 reference pixels — the overlay scales it to
    /// the on-screen content rect so the editor's text matches the rendered
    /// text size exactly. Commit (Enter / focus out) emits
    /// inlineTextCommitted(); Esc cancels.
    /// `horizontalStretch` accounts for anisotropic layer scaling: the
    /// renderer applies painter.scale(scaleX, scaleY) so glyph height ∝
    /// scaleY (baked into fontSizeRef at the call site) and glyph width
    /// ∝ scaleX. Pass scaleX / scaleY so the editor matches; 1.0 = normal
    /// width, 2.0 = glyphs twice as wide as tall.
    /// `hAlignFlag` is a Qt::Alignment horizontal flag (AlignLeft / AlignHCenter
    /// / AlignRight / AlignJustify) so the inline editor's text sits in the
    /// same position the renderer would put it — center-aligned text doesn't
    /// jump to the left edge during edit.
    void beginInlineTextEdit(const QString& initial,
                             const QString& fontFamily,
                             float fontSizeRef,
                             int fontWeight,
                             bool italic,
                             const QColor& textColor,
                             float horizontalStretch = 1.0f,
                             Qt::Alignment hAlignFlag = Qt::AlignHCenter);

    /// True while the in-place text editor is shown.
    [[nodiscard]] bool isInlineTextEditing() const noexcept;

    /// Set the offset from the overlay's top-left to the viewport's top-left.
    /// When the overlay is clipped to the panel bounds, this offset lets
    /// computeFrameRect() use the viewport's full unclipped dimensions.
    void setViewportOffset(const QPoint& offset) noexcept { m_vpOffset = offset; }

    /// Set mask data for overlay drawing + editing. Call after mask changes.
    void setMasks(std::vector<OpacityMask>* masks) noexcept {
        m_masks = masks;
        if (!m_masks || m_masks->empty()) {
            m_activeMaskIndex = -1;
        }
        update();
    }

    /// Set which mask is actively selected for editing (-1 = all drawn, none focused).
    void setActiveMaskIndex(int idx) noexcept { m_activeMaskIndex = idx; update(); }

    /// Set the CLIP-LOCAL time (ticks, same basis the renderer evaluates
    /// masks at: playhead − clip timelineIn) used to evaluate mask geometry
    /// and keyframed scalars for drawing/hit-testing, and to write Mask Path
    /// keyframes during drags when the path stopwatch is on.
    void setMaskTime(int64_t clipLocalTime) noexcept {
        if (m_maskTime == clipLocalTime) return;
        m_maskTime = clipLocalTime;
        update();
    }
    [[nodiscard]] int64_t maskTime() const noexcept { return m_maskTime; }

    /// Get the currently active mask index (-1 = none).
    [[nodiscard]] int activeMaskIndex() const noexcept { return m_activeMaskIndex; }

    /// Are safe areas currently visible?
    [[nodiscard]] bool safeAreasVisible() const noexcept { return m_showSafeAreas; }

    /// Is the grid overlay currently visible?
    [[nodiscard]] bool gridVisible() const noexcept { return m_showGrid; }

    /// Get current overlay info.
    [[nodiscard]] const TransformOverlayInfo& transformOverlay() const noexcept { return m_overlay; }

    // ── Motion path (Premiere-style 2D Position keyframes) ────────────
    /// Attach the selected clip's Position X/Y tracks for motion-path
    /// drawing + spatial-interpolation editing. Pass nullptr to hide the
    /// path. The CommandStack is used when the user changes spatial
    /// interpolation from the right-click menu (so undo works).
    void setMotionPathTracks(KeyframeTrack<float>* trackX,
                             KeyframeTrack<float>* trackY,
                             CommandStack* commandStack) noexcept;
    void clearMotionPath() noexcept;

    /// Frame dimensions (sequence resolution) — needed to map REF-1920
    /// keyframe values to frame-space for motion-path drawing.
    void setSequenceResolution(uint32_t w, uint32_t h) noexcept {
        m_seqW = (w > 0 ? w : m_seqW);
        m_seqH = (h > 0 ? h : m_seqH);
        update();
    }

signals:
    /// Emitted when the user drags the body to change position (ref-space).
    void transformPositionChanged(float posX, float posY);

    /// Emitted when the user drag-scales via corner handles.
    void transformScaleChanged(float scaleX, float scaleY);

    /// Emitted when the user drag-rotates via outside-corner handles.
    void transformRotationChanged(float rotation);

    /// Emitted when the user drags the anchor handle. Values are in the
    /// same units as the transform's anchor track (canvas pixels for
    /// graphic layers — relative to the layer's geometric center).
    void transformAnchorChanged(float anchorX, float anchorY);

    /// Emitted on mouse-release after an anchor drag — carries the
    /// pre-drag and post-drag values so the workspace can push a single
    /// undo command for the whole drag.
    void transformAnchorDragFinished(float oldX, float oldY,
                                     float newX, float newY);

    /// Emitted when the transform drag completes (for undo recording).
    void transformDragFinished(float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                               float newPosX, float newPosY, float newScX, float newScY, float newRot);

    /// Emitted when a mask drag completes for undo recording.
    void maskDragFinished(int maskIndex, OpacityMask oldMask, OpacityMask newMask);

    /// Emitted during mask drag to trigger live composite refresh.
    void maskLiveUpdate();

    /// Emitted during a motion-path spatial-handle drag so the workspace can
    /// invalidate the composite cache and request a Program Monitor refresh.
    void motionPathLiveUpdate();

    /// Emitted when the user clicks on empty area (no handle/body hit).
    /// Coordinates are in frame-space (0..outputWidth, 0..outputHeight).
    /// `modifiers` carries the keyboard modifiers at click time so the
    /// workspace can implement Shift/Ctrl multi-select against the hit
    /// layer (instead of replacing the selection on every click).
    void emptyAreaClicked(float frameX, float frameY,
                          Qt::KeyboardModifiers modifiers);

    /// Emitted on a double-click in the monitor — used to enter text-edit
    /// mode on the text layer under the cursor (Premiere Pro behavior).
    /// Coordinates are in frame-space (0..outputWidth, 0..outputHeight).
    void textEditRequested(float frameX, float frameY);

    /// Emitted when in-place text editing is committed (Enter / focus out)
    /// with the new text. The workspace writes it back to the text layer.
    void inlineTextCommitted(const QString& text);

    /// Emitted when the eyedropper tool picks a color at frame-space coords.
    void colorPicked(float frameX, float frameY);

    /// Emitted during a crop-edge drag (live composite refresh). Values are
    /// percent cropped per edge (0..100).
    void cropChanged(float cropL, float cropR, float cropT, float cropB);

    /// Emitted on mouse-release after a crop drag — carries pre- and post-drag
    /// crop so the workspace can push a single undo command.
    void cropDragFinished(float oldL, float oldR, float oldT, float oldB,
                          float newL, float newR, float newT, float newB);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Drop the application override cursor if we installed one.  Safe to
    /// call repeatedly; keeps the override stack balanced.
    void clearCursorOverride();
    bool m_haveCursorOverride{false};

    /// Compute the frame draw-rect (where the frame appears in widget space).
    QRectF computeFrameRect() const;

    /// Map frame-space point to widget-space.
    QPointF frameToWidget(const QPointF& fp) const;

    /// Compute the 4 widget-space corners of the overlay bounding box.
    void computeOverlayCorners(QPointF corners[4]) const;

    /// Same as computeOverlayCorners but for an arbitrary TransformOverlayInfo
    /// (used to draw outline boxes for multi-selection siblings).
    void computeOverlayCornersFor(const TransformOverlayInfo& ov,
                                   QPointF corners[4]) const;

    /// Hit-test a corner handle; returns 0–3 or -1.
    int hitTestHandle(const QPointF& widgetPos) const;

    /// Hit-test the rotation zone (outside but near a corner). Returns 0–3 or -1.
    int hitTestRotate(const QPointF& widgetPos) const;

    /// Hit-test inside the overlay body.
    bool hitTestBody(const QPointF& widgetPos) const;

    /// Hit-test the body inflated outward by `marginPx` widget pixels along
    /// the box's own edge axes. Used to make a thin text box forgiving to grab
    /// at any zoom (the tight box can be only a few px tall on screen). The
    /// caller still yields to corner scale/rotate handles in that margin.
    bool hitTestBodyMargin(const QPointF& widgetPos, double marginPx) const;

    /// Compute the 4 crop edge-handle widget positions (order: Left, Right,
    /// Top, Bottom) at the current crop insets. Returns false if no overlay.
    bool cropHandlePositions(QPointF handles[4]) const;

    /// Hit-test a crop edge handle. Returns 0=Left,1=Right,2=Top,3=Bottom,-1=none.
    int hitTestCropHandle(const QPointF& widgetPos) const;

    /// Draw the crop overlay (dimmed cropped-out region + edge handles).
    void drawCropOverlay(class QPainter& painter);

    /// Hit-test mask control points. Returns handle index (or -1) via outHandle,
    /// and sets outMaskIndex to the mask that was hit.
    int hitTestMaskHandle(const QPointF& widgetPos, int& outMaskIndex) const;

    /// Hit-test inside a mask shape body. Returns the mask index (or -1).
    int hitTestMaskBody(const QPointF& widgetPos) const;

    /// Draw the transform overlay (bounding box + handles).
    void drawTransformOverlay(class QPainter& painter);

    /// Draw mask shape overlays (blue outlines + control points).
    void drawMaskOverlay(class QPainter& painter);

    /// Add a control point to the closest mask edge near widgetPos.
    /// Returns true if a point was added; sets outMaskIndex.
    bool addPointOnMaskEdge(const QPointF& widgetPos, const QRectF& fr, int& outMaskIndex);

    /// Hit-test whether widgetPos is near a mask edge (for pen cursor hint).
    bool hitTestMaskEdge(const QPointF& widgetPos) const;

    /// Draw safe area guides (action-safe + title-safe).
    void drawSafeAreas(class QPainter& painter);

    /// Draw rule-of-thirds grid.
    void drawGrid(class QPainter& painter);

    /// Set cursor on the native QWindow (which is on top and receives events).
    void applyCursor(Qt::CursorShape shape);
    void applyCursor(const QCursor& cursor);

    /// Build a Premiere-style curved-arrow rotation cursor.
    static QCursor rotateCursor();

    /// Build a pen cursor for add-point-on-edge feedback.
    static QCursor penCursor();

    /// Build a magnifying-glass zoom cursor matching Premiere Pro.
    static QCursor zoomCursor();

    VulkanViewport* m_vulkanVp{nullptr};

    TransformOverlayInfo m_overlay;
    /// Outline-only sibling overlays for multi-selection. Updated by
    /// setSecondaryOverlays; consumed by drawTransformOverlay to paint
    /// a thin rectangle around each non-focused selected layer.
    std::vector<TransformOverlayInfo> m_secondaryOverlays;

    enum class DragMode : uint8_t {
        None,
        MoveBody,
        ScaleCorner,
        RotateCorner,
        Pan,
        DragMaskPoint,
        DragMotionHandle,   ///< spatial bezier handle on a Position keyframe
        MoveAnchor,         ///< anchor point (rotation/scale pivot) handle
        CropEdge,           ///< crop edge handle (Left/Right/Top/Bottom)
    };
    DragMode m_dragMode{DragMode::None};
    int      m_dragHandle{-1};
    int      m_cropHandle{-1};          ///< 0=L,1=R,2=T,3=B during a CropEdge drag
    float    m_dragStartCrop[4]{0, 0, 0, 0}; ///< L,R,T,B crop% snapshot at drag start
    QPointF  m_dragStartWidget;
    float    m_dragStartPosX{0.0f};
    float    m_dragStartPosY{0.0f};
    float    m_dragStartScX{1.0f};
    float    m_dragStartScY{1.0f};
    float    m_dragStartRot{0.0f};
    float    m_dragStartAngle{0.0f}; ///< Angle from center at drag start (for rotation)
    float    m_dragStartAnchorX{0.0f};
    float    m_dragStartAnchorY{0.0f};

    // Pan state (middle mouse)
    QPointF  m_panStartPos;
    float    m_panStartVpX{0.0f};
    float    m_panStartVpY{0.0f};

    // Safe area guides
    bool     m_showSafeAreas{false};

    // Rule-of-thirds grid
    bool     m_showGrid{false};

    // Active editing tool (uses uint8_t to avoid EditTool dependency)
    uint8_t  m_editTool{0};  // 0 = Selection, 6 = Text

    // In-place text editor (lazily created child widget shown over the
    // selected text layer's bounding box). Owned via Qt parent.
    QPlainTextEdit* m_inlineTextEdit{nullptr};
    bool             m_committingInlineText{false};
    /// Screen-space center the inline editor should stay anchored to as
    /// the text grows/shrinks during typing. Updated in beginInlineTextEdit
    /// from the transform box centroid; consumed by the textChanged
    /// handler to resize symmetrically rather than scroll the text.
    QPoint           m_inlineEditCenter{0, 0};
    int              m_inlineEditHeight{32};
    /// Alignment-anchor screen-X coords for the inline editor (snapshotted
    /// from the layer's content rect at edit start). The textChanged
    /// handler picks one of these as the fixed pivot depending on the
    /// layer's horizontal alignment so left-aligned text grows right and
    /// right-aligned text grows left instead of jumping around the center.
    int              m_inlineEditAnchorLeftX{0};
    int              m_inlineEditAnchorRightX{0};
    int              m_inlineEditAnchorCenterX{0};
    int              m_inlineEditAnchorCenterY{0};
    Qt::Alignment    m_inlineEditAlignH{Qt::AlignHCenter};
    /// Saved overlay info so we can restore the transform box after
    /// inline text editing ends (it is hidden during editing to prevent
    /// a ghost anchor/box from appearing in a different spot).
    TransformOverlayInfo m_savedOverlayBeforeEdit{};
    /// Original text when inline editing began — used for Esc→cancel
    /// (restores this value via inlineTextCommitted so the wiring code
    /// detects no-change and restores the layer without an undo entry).
    std::string          m_preEditOriginalText{};

    // Mask overlay data (non-owning pointer to the clip's or an effect's
    // masks vector — both use the same OpacityMask type)
    std::vector<OpacityMask>* m_masks{nullptr};

    // Clip-local time for mask evaluation (geometry + keyframed scalars)
    int64_t m_maskTime{0};

    // Mask drag state
    int m_dragMaskIndex{-1};
    int m_dragMaskHandle{-1};
    OpacityMask m_dragStartMask{};

    // Mask hover state (for handle glow)
    int m_hoverMaskIndex{-1};
    int m_hoverMaskHandle{-1};

    // Active mask index (-1 = all masks visible, none focused)
    int m_activeMaskIndex{-1};

    // Offset from overlay origin to viewport origin (overlay is clipped to panel)
    QPoint m_vpOffset{0, 0};

    // ── Motion path state ────────────────────────────────────────────
    KeyframeTrack<float>* m_motionX{nullptr};
    KeyframeTrack<float>* m_motionY{nullptr};
    CommandStack*         m_motionCmdStack{nullptr};
    uint32_t              m_seqW{1920};
    uint32_t              m_seqH{1080};

    /// Convert a Position keyframe value pair (REF-1920 px) to widget pixels.
    QPointF refToWidget(float refX, float refY, const QRectF& frameRect) const;
    /// Inverse of refToWidget — widget pixel coords back to REF-1920 px.
    QPointF widgetToRef(const QPointF& widgetPos, const QRectF& frameRect) const;

    /// Draw the motion path curve + waypoint markers (and spatial handles
    /// for keyframes whose spatial mode is Bezier/ContinuousBezier).
    void drawMotionPath(class QPainter& painter);

    /// Hit-test motion-path waypoints. Returns the keyframe index or -1.
    int hitTestMotionWaypoint(const QPointF& widgetPos) const;

    /// Hit-test spatial bezier handles. Returns true and fills outKfIdx and
    /// outIsIn (true = incoming handle of the keyframe) when hit.
    bool hitTestMotionHandle(const QPointF& widgetPos, int& outKfIdx, bool& outIsIn) const;

    // ── Spatial-handle drag state ────────────────────────────────────
    int   m_dragMotionKfIdx{-1};
    bool  m_dragMotionIsIn{false};
    // Snapshots at drag start (used by the undo command pushed on release).
    float m_dragOrigInX{0.0f},  m_dragOrigInY{0.0f};
    float m_dragOrigOutX{0.0f}, m_dragOrigOutY{0.0f};
    int64_t m_dragKfTime{0};
};

} // namespace rt
