/*
 * TransformOverlayMask.cpp - Mask hit-testing, edge detection, shape conversion.
 *
 * All geometry reads evaluate the mask at the overlay's clip-local time
 * (m_maskTime) so animated Mask Paths hit-test where they are drawn.
 * Structural edits (add point / shape→bezier conversion) apply to the base
 * geometry AND every path keyframe so vertex counts stay consistent for
 * interpolation.
 */

#include "viewport/TransformOverlayWidget.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"

#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPolygonF>

#include <cmath>
#include <cstddef>
#include <string>

#include <spdlog/spdlog.h>

namespace rt {

namespace {
constexpr int kMaskTangentHandleBase = 10000;

QPointF sourceToDisplayUv(float u, float v, int rotation)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90:  return {1.0 - static_cast<double>(v), static_cast<double>(u)};
    case 180: return {1.0 - static_cast<double>(u), 1.0 - static_cast<double>(v)};
    case 270: return {static_cast<double>(v), 1.0 - static_cast<double>(u)};
    default:  return {static_cast<double>(u), static_cast<double>(v)};
    }
}

QPointF displayToSourceUv(double u, double v, int rotation)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90:  return {v, 1.0 - u};
    case 180: return {1.0 - u, 1.0 - v};
    case 270: return {1.0 - v, u};
    default:  return {u, v};
    }
}

QPointF rotateMaskOffset(float dxNorm, float dyNorm, float degrees,
                         const QSizeF& sourceSize)
{
    const double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double px = static_cast<double>(dxNorm) * sourceSize.width();
    const double py = static_cast<double>(dyNorm) * sourceSize.height();
    return { (px * c - py * s) / sourceSize.width(),
             (px * s + py * c) / sourceSize.height() };
}
} // namespace

QSizeF TransformOverlayWidget::maskSourceSize() const
{
    const auto& owner = (m_hasMaskOwnerOverlay && !m_maskOwnerFollowsPrimary)
        ? m_maskOwnerOverlay : m_overlay;
    double w = static_cast<double>(owner.srcW);
    double h = static_cast<double>(owner.srcH);
    if (w <= 0.0 && m_vulkanVp) w = static_cast<double>(m_vulkanVp->srcWidth());
    if (h <= 0.0 && m_vulkanVp) h = static_cast<double>(m_vulkanVp->srcHeight());
    return {std::max(1.0, w), std::max(1.0, h)};
}

QPointF TransformOverlayWidget::maskLocalToWidget(
    float u, float v, MaskCoordinateSpace space) const
{
    if (space == MaskCoordinateSpace::LegacySequenceFrame) {
        const QRectF frameRect = computeFrameRect();
        return {frameRect.x() + static_cast<double>(u) * frameRect.width(),
                frameRect.y() + static_cast<double>(v) * frameRect.height()};
    }
    QPointF corners[4];
    const auto& owner = (m_hasMaskOwnerOverlay && !m_maskOwnerFollowsPrimary)
        ? m_maskOwnerOverlay : m_overlay;
    computeOverlayCornersFor(owner, corners);
    const QPointF displayUv = sourceToDisplayUv(u, v, owner.srcRotation);
    return corners[0] + (corners[1] - corners[0]) * displayUv.x()
                      + (corners[3] - corners[0]) * displayUv.y();
}

QPointF TransformOverlayWidget::maskVectorToWidget(
    float du, float dv, MaskCoordinateSpace space) const
{
    return maskLocalToWidget(du, dv, space)
         - maskLocalToWidget(0.0f, 0.0f, space);
}

