/*
 * CurveEditor.cpp — Cubic spline color curve editor.
 */

#include "widgets/CurveEditor.h"
#include "Theme.h"

#include <QPainter>
#include <QMouseEvent>
#include <QtMath>
#include <algorithm>

namespace rt {

static constexpr int kPadding = 8;
static constexpr int kPointRadius = 5;
static constexpr int kHitRadius = 10;

CurveEditor::CurveEditor(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    resetAll();
}

void CurveEditor::setActiveChannel(Channel ch)
{
    m_activeChannel = ch;
    update();
}

void CurveEditor::resetChannel(Channel ch)
{
    m_hasLoadedLut[ch] = false;
    m_points[ch].clear();
    m_points[ch].push_back({0.0f, 0.0f});
    m_points[ch].push_back({1.0f, 1.0f});
    update();
    emit curveChanged();
}

void CurveEditor::resetAll()
{
    for (int i = 0; i < ChannelCount; ++i)
        resetChannel(static_cast<Channel>(i));
}

QSize CurveEditor::sizeHint() const { return {200, 200}; }
QSize CurveEditor::minimumSizeHint() const { return {120, 120}; }

QPointF CurveEditor::toWidget(float x, float y) const
{
    double w = width() - 2 * kPadding;
    double h = height() - 2 * kPadding;
    return QPointF(kPadding + x * w, kPadding + (1.0f - y) * h);
}

QPointF CurveEditor::fromWidget(const QPointF& pos) const
{
    double w = width() - 2 * kPadding;
    double h = height() - 2 * kPadding;
    float x = static_cast<float>((pos.x() - kPadding) / w);
    float y = static_cast<float>(1.0 - (pos.y() - kPadding) / h);
    return QPointF(std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
}

int CurveEditor::hitTest(Channel ch, const QPointF& widgetPos) const
{
    const auto& pts = m_points[ch];
    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        QPointF wp = toWidget(pts[i].x, pts[i].y);
        double dx = widgetPos.x() - wp.x();
        double dy = widgetPos.y() - wp.y();
        if (dx * dx + dy * dy <= kHitRadius * kHitRadius)
            return i;
    }
    return -1;
}

float CurveEditor::evaluate(Channel ch, float t) const
{
    const auto& pts = m_points[ch];
    if (pts.empty()) return t;
    if (t <= pts.front().x) return pts.front().y;
    if (t >= pts.back().x)  return pts.back().y;

    // Find segment
    size_t i = 0;
    for (; i + 1 < pts.size(); ++i) {
        if (t < pts[i + 1].x) break;
    }

    // Linear interpolation between control points
    // (monotone cubic is more complex; linear gives good visual feedback)
    float dx = pts[i + 1].x - pts[i].x;
    if (dx < 1e-6f) return pts[i].y;
    float frac = (t - pts[i].x) / dx;

    // Use Hermite interpolation for smoother curves
    // Compute tangents using Catmull-Rom
    float m0, m1;
    if (i == 0)
        m0 = (pts[i + 1].y - pts[i].y) / dx;
    else {
        float dx_prev = pts[i].x - pts[i - 1].x;
        m0 = 0.5f * ((pts[i + 1].y - pts[i].y) / dx + (pts[i].y - pts[i - 1].y) / dx_prev);
    }
    if (i + 2 >= pts.size())
        m1 = (pts[i + 1].y - pts[i].y) / dx;
    else {
        float dx_next = pts[i + 2].x - pts[i + 1].x;
        m1 = 0.5f * ((pts[i + 2].y - pts[i + 1].y) / dx_next + (pts[i + 1].y - pts[i].y) / dx);
    }

    // Scale tangents to segment length
    m0 *= dx;
    m1 *= dx;

    // Hermite basis
    float t2 = frac * frac;
    float t3 = t2 * frac;
    float h00 = 2 * t3 - 3 * t2 + 1;
    float h10 = t3 - 2 * t2 + frac;
    float h01 = -2 * t3 + 3 * t2;
    float h11 = t3 - t2;

    return std::clamp(h00 * pts[i].y + h10 * m0 + h01 * pts[i + 1].y + h11 * m1, 0.0f, 1.0f);
}

std::array<float, 256> CurveEditor::lut(Channel ch) const
{
    if (m_hasLoadedLut[ch])
        return m_loadedLuts[ch];

    std::array<float, 256> result{};
    for (int i = 0; i < 256; ++i)
        result[i] = evaluate(ch, i / 255.0f);
    return result;
}

void CurveEditor::setLut(Channel ch, const std::array<float, 256>& lutValues)
{
    m_loadedLuts[ch] = lutValues;
    m_hasLoadedLut[ch] = true;

    // Seventeen display points communicate the shape without drawing 256
    // overlapping handles. The full LUT above is returned unchanged unless
    // the user starts editing this channel.
    auto& points = m_points[ch];
    points.clear();
    for (int sample = 0; sample <= 16; ++sample) {
        const int index = sample == 16 ? 255 : sample * 16;
        points.push_back({static_cast<float>(index) / 255.0f,
                          std::clamp(lutValues[index], 0.0f, 1.0f)});
    }
    update();
}

void CurveEditor::paintEvent(QPaintEvent* event)
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
    QColor bgColor = tc.surface0;
    QColor gridColor = tc.surface3;
    gridColor.setAlpha(80);

