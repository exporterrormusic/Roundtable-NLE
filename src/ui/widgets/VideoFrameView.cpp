/*
 * VideoFrameView.cpp — see header.
 */

#include "widgets/VideoFrameView.h"

#include "Theme.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace rt {

VideoFrameView::VideoFrameView(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void VideoFrameView::setPoster(const QImage& image)
{
    m_poster = image;
    if (m_frame.isNull()) update();
}

void VideoFrameView::setFrame(const QImage& image)
{
    m_frame = image;
    update();
}

void VideoFrameView::showPoster()
{
    if (m_frame.isNull()) return;
    m_frame = QImage();
    update();
}

void VideoFrameView::setHoverScrubEnabled(bool enabled)
{
    m_hoverScrub = enabled;
    setMouseTracking(enabled);
    if (!enabled) m_hoverFraction = -1.0;
    update();
}

void VideoFrameView::setOutline(const QColor& color)
{
    m_outline = color;
    update();
}

QSize VideoFrameView::requestSize() const
{
    const qreal dpr = devicePixelRatioF();
    return QSize(std::max(2, static_cast<int>(std::lround(width() * dpr))),
                 std::max(2, static_cast<int>(std::lround(height() * dpr))));
}

void VideoFrameView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    const QImage& img = m_frame.isNull() ? m_poster : m_frame;
    if (!img.isNull()) {
        QSizeF fit = QSizeF(img.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        QRectF target((width() - fit.width()) / 2.0, (height() - fit.height()) / 2.0,
                      fit.width(), fit.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(target, img);
    }

    if (m_outline.isValid()) {
        p.setPen(QPen(m_outline, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1, 1, -1, -1));
    }

    if (m_hoverFraction >= 0.0) {
        const auto& tc = Theme::colors();
        const int x = std::clamp(static_cast<int>(std::lround(m_hoverFraction * (width() - 1))),
                                 0, width() - 1);
        p.fillRect(QRect(0, height() - 3, x, 3), tc.accent);
        p.fillRect(QRect(x, 0, 1, height()), tc.accent);
    }
}

void VideoFrameView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_hoverScrub && width() > 1) {
        m_hoverFraction = std::clamp(event->position().x() / (width() - 1), 0.0, 1.0);
        update();
        emit hoverScrubbed(m_hoverFraction);
    }
    QWidget::mouseMoveEvent(event);
}

void VideoFrameView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoFrameView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_pressed) {
        m_pressed = false;
        if (rect().contains(event->position().toPoint())) emit clicked();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VideoFrameView::leaveEvent(QEvent* event)
{
    m_pressed = false;
    if (m_hoverFraction >= 0.0) {
        m_hoverFraction = -1.0;
        update();
        emit hoverEnded();
    }
    QWidget::leaveEvent(event);
}

} // namespace rt