bool TransformOverlayWidget::widgetToMaskLocal(const QPointF& widgetPos,
                                                QPointF& local,
                                                MaskCoordinateSpace space) const
{
    if (space == MaskCoordinateSpace::LegacySequenceFrame) {
        const QRectF frameRect = computeFrameRect();
        if (frameRect.isEmpty()) return false;
        local = {(widgetPos.x() - frameRect.x()) / frameRect.width(),
                 (widgetPos.y() - frameRect.y()) / frameRect.height()};
        return std::isfinite(local.x()) && std::isfinite(local.y());
    }
    QPointF corners[4];
    const auto& owner = (m_hasMaskOwnerOverlay && !m_maskOwnerFollowsPrimary)
        ? m_maskOwnerOverlay : m_overlay;
    computeOverlayCornersFor(owner, corners);
    const QPointF uAxis = corners[1] - corners[0];
    const QPointF vAxis = corners[3] - corners[0];
    const QPointF d = widgetPos - corners[0];
    const double det = uAxis.x() * vAxis.y() - uAxis.y() * vAxis.x();
    if (std::abs(det) < 1.0e-9) return false;
    const double displayU =
        (d.x() * vAxis.y() - d.y() * vAxis.x()) / det;
    const double displayV =
        (uAxis.x() * d.y() - uAxis.y() * d.x()) / det;
    local = displayToSourceUv(displayU, displayV, owner.srcRotation);
    return std::isfinite(local.x()) && std::isfinite(local.y());
}

