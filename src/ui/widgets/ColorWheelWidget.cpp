/*
 * ColorWheelWidget.cpp — Circular color wheel with luminance slider.
 */

#include "widgets/ColorWheelWidget.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QtMath>

namespace rt {

static constexpr int kWheelPadding = 6;
static constexpr int kMasterSliderWidth = 14;
static constexpr int kMasterSliderGap = 8;
static constexpr int kLabelHeight = 18;
static constexpr int kDotRadius = 5;

ColorWheelWidget::ColorWheelWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
}

void ColorWheelWidget::setOffset(float r, float g, float b)
{
    m_offsetR = std::clamp(r, -1.0f, 1.0f);
    m_offsetG = std::clamp(g, -1.0f, 1.0f);
    m_offsetB = std::clamp(b, -1.0f, 1.0f);
    update();
}

void ColorWheelWidget::setMaster(float val)
{
    m_master = std::clamp(val, -1.0f, 1.0f);
    update();
}

void ColorWheelWidget::setLabel(const QString& label)
{
    m_label = label;
    update();
}

QSize ColorWheelWidget::sizeHint() const
{
    return {140, 160};
}

QSize ColorWheelWidget::minimumSizeHint() const
{
    return {100, 120};
}

QRectF ColorWheelWidget::wheelRect() const
{
    int side = qMin(width() - kMasterSliderWidth - kMasterSliderGap - 2 * kWheelPadding,
                    height() - kLabelHeight - 2 * kWheelPadding);
    if (side < 20) side = 20;
    double x = kWheelPadding;
    double y = kWheelPadding;
    return QRectF(x, y, side, side);
}

QRectF ColorWheelWidget::masterSliderRect() const
{
    auto wr = wheelRect();
    double x = wr.right() + kMasterSliderGap;
    return QRectF(x, wr.y(), kMasterSliderWidth, wr.height());
}

void ColorWheelWidget::rebuildWheelImage()
{
    auto wr = wheelRect();
    int size = static_cast<int>(wr.width());
    if (size < 4) return;

    m_wheelImage = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
    m_wheelImage.fill(Qt::transparent);

    float cx = size * 0.5f;
    float cy = size * 0.5f;
    float radius = cx - 1.0f;

    for (int y = 0; y < size; ++y) {
        auto* scanline = reinterpret_cast<QRgb*>(m_wheelImage.scanLine(y));
        for (int x = 0; x < size; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > radius) {
                scanline[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            float angle = std::atan2(dy, dx);
            float hue = (angle + static_cast<float>(M_PI)) / (2.0f * static_cast<float>(M_PI));
            float sat = dist / radius;
            // Convert HSV to RGB (value = 0.65 for dark wheel appearance)
            QColor c = QColor::fromHsvF(hue, sat, 0.55f + sat * 0.15f);
            // Fade alpha at edges for anti-aliasing
            float edgeDist = radius - dist;
            int alpha = (edgeDist < 1.0f) ? static_cast<int>(edgeDist * 255) : 255;
            scanline[x] = qRgba(c.red(), c.green(), c.blue(), alpha);
        }
    }

    m_wheelDirty = false;
}

QPointF ColorWheelWidget::offsetToPos() const
{
    auto wr = wheelRect();
    float cx = static_cast<float>(wr.center().x());
    float cy = static_cast<float>(wr.center().y());
    float radius = static_cast<float>(wr.width()) * 0.5f - 2.0f;

    // Convert RGB offset to hue/saturation offset angle
    // Simple mapping: R pushes toward red (right), G pushes toward green (top-left),
    // B pushes toward blue (bottom-left)
    float dx = m_offsetR * 0.866f - m_offsetB * 0.866f; // R right, B left
    float dy = -m_offsetG + (m_offsetR + m_offsetB) * 0.5f; // G up, R+B down

    float mag = std::sqrt(dx * dx + dy * dy);
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }

    return QPointF(cx + dx * radius, cy + dy * radius);
}

void ColorWheelWidget::posToOffset(const QPointF& pos)
{
    auto wr = wheelRect();
    float cx = static_cast<float>(wr.center().x());
    float cy = static_cast<float>(wr.center().y());
    float radius = static_cast<float>(wr.width()) * 0.5f - 2.0f;
    if (radius < 1.0f) return;

    float dx = (static_cast<float>(pos.x()) - cx) / radius;
    float dy = (static_cast<float>(pos.y()) - cy) / radius;

    // Clamp to unit circle
    float mag = std::sqrt(dx * dx + dy * dy);
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }

    // Inverse of offsetToPos transform
    // dx = R * 0.866 - B * 0.866
    // dy = -G + (R + B) * 0.5
    // With constraint: try to reconstruct R, G, B from the 2D position
    // Use angle-based approach: angle → hue, magnitude → saturation
    float angle = std::atan2(-dy, dx); // negate dy since Qt Y is down
    float sat = std::sqrt(dx * dx + dy * dy);

    // Map angle to RGB offsets (simplified primary color wheel mapping)
    m_offsetR = sat * std::cos(angle) * 0.7f;
    m_offsetG = sat * std::cos(angle - 2.0f * static_cast<float>(M_PI) / 3.0f) * 0.7f;
    m_offsetB = sat * std::cos(angle + 2.0f * static_cast<float>(M_PI) / 3.0f) * 0.7f;
}

