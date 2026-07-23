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
#include <QSizeF>
#include <QPlainTextEdit>
#include <QPainterPath>
#include <QPointer>

#include <cstdint>
#include <cmath>
#include <utility>
#include <vector>

#include "viewport/OverlayMath.h"
#include "timeline/OpacityMask.h"
#include "timeline/GraphicLayer.h"

namespace rt {

enum InlineTextMixedFlag : uint32_t
{
    InlineMixedFamily         = 1u << 0,
    InlineMixedSize           = 1u << 1,
    InlineMixedWeight         = 1u << 2,
    InlineMixedItalic         = 1u << 3,
    InlineMixedCapitalization = 1u << 4,
    InlineMixedTracking       = 1u << 5,
    InlineMixedBaseline       = 1u << 6,
    InlineMixedLeading        = 1u << 7,
    InlineMixedFontStyle      = 1u << 8,
    InlineMixedKerning        = 1u << 9,
    InlineMixedTabWidth       = 1u << 10,
    InlineMixedTsume          = 1u << 11,
    InlineMixedFauxStyle      = 1u << 12,
    InlineMixedDecoration     = 1u << 13,
    InlineMixedScript         = 1u << 14,
    InlineMixedFill           = 1u << 15,
    InlineMixedStroke         = 1u << 16,
    InlineMixedShadow         = 1u << 17,
    InlineMixedBackground     = 1u << 18,
    InlineMixedParagraph      = 1u << 19,
    InlineMixedDirection      = 1u << 20
};

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
    /// text size exactly. Commit (Ctrl/Cmd+Enter / focus out) emits
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
                             Qt::Alignment hAlignFlag = Qt::AlignHCenter,
                             const std::vector<TextStyleRun>& styleRuns = {},
                             float verticalScale = 1.0f,
                             bool allCaps = false,
                             bool smallCaps = false,
                             float tracking = 0.0f,
                             float baselineShift = 0.0f,
                             float leading = 0.0f,
                             const TextRunAppearance& baseAppearance = {},
                             const std::vector<TextParagraphStyle>& paragraphStyles = {},
                             const QString& fontStyle = {},
                             float kerning = 0.0f,
                             float tabWidth = 48.0f,
                             float tsume = 0.0f,
                             bool fauxBold = false,
                             bool fauxItalic = false,
                             bool underline = false,
                             bool superscript = false,
                             bool subscript = false,
                             bool rightToLeft = false);

    /// True while the in-place text editor is shown.
    [[nodiscard]] bool isInlineTextEditing() const noexcept;

    /// Keep inline editing alive while focus moves to this widget or one of
    /// its children (the Graphics Editor's font/style controls).
    void setInlineTextFormattingWidget(QWidget* widget) noexcept {
        m_inlineTextFormattingWidget = widget;
    }

    /// Apply character formatting to the active monitor selection. With only
    /// a caret, these set the typing format for subsequently inserted text.
    bool applyInlineTextFontFamily(const QString& family);
    bool applyInlineTextFontSize(float pointSizeRef);
    bool applyInlineTextFontWeight(int weight);
    bool applyInlineTextItalic(bool italic);
    bool applyInlineTextCapitalization(bool allCaps, bool smallCaps);
    bool applyInlineTextTracking(float tracking);
    bool applyInlineTextBaselineShift(float baselineShift);
    bool applyInlineTextLeading(float leading);
    bool applyInlineTextFontStyle(const QString& styleName);
    bool applyInlineTextKerning(float kerning);
    bool applyInlineTextTabWidth(float tabWidth);
    bool applyInlineTextTsume(float tsume);
    bool applyInlineTextFauxStyles(bool fauxBold, bool fauxItalic);
    bool applyInlineTextUnderline(bool underline);
    bool applyInlineTextScript(bool superscript, bool subscript);
    bool applyInlineTextFill(bool enabled, uint32_t color);
    bool applyInlineTextStroke(bool enabled, uint32_t color, float width,
                               int position);
    bool applyInlineTextShadow(bool enabled, uint32_t color, float distance,
                               float angle, float softness, float opacity);
    bool applyInlineTextBackground(bool enabled, uint32_t color,
                                   float padding);
    bool applyInlineParagraphAlignment(int alignment);
    bool applyInlineParagraphDirection(bool rightToLeft);

    /// Selection helpers are public for controller synchronization and
    /// focused UI regression tests. Positions are UTF-16 code units.
    void setInlineTextSelection(int start, int length);
    [[nodiscard]] std::pair<int, int> inlineTextSelection() const;

    /// Rich styles captured immediately before inlineTextCommitted() fires.
    [[nodiscard]] const std::vector<TextStyleRun>& committedInlineTextStyles()
        const noexcept { return m_committedInlineTextStyles; }
    [[nodiscard]] std::vector<TextStyleRun> currentInlineTextStyles() const {
        return collectInlineTextStyles();
    }
    [[nodiscard]] const std::vector<TextParagraphStyle>&
    committedInlineParagraphStyles() const noexcept {
        return m_committedInlineParagraphStyles;
    }
    [[nodiscard]] std::vector<TextParagraphStyle>
    currentInlineParagraphStyles() const {
        return collectInlineParagraphStyles();
    }

    /// Set the offset from the overlay's top-left to the viewport's top-left.
    /// When the overlay is clipped to the panel bounds, this offset lets
    /// computeFrameRect() use the viewport's full unclipped dimensions.
    void setViewportOffset(const QPoint& offset) noexcept { m_vpOffset = offset; }

    /// Set mask data for overlay drawing + editing. Call after mask changes.
    void setMasks(std::vector<OpacityMask>* masks) noexcept;

    /// Transform of the clip/effect texture that owns the current mask list.
    /// This is kept separate from the focused graphic-layer transform so a
    /// clip mask always follows the clip, even while an inner graphic layer
    /// is selected for its own transform editing.
    void setMaskOwnerOverlay(const TransformOverlayInfo& info,
                             bool followsPrimary = false) noexcept {
        m_maskOwnerOverlay = info;
        m_hasMaskOwnerOverlay = true;
        m_maskOwnerFollowsPrimary = followsPrimary;
        update();
    }

    /// Set which mask is actively selected for editing (-1 = all drawn, none focused).
    void setActiveMaskIndex(int idx) noexcept {
        if (!m_masks || idx < 0 || static_cast<size_t>(idx) >= m_masks->size())
            m_activeMaskIndex = -1;
        else
            m_activeMaskIndex = idx;
        update();
    }

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

    /// Program Monitor crop is a Ctrl-modified edge gesture. Plain edge/corner
    /// interaction remains reserved for transform resizing.
    [[nodiscard]] static bool cropGestureRequested(
        Qt::KeyboardModifiers modifiers) noexcept
    {
        return modifiers.testFlag(Qt::ControlModifier);
    }

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

    /// Emitted after a Pen Mask draft is closed and appended to the active
    /// clip/effect mask list. The controller records the insertion as one
    /// undoable command.
    void maskCreated(int maskIndex, OpacityMask mask);

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

    /// Character format at the active caret/selection.  The Graphics Editor
    /// follows this while rich text is being edited in the monitor.
    void inlineTextSelectionFormatChanged(const QString& family,
                                          float pointSize,
                                          int weight,
                                          bool italic,
                                          bool allCaps,
                                          bool smallCaps,
                                          float tracking,
                                          float baselineShift,
                                          float leading,
                                          uint32_t mixedFlags);
    void inlineTextAdvancedFormatChanged(const QString& fontStyle,
                                         float kerning, float tabWidth,
                                         float tsume, bool fauxBold,
                                         bool fauxItalic, bool underline,
                                         bool superscript, bool subscript,
                                         uint32_t mixedFlags);
    void inlineTextSelectionAppearanceChanged(
        bool fillEnabled, uint32_t fillColor,
        bool strokeEnabled, uint32_t strokeColor, float strokeWidth,
        int strokePosition, bool shadowEnabled, uint32_t shadowColor,
        float shadowDistance, float shadowAngle, float shadowSoftness,
        float shadowOpacity, bool backgroundEnabled,
        uint32_t backgroundColor, float backgroundPadding,
        uint32_t mixedFlags);
    void inlineParagraphFormatChanged(int alignment, bool rightToLeft,
                                      uint32_t mixedFlags);

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
    void resizeInlineTextEditorToDocument();
    [[nodiscard]] QTextCursor inlineTextFormattingCursor() const;
    void finishInlineTextEdit(bool cancel);
    [[nodiscard]] std::vector<TextStyleRun> collectInlineTextStyles() const;
    [[nodiscard]] std::vector<TextParagraphStyle>
        collectInlineParagraphStyles() const;
    void notifyInlineTextSelectionFormat();
    [[nodiscard]] bool focusIsInInlineFormattingUi() const;
    bool m_haveCursorOverride{false};

    /// Compute the frame draw-rect (where the frame appears in widget space).
    QRectF computeFrameRect() const;

    /// Map frame-space point to widget-space.
    QPointF frameToWidget(const QPointF& fp) const;

    /// Compute the 4 widget-space corners of the overlay bounding box.
    void computeOverlayCorners(QPointF corners[4]) const;

    /// Map normalized mask-owner coordinates through the same affine clip
    /// transform used by the compositor. This keeps masks attached to clips.
    [[nodiscard]] QPointF maskLocalToWidget(
        float u, float v,
        MaskCoordinateSpace space = MaskCoordinateSpace::SourceLocal) const;
    [[nodiscard]] bool widgetToMaskLocal(const QPointF& widgetPos,
                                         QPointF& local,
        MaskCoordinateSpace space = MaskCoordinateSpace::SourceLocal) const;
    [[nodiscard]] QPointF maskVectorToWidget(
        float du, float dv,
        MaskCoordinateSpace space = MaskCoordinateSpace::SourceLocal) const;
    [[nodiscard]] QSizeF maskSourceSize() const;
    void syncMaskOwnerToEditedOuterTransform() noexcept;
    [[nodiscard]] QPainterPath buildMaskWidgetPath(const OpacityMask& mask,
                                                   const MaskGeometry& geo,
                                                   float expansion = 0.0f,
                                                   bool closed = true) const;

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
    bool addPointOnMaskEdge(const QPointF& widgetPos, int& outMaskIndex);

    /// Pen-mask draft lifecycle. Drafts stay out of the render list until
    /// closed, preventing an incomplete one/two-point path from blanking a clip.
    void beginPenPoint(const QPointF& widgetPos);
    bool commitPenMask();
    void cancelPenMask() noexcept;
    bool editExistingMaskWithPen(const QPointF& widgetPos,
                                 Qt::KeyboardModifiers modifiers);

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
        DrawMaskPoint,      ///< click/drag a new Pen Mask vertex + tangents
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
    uint8_t  m_editTool{0};  // 0=Selection, 6=Text, 8=Eyedropper, 9=Pen Mask

    // In-place text editor (lazily created child widget shown over the
    // selected text layer's bounding box). Owned via Qt parent.
    QPlainTextEdit* m_inlineTextEdit{nullptr};
    bool             m_committingInlineText{false};
    bool             m_initializingInlineText{false};
    QPointer<QWidget> m_inlineTextFormattingWidget;
    std::vector<TextStyleRun> m_originalInlineTextStyles;
    std::vector<TextStyleRun> m_committedInlineTextStyles;
    std::vector<TextParagraphStyle> m_originalInlineParagraphStyles;
    std::vector<TextParagraphStyle> m_committedInlineParagraphStyles;
    QString          m_inlineBaseFontFamily;
    float            m_inlineBaseFontSize{72.0f};
    int              m_inlineBaseFontWeight{400};
    bool             m_inlineBaseItalic{false};
    bool             m_inlineBaseAllCaps{false};
    bool             m_inlineBaseSmallCaps{false};
    float            m_inlineBaseTracking{0.0f};
    float            m_inlineBaseBaselineShift{0.0f};
    float            m_inlineBaseLeading{0.0f};
    QString          m_inlineBaseFontStyle;
    float            m_inlineBaseKerning{0.0f};
    float            m_inlineBaseTabWidth{48.0f};
    float            m_inlineBaseTsume{0.0f};
    bool             m_inlineBaseFauxBold{false};
    bool             m_inlineBaseFauxItalic{false};
    bool             m_inlineBaseUnderline{false};
    bool             m_inlineBaseSuperscript{false};
    bool             m_inlineBaseSubscript{false};
    bool             m_inlineBaseRightToLeft{false};
    TextRunAppearance m_inlineBaseAppearance;
    double           m_inlineFontPointScale{1.0};
    int              m_inlineFontStretch{100};
    /// Legacy screen-space center retained for transform-overlay bookkeeping.
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
    /// Screen-space top of the first line. Point text grows downward from
    /// here, so Return never pushes already-entered lines upward.
    int              m_inlineEditAnchorTopY{0};
    /// Keep the editor at least as wide as the selected layer's transform
    /// box, so blank pixels beside the glyphs remain a text-selection surface.
    int              m_inlineEditMinWidth{40};
    /// Last explicit monitor-text selection. Native formatting controls can
    /// transiently collapse the QTextCursor when taking focus; formatting
    /// still belongs to this highlighted character range.
    int              m_inlineRetainedSelectionStart{0};
    int              m_inlineRetainedSelectionEnd{0};
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

    TransformOverlayInfo m_maskOwnerOverlay{};
    bool m_hasMaskOwnerOverlay{false};
    bool m_maskOwnerFollowsPrimary{false};

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

    // Interactive Pen Mask draft. A click adds a corner; click-drag creates
    // symmetric Bezier handles; clicking the first point closes/commits.
    bool        m_penDrawing{false};
    OpacityMask m_penDraft{};
    QPointF     m_penPressLocal{};
    QPointF     m_penHoverWidget{};

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
