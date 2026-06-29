/*
 * OverlayMath.h — Shared transform overlay math utilities.
 *
 * Provides free functions for computing overlay bounding box corners
 * from transform parameters. Used by both Viewport (software QPainter)
 * and TransformOverlayWidget (transparent overlay window).
 *
 * The core algorithm mirrors blitLayerWithTransform's coordinate math
 * so the overlay box matches exactly where the clip is rendered.
 */

#pragma once

#include <QPointF>
#include <QRectF>

#include <cmath>
#include <functional>

namespace rt {

/// Transform overlay information for a selected clip.
/// Positions are pixel offsets from center at reference resolution (1920×1080).
/// Scale values are multipliers.
struct TransformOverlayInfo
{
    bool     visible{false};
    float    posX{0.0f};       ///< X offset from center (ref 1920 px)
    float    posY{0.0f};       ///< Y offset from center (ref 1080 px)
    float    scaleX{1.0f};     ///< Horizontal scale
    float    scaleY{1.0f};     ///< Vertical scale
    float    rotation{0.0f};   ///< Degrees
    uint32_t srcW{0};          ///< Source frame width (after decode)
    uint32_t srcH{0};          ///< Source frame height (after decode)
    bool     directSize{false};///< If true, srcW/srcH are pixel dims in ref space (no fill-scale)
    bool     containFit{false}; ///< If true, use contain-fit (min) instead of cover-fit (max)

    /// Per-layer content rect mode: overlay corners are computed by
    /// applying the layer's QPainter transform (same as renderGraphicClip)
    /// to the content rect (pre-transform canvas coordinates).
    bool  useContentRect{false};
    float contentL{0}, contentT{0}, contentR{0}, contentB{0};
    float contentCanvasW{1920}, contentCanvasH{1080}; ///< Canvas dims used for content bounds

    /// Clip-level (outer) transform for GraphicClip layers.
    float clipPosX{0.0f}, clipPosY{0.0f};
    float clipScaleX{1.0f}, clipScaleY{1.0f};
    float clipRotation{0.0f};

    /// Anchor point — layer-local offset from the layer's geometric
    /// center that acts as the pivot for rotation/scale (Premiere-style).
    /// Defaults to (0,0), which makes the pivot the layer center and
    /// preserves the legacy renderer/overlay behavior.
    float anchorX{0.0f}, anchorY{0.0f};

    /// Crop (percent cropped per edge, 0..100) for crop-capable clips.
    /// When cropEnabled is true the overlay draws draggable edge handles and
    /// dims the cropped-out border so the user can crop/uncrop directly in the
    /// Program Monitor (Premiere-style). Corner handles remain scale handles.
    bool  cropEnabled{false};
    float cropL{0.0f}, cropR{0.0f}, cropT{0.0f}, cropB{0.0f};
};

/// Map a point in canvas/frame coordinates to widget-space coordinates.
using FrameToWidgetFn = std::function<QPointF(float x, float y)>;

/// Compute the 4 widget-space corners of a clip's transform bounding box.
///
/// @param overlay   Transform overlay parameters.
/// @param outW      Composited frame width (canvas width).
/// @param outH      Composited frame height (canvas height).
/// @param frameRect Widget-space rectangle where the frame is drawn.
/// @param toWidget  Callback that maps canvas-space (x,y) to widget-space.
/// @param corners   Output array of 4 QPointF: TL, TR, BR, BL.
///
/// The algorithm mirrors the compositor's blitLayerWithTransform math:
///   - Scale positions from reference (1920×1080) to output resolution
///   - Apply contain/cover fit scaling based on src dimensions
///   - Apply per-axis scale and rotation around center
///   - Map to widget space via the toWidget callback
void computeOverlayCorners(
    const TransformOverlayInfo& overlay,
    float outW, float outH,
    const QRectF& frameRect,
    const FrameToWidgetFn& toWidget,
    QPointF corners[4]);

/// Test if a widget-space point is near a corner handle.
/// Returns handle index 0-3 or -1 if no handle hit.
int hitTestHandle(const QPointF& widgetPos, const QPointF corners[4]);

/// Test if a widget-space point is inside the overlay bounding box.
bool hitTestBody(const QPointF& widgetPos, const QPointF corners[4]);

} // namespace rt