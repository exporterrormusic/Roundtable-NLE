/*
 * TransformOverlayMaskDrag.cpp — dragging a mask's handle or body in the
 * Program Monitor (TransformOverlayDrag.h).
 */

#include "viewport/TransformOverlayDrag.h"
#include "viewport/TransformOverlayEdit.h"
#include "viewport/VulkanViewport.h"
#include "timeline/OpacityMask.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Keyframe.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "edit/EditSession.h"

#include <QMouseEvent>

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <utility>

namespace rt {

using namespace overlay_edit;

OverlayMaskDrag::OverlayMaskDrag(TransformOverlayWidget& w, int maskIndex, int handle,
                                 QPointF press)
    : m_w(w), m_maskIndex(maskIndex), m_handle(handle),
      m_startMask((*w.m_masks)[static_cast<size_t>(maskIndex)]), m_start(press)
{
}

bool OverlayMaskDrag::move(QMouseEvent* event)
{
    if (!m_w.m_masks || !(event->buttons() & Qt::LeftButton)) return false;
    const QPointF wPos = event->position();
    if (m_maskIndex < 0
        || static_cast<size_t>(m_maskIndex) >= m_w.m_masks->size())
        return true;

    auto& mask = (*m_w.m_masks)[static_cast<size_t>(m_maskIndex)];
    QPointF startLocal, currentLocal;
    if (!m_w.widgetToMaskLocal(
            m_start, startLocal, mask.coordinateSpace)
        || !m_w.widgetToMaskLocal(wPos, currentLocal, mask.coordinateSpace))
        return true;
    float dxNorm = static_cast<float>(currentLocal.x() - startLocal.x());
    float dyNorm = static_cast<float>(currentLocal.y() - startLocal.y());

    // Shift constrains anchor/body movement to the dominant screen axis.
    // Tangent handles use 45-degree angle snapping further below.
    if ((event->modifiers() & Qt::ShiftModifier)
        && m_handle < kMaskTangentHandleBase
        && (mask.shape == MaskShape::FreeDrawBezier
            || m_handle == 4
            || m_handle == INT_MAX)) {
        QPointF delta = wPos - m_start;
        if (std::abs(delta.x()) >= std::abs(delta.y()))
            delta.setY(0.0);
        else
            delta.setX(0.0);
        QPointF constrainedLocal;
        if (m_w.widgetToMaskLocal(m_start + delta,
                              constrainedLocal, mask.coordinateSpace)) {
            dxNorm = static_cast<float>(
                constrainedLocal.x() - startLocal.x());
            dyNorm = static_cast<float>(
                constrainedLocal.y() - startLocal.y());
        }
    }

    // Work on the geometry evaluated at the current time; write back
    // through writeGeometry (Premiere stopwatch model: updates the
    // static path, or records a Mask Path keyframe when animated).
    const MaskGeometry startGeo = m_startMask.geometryAt(m_w.m_maskTime);
    MaskGeometry geo = mask.geometryAt(m_w.m_maskTime);

    // Resize parametric masks in their own rotated source-pixel axes.
    const QSizeF sourceSize =
        mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame
        ? QSizeF(std::max<uint32_t>(
                     1u, m_w.m_vulkanVp ? m_w.m_vulkanVp->srcWidth() : 1u),
                 std::max<uint32_t>(
                     1u, m_w.m_vulkanVp ? m_w.m_vulkanVp->srcHeight() : 1u))
        : m_w.maskSourceSize();
    const double radians = -static_cast<double>(startGeo.rotation)
        * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians), s = std::sin(radians);
    const double dxPx = static_cast<double>(dxNorm) * sourceSize.width();
    const double dyPx = static_cast<double>(dyNorm) * sourceSize.height();
    const float shapeDx = static_cast<float>(
        (dxPx * c - dyPx * s) / sourceSize.width());
    const float shapeDy = static_cast<float>(
        (dxPx * s + dyPx * c) / sourceSize.height());

