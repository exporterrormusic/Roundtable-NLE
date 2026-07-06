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
#include <QPolygonF>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

int TransformOverlayWidget::hitTestMaskHandle(const QPointF& widgetPos, int& outMaskIndex) const
{
    if (!m_masks || !m_vulkanVp) return -1;

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return -1;

    float srcW = static_cast<float>(m_vulkanVp->srcWidth());
    float srcH = static_cast<float>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0f || srcH <= 0.0f) return -1;

    double pxToWX = fr.width()  / static_cast<double>(srcW);
    double pxToWY = fr.height() / static_cast<double>(srcH);

    constexpr double HIT_RADIUS = 18.0;

    auto toWidget = [&](float nx, float ny) -> QPointF {
        return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                       fr.y() + static_cast<double>(ny) * fr.height());
    };

    for (int mi = static_cast<int>(m_masks->size()) - 1; mi >= 0; --mi) {
        // Only hit-test the active mask when one is selected
        if (m_activeMaskIndex >= 0 && mi != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[static_cast<size_t>(mi)];
        const MaskGeometry geo = mask.geometryAt(m_maskTime);
        const float expansion = mask.expansion.evaluate(m_maskTime);
        double expW = static_cast<double>(expansion) * pxToWX;
        double expH = static_cast<double>(expansion) * pxToWY;

        if (mask.shape == MaskShape::Ellipse) {
            QPointF center = toWidget(geo.centerX, geo.centerY);
            double rw = std::max(0.0, static_cast<double>(geo.width)  * fr.width()  * 0.5 + expW);
            double rh = std::max(0.0, static_cast<double>(geo.height) * fr.height() * 0.5 + expH);
            double rotRad = static_cast<double>(geo.rotation) * 3.14159265 / 180.0;
            double cosR = std::cos(rotRad), sinR = std::sin(rotRad);

            // Cardinal handles: right(0), left(1), bottom(2), top(3), center(4)
            QPointF handles[5];
            handles[0] = QPointF(rw, 0);
            handles[1] = QPointF(-rw, 0);
            handles[2] = QPointF(0, rh);
            handles[3] = QPointF(0, -rh);
            handles[4] = QPointF(0, 0); // center = move handle
            for (int h = 0; h < 5; ++h) {
                double rx = handles[h].x() * cosR - handles[h].y() * sinR + center.x();
                double ry = handles[h].x() * sinR + handles[h].y() * cosR + center.y();
                if (std::hypot(widgetPos.x() - rx, widgetPos.y() - ry) <= HIT_RADIUS) {
                    outMaskIndex = mi;
                    return h;
                }
            }
        }
        else if (mask.shape == MaskShape::Rectangle) {
            QPointF center = toWidget(geo.centerX, geo.centerY);
            double hw = std::max(0.0, static_cast<double>(geo.width)  * fr.width()  * 0.5 + expW);
            double hh = std::max(0.0, static_cast<double>(geo.height) * fr.height() * 0.5 + expH);
            double rotRad = static_cast<double>(geo.rotation) * 3.14159265 / 180.0;
            double cosR = std::cos(rotRad), sinR = std::sin(rotRad);

            // Corner handles: TL(0), TR(1), BR(2), BL(3), center(4),
            // Mid-edge handles: top(5), right(6), bottom(7), left(8)
            QPointF handles[9];
            handles[0] = QPointF(-hw, -hh);
            handles[1] = QPointF( hw, -hh);
            handles[2] = QPointF( hw,  hh);
            handles[3] = QPointF(-hw,  hh);
            handles[4] = QPointF(0, 0);
            handles[5] = QPointF(0, -hh);   // top mid-edge
            handles[6] = QPointF(hw, 0);     // right mid-edge
            handles[7] = QPointF(0, hh);     // bottom mid-edge
            handles[8] = QPointF(-hw, 0);    // left mid-edge
            for (int h = 0; h < 9; ++h) {
                double rx = handles[h].x() * cosR - handles[h].y() * sinR + center.x();
                double ry = handles[h].x() * sinR + handles[h].y() * cosR + center.y();
                if (std::hypot(widgetPos.x() - rx, widgetPos.y() - ry) <= HIT_RADIUS) {
                    outMaskIndex = mi;
                    return h;
                }
            }
        }
        else if (mask.shape == MaskShape::FreeDrawBezier) {
            // Vertex handles
            for (int vi = static_cast<int>(geo.vertices.size()) - 1; vi >= 0; --vi) {
                QPointF pt = toWidget(geo.vertices[static_cast<size_t>(vi)].x,
                                      geo.vertices[static_cast<size_t>(vi)].y);
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

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return -1;

    float srcW = static_cast<float>(m_vulkanVp->srcWidth());
    float srcH = static_cast<float>(m_vulkanVp->srcHeight());
    if (srcW <= 0.0f || srcH <= 0.0f) return -1;

    double pxToWX = fr.width()  / static_cast<double>(srcW);
    double pxToWY = fr.height() / static_cast<double>(srcH);

    auto toWidget = [&](float nx, float ny) -> QPointF {
        return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                       fr.y() + static_cast<double>(ny) * fr.height());
    };

    // Top-most mask first (drawn last = on top)
    for (int mi = static_cast<int>(m_masks->size()) - 1; mi >= 0; --mi) {
        // Only hit-test the active mask when one is selected
        if (m_activeMaskIndex >= 0 && mi != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[static_cast<size_t>(mi)];
        const MaskGeometry geo = mask.geometryAt(m_maskTime);
        const float expansion = mask.expansion.evaluate(m_maskTime);
        double expW = static_cast<double>(expansion) * pxToWX;
        double expH = static_cast<double>(expansion) * pxToWY;

        if (mask.shape == MaskShape::Ellipse) {
            QPointF center = toWidget(geo.centerX, geo.centerY);
            double rw = std::max(0.0, static_cast<double>(geo.width)  * fr.width()  * 0.5 + expW);
            double rh = std::max(0.0, static_cast<double>(geo.height) * fr.height() * 0.5 + expH);
            if (rw < 1.0 || rh < 1.0) continue;
            double rotRad = static_cast<double>(geo.rotation) * 3.14159265 / 180.0;
            double cosR = std::cos(rotRad), sinR = std::sin(rotRad);
            double lx = (widgetPos.x() - center.x()) * cosR + (widgetPos.y() - center.y()) * sinR;
            double ly = -(widgetPos.x() - center.x()) * sinR + (widgetPos.y() - center.y()) * cosR;
            double nd = (lx * lx) / (rw * rw) + (ly * ly) / (rh * rh);
            if (nd <= 1.0) return mi;
        }
        else if (mask.shape == MaskShape::Rectangle) {
            QPointF center = toWidget(geo.centerX, geo.centerY);
            double hw = std::max(0.0, static_cast<double>(geo.width)  * fr.width()  * 0.5 + expW);
            double hh = std::max(0.0, static_cast<double>(geo.height) * fr.height() * 0.5 + expH);
            if (hw < 1.0 || hh < 1.0) continue;
            double rotRad = static_cast<double>(geo.rotation) * 3.14159265 / 180.0;
            double cosR = std::cos(rotRad), sinR = std::sin(rotRad);
            double lx = (widgetPos.x() - center.x()) * cosR + (widgetPos.y() - center.y()) * sinR;
            double ly = -(widgetPos.x() - center.x()) * sinR + (widgetPos.y() - center.y()) * cosR;
            if (std::abs(lx) <= hw && std::abs(ly) <= hh) return mi;
        }
        else if (mask.shape == MaskShape::FreeDrawBezier && geo.vertices.size() >= 3) {
            QPainterPath path;
            const auto& verts = geo.vertices;
            path.moveTo(toWidget(verts[0].x, verts[0].y));
            for (size_t vi = 0; vi < verts.size(); ++vi) {
                size_t ni = (vi + 1) % verts.size();
                QPointF p0  = toWidget(verts[vi].x, verts[vi].y);
                QPointF cp1(p0.x() + static_cast<double>(verts[vi].outTanX) * fr.width(),
                            p0.y() + static_cast<double>(verts[vi].outTanY) * fr.height());
                QPointF p1  = toWidget(verts[ni].x, verts[ni].y);
                QPointF cp2(p1.x() + static_cast<double>(verts[ni].inTanX) * fr.width(),
                            p1.y() + static_cast<double>(verts[ni].inTanY) * fr.height());
                path.cubicTo(cp1, cp2, p1);
            }
            if (path.contains(widgetPos)) return mi;
        }
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

    QRectF fr = computeFrameRect();
    if (fr.isEmpty()) return false;

    constexpr double SNAP_DIST = 22.0;

    auto toWidget = [&](float nx, float ny) -> QPointF {
        return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                       fr.y() + static_cast<double>(ny) * fr.height());
    };

    for (size_t mi = 0; mi < m_masks->size(); ++mi) {
        if (m_activeMaskIndex >= 0 && static_cast<int>(mi) != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[mi];

        MaskGeometry geo = mask.geometryAt(m_maskTime);
        if (mask.shape == MaskShape::Ellipse)
            convertEllipseGeoToBezier(geo, fr);
        else if (mask.shape == MaskShape::Rectangle)
            convertRectangleGeoToBezier(geo, fr);
        const auto& verts = geo.vertices;
        if (verts.size() < 2) continue;

        for (size_t vi = 0; vi < verts.size(); ++vi) {
            size_t ni = (vi + 1) % verts.size();
            QPointF p0 = toWidget(verts[vi].x, verts[vi].y);
            QPointF c1(p0.x() + static_cast<double>(verts[vi].outTanX) * fr.width(),
                       p0.y() + static_cast<double>(verts[vi].outTanY) * fr.height());
            QPointF p1 = toWidget(verts[ni].x, verts[ni].y);
            QPointF c2(p1.x() + static_cast<double>(verts[ni].inTanX) * fr.width(),
                       p1.y() + static_cast<double>(verts[ni].inTanY) * fr.height());

            // Coarse pass (16 samples) then refine around closest hit (8 samples).
            // 64 uniform samples per segment is excessive for cursor snapping.
            constexpr int N_COARSE = 16;
            int bestS = -1;
            double bestDistSeg = SNAP_DIST;
            for (int s = 0; s <= N_COARSE; ++s) {
                double t = static_cast<double>(s) / N_COARSE;
                QPointF pt = evalCubicBezier(p0, c1, c2, p1, t);
                double dist = std::hypot(pt.x() - widgetPos.x(), pt.y() - widgetPos.y());
                if (dist < bestDistSeg) {
                    bestDistSeg = dist;
                    bestS = s;
                }
            }
            if (bestDistSeg < SNAP_DIST) return true;

            // Refine around coarse hit
            if (bestS > 0 && bestS < N_COARSE) {
                double tCenter = static_cast<double>(bestS) / N_COARSE;
                double tSpan = 1.0 / N_COARSE;
                constexpr int N_FINE = 8;
                for (int s = 0; s <= N_FINE; ++s) {
                    double t = (tCenter - tSpan) + (2.0 * tSpan) * s / N_FINE;
                    if (t < 0.0 || t > 1.0) continue;
                    QPointF pt = evalCubicBezier(p0, c1, c2, p1, t);
                    double dist = std::hypot(pt.x() - widgetPos.x(), pt.y() - widgetPos.y());
                    if (dist < SNAP_DIST) return true;
                }
            }
        }
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Add-point on mask edge
// ═════════════════════════════════════════════════════════════════════════════

bool TransformOverlayWidget::addPointOnMaskEdge(const QPointF& widgetPos,
                                                const QRectF& fr,
                                                int& outMaskIndex)
{
    if (!m_masks || m_masks->empty() || !m_vulkanVp) return false;

    constexpr double SNAP_DIST = 22.0; // max widget-space pixels to snap

    auto toWidget = [&](float nx, float ny) -> QPointF {
        return QPointF(fr.x() + static_cast<double>(nx) * fr.width(),
                       fr.y() + static_cast<double>(ny) * fr.height());
    };

    double bestDist = SNAP_DIST;
    int    bestMask = -1;
    int    bestSeg  = -1;
    double bestT    = 0.5;

    for (size_t mi = 0; mi < m_masks->size(); ++mi) {
        // Only operate on the active mask when one is selected
        if (m_activeMaskIndex >= 0 && static_cast<int>(mi) != m_activeMaskIndex) continue;
        const auto& mask = (*m_masks)[mi];

        // Evaluate at the current time; convert parametric shapes to bezier
        // for edge testing.
        MaskGeometry geo = mask.geometryAt(m_maskTime);
        if (mask.shape == MaskShape::Ellipse)
            convertEllipseGeoToBezier(geo, fr);
        else if (mask.shape == MaskShape::Rectangle)
            convertRectangleGeoToBezier(geo, fr);
        const auto& verts = geo.vertices;
        if (verts.size() < 2) continue;

        // Sample every segment to find the closest point to click
        for (size_t vi = 0; vi < verts.size(); ++vi) {
            size_t ni = (vi + 1) % verts.size();
            QPointF p0 = toWidget(verts[vi].x, verts[vi].y);
            QPointF c1(p0.x() + static_cast<double>(verts[vi].outTanX) * fr.width(),
                       p0.y() + static_cast<double>(verts[vi].outTanY) * fr.height());
            QPointF p1 = toWidget(verts[ni].x, verts[ni].y);
            QPointF c2(p1.x() + static_cast<double>(verts[ni].inTanX) * fr.width(),
                       p1.y() + static_cast<double>(verts[ni].inTanY) * fr.height());

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

    // Convert parametric shape to bezier if needed — applies to base AND
    // every path keyframe so vertex counts stay consistent.
    if (mask.shape != MaskShape::FreeDrawBezier)
        convertMaskToBezier(mask, fr);

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

} // namespace rt