void ColorWheelWidget::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto& tc = Theme::colors();
    auto wr = wheelRect();

    // Draw wheel image
    if (m_wheelDirty || m_wheelImage.isNull())
        rebuildWheelImage();

    p.drawImage(wr.topLeft(), m_wheelImage);

    // Draw center crosshair (subtle)
    p.setPen(QPen(QColor(120, 120, 120, 80), 1));
    QPointF center = wr.center();
    p.drawLine(QPointF(center.x() - 4, center.y()), QPointF(center.x() + 4, center.y()));
    p.drawLine(QPointF(center.x(), center.y() - 4), QPointF(center.x(), center.y() + 4));

    // Draw dot at current offset position
    QPointF dotPos = offsetToPos();
    p.setPen(QPen(Qt::white, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(dotPos, kDotRadius, kDotRadius);
    // Inner filled dot
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 200));
    p.drawEllipse(dotPos, 2, 2);

    // ── Master slider (vertical bar on the right) ───────────────────────
    auto sr = masterSliderRect();
    // Track background
    QLinearGradient grad(sr.center().x(), sr.top(), sr.center().x(), sr.bottom());
    grad.setColorAt(0.0, QColor(200, 200, 200));   // +1 = bright
    grad.setColorAt(0.5, QColor(80, 80, 80));       // 0 = neutral
    grad.setColorAt(1.0, QColor(20, 20, 20));        // -1 = dark
    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    p.drawRoundedRect(sr, 3, 3);

    // Slider handle
    float normalizedMaster = 0.5f - m_master * 0.5f; // map [-1,1] to [1,0]
    float handleY = sr.y() + normalizedMaster * sr.height();
    QRectF handle(sr.x() - 1, handleY - 3, sr.width() + 2, 6);
    p.setPen(QPen(QColor(200, 200, 200), 1));
    p.setBrush(tc.surface3);
    p.drawRoundedRect(handle, 2, 2);

    // ── Label ───────────────────────────────────────────────────────────
    if (!m_label.isEmpty()) {
        p.setPen(tc.textSecondary);
        QFont f = font();
        f.setPixelSize(Theme::typography().sizeXs);
        p.setFont(f);
        QRectF labelRect(wr.x(), wr.bottom() + 4, wr.width() + kMasterSliderGap + kMasterSliderWidth, kLabelHeight);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, m_label);
    }

    --s_paintDepth;
}

void ColorWheelWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    QPointF pos = event->position();

    // Check master slider
    auto sr = masterSliderRect();
    if (sr.adjusted(-4, 0, 4, 0).contains(pos)) {
        m_draggingMaster = true;
        m_dragStartMaster = m_master;
        float normalizedY = std::clamp(static_cast<float>((pos.y() - sr.y()) / sr.height()), 0.0f, 1.0f);
        m_master = (0.5f - normalizedY) * 2.0f;
        emit masterChanged(m_master);
        update();
        event->accept();
        return;
    }

    // Check wheel
    auto wr = wheelRect();
    QPointF center = wr.center();
    float dx = static_cast<float>(pos.x() - center.x());
    float dy = static_cast<float>(pos.y() - center.y());
    float dist = std::sqrt(dx * dx + dy * dy);
    float radius = static_cast<float>(wr.width()) * 0.5f;

    if (dist <= radius + 4) {
        m_draggingWheel = true;
        m_dragStartR = m_offsetR;
        m_dragStartG = m_offsetG;
        m_dragStartB = m_offsetB;
        posToOffset(pos);
        emit offsetChanged(m_offsetR, m_offsetG, m_offsetB);
        update();
        event->accept();
    }
}

void ColorWheelWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_draggingWheel) {
        posToOffset(event->position());
        emit offsetChanged(m_offsetR, m_offsetG, m_offsetB);
        update();
        event->accept();
    } else if (m_draggingMaster) {
        auto sr = masterSliderRect();
        float normalizedY = std::clamp(static_cast<float>((event->position().y() - sr.y()) / sr.height()), 0.0f, 1.0f);
        m_master = (0.5f - normalizedY) * 2.0f;
        emit masterChanged(m_master);
        update();
        event->accept();
    }
}

void ColorWheelWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    if (m_draggingWheel) {
        m_draggingWheel = false;
        emit offsetCommitted(m_dragStartR, m_dragStartG, m_dragStartB,
                             m_offsetR, m_offsetG, m_offsetB);
        event->accept();
    } else if (m_draggingMaster) {
        m_draggingMaster = false;
        emit masterCommitted(m_dragStartMaster, m_master);
        event->accept();
    }
}

void ColorWheelWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click resets to center
    QPointF pos = event->position();
    auto wr = wheelRect();
    QPointF center = wr.center();
    float dx = static_cast<float>(pos.x() - center.x());
    float dy = static_cast<float>(pos.y() - center.y());
    float dist = std::sqrt(dx * dx + dy * dy);
    float radius = static_cast<float>(wr.width()) * 0.5f;

    if (dist <= radius + 4) {
        float oldR = m_offsetR, oldG = m_offsetG, oldB = m_offsetB;
        m_offsetR = m_offsetG = m_offsetB = 0.0f;
        emit offsetChanged(0.0f, 0.0f, 0.0f);
        emit offsetCommitted(oldR, oldG, oldB, 0.0f, 0.0f, 0.0f);
        update();
    } else {
        auto sr = masterSliderRect();
        if (sr.adjusted(-4, 0, 4, 0).contains(pos)) {
            float oldMaster = m_master;
            m_master = 0.0f;
            emit masterChanged(0.0f);
            emit masterCommitted(oldMaster, 0.0f);
            update();
        }
    }
}

void ColorWheelWidget::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    m_wheelDirty = true;
    QWidget::resizeEvent(event);
    s_inResize = false;
}

} // namespace rt
