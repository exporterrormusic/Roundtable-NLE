/*
 * FramingEditor.cpp — see header.
 */
#include "panels/comfy/FramingEditor.h"

#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace rt {

namespace {
constexpr double kOutW = 1920.0;
constexpr double kOutH = 1080.0;
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 4.0;
constexpr double kCornerGrab = 10.0;
} // namespace

FramingEditor::FramingEditor(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drag the box to move it. Drag a corner or scroll to zoom."));
}

void FramingEditor::setSource(const QImage& frame, int srcW, int srcH)
{
    m_frame = frame;
    if (srcW > 0 && srcH > 0) {
        m_srcW = srcW;
        m_srcH = srcH;
    }
    update();
}

void FramingEditor::setFraming(const ShotFraming& framing)
{
    m_framing = framing;
    update();
}

QRectF FramingEditor::imageRect() const
{
    // Leave room around the source so an out-of-frame box stays visible.
    const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);
    const double aspect = static_cast<double>(m_srcW) / m_srcH;
    double w = area.width();
    double h = w / aspect;
    if (h > area.height()) {
        h = area.height();
        w = h * aspect;
    }
    return QRectF(area.center().x() - w / 2, area.center().y() - h / 2, w, h);
}

QRectF FramingEditor::outputRect() const
{
    const QRectF img = imageRect();
    const double px = img.width() / m_srcW;              // widget px per source px
    const double boxW = kOutW / m_framing.scale * px;
    const double boxH = kOutH / m_framing.scale * px;
    const double cx = img.left() + m_framing.centerX * img.width();
    const double cy = img.top() + m_framing.centerY * img.height();
    return QRectF(cx - boxW / 2, cy - boxH / 2, boxW, boxH);
}

QImage FramingEditor::renderOutput(const QImage& frame, int srcW, int srcH,
                                   const ShotFraming& framing, QSize outSize)
{
    QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
    out.fill(QColor(0, 255, 0));
    if (frame.isNull() || srcW <= 0 || srcH <= 0) return out;
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // Output px per source px, at this preview size.
    const double k = framing.scale * outSize.width() / kOutW;
    const double left = outSize.width() / 2.0 - framing.centerX * srcW * k;
    const double top  = outSize.height() / 2.0 - framing.centerY * srcH * k;
    p.drawImage(QRectF(left, top, srcW * k, srcH * k), frame);
    return out;
}

void FramingEditor::paintEvent(QPaintEvent*)
{
    const auto& tc = Theme::colors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), tc.surface0);

    const QRectF img = imageRect();
    if (m_frame.isNull()) {
        p.fillRect(img, QColor(20, 20, 20));
        p.setPen(tc.textTertiary);
        p.drawText(img, Qt::AlignCenter, QStringLiteral("Select a shot with a picked take"));
    } else {
        p.drawImage(img, m_frame);
    }

    // Dim everything outside the output box.
    const QRectF box = outputRect();
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    QPainterPath inside;
    inside.addRect(box);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));

    p.setPen(QPen(tc.accent, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(box);
    // Rule-of-thirds guides help place the eyes.
    p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
    for (int i = 1; i < 3; ++i) {
        p.drawLine(QPointF(box.left() + box.width() * i / 3, box.top()),
                   QPointF(box.left() + box.width() * i / 3, box.bottom()));
        p.drawLine(QPointF(box.left(), box.top() + box.height() * i / 3),
                   QPointF(box.right(), box.top() + box.height() * i / 3));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(tc.accent);
    for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
        p.drawRect(QRectF(c.x() - 4, c.y() - 4, 8, 8));

    p.setPen(tc.textPrimary);
    p.drawText(QRectF(box.left() + 6, box.top() + 4, 200, 18), Qt::AlignLeft | Qt::AlignTop,
               QString("%1%").arg(m_framing.scale * 100.0, 0, 'f', 1));
}

void FramingEditor::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    const QRectF box = outputRect();
    const QPointF pos = event->position();
    m_dragStart = pos;
    m_dragStartFraming = m_framing;
    m_drag = Drag::Move;
    for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}) {
        if (std::abs(pos.x() - c.x()) <= kCornerGrab && std::abs(pos.y() - c.y()) <= kCornerGrab) {
            m_drag = Drag::Corner;
            break;
        }
    }
    setCursor(m_drag == Drag::Corner ? Qt::SizeFDiagCursor : Qt::ClosedHandCursor);
}

void FramingEditor::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_drag == Drag::None) {
        const QRectF box = outputRect();
        bool corner = false;
        for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
            corner |= std::abs(pos.x() - c.x()) <= kCornerGrab && std::abs(pos.y() - c.y()) <= kCornerGrab;
        setCursor(corner ? Qt::SizeFDiagCursor : Qt::OpenHandCursor);
        return;
    }
    const QRectF img = imageRect();
    if (m_drag == Drag::Move) {
        const QPointF d = pos - m_dragStart;
        m_framing.centerX = m_dragStartFraming.centerX + d.x() / img.width();
        m_framing.centerY = m_dragStartFraming.centerY + d.y() / img.height();
    } else {
        // Box half-width follows the cursor's distance from the box centre.
        const double cx = img.left() + m_framing.centerX * img.width();
        const double cy = img.top() + m_framing.centerY * img.height();
        const double halfW = std::max(std::abs(pos.x() - cx), std::abs(pos.y() - cy) * kOutW / kOutH);
        const double px = img.width() / m_srcW;
        if (halfW > 4.0)
            m_framing.scale = std::clamp(kOutW * px / (2.0 * halfW), kMinScale, kMaxScale);
    }
    emitChanged(false);
}

void FramingEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_drag == Drag::None) return;
    m_drag = Drag::None;
    setCursor(Qt::OpenHandCursor);
    if (!(m_framing == m_dragStartFraming)) emitChanged(true);
}

void FramingEditor::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    // 1% per notch, like nudging Premiere's Scale field.
    m_framing.scale = std::clamp(m_framing.scale + 0.01 * steps, kMinScale, kMaxScale);
    emitChanged(true);
    event->accept();
}

void FramingEditor::emitChanged(bool committed)
{
    update();
    emit framingChanged(m_framing);
    if (committed) emit framingCommitted(m_framing);
}

} // namespace rt
