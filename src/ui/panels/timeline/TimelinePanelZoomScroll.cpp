/*
 * TimelinePanelZoomScroll.cpp — Zoom, scroll, playhead, and wheel handling.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: zoomToFit(), zoomIn(), zoomOut(), wheelEvent(), setFrameRate(),
 *           headerWidth(), updateMinHeaderWidth(), onScrollChanged(),
 *           setPlayheadPosition(), onRulerScrub(), updatePlayheadOverlay(),
 *           updateInOutRange().
 */

#include "panels/timeline/TimelinePanel.h"
#include "widgets/TimelineRuler.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/NLEScrollBar.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QWheelEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>

#include <spdlog/spdlog.h>

#include <chrono>
#include <algorithm>
#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Zoom operations
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::zoomToFit()
{
    if (!m_timeline) return;
    int64_t dur = m_timeline->duration();
    if (dur <= 0) dur = TimelineLayoutEngine::secondsToTicks(60.0);
    m_layoutEngine.zoomToFit(0, dur, m_ruler->width());
    onScrollChanged();
}

void TimelinePanel::zoomIn()
{
    double playheadPx = m_layoutEngine.timeToPixelX(m_playheadTick);
    m_layoutEngine.zoomAt(playheadPx, 2.0);
    // Center on playhead: adjust scroll so playhead moves to viewport center
    double viewW = std::max(m_ruler->width(), 100);
    double centerPx = viewW / 2.0;
    double newScroll = m_layoutEngine.scrollX() + (playheadPx - centerPx);
    m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
    onScrollChanged();
}

void TimelinePanel::zoomOut()
{
    double playheadPx = m_layoutEngine.timeToPixelX(m_playheadTick);
    m_layoutEngine.zoomAt(playheadPx, 0.5);
    double viewW = std::max(m_ruler->width(), 100);
    double centerPx = viewW / 2.0;
    double newScroll = m_layoutEngine.scrollX() + (playheadPx - centerPx);
    m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
    onScrollChanged();
}

