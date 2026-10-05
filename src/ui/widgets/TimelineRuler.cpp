/*
 * TimelineRuler.cpp — Timecode ruler widget.
 * Step 12: Timeline Panel — Core UI
 */

#include "widgets/TimelineRuler.h"
#include "Theme.h"

#include "timeline/TimelineLayoutEngine.h"
#include "timeline/Marker.h"

#include <spdlog/spdlog.h>

#include <QPainter>
#include <QMouseEvent>

#include <chrono>
#include <cmath>

namespace rt {

TimelineRuler::TimelineRuler(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(kRulerHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);

    m_font = QFont("Segoe UI");
    m_font.setPixelSize(11);

    m_majorPen = QPen(Theme::colors().textPrimary, 1.0);
    m_minorPen = QPen(Theme::colors().textDisabled, 1.0);
}

void TimelineRuler::setLayoutEngine(const TimelineLayoutEngine* engine)
{
    m_engine = engine;
    update();
}

void TimelineRuler::setPlayheadTick(int64_t tick)
{
    if (m_playheadTick == tick) return;
    m_playheadTick = tick;

    // Erase strip at the LAST PAINTED pixel position (immune to zoom changes)
    // and schedule strip at the new position.
    if (m_engine) {
        auto scheduleStripPx = [this](double px) {
            if (px >= -10 && px <= width() + 10) {
                int x = std::max(0, static_cast<int>(px) - 8);
                update(QRect(x, 0, 17, height()));
            }
        };
        scheduleStripPx(m_lastPaintedPlayheadPx);  // erase where it was actually drawn
        scheduleStripPx(m_engine->timeToPixelX(tick));  // draw new
    } else {
        update();
    }
}

void TimelineRuler::setInOutRange(int64_t inTick, int64_t outTick)
{
    m_inPoint = inTick;
    m_outPoint = outTick;
    update();
}

void TimelineRuler::clearInOutRange()
{
    m_inPoint = -1;
    m_outPoint = -1;
    update();
}

void TimelineRuler::setMarkers(const std::vector<Marker>* markers)
{
    m_markers = markers;
    update();
}

void TimelineRuler::setRenderBar(std::vector<RenderBarSegment> segments)
{
    m_renderBar = std::move(segments);
    update();
}

void TimelineRuler::clearRenderBar()
{
    m_renderBar.clear();
    update();
}

QSize TimelineRuler::sizeHint() const
{
    return {400, kRulerHeight};
}

QSize TimelineRuler::minimumSizeHint() const
{
    return {100, kRulerHeight};
}

void TimelineRuler::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }
    if (!m_engine) { --s_paintDepth; return; }

    auto rulerT0 = std::chrono::steady_clock::now();

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setFont(m_font);

    int w = width();
    int h = height();

    const auto& tc = Theme::colors();

    // Background
    painter.fillRect(rect(), tc.surface0);

    // Bottom border line
    painter.setPen(tc.trackDivider);
    painter.drawLine(0, h - 1, w, h - 1);

    // ── Render bar (Premiere Pro style colored strip at top of ruler) ─
    if (!m_renderBar.empty()) {
        constexpr int barHeight = 4;
        for (const auto& seg : m_renderBar) {
            double x0 = m_engine->timeToPixelX(seg.startTick);
            double x1 = m_engine->timeToPixelX(seg.endTick);
            if (x1 < 0 || x0 > w) continue;
            QColor barColor;
            switch (seg.status) {
                case RenderBarStatus::NeedsRender: barColor = QColor(0xCC, 0x33, 0x33); break;
                case RenderBarStatus::Mixed:       barColor = QColor(0xCC, 0xCC, 0x33); break;
                case RenderBarStatus::Cached:      barColor = QColor(0x33, 0xCC, 0x33); break;
                case RenderBarStatus::RealTime:    barColor = QColor(0x33, 0xCC, 0x33, 0x80); break;
                default: continue;
            }
            painter.fillRect(QRectF(x0, 0, x1 - x0, barHeight), barColor);
        }
    }

    // ── In/out range highlight (Premiere Pro style) ────────────────
    if (m_inPoint >= 0 || m_outPoint >= 0)
    {
        double inX  = (m_inPoint >= 0)  ? m_engine->timeToPixelX(m_inPoint)  : 0.0;
        double outX = (m_outPoint >= 0) ? m_engine->timeToPixelX(m_outPoint) : static_cast<double>(w);

        // Dim the regions OUTSIDE the in/out range
        QColor dimOverlay(0, 0, 0, 100);

        if (m_inPoint >= 0 && inX > 0)
            painter.fillRect(QRectF(0, 0, inX, h), dimOverlay);

        if (m_outPoint >= 0 && outX < w)
            painter.fillRect(QRectF(outX, 0, w - outX, h), dimOverlay);

        // In-point — thin 1px accent line only (Premiere style, no brackets/arrows)
        if (m_inPoint >= 0)
        {
            double ix = m_engine->timeToPixelX(m_inPoint);
            painter.setPen(QPen(tc.accent, 1.0));
            painter.drawLine(QPointF(ix, 0), QPointF(ix, h));
        }

        // Out-point — thin 1px accent line only
        if (m_outPoint >= 0)
        {
            double ox = m_engine->timeToPixelX(m_outPoint);
            painter.setPen(QPen(tc.accent, 1.0));
            painter.drawLine(QPointF(ox, 0), QPointF(ox, h));
        }
    }

    // Ruler marks
    auto marks = m_engine->computeRulerMarks();
    for (const auto& mark : marks)
    {
        double px = m_engine->timeToPixelX(mark.tick);

        if (mark.isMajor)
        {
            painter.setPen(m_majorPen);
            painter.drawLine(QPointF(px, h * 0.3), QPointF(px, h - 1));

            if (!mark.label.empty())
            {
                painter.setPen(tc.textSecondary);
                painter.drawText(QPointF(px + 4, h * 0.35 + painter.fontMetrics().ascent()),
                                 QString::fromStdString(mark.label));
            }
        }
        else
        {
            painter.setPen(m_minorPen);
            painter.drawLine(QPointF(px, h * 0.65), QPointF(px, h - 1));
        }
    }

    // Markers (small colored triangles along the bottom edge of the ruler)
    if (m_markers && !m_markers->empty())
    {
        for (const auto& marker : *m_markers)
        {
            double mx = m_engine->timeToPixelX(marker.time);
            if (mx < -8 || mx > w + 8) continue;

            QColor mc = QColor::fromRgba(marker.color);
            painter.setPen(Qt::NoPen);
            painter.setBrush(mc);

            // Small downward-pointing triangle at bottom of ruler
            QPolygonF tri;
            tri << QPointF(mx - 4, h - 8)
                << QPointF(mx + 4, h - 8)
                << QPointF(mx, h - 1);
            painter.drawPolygon(tri);

            // Draw label if there's room
            if (!marker.label.empty()) {
                painter.setPen(mc.lighter(150));
                QFont mf("Segoe UI", 7);
                painter.setFont(mf);
                painter.drawText(QPointF(mx + 5, h - 2),
                                 QString::fromStdString(marker.label));
                painter.setFont(m_font); // restore
            }
        }
    }

    // Playhead triangle
    {
        double px = m_engine->timeToPixelX(m_playheadTick);
        m_lastPaintedPlayheadPx = px;  // track for correct dirty-rect erase
        painter.setPen(Qt::NoPen);
        painter.setBrush(tc.playhead);

        QPolygonF triangle;
        triangle << QPointF(px - 6, 0)
                 << QPointF(px + 6, 0)
                 << QPointF(px, 10);
        painter.drawPolygon(triangle);

        // Playhead line
        painter.setPen(QPen(tc.playhead, 1.0));
        painter.drawLine(QPointF(px, 10), QPointF(px, h));
    }

    // ── Ruler paint perf logging ────────────────────────────────────────
    {
        double rulerMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rulerT0).count();
        if (rulerMs > 4.0) {
            spdlog::info("[PERF] Ruler::paintEvent: {:.1f}ms  marks={}",
                         rulerMs, marks.size());
        }
    }

    --s_paintDepth;
}

void TimelineRuler::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_scrubbing = true;
        scrubToPosition(event->pos().x());
    }
}

void TimelineRuler::mouseMoveEvent(QMouseEvent* event)
{
    if (m_scrubbing)
    {
        scrubToPosition(event->pos().x());
    }
}

void TimelineRuler::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_scrubbing = false;
    }
}

void TimelineRuler::scrubToPosition(int x)
{
    if (!m_engine) return;
    // Park on the start of the frame under the cursor, like other NLEs:
    // the picture is the same, and edits at the playhead stay frame-exact.
    const int64_t tick = m_engine->frameStartAt(
        m_engine->pixelXToTime(static_cast<double>(x)));
    m_playheadTick = tick;
    update();
    emit playheadScrubbed(tick);
}

} // namespace rt