int TransformOverlayWidget::hitTestMaskHandle(const QPointF& widgetPos, int& outMaskIndex) const
{
    if (!m_masks || !m_vulkanVp) return -1;
    constexpr double HIT_RADIUS = 18.0;

    for (int mi = static_cast<int>(m_masks->size()) - 1; mi >= 0; --mi) {
        // Only hit-test the active mask when one is selected
        if (m_activeMaskIndex >= 0 && mi != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[static_cast<size_t>(mi)];
        const QSizeF sourceSize =
            mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
            ? QSizeF(std::max<uint32_t>(1u, m_vulkanVp->srcWidth()),
                     std::max<uint32_t>(1u, m_vulkanVp->srcHeight()))
            : maskSourceSize();
        const MaskGeometry geo = mask.geometryAt(m_maskTime);
        const float expansion = mask.expansion.evaluate(m_maskTime);
        const float expX = expansion / static_cast<float>(sourceSize.width());
        const float expY = expansion / static_cast<float>(sourceSize.height());

        auto rotatedPoint = [&](float dx, float dy) {
            const QPointF r = rotateMaskOffset(dx, dy, geo.rotation, sourceSize);
            return maskLocalToWidget(
                geo.centerX + static_cast<float>(r.x()),
                geo.centerY + static_cast<float>(r.y()), mask.coordinateSpace);
        };

        if (mask.shape == MaskShape::Ellipse) {
            // Cardinal handles: right(0), left(1), bottom(2), top(3), center(4)
            const float rw = std::max(0.0f, geo.width * 0.5f + expX);
            const float rh = std::max(0.0f, geo.height * 0.5f + expY);
            QPointF handles[5] = {
                rotatedPoint(rw, 0.0f), rotatedPoint(-rw, 0.0f),
                rotatedPoint(0.0f, rh), rotatedPoint(0.0f, -rh),
                maskLocalToWidget(geo.centerX, geo.centerY, mask.coordinateSpace)
            };
            for (int h = 0; h < 5; ++h) {
                if (std::hypot(widgetPos.x() - handles[h].x(),
                               widgetPos.y() - handles[h].y()) <= HIT_RADIUS) {
                    outMaskIndex = mi;
                    return h;
                }
            }
        }
        else if (mask.shape == MaskShape::Rectangle) {
            // Corner handles: TL(0), TR(1), BR(2), BL(3), center(4),
            // Mid-edge handles: top(5), right(6), bottom(7), left(8)
            const float hw = std::max(0.0f, geo.width * 0.5f + expX);
            const float hh = std::max(0.0f, geo.height * 0.5f + expY);
            QPointF handles[9] = {
                rotatedPoint(-hw, -hh), rotatedPoint(hw, -hh),
                rotatedPoint(hw, hh), rotatedPoint(-hw, hh),
                maskLocalToWidget(geo.centerX, geo.centerY, mask.coordinateSpace),
                rotatedPoint(0.0f, -hh), rotatedPoint(hw, 0.0f),
                rotatedPoint(0.0f, hh), rotatedPoint(-hw, 0.0f)
            };
            for (int h = 0; h < 9; ++h) {
                if (std::hypot(widgetPos.x() - handles[h].x(),
                               widgetPos.y() - handles[h].y()) <= HIT_RADIUS) {
                    outMaskIndex = mi;
                    return h;
                }
            }
        }
        else if (mask.shape == MaskShape::FreeDrawBezier) {
            // Tangent handles take priority over their nearby anchor. Encoded
            // indices are decoded by TransformOverlayInput.cpp.
            for (int vi = static_cast<int>(geo.vertices.size()) - 1; vi >= 0; --vi) {
                const auto& v = geo.vertices[static_cast<size_t>(vi)];
                const float tangents[2][2] = {
                    {v.inTanX, v.inTanY}, {v.outTanX, v.outTanY}
                };
                for (int which = 0; which < 2; ++which) {
                    if (std::hypot(tangents[which][0], tangents[which][1]) < 1.0e-6f)
                        continue;
                    const QPointF pt = maskLocalToWidget(
                        v.x + tangents[which][0], v.y + tangents[which][1],
                        mask.coordinateSpace);
                    if (std::hypot(widgetPos.x() - pt.x(), widgetPos.y() - pt.y())
                        <= HIT_RADIUS) {
                        outMaskIndex = mi;
                        return kMaskTangentHandleBase + vi * 2 + which;
                    }
                }
            }
            // Vertex handles
            for (int vi = static_cast<int>(geo.vertices.size()) - 1; vi >= 0; --vi) {
                QPointF pt = maskLocalToWidget(
                    geo.vertices[static_cast<size_t>(vi)].x,
                    geo.vertices[static_cast<size_t>(vi)].y,
                    mask.coordinateSpace);
                if (std::hypot(widgetPos.x() - pt.x(), widgetPos.y() - pt.y()) <= HIT_RADIUS) {
                    outMaskIndex = mi;
                    return vi;
                }
            }
        }
    }
    return -1;
}

int TransformOverlayWidget::hitTestMaskBody(const QPointF& widgetPos) const
{
    if (!m_masks || !m_vulkanVp) return -1;

    // Include the visible outline in the draggable mask target. contains()
    // only covers the interior, so an exact click on a thin or concave path
    // could otherwise fall through and move the whole clip. Point handles
    // still win because mousePressEvent tests them first.
    QPainterPathStroker pathHitStroker;
    pathHitStroker.setWidth(12.0);
    pathHitStroker.setCapStyle(Qt::RoundCap);
    pathHitStroker.setJoinStyle(Qt::RoundJoin);

    // Top-most mask first (drawn last = on top)
    for (int mi = static_cast<int>(m_masks->size()) - 1; mi >= 0; --mi) {
        // Only hit-test the active mask when one is selected
        if (m_activeMaskIndex >= 0 && mi != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[static_cast<size_t>(mi)];
        const MaskGeometry geo = mask.geometryAt(m_maskTime);
        const float expansion = mask.expansion.evaluate(m_maskTime);
        const QPainterPath path = buildMaskWidgetPath(mask, geo, expansion, true);
        if (!path.isEmpty()
            && (path.contains(widgetPos)
                || pathHitStroker.createStroke(path).contains(widgetPos)))
            return mi;
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mask shape conversion helpers (for add-point mode)
// ═════════════════════════════════════════════════════════════════════════════

/// Convert an Ellipse geometry to bezier vertices (4-point cubic approximation).
static void convertEllipseGeoToBezier(MaskGeometry& g, const QRectF& fr)
{
    constexpr float k = 0.5522847498f; // kappa for cubic circle approximation
    float cx = g.centerX, cy = g.centerY;
    float rx = g.width * 0.5f, ry = g.height * 0.5f;
    float rot = g.rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(rot), sinR = std::sin(rot);
    float fw = static_cast<float>(fr.width());
    float fh = static_cast<float>(fr.height());

    // Rotate through widget space for correct aspect ratio
    auto rotNorm = [&](float nx, float ny) -> std::pair<float, float> {
        float wx = nx * fw, wy = ny * fh;
        float rwx = wx * cosR - wy * sinR;
        float rwy = wx * sinR + wy * cosR;
        return { rwx / fw, rwy / fh };
    };

    // Cardinal points: Right, Bottom, Left, Top with tangement handles
    struct PtData { float dx, dy, itx, ity, otx, oty; };
    PtData pts[4] = {
        { rx,  0,      0, -k*ry,    0,  k*ry },  // Right
        {  0,  ry,  k*rx,     0, -k*rx,     0 },  // Bottom
        {-rx,  0,      0,  k*ry,    0, -k*ry },  // Left
        {  0, -ry, -k*rx,     0,  k*rx,     0 },  // Top
    };

    g.vertices.clear();
    g.vertices.resize(4);
    for (int i = 0; i < 4; ++i) {
        auto [px, py] = rotNorm(pts[i].dx, pts[i].dy);
        auto [itx, ity] = rotNorm(pts[i].itx, pts[i].ity);
        auto [otx, oty] = rotNorm(pts[i].otx, pts[i].oty);
        g.vertices[static_cast<size_t>(i)] = { cx + px, cy + py, itx, ity, otx, oty };
    }
    g.rotation = 0.0f;
}

/// Convert a Rectangle geometry to bezier vertices (4 corners, zero tangents).
static void convertRectangleGeoToBezier(MaskGeometry& g, const QRectF& fr)
{
    float cx = g.centerX, cy = g.centerY;
    float hw = g.width * 0.5f, hh = g.height * 0.5f;
    float rot = g.rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(rot), sinR = std::sin(rot);
    float fw = static_cast<float>(fr.width());
    float fh = static_cast<float>(fr.height());

    auto rotNorm = [&](float nx, float ny) -> std::pair<float, float> {
        float wx = nx * fw, wy = ny * fh;
        float rwx = wx * cosR - wy * sinR;
        float rwy = wx * sinR + wy * cosR;
        return { rwx / fw, rwy / fh };
    };

    struct { float dx, dy; } corners[4] = {
        {-hw, -hh}, { hw, -hh}, { hw, hh}, {-hw, hh}
    };

    g.vertices.clear();
    g.vertices.resize(4);
    for (int i = 0; i < 4; ++i) {
        auto [px, py] = rotNorm(corners[i].dx, corners[i].dy);
        g.vertices[static_cast<size_t>(i)] = { cx + px, cy + py, 0, 0, 0, 0 };
    }
    g.rotation = 0.0f;
}

QPainterPath TransformOverlayWidget::buildMaskWidgetPath(
    const OpacityMask& mask, const MaskGeometry& geometry,
    float expansion, bool closed) const
{
    MaskGeometry geo = geometry;
    const QSizeF sourceSize =
        mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
        ? QSizeF(std::max<uint32_t>(
                     1u, m_vulkanVp ? m_vulkanVp->srcWidth() : 1u),
                 std::max<uint32_t>(
                     1u, m_vulkanVp ? m_vulkanVp->srcHeight() : 1u))
        : maskSourceSize();

    // Parametric expansion is expressed in owner/source pixels. Applying it
    // before the owner affine transform makes the edge scale/rotate with the
    // clip, matching the compositor's local-space mask raster.
    if (mask.shape != MaskShape::FreeDrawBezier) {
        geo.width = std::max(0.0f, geo.width
            + 2.0f * expansion / static_cast<float>(sourceSize.width()));
        geo.height = std::max(0.0f, geo.height
            + 2.0f * expansion / static_cast<float>(sourceSize.height()));
        const QRectF sourceRect(0.0, 0.0, sourceSize.width(), sourceSize.height());
        if (mask.shape == MaskShape::Ellipse)
            convertEllipseGeoToBezier(geo, sourceRect);
        else
            convertRectangleGeoToBezier(geo, sourceRect);
    }

    QPainterPath path;
    const auto& verts = geo.vertices;
    if (verts.empty()) return path;

    path.moveTo(maskLocalToWidget(
        verts.front().x, verts.front().y, mask.coordinateSpace));
    const size_t segmentCount = closed ? verts.size() : verts.size() - 1;
    for (size_t vi = 0; vi < segmentCount; ++vi) {
        const size_t ni = (vi + 1) % verts.size();
        const auto& v0 = verts[vi];
        const auto& v1 = verts[ni];
        path.cubicTo(maskLocalToWidget(
                         v0.x + v0.outTanX, v0.y + v0.outTanY,
                         mask.coordinateSpace),
                     maskLocalToWidget(
                         v1.x + v1.inTanX, v1.y + v1.inTanY,
                         mask.coordinateSpace),
                     maskLocalToWidget(v1.x, v1.y, mask.coordinateSpace));
    }
    if (closed) path.closeSubpath();

    if (closed && mask.shape == MaskShape::FreeDrawBezier
        && std::abs(expansion) > 0.01f) {
        const QPointF pxX = maskVectorToWidget(
            1.0f / static_cast<float>(sourceSize.width()), 0.0f,
            mask.coordinateSpace);
        const QPointF pxY = maskVectorToWidget(
            0.0f, 1.0f / static_cast<float>(sourceSize.height()),
            mask.coordinateSpace);
        const double pixelScale = 0.5 * (
            std::hypot(pxX.x(), pxX.y()) + std::hypot(pxY.x(), pxY.y()));
        QPainterPathStroker stroker;
        stroker.setWidth(std::max(0.5,
            2.0 * std::abs(static_cast<double>(expansion)) * pixelScale));
        const QPainterPath edgeBand = stroker.createStroke(path);
        path = expansion > 0.0f ? path.united(edgeBand)
                                : path.subtracted(edgeBand);
    }
    return path;
}

/// Convert a whole MASK (base + every path keyframe) to FreeDrawBezier.
/// Keeps vertex counts consistent across keyframes so interpolation works.
static void convertMaskToBezier(OpacityMask& mask, const QRectF& fr)
{
    auto convertGeo = [&](MaskGeometry& g) {
        if (mask.shape == MaskShape::Ellipse)
            convertEllipseGeoToBezier(g, fr);
        else if (mask.shape == MaskShape::Rectangle)
            convertRectangleGeoToBezier(g, fr);
    };
    convertGeo(mask.base);
    for (auto& k : mask.pathKeys)
        convertGeo(k.geometry);
    mask.shape = MaskShape::FreeDrawBezier;
}

/// Evaluate cubic bezier at parameter t.
static QPointF evalCubicBezier(QPointF p0, QPointF c1, QPointF c2, QPointF p1, double t)
{
    double mt = 1.0 - t;
    return p0 * (mt*mt*mt) + c1 * (3*mt*mt*t) + c2 * (3*mt*t*t) + p1 * (t*t*t);
}

/// De Casteljau split of segment `segIdx` of a bezier geometry at param t —
/// inserts one vertex without changing the curve's shape.
static void splitGeoSegment(MaskGeometry& g, size_t segIdx, float t)
{
    auto& verts = g.vertices;
    if (verts.size() < 2 || segIdx >= verts.size()) return;
    size_t vi = segIdx;
    size_t ni = (vi + 1) % verts.size();

    float p0x = verts[vi].x,                     p0y = verts[vi].y;
    float c1x = verts[vi].x + verts[vi].outTanX, c1y = verts[vi].y + verts[vi].outTanY;
    float c2x = verts[ni].x + verts[ni].inTanX,  c2y = verts[ni].y + verts[ni].inTanY;
    float p1x = verts[ni].x,                     p1y = verts[ni].y;

    float mt = 1.0f - t;

    // First level
    float q0x = mt*p0x + t*c1x,  q0y = mt*p0y + t*c1y;
    float q1x = mt*c1x + t*c2x,  q1y = mt*c1y + t*c2y;
    float q2x = mt*c2x + t*p1x,  q2y = mt*c2y + t*p1y;
    // Second level
    float r0x = mt*q0x + t*q1x,  r0y = mt*q0y + t*q1y;
    float r1x = mt*q1x + t*q2x,  r1y = mt*q1y + t*q2y;
    // Split point
    float sx = mt*r0x + t*r1x,   sy = mt*r0y + t*r1y;

    // Update existing tangent handles for the split
    verts[vi].outTanX = q0x - p0x;
    verts[vi].outTanY = q0y - p0y;
    verts[ni].inTanX  = q2x - p1x;
    verts[ni].inTanY  = q2y - p1y;

    // New vertex at the split point
    MaskVertex newVert{};
    newVert.x       = sx;
    newVert.y       = sy;
    newVert.inTanX  = r0x - sx;
    newVert.inTanY  = r0y - sy;
    newVert.outTanX = r1x - sx;
    newVert.outTanY = r1y - sy;

    // Insert between vi and ni
    if (ni == 0)
        verts.push_back(newVert);
    else
        verts.insert(verts.begin() + static_cast<ptrdiff_t>(ni), newVert);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Hit-test mask edge (for pen cursor hint)
// ═════════════════════════════════════════════════════════════════════════════

bool TransformOverlayWidget::hitTestMaskEdge(const QPointF& widgetPos) const
{
    if (!m_masks || m_masks->empty() || !m_vulkanVp) return false;

    for (size_t mi = 0; mi < m_masks->size(); ++mi) {
        if (m_activeMaskIndex >= 0 && static_cast<int>(mi) != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[mi];
        const QPainterPath path = buildMaskWidgetPath(
            mask, mask.geometryAt(m_maskTime), 0.0f, true);
        QPainterPathStroker hitStroke;
        hitStroke.setWidth(44.0); // 22 px either side, independent of zoom
        if (hitStroke.createStroke(path).contains(widgetPos)) return true;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Add-point on mask edge
// ═════════════════════════════════════════════════════════════════════════════

bool TransformOverlayWidget::addPointOnMaskEdge(const QPointF& widgetPos,
                                                 int& outMaskIndex)
{
    if (!m_masks || m_masks->empty() || !m_vulkanVp) return false;

    constexpr double SNAP_DIST = 22.0; // max widget-space pixels to snap

    double bestDist = SNAP_DIST;
    int    bestMask = -1;
    int    bestSeg  = -1;
    double bestT    = 0.5;

    for (size_t mi = 0; mi < m_masks->size(); ++mi) {
        // Only operate on the active mask when one is selected
        if (m_activeMaskIndex >= 0 && static_cast<int>(mi) != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[mi];
        const QSizeF coordinateSize =
            mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
            ? QSizeF(std::max<uint32_t>(1u, m_vulkanVp->srcWidth()),
                     std::max<uint32_t>(1u, m_vulkanVp->srcHeight()))
            : maskSourceSize();
        const QRectF coordinateRect(
            0.0, 0.0, coordinateSize.width(), coordinateSize.height());

        // Evaluate at the current time; convert parametric shapes to bezier
        // for edge testing.
        MaskGeometry geo = mask.geometryAt(m_maskTime);
        if (mask.shape == MaskShape::Ellipse)
            convertEllipseGeoToBezier(geo, coordinateRect);
        else if (mask.shape == MaskShape::Rectangle)
            convertRectangleGeoToBezier(geo, coordinateRect);
        const auto& verts = geo.vertices;
        if (verts.size() < 2) continue;

        // Sample every segment to find the closest point to click
        for (size_t vi = 0; vi < verts.size(); ++vi) {
            size_t ni = (vi + 1) % verts.size();
            QPointF p0 = maskLocalToWidget(
                verts[vi].x, verts[vi].y, mask.coordinateSpace);
            QPointF c1 = maskLocalToWidget(verts[vi].x + verts[vi].outTanX,
                                           verts[vi].y + verts[vi].outTanY,
                                           mask.coordinateSpace);
            QPointF p1 = maskLocalToWidget(
                verts[ni].x, verts[ni].y, mask.coordinateSpace);
            QPointF c2 = maskLocalToWidget(verts[ni].x + verts[ni].inTanX,
                                           verts[ni].y + verts[ni].inTanY,
                                           mask.coordinateSpace);

            // Coarse pass (16 samples) then refine around closest hit (8 samples).
            // 64 uniform samples per segment is excessive for add-point snapping.
            constexpr int N_COARSE = 16;
            int bestS = -1;
            double bestDistSeg = SNAP_DIST;
            double bestTCoarse = 0.5;
            for (int s = 0; s <= N_COARSE; ++s) {
                double t = static_cast<double>(s) / N_COARSE;
                QPointF pt = evalCubicBezier(p0, c1, c2, p1, t);
                double dist = std::hypot(pt.x() - widgetPos.x(), pt.y() - widgetPos.y());
                if (dist < bestDistSeg) {
                    bestDistSeg = dist;
                    bestS = s;
                    bestTCoarse = t;
                }
            }
            if (bestDistSeg < bestDist) {
                // Refine around coarse hit
                if (bestS > 0 && bestS < N_COARSE) {
                    double tSpan = 1.0 / N_COARSE;
                    constexpr int N_FINE = 8;
                    for (int s = 0; s <= N_FINE; ++s) {
                        double t = (bestTCoarse - tSpan) + (2.0 * tSpan) * s / N_FINE;
                        if (t < 0.0 || t > 1.0) continue;
                        QPointF pt = evalCubicBezier(p0, c1, c2, p1, t);
                        double dist = std::hypot(pt.x() - widgetPos.x(), pt.y() - widgetPos.y());
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestMask = static_cast<int>(mi);
                            bestSeg  = static_cast<int>(vi);
                            bestT    = t;
                        }
                    }
                } else if (bestDistSeg < bestDist) {
                    bestDist = bestDistSeg;
                    bestMask = static_cast<int>(mi);
                    bestSeg  = static_cast<int>(vi);
                    bestT    = bestTCoarse;
                }
            }
        }
    }

    if (bestMask < 0) return false;

    auto& mask = (*m_masks)[static_cast<size_t>(bestMask)];
    OpacityMask oldMask = mask;
    const QSizeF coordinateSize =
        mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
        ? QSizeF(std::max<uint32_t>(1u, m_vulkanVp->srcWidth()),
                 std::max<uint32_t>(1u, m_vulkanVp->srcHeight()))
        : maskSourceSize();
    const QRectF coordinateRect(
        0.0, 0.0, coordinateSize.width(), coordinateSize.height());

    // Convert parametric shape to bezier if needed — applies to base AND
    // every path keyframe so vertex counts stay consistent.
    if (mask.shape != MaskShape::FreeDrawBezier)
        convertMaskToBezier(mask, coordinateRect);

    // Split the same segment at the same t in the base geometry and every
    // path keyframe. Each keyframe's curve shape is preserved by its own
    // De Casteljau split; vertex indices stay aligned for interpolation.
    splitGeoSegment(mask.base, static_cast<size_t>(bestSeg),
                    static_cast<float>(bestT));
    for (auto& key : mask.pathKeys)
        splitGeoSegment(key.geometry, static_cast<size_t>(bestSeg),
                        static_cast<float>(bestT));

    outMaskIndex = bestMask;
    emit maskDragFinished(bestMask, oldMask, mask);
    emit maskLiveUpdate();
    update();
    return true;
}

void TransformOverlayWidget::beginPenPoint(const QPointF& widgetPos)
{
    if (!m_masks) return;

    QPointF local;
    if (!widgetToMaskLocal(widgetPos, local)) return;

    if (!m_penDrawing) {
        m_penDraft = OpacityMask{};
        m_penDraft.shape = MaskShape::FreeDrawBezier;
        m_penDraft.base.vertices.clear();
        m_penDrawing = true;
    }

    MaskVertex vertex{};
    vertex.x = static_cast<float>(local.x());
    vertex.y = static_cast<float>(local.y());
    m_penDraft.base.vertices.push_back(vertex);
    m_penPressLocal = local;
    m_penHoverWidget = widgetPos;
    m_dragStartWidget = widgetPos;
    m_dragMode = DragMode::DrawMaskPoint;
    update();
}

bool TransformOverlayWidget::commitPenMask()
{
    if (!m_penDrawing || !m_masks
        || m_penDraft.base.vertices.size() < 3)
        return false;

    m_penDraft.name = "Mask " + std::to_string(m_masks->size() + 1);
    const int index = static_cast<int>(m_masks->size());
    const OpacityMask committed = m_penDraft;
    m_masks->push_back(committed);
    m_activeMaskIndex = index;
    m_penDrawing = false;
    m_penDraft = OpacityMask{};
    m_dragMode = DragMode::None;
    emit maskCreated(index, committed);
    emit maskLiveUpdate();
    update();
    return true;
}

void TransformOverlayWidget::cancelPenMask() noexcept
{
    if (!m_penDrawing) return;
    m_penDrawing = false;
    m_penDraft = OpacityMask{};
    if (m_dragMode == DragMode::DrawMaskPoint)
        m_dragMode = DragMode::None;
    update();
}

bool TransformOverlayWidget::editExistingMaskWithPen(
    const QPointF& widgetPos, Qt::KeyboardModifiers modifiers)
{
    if (!m_masks || m_masks->empty()) return false;

    int maskIndex = -1;
    const int handle = hitTestMaskHandle(widgetPos, maskIndex);
    if (maskIndex >= 0 && handle >= 0) {
        auto& mask = (*m_masks)[static_cast<size_t>(maskIndex)];
        if (mask.shape != MaskShape::FreeDrawBezier) return false;

        // Existing anchors and tangent handles remain directly draggable with
        // the Pen Mask tool, matching Premiere's temporary direct-selection
        // behavior.
        if (handle >= kMaskTangentHandleBase) {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIndex;
            m_dragMaskHandle = handle;
            m_dragStartMask = mask;
            m_dragStartWidget = widgetPos;
            m_activeMaskIndex = maskIndex;
            return true;
        }

        const size_t vertexIndex = static_cast<size_t>(handle);
        MaskGeometry current = mask.geometryAt(m_maskTime);
        if (vertexIndex >= current.vertices.size()) return false;

        const OpacityMask oldMask = mask;
        if (modifiers.testFlag(Qt::AltModifier)) {
            auto& vertex = current.vertices[vertexIndex];
            const bool hasHandles = std::hypot(vertex.inTanX, vertex.inTanY)
                    + std::hypot(vertex.outTanX, vertex.outTanY) > 1.0e-6f;
            if (hasHandles) {
                vertex.inTanX = vertex.inTanY = 0.0f;
                vertex.outTanX = vertex.outTanY = 0.0f;
            } else {
                const size_t count = current.vertices.size();
                const auto& prev = current.vertices[(vertexIndex + count - 1) % count];
                const auto& next = current.vertices[(vertexIndex + 1) % count];
                const float tx = (next.x - prev.x) * 0.16f;
                const float ty = (next.y - prev.y) * 0.16f;
                vertex.inTanX = -tx;
                vertex.inTanY = -ty;
                vertex.outTanX = tx;
                vertex.outTanY = ty;
            }
            mask.writeGeometry(m_maskTime, current);
        } else if (modifiers.testFlag(Qt::ControlModifier)) {
            if (current.vertices.size() <= 3) return true;
            auto eraseVertex = [vertexIndex](MaskGeometry& geometry) {
                if (vertexIndex < geometry.vertices.size())
                    geometry.vertices.erase(
                        geometry.vertices.begin()
                        + static_cast<ptrdiff_t>(vertexIndex));
            };
            eraseVertex(mask.base);
            for (auto& key : mask.pathKeys) eraseVertex(key.geometry);
        } else {
            m_dragMode = DragMode::DragMaskPoint;
            m_dragMaskIndex = maskIndex;
            m_dragMaskHandle = handle;
            m_dragStartMask = mask;
            m_dragStartWidget = widgetPos;
            m_activeMaskIndex = maskIndex;
            return true;
        }

        m_activeMaskIndex = maskIndex;
        emit maskDragFinished(maskIndex, oldMask, mask);
        emit maskLiveUpdate();
        update();
        return true;
    }

    int edgeMask = -1;
    if (addPointOnMaskEdge(widgetPos, edgeMask)) {
        m_activeMaskIndex = edgeMask;
        return true;
    }
    return false;
}

} // namespace rt