void TimelinePanel::setFrameRate(double fps)
{
    m_layoutEngine.setFrameRate(fps);
    m_ruler->update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Header width helpers
// ═════════════════════════════════════════════════════════════════════════════

int TimelinePanel::headerWidth() const
{
    // Include the splitter handle width so that converting from panel
    // coordinates to viewport-relative coordinates is correct.
    // Without this, all mouse-hit-testing is off by 3px (the handle).
    constexpr int kHandleW = 3;
    return (m_headerScroll ? m_headerScroll->width() : TrackHeader::kHeaderWidth) + kHandleW;
}

void TimelinePanel::updateMinHeaderWidth()
{
    if (!m_timeline || !m_headerScroll) return;

    // Layout constants from TrackHeader::buttonRect / paintEvent
    constexpr int leftPad    = 14;   // label starts at x=14 (past target indicator)
    constexpr int rightPad   = 4;    // right edge padding
    constexpr int bw         = 26;   // button width
    constexpr int gapX       = 3;    // gap between buttons
    constexpr int numButtons = 4;
    constexpr int labelH     = 22;
    constexpr int bh         = 20;
    constexpr int shortThreshold = labelH + bh + 4;  // 46

    // Total button row width (all 4 inline)
    constexpr int totalBtnW  = numButtons * bw + (numButtons - 1) * gapX; // 113

    QFont labelFont("Segoe UI", 10, QFont::Bold);
    QFontMetrics fm(labelFont);

    int minW = TrackHeader::kHeaderWidth; // absolute floor

    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        const Track* track = m_timeline->track(i);
        int nameW = fm.horizontalAdvance(QString::fromStdString(track->name()));
        int trackH = static_cast<int>(track->height());

        int needed;
        if (trackH < shortThreshold) {
            // Short: name and buttons are on the same row
            // Layout: [14px pad][name][2px gap][buttons (113px)][4px pad]
            needed = leftPad + nameW + 2 + totalBtnW + rightPad;
        } else {
            // Tall: name is above buttons, use the wider of the two
            int nameRow = leftPad + nameW + rightPad;
            int btnRow  = leftPad + totalBtnW + rightPad;
            needed = std::max(nameRow, btnRow);
        }
        if (needed > minW)
            minW = needed;
    }

    // Capture the width BEFORE applying the new minimum, because
    // setMinimumWidth() can cause the QSplitter to relayout immediately,
    // which would make the subsequent width() check always pass.
    int prevW = m_headerScroll->width();

    m_headerScroll->setMinimumWidth(minW);

    // If the header needs to grow, explicitly set splitter sizes
    if (prevW < minW && m_headerSplitter) {
        int total = m_headerSplitter->width();
        int hw    = m_headerSplitter->handleWidth();
        m_headerSplitter->setSizes({minW, std::max(1, total - minW - hw)});
        m_trackHeaderArea->setFixedWidth(minW);
        for (auto hdr : m_trackHeaders)
            hdr->setFixedWidth(minW);
    }

    // Always sync spacers with the actual header width so the ruler and
    // scrollbar stay aligned with the track content viewport.
    if (m_headerSplitter) {
        int actualW = m_headerScroll->width();
        int handle  = m_headerSplitter->handleWidth();
        m_headerSpacer->setFixedWidth(actualW + handle);
        m_scrollSpacer->setFixedWidth(actualW + handle);
        // Defer scroll/playhead update until Qt finishes the layout pass.
        QTimer::singleShot(0, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            onScrollChanged();
        });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  wheelEvent — Zoom/scroll/track-height via mouse wheel
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier)
    {
        // Ctrl+wheel = zoom anchored to cursor (Premiere behavior).
        // zoomAt() already adjusts scrollX so the tick under the cursor
        // stays under the cursor — DON'T then re-pan to viewport centre,
        // that just throws the anchor away and feels like the timeline
        // is sliding around as you scroll-zoom.
        double factor = (event->angleDelta().y() > 0) ? 1.15 : 1.0 / 1.15;
        double px = event->position().x() - headerWidth();
        m_layoutEngine.zoomAt(px, factor);
        onScrollChanged();
        event->accept();
    }
    else if (event->modifiers() & Qt::ShiftModifier)
    {
        // Shift+wheel = resize all track heights (Premiere Pro style)
        if (!m_timeline) { event->accept(); return; }
        float delta = (event->angleDelta().y() > 0) ? 10.0f : -10.0f;
        constexpr float kMinHeight = 30.0f;
        constexpr float kMaxHeight = 300.0f;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            float h = std::clamp(t->height() + delta, kMinHeight, kMaxHeight);
            t->setHeight(h);
        }
        rebuildTracks();
        event->accept();
    }
    else
    {
        // Regular wheel = horizontal scroll (Premiere Pro style), always —
        // even when track content overflows vertically.  To scroll vertically
        // the user hovers directly over the vertical scroll bar; that widget
        // receives wheel events itself and is not intercepted by this panel's
        // event filter (which only covers the QScrollArea and its viewport,
        // not the scroll bar child).
        double delta = (event->angleDelta().y() > 0) ? -50.0 : 50.0;
        m_layoutEngine.setScrollX(m_layoutEngine.scrollX() + delta);
        onScrollChanged();
        event->accept();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Playhead / ruler scrub
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::onRulerScrub(int64_t tick)
{
    m_playheadTick = tick;
    emit playheadMoved(tick);
    // Store tick in track widgets (for static rendering) without repainting.
    // The playhead overlay widget handles the moving line.
    // Use a COPY of the widget vector — if rebuildTracks is triggered during
    // iteration (e.g. from a signal emitted above), the original vector is
    // cleared but our copy stays valid. The old widgets are hidden + deferred-
    // deleted, so calling setPlayheadTickNoRepaint on them is safe.
    {
        auto widgets = m_trackWidgets;
        for (auto tw : widgets)
            tw->setPlayheadTickNoRepaint(tick);
    }
    updatePlayheadOverlay();
}

void TimelinePanel::setPlayheadPosition(int64_t tick)
{
    m_playheadTick = tick;
    m_ruler->setPlayheadTick(tick);

    // Auto-scroll to keep playhead visible
    double newScroll = m_layoutEngine.scrollXToShow(tick);
    bool scrolled = false;
    if (newScroll != m_layoutEngine.scrollX())
    {
        m_layoutEngine.setScrollX(newScroll);
        onScrollChanged();  // already calls tw->update() for all tracks
        scrolled = true;
    }

    // Store the tick in each track widget (for static rendering when paused)
    // but do NOT trigger a full repaint — the overlay widget handles the
    // playhead line at near-zero cost.
    for (auto tw : m_trackWidgets)
        tw->setPlayheadTickNoRepaint(tick);

    // Reposition the lightweight playhead overlay
    if (!scrolled)
        updatePlayheadOverlay();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scroll / overlay
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::onScrollChanged()
{
    auto scrollT0 = std::chrono::steady_clock::now();

    // Keep total duration in sync so the scrollbar range stays accurate.
    if (m_timeline)
        m_layoutEngine.setTotalDuration(m_timeline->duration());

    m_ruler->update();
    m_scrollBar->update();
    // Use a COPY of the widget vector (see onRulerScrub for rationale).
    {
        auto widgets = m_trackWidgets;
        for (auto tw : widgets)
            tw->update();
    }

    double scrollMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - scrollT0).count();
    if (scrollMs > 4.0) {
        spdlog::info("[PERF] onScrollChanged: {:.1f}ms  tracks={}", scrollMs, m_trackWidgets.size());
    }

    // Keep the playhead overlay position in sync after scroll changes.
    updatePlayheadOverlay();
}

void TimelinePanel::updatePlayheadOverlay()
{
    if (!m_playheadOverlay || !m_verticalScroll->viewport()) return;

    // Convert playhead tick to pixel X in the scroll viewport
    double px = m_layoutEngine.timeToPixelX(m_playheadTick);
    int x = static_cast<int>(std::round(px)) - 1; // center the 3px-wide widget
    int vpH = m_verticalScroll->viewport()->height();

    if (x < -3 || x > m_verticalScroll->viewport()->width() + 3) {
        m_playheadOverlay->setVisible(false);
    } else {
        m_playheadOverlay->setGeometry(x, 0, 3, vpH);
        m_playheadOverlay->setVisible(true);
        m_playheadOverlay->raise();
    }
}

void TimelinePanel::updateInOutRange()
{
    if (!m_timeline || !m_ruler) return;
    int64_t inPt  = m_timeline->inPoint();
    int64_t outPt = m_timeline->outPoint();
    if (inPt >= 0 || outPt >= 0) {
        m_ruler->setInOutRange(inPt, outPt);
    } else {
        m_ruler->clearInOutRange();
    }
    m_ruler->update();

    // Push in/out points to every track widget so they draw overlays
    for (auto tw : m_trackWidgets)
        tw->setInOutPoints(inPt, outPt);
}

} // namespace rt
