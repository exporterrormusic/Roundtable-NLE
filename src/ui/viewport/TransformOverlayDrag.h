/*
 * TransformOverlayDrag.h — one Program Monitor drag, from press to release.
 *
 * A press that grabs something (the box, a handle, the anchor, a crop edge,
 * a mask, a motion-path handle, a new mask being drawn, the pan button)
 * creates one of these; it gets every move until the release. Each drag
 * owns the state it started from. It applies its change live (the overlay's
 * *Changed signals, or the mask/keyframe it edits) and reports the whole
 * drag once on release (*Finished), which the workspace records as one undo
 * step. The overlay brackets the drags that edit the clip with
 * editDragStarted/editDragEnded.
 *
 * The drags are friends of TransformOverlayWidget and work on its state.
 */
#pragma once

#include "viewport/TransformOverlayWidget.h"
#include "timeline/OpacityMask.h"

#include <QPointF>

#include <cstdint>

class QMouseEvent;

namespace rt {

class TransformOverlayDrag
{
public:
    using Mode = TransformOverlayWidget::DragMode;

    virtual ~TransformOverlayDrag() = default;

    [[nodiscard]] virtual Mode mode() const noexcept = 0;
    /// False when the move is not part of the drag (its button is no longer
    /// held, or what it edits is gone); the overlay treats it as a hover.
    virtual bool move(QMouseEvent* event) = 0;
    virtual void release(QMouseEvent* event) = 0;
};

/// Middle button: pans the viewport.
class OverlayPanDrag final : public TransformOverlayDrag
{
public:
    OverlayPanDrag(TransformOverlayWidget& w, QPointF press);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::Pan; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    QPointF m_start;
    float m_startPanX;
    float m_startPanY;
};

/// The box: move (body), scale (corner handle) or rotate (outside a corner).
class OverlayTransformDrag final : public TransformOverlayDrag
{
public:
    /// `kind` is MoveBody, ScaleCorner or RotateCorner.
    OverlayTransformDrag(TransformOverlayWidget& w, Mode kind, QPointF press);
    [[nodiscard]] Mode mode() const noexcept override { return m_kind; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    bool moveBody(QMouseEvent* event);
    bool scale(QMouseEvent* event);
    bool rotate(QMouseEvent* event);

    TransformOverlayWidget& m_w;
    Mode m_kind;
    QPointF m_start;
    float m_startPosX;
    float m_startPosY;
    float m_startScX;
    float m_startScY;
    float m_startRot;
    float m_startAngle{0.0f};   ///< Rotate: angle from the centre to the press
};

/// Ctrl on the anchor crosshair: moves the rotation/scale pivot.
class OverlayAnchorDrag final : public TransformOverlayDrag
{
public:
    OverlayAnchorDrag(TransformOverlayWidget& w, QPointF press);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::MoveAnchor; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    QPointF m_start;
    float m_startAnchorX;
    float m_startAnchorY;
};

/// Ctrl on a crop handle: crops one edge.
class OverlayCropDrag final : public TransformOverlayDrag
{
public:
    /// `handle`: 0 = left, 1 = right, 2 = top, 3 = bottom.
    OverlayCropDrag(TransformOverlayWidget& w, int handle, QPointF press);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::CropEdge; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    int m_handle;
    QPointF m_start;
    float m_startCrop[4];   ///< L, R, T, B
};

/// A spatial Bezier handle on a Position keyframe (motion path).
class OverlayMotionHandleDrag final : public TransformOverlayDrag
{
public:
    OverlayMotionHandleDrag(TransformOverlayWidget& w, int keyframeIndex, bool inHandle);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::DragMotionHandle; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    int m_kfIdx;
    bool m_isIn;
    int64_t m_kfTime;
    float m_origInX, m_origInY;
    float m_origOutX, m_origOutY;
};

/// A mask's handle (vertex, tangent, cardinal/corner/edge) or its body.
class OverlayMaskDrag final : public TransformOverlayDrag
{
public:
    /// `handle` as hitTestMaskHandle returns it, or INT_MAX for the body.
    OverlayMaskDrag(TransformOverlayWidget& w, int maskIndex, int handle, QPointF press);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::DragMaskPoint; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    int m_maskIndex;
    int m_handle;
    OpacityMask m_startMask;
    QPointF m_start;
};

/// Pen Mask tool: the press placed a vertex; dragging pulls out its tangents.
class OverlayPenPointDrag final : public TransformOverlayDrag
{
public:
    OverlayPenPointDrag(TransformOverlayWidget& w, QPointF press, QPointF pressLocal);
    [[nodiscard]] Mode mode() const noexcept override { return Mode::DrawMaskPoint; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
    QPointF m_start;
    QPointF m_pressLocal;
};

/// Ellipse/Rectangle Mask tool: sweeps out a new mask.
class OverlayShapeDrag final : public TransformOverlayDrag
{
public:
    explicit OverlayShapeDrag(TransformOverlayWidget& w) : m_w(w) {}
    [[nodiscard]] Mode mode() const noexcept override { return Mode::DrawMaskShape; }
    bool move(QMouseEvent* event) override;
    void release(QMouseEvent* event) override;

private:
    TransformOverlayWidget& m_w;
};

} // namespace rt