    // Background
    p.fillRect(rect(), bgColor);

    // Grid (4x4)
    p.setPen(QPen(gridColor, 1));
    for (int i = 1; i < 4; ++i) {
        float f = i / 4.0f;
        QPointF left = toWidget(0, f);
        QPointF right = toWidget(1, f);
        p.drawLine(QPointF(left.x(), left.y()), QPointF(right.x(), right.y()));
        QPointF top = toWidget(f, 1);
        QPointF bottom = toWidget(f, 0);
        p.drawLine(QPointF(top.x(), top.y()), QPointF(bottom.x(), bottom.y()));
    }

    // Diagonal reference line (identity)
    p.setPen(QPen(QColor(80, 80, 80), 1, Qt::DashLine));
    p.drawLine(toWidget(0, 0), toWidget(1, 1));

    // Channel colors
    static const QColor channelColors[] = {
        QColor(200, 200, 200),  // Master: white
        QColor(220, 60, 60),    // Red
        QColor(60, 200, 60),    // Green
        QColor(60, 100, 220)    // Blue
    };

    // Draw inactive channel curves (dimmed)
    for (int ch = 0; ch < ChannelCount; ++ch) {
        if (ch == m_activeChannel) continue;
        QColor color = channelColors[ch];
        color.setAlpha(40);
        p.setPen(QPen(color, 1));

        QPointF prev = toWidget(0, evaluate(static_cast<Channel>(ch), 0));
        for (int x = 1; x <= 200; ++x) {
            float t = x / 200.0f;
            QPointF cur = toWidget(t, evaluate(static_cast<Channel>(ch), t));
            p.drawLine(prev, cur);
            prev = cur;
        }
    }

    // Draw active channel curve
    {
        QColor color = channelColors[m_activeChannel];
        p.setPen(QPen(color, 2));

        QPointF prev = toWidget(0, evaluate(m_activeChannel, 0));
        for (int x = 1; x <= 200; ++x) {
            float t = x / 200.0f;
            QPointF cur = toWidget(t, evaluate(m_activeChannel, t));
            p.drawLine(prev, cur);
            prev = cur;
        }
    }

    // Draw control points for active channel
    {
        QColor color = channelColors[m_activeChannel];
        const auto& pts = m_points[m_activeChannel];
        for (size_t i = 0; i < pts.size(); ++i) {
            QPointF wp = toWidget(pts[i].x, pts[i].y);
            p.setPen(QPen(color, 1.5));
            p.setBrush(static_cast<int>(i) == m_dragIndex ? color : bgColor);
            p.drawEllipse(wp, kPointRadius, kPointRadius);
        }
    }

    --s_paintDepth;
}

void CurveEditor::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;

    // From the first real edit onward, generate the LUT from the displayed
    // control points. Until now lut() returned the exact loaded effect LUT.
    m_hasLoadedLut[m_activeChannel] = false;

    m_dragIndex = hitTest(m_activeChannel, event->position());
    if (m_dragIndex >= 0) {
        m_dragging = true;
    } else {
        // Add new point
        QPointF norm = fromWidget(event->position());
        ControlPoint cp;
        cp.x = static_cast<float>(norm.x());
        cp.y = static_cast<float>(norm.y());

        auto& pts = m_points[m_activeChannel];
        // Insert sorted by x
        auto it = std::lower_bound(pts.begin(), pts.end(), cp,
            [](const ControlPoint& a, const ControlPoint& b) { return a.x < b.x; });
        auto insertedIt = pts.insert(it, cp);
        m_dragIndex = static_cast<int>(std::distance(pts.begin(), insertedIt));
        m_dragging = true;
        emit curveChanged();
    }
    update();
    event->accept();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging || m_dragIndex < 0) return;

    auto& pts = m_points[m_activeChannel];
    QPointF norm = fromWidget(event->position());

    // First and last points can only move vertically (x locked at 0 and 1)
    if (m_dragIndex == 0) {
        pts[m_dragIndex].y = static_cast<float>(norm.y());
    } else if (m_dragIndex == static_cast<int>(pts.size()) - 1) {
        pts[m_dragIndex].y = static_cast<float>(norm.y());
    } else {
        // Interior points: constrain x between neighbors
        float minX = pts[m_dragIndex - 1].x + 0.005f;
        float maxX = pts[m_dragIndex + 1].x - 0.005f;
        pts[m_dragIndex].x = std::clamp(static_cast<float>(norm.x()), minX, maxX);
        pts[m_dragIndex].y = static_cast<float>(norm.y());
    }

    emit curveChanged();
    update();
    event->accept();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_dragging) {
        m_dragging = false;
        m_dragIndex = -1;
        emit curveCommitted();
        update();
    }
    event->accept();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Double-click on a control point (not endpoints) to remove it
    int idx = hitTest(m_activeChannel, event->position());
    auto& pts = m_points[m_activeChannel];
    if (idx > 0 && idx < static_cast<int>(pts.size()) - 1) {
        pts.erase(pts.begin() + idx);
        emit curveChanged();
        emit curveCommitted();
        update();
    }
    event->accept();
}

} // namespace rt
