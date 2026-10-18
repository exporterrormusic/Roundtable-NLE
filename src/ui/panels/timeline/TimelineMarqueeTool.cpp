/*
 * TimelineMarqueeTool.cpp — see TimelineMarqueeTool.h.
 */

#include "panels/timeline/TimelineMarqueeTool.h"
#include "panels/timeline/TimelinePanel.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"

#include <QMouseEvent>
#include <QRubberBand>
#include <QTimer>

#include <algorithm>
#include <set>

namespace rt {

namespace {
constexpr double kMarqueeDeadzonePx = 5.0;
constexpr int    kMarqueeScrollEdgePx = 40;
constexpr double kMarqueeMaxScrollPx = 18.0;
} // namespace

TimelineMarqueeTool::TimelineMarqueeTool(TimelinePanel& panel, QPointF pressPos)
    : m_panel(panel), m_anchor(pressPos), m_cursor(pressPos)
{
}

TimelineMarqueeTool::~TimelineMarqueeTool() = default;

void TimelineMarqueeTool::move(const QMouseEvent& event, int64_t /*tickDelta*/)
{
    m_cursor = event.position();
    if (!m_started) {
        if ((m_cursor - m_anchor).manhattanLength() < kMarqueeDeadzonePx)
            return;
        start(event);
    }

    // Auto-scroll while the cursor pegs against the left or right edge of
    // the track area (Premiere), every tick for as long as it stays there.
    const int hdr = m_panel.headerWidth();
    const bool atEdge = m_cursor.x() < hdr + kMarqueeScrollEdgePx ||
                        m_cursor.x() > m_panel.width() - kMarqueeScrollEdgePx;
    if (atEdge && !m_scrollTimer->isActive())
        m_scrollTimer->start();
    else if (!atEdge && m_scrollTimer->isActive())
        m_scrollTimer->stop();

    updateBand();
    refreshSelection();
    m_panel.update();
}

void TimelineMarqueeTool::start(const QMouseEvent& event)
{
    m_started = true;
    // Decided once, from the event: the auto-scroll tick re-selects with no
    // event to read Shift from.
    m_additive = event.modifiers() & Qt::ShiftModifier;
    if (m_additive) {
        m_baseSelection = m_panel.m_selection.clips();
    } else {
        m_panel.m_selection.clear();
        m_panel.m_selectedTransitionTrack = SIZE_MAX;
        m_panel.m_selectedTransitionIndex = SIZE_MAX;
        for (auto& tw : m_panel.m_trackWidgets)
            tw->setSelectedTransition(SIZE_MAX);
        emit m_panel.selectionChanged();
    }
    if (m_panel.m_gapSelection.active) {
        m_panel.m_gapSelection.active = false;
        for (auto& tw : m_panel.m_trackWidgets)
            tw->setGapHighlight(-1, -1);
    }
    m_panel.m_dragMode = TimelinePanel::DragMode::MarqueeSelect;

    m_band = std::make_unique<QRubberBand>(QRubberBand::Rectangle, &m_panel);
    m_scrollTimer = std::make_unique<QTimer>();
    m_scrollTimer->setInterval(16);
    QObject::connect(m_scrollTimer.get(), &QTimer::timeout,
                     &m_panel, [this]() { autoScroll(); });
}

void TimelineMarqueeTool::autoScroll()
{
    const int hdr = m_panel.headerWidth();
    const double cx = m_cursor.x();
    double dx = 0.0;
    if (cx < hdr + kMarqueeScrollEdgePx)
        dx = -kMarqueeMaxScrollPx *
             std::clamp((hdr + kMarqueeScrollEdgePx - cx) / kMarqueeScrollEdgePx, 0.0, 1.0);
    else if (cx > m_panel.width() - kMarqueeScrollEdgePx)
        dx = kMarqueeMaxScrollPx *
             std::clamp((cx - (m_panel.width() - kMarqueeScrollEdgePx)) / kMarqueeScrollEdgePx, 0.0, 1.0);
    if (dx == 0.0) return;
    auto& layout = m_panel.m_layoutEngine;
    const double oldScroll = layout.scrollX();
    const double newScroll = std::max(0.0, oldScroll + dx);
    if (newScroll == oldScroll) return;
    layout.setScrollX(newScroll);
    // Keep the anchor on the same tick as the view scrolls under it.
    m_anchor.setX(m_anchor.x() - (newScroll - oldScroll));
    m_panel.onScrollChanged();
    updateBand();
    refreshSelection();
}

void TimelineMarqueeTool::updateBand()
{
    const double left = m_panel.headerWidth();
    const double right = m_panel.width();
    const double x1 = std::clamp(m_anchor.x(), left, right);
    const double x2 = std::clamp(m_cursor.x(), left, right);
    m_band->setGeometry(QRect(
        QPoint(static_cast<int>(std::min(x1, x2)),
               static_cast<int>(std::min(m_anchor.y(), m_cursor.y()))),
        QPoint(static_cast<int>(std::max(x1, x2)),
               static_cast<int>(std::max(m_anchor.y(), m_cursor.y())))));
    m_band->show();
}

void TimelineMarqueeTool::refreshSelection()
{
    const Timeline* timeline = m_panel.m_timeline;
    if (!timeline || !m_band || !m_band->isVisible())
        return;
    const QRect rb = m_band->geometry();
    if (rb.isEmpty())
        return;

    // Pixel X → timeline ticks.
    const auto& layout = m_panel.m_layoutEngine;
    const int hdr = m_panel.headerWidth();
    int64_t startTick = layout.pixelXToTime(std::max(0.0, static_cast<double>(rb.left()) - hdr));
    int64_t endTick = layout.pixelXToTime(std::max(0.0, static_cast<double>(rb.right()) - hdr));
    if (startTick > endTick) std::swap(startTick, endTick);

    // Pixel Y → track range.
    size_t topTrack = SIZE_MAX;
    size_t bottomTrack = SIZE_MAX;
    for (size_t i = 0; i < m_panel.m_trackWidgets.size(); ++i) {
        TimelineTrackWidget* tw = m_panel.m_trackWidgets[i].data();
        if (!tw) continue;
        const double trackTop = tw->mapTo(&m_panel, QPoint(0, 0)).y();
        const double trackBottom = trackTop + tw->height();
        if (rb.top() < trackBottom && rb.bottom() > trackTop) {
            if (topTrack == SIZE_MAX)
                topTrack = i;
            bottomTrack = i;
        }
    }
    if (topTrack == SIZE_MAX)
        return;   // the band is outside every track

    TimelineRect rect;
    rect.startTick = startTick;
    rect.endTick = endTick;
    rect.topTrack = topTrack;
    rect.bottomTrack = bottomTrack;

    SelectionSet& selection = m_panel.m_selection;
    if (m_additive) {
        // The selection from before the band, plus everything it touches.
        SelectionSet inRect;
        inRect.selectRect(*timeline, rect);
        std::set<std::pair<size_t, uint64_t>> combined;
        for (const auto& ref : m_baseSelection)
            combined.insert({ref.trackIndex, ref.clipId});
        for (const auto& ref : inRect.clips())
            combined.insert({ref.trackIndex, ref.clipId});
        selection.clear();
        for (const auto& [track, clip] : combined)
            selection.selectClip(ClipRef{track, clip}, true);
    } else {
        selection.selectRect(*timeline, rect);
    }
    emit m_panel.selectionChanged();
}

void TimelineMarqueeTool::release(const QMouseEvent& /*event*/)
{
    if (m_started) return;
    // A click on empty space, not a drag: clear the selection.
    m_panel.m_selection.clear();
    m_panel.m_selectedTransitionTrack = SIZE_MAX;
    m_panel.m_selectedTransitionIndex = SIZE_MAX;
    for (auto& tw : m_panel.m_trackWidgets)
        tw->setSelectedTransition(SIZE_MAX);
    emit m_panel.selectionChanged();
}

void TimelineMarqueeTool::finish()
{
    if (m_scrollTimer) m_scrollTimer->stop();
    if (m_band) m_band->hide();
}

} // namespace rt