    if (mask.shape == MaskShape::Ellipse) {
        if (m_handle == 4 || m_handle == INT_MAX) {
            // Move center
            geo.centerX = startGeo.centerX + dxNorm;
            geo.centerY = startGeo.centerY + dyNorm;
        } else if (m_handle == 0 || m_handle == 1) {
            // Right/left cardinal → scale width
            float d = (m_handle == 0) ? shapeDx : -shapeDx;
            geo.width = std::max(0.01f, startGeo.width + d * 2.0f);
            if ((event->modifiers() & Qt::ShiftModifier)
                && startGeo.width > 1.0e-6f)
                geo.height = std::max(
                    0.01f, startGeo.height * geo.width / startGeo.width);
        } else {
            // Bottom/top cardinal → scale height
            float d = (m_handle == 2) ? shapeDy : -shapeDy;
            geo.height = std::max(0.01f, startGeo.height + d * 2.0f);
            if ((event->modifiers() & Qt::ShiftModifier)
                && startGeo.height > 1.0e-6f)
                geo.width = std::max(
                    0.01f, startGeo.width * geo.height / startGeo.height);
        }
    }
    else if (mask.shape == MaskShape::Rectangle) {
        if (m_handle == 4 || m_handle == INT_MAX) {
            geo.centerX = startGeo.centerX + dxNorm;
            geo.centerY = startGeo.centerY + dyNorm;
        } else if (m_handle >= 5 && m_handle <= 8) {
            // Mid-edge handles: resize one dimension only
            // 5=top, 6=right, 7=bottom, 8=left
            if (m_handle == 5) {
                // Top edge: shrink height from top
                geo.height  = std::max(0.01f, startGeo.height - shapeDy * 2.0f);
            } else if (m_handle == 7) {
                // Bottom edge: grow height from bottom
                geo.height  = std::max(0.01f, startGeo.height + shapeDy * 2.0f);
            } else if (m_handle == 6) {
                // Right edge: grow width from right
                geo.width   = std::max(0.01f, startGeo.width + shapeDx * 2.0f);
            } else { // 8 = left
                // Left edge: shrink width from left
                geo.width   = std::max(0.01f, startGeo.width - shapeDx * 2.0f);
            }
        } else {
            // Corner drag → scale width/height symmetrically
            float signX = (m_handle == 1 || m_handle == 2) ? 1.0f : -1.0f;
            float signY = (m_handle == 2 || m_handle == 3) ? 1.0f : -1.0f;
            if ((event->modifiers() & Qt::ShiftModifier)
                && startGeo.width > 1.0e-6f
                && startGeo.height > 1.0e-6f) {
                const float sx = signX * shapeDx * 2.0f / startGeo.width;
                const float sy = signY * shapeDy * 2.0f / startGeo.height;
                const float deltaScale = std::abs(sx) >= std::abs(sy)
                    ? sx : sy;
                const float scale = std::max(0.01f, 1.0f + deltaScale);
                geo.width = startGeo.width * scale;
                geo.height = startGeo.height * scale;
            } else {
                geo.width  = std::max(
                    0.01f, startGeo.width + signX * shapeDx * 2.0f);
                geo.height = std::max(
                    0.01f, startGeo.height + signY * shapeDy * 2.0f);
            }
        }
    }
    else if (mask.shape == MaskShape::FreeDrawBezier) {
        // INT_MAX is the whole-mask body sentinel; do not interpret it
        // as an encoded Bezier tangent handle.
        if (m_handle != INT_MAX
            && m_handle >= kMaskTangentHandleBase) {
            const int encoded = m_handle - kMaskTangentHandleBase;
            const size_t tangentVertex = static_cast<size_t>(encoded / 2);
            const bool outgoing = (encoded % 2) == 1;
            if (tangentVertex < geo.vertices.size()
                && tangentVertex < startGeo.vertices.size()) {
                auto& vertex = geo.vertices[tangentVertex];
                const auto& startVertex = startGeo.vertices[tangentVertex];
                auto snapTangent = [&](float tx, float ty) {
                    if (!(event->modifiers() & Qt::ShiftModifier))
                        return std::pair<float, float>{tx, ty};
                    const QPointF anchor = m_w.maskLocalToWidget(
                        startVertex.x, startVertex.y,
                        mask.coordinateSpace);
                    QPointF vector = m_w.maskVectorToWidget(
                        tx, ty, mask.coordinateSpace);
                    const double length = std::hypot(vector.x(), vector.y());
                    if (length < 1.0e-6)
                        return std::pair<float, float>{tx, ty};
                    constexpr double step = 3.14159265358979323846 / 4.0;
                    const double angle = std::round(
                        std::atan2(vector.y(), vector.x()) / step) * step;
                    const QPointF target = anchor + QPointF(
                        std::cos(angle) * length,
                        std::sin(angle) * length);
                    QPointF localTarget;
                    if (!m_w.widgetToMaskLocal(
                            target, localTarget, mask.coordinateSpace))
                        return std::pair<float, float>{tx, ty};
                    return std::pair<float, float>{
                        static_cast<float>(localTarget.x() - startVertex.x),
                        static_cast<float>(localTarget.y() - startVertex.y)};
                };
                auto mirrorDirectionPreservingLength = [&](float tx, float ty,
                                                            float oldX,
                                                            float oldY) {
                    const float px = tx * static_cast<float>(sourceSize.width());
                    const float py = ty * static_cast<float>(sourceSize.height());
                    const float len = std::hypot(px, py);
                    const float oldLen = std::hypot(
                        oldX * static_cast<float>(sourceSize.width()),
                        oldY * static_cast<float>(sourceSize.height()));
                    const float useLen = oldLen > 1.0e-6f ? oldLen : len;
                    if (len < 1.0e-6f)
                        return std::pair<float, float>{0.0f, 0.0f};
                    return std::pair<float, float>{
                        -(px / len) * useLen /
                            static_cast<float>(sourceSize.width()),
                        -(py / len) * useLen /
                            static_cast<float>(sourceSize.height())};
                };
                if (outgoing) {
                    auto tangent = snapTangent(
                        startVertex.outTanX + dxNorm,
                        startVertex.outTanY + dyNorm);
                    vertex.outTanX = tangent.first;
                    vertex.outTanY = tangent.second;
                    if (!(event->modifiers() & Qt::AltModifier)) {
                        auto opposite = mirrorDirectionPreservingLength(
                            vertex.outTanX, vertex.outTanY,
                            startVertex.inTanX, startVertex.inTanY);
                        vertex.inTanX = opposite.first;
                        vertex.inTanY = opposite.second;
                    }
                } else {
                    auto tangent = snapTangent(
                        startVertex.inTanX + dxNorm,
                        startVertex.inTanY + dyNorm);
                    vertex.inTanX = tangent.first;
                    vertex.inTanY = tangent.second;
                    if (!(event->modifiers() & Qt::AltModifier)) {
                        auto opposite = mirrorDirectionPreservingLength(
                            vertex.inTanX, vertex.inTanY,
                            startVertex.outTanX, startVertex.outTanY);
                        vertex.outTanX = opposite.first;
                        vertex.outTanY = opposite.second;
                    }
                }
            }
        } else {
        auto vi = static_cast<size_t>(m_handle);
        if (vi < geo.vertices.size() && vi < startGeo.vertices.size()) {
            // Drag single vertex
            geo.vertices[vi].x = startGeo.vertices[vi].x + dxNorm;
            geo.vertices[vi].y = startGeo.vertices[vi].y + dyNorm;
        } else if (geo.vertices.size() == startGeo.vertices.size()) {
            // Body drag — translate all vertices
            for (size_t i = 0; i < geo.vertices.size(); ++i) {
                geo.vertices[i].x = startGeo.vertices[i].x + dxNorm;
                geo.vertices[i].y = startGeo.vertices[i].y + dyNorm;
            }
        }
        }
    }

    mask.writeGeometry(m_w.m_maskTime, geo);

    emit m_w.maskLiveUpdate();
    m_w.update();
    event->accept();
    return true;
}

void OverlayMaskDrag::release(QMouseEvent* /*event*/)
{
    if (m_w.m_masks && m_maskIndex >= 0 &&
        static_cast<size_t>(m_maskIndex) < m_w.m_masks->size())
    {
        emit m_w.maskDragFinished(m_maskIndex, m_startMask,
                                  (*m_w.m_masks)[static_cast<size_t>(m_maskIndex)]);
    }
    m_w.applyCursor(Qt::ArrowCursor);
}

} // namespace rt
