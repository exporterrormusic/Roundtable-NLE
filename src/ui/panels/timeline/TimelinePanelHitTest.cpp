/*
 * TimelinePanelHitTest.cpp — Hit-testing and drag-reorder helpers.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: hitTestClip(), hitTestTrack(), hitTestClipEdge(),
 *           computeReorderInsertionIndex(), updateReorderOverlay().
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include <QPoint>

#include <spdlog/spdlog.h>

#include <limits>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Hit testing
// ═════════════════════════════════════════════════════════════════════════════

std::optional<ClipRef> TimelinePanel::hitTestClip(const QPointF& pos) const
{
    if (!m_timeline) return std::nullopt;

    double px = pos.x() - headerWidth();
    int64_t tick = m_layoutEngine.pixelXToTime(px);
    size_t ti = hitTestTrack(pos.y());

    if (ti >= m_timeline->trackCount()) return std::nullopt;

    const Track* track = m_timeline->track(ti);
    for (size_t ci = 0; ci < track->clipCount(); ++ci)
    {
        const Clip* clip = track->clip(ci);
        if (tick >= clip->timelineIn() && tick < clip->timelineOut())
            return ClipRef{ti, clip->id()};
    }
    return std::nullopt;
}

size_t TimelinePanel::hitTestTrack(double y) const
{
    if (!m_timeline) return SIZE_MAX;

    // Use the actual widget positions of the track widgets.
    // The track widgets live inside m_trackContentArea which is inside
    // m_verticalScroll.  We map each widget's position to TimelinePanel
    // coordinates for accurate hit-testing that accounts for scroll offset.
    for (size_t i = 0; i < m_trackWidgets.size(); ++i)
    {
        auto tw = m_trackWidgets[i];
        QPoint topLeft = tw->mapTo(this, QPoint(0, 0));
        double trackTop    = topLeft.y();
        double trackBottom = trackTop + tw->height();
        if (y >= trackTop && y < trackBottom)
            return i;
    }
    return SIZE_MAX;
}

ClipEdge TimelinePanel::hitTestClipEdge(const QPointF& pos, const ClipRef& ref) const
{
    // Default to head — the actual edge proximity check is done in mousePressEvent
    (void)pos;
    (void)ref;
    return ClipEdge::Head;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag-reorder helpers: phantom insertion line + insertion-index mapping
// ═════════════════════════════════════════════════════════════════════════════

size_t TimelinePanel::computeReorderInsertionIndex(const QPoint& globalMousePos) const
{
    // Map global Y to the track-header column in local coordinates, then find
    // which track-gap the cursor is closest to. Returns index in [0..count].
    if (m_trackHeaders.empty()) return 0;
    if (!m_trackHeaderArea) return 0;

    const QPoint localInArea = m_trackHeaderArea->mapFromGlobal(globalMousePos);
    const int y = localInArea.y();

    // Headers are laid out top-to-bottom inside m_trackHeaderArea with a
    // leading stretch.  Find the closest gap (before/after each header).
    // Skip the V/A divider row — it isn't a real track, so an insertion
    // index pointing AT the divider would put a reordered track on the
    // wrong side of the V/A split.
    int bestIdx = 0;
    int bestDist = std::numeric_limits<int>::max();
    for (size_t i = 0; i < m_trackHeaders.size(); ++i) {
        auto hdr = m_trackHeaders[i];
        if (!hdr) continue;
        const Track* t = (m_timeline && i < m_timeline->trackCount())
                             ? m_timeline->track(i) : nullptr;
        if (t && t->isDivider()) continue;
        int top    = hdr->y();
        int bottom = hdr->y() + hdr->height();
        int mid    = (top + bottom) / 2;
        // Candidate "above this track" (insertion index = i)
        int dTop = std::abs(y - top);
        if (dTop < bestDist) { bestDist = dTop; bestIdx = static_cast<int>(i); }
        // Candidate "below this track" (insertion index = i + 1)
        int dBot = std::abs(y - bottom);
        if (dBot < bestDist) { bestDist = dBot; bestIdx = static_cast<int>(i) + 1; }
        // If cursor is strictly inside the upper half, treat as "above"
        if (y >= top && y <= mid) {
            bestIdx = static_cast<int>(i);
            break;
        }
        if (y > mid && y <= bottom) {
            bestIdx = static_cast<int>(i) + 1;
            break;
        }
    }
    if (bestIdx < 0) bestIdx = 0;
    const int maxIdx = static_cast<int>(m_trackHeaders.size());
    if (bestIdx > maxIdx) bestIdx = maxIdx;
    return static_cast<size_t>(bestIdx);
}

void TimelinePanel::updateReorderOverlay(const QPoint& globalMousePos)
{
    if (!m_ghostOverlay) return;
    size_t insertIdx = computeReorderInsertionIndex(globalMousePos);

    // Compute Y of the insertion line in TimelinePanel coords, spanning the
    // full width of the panel so it's visible over both header and tracks.
    int lineY = 0;
    if (m_trackHeaders.empty() || !m_trackHeaderArea) {
        lineY = 0;
    } else if (insertIdx == 0) {
        auto first = m_trackHeaders.front();
        QPoint tl = first->mapTo(this, QPoint(0, 0));
        lineY = tl.y();
    } else if (insertIdx >= m_trackHeaders.size()) {
        auto last = m_trackHeaders.back();
        QPoint tl = last->mapTo(this, QPoint(0, last->height()));
        lineY = tl.y();
    } else {
        auto hdr = m_trackHeaders[insertIdx];
        QPoint tl = hdr->mapTo(this, QPoint(0, 0));
        lineY = tl.y();
    }

    constexpr int kBarH = 3;
    m_ghostOverlay->setGeometry(0, lineY - kBarH / 2, width(), kBarH);
    m_ghostOverlay->reorderMode = true;
    m_ghostOverlay->raise();
    m_ghostOverlay->show();
    m_ghostOverlay->update();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Snap indicator
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::setSnapIndicator(int64_t tick)
{
    m_snapIndicatorTick = tick;
    for (auto& twPtr : m_trackWidgets)
        if (auto* tw = twPtr.data())
            tw->setSnapIndicatorTick(tick);
}

void TimelinePanel::setEditPointSelection(size_t trackIndex, int64_t tick,
                                          EditPointSide side)
{
    for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
        auto* tw = m_trackWidgets[i].data();
        if (!tw) continue;
        if (i == trackIndex)
            tw->setEditPointTick(tick, side);
        else
            tw->setEditPointTick(-1);
    }
}

void TimelinePanel::clearEditPointSelection()
{
    for (auto& twPtr : m_trackWidgets)
        if (auto* tw = twPtr.data())
            tw->setEditPointTick(-1);
}

void TimelinePanel::setLinkPartnersSelected(const ClipRef& seed, bool selected)
{
    if (!m_linkedSelectionEnabled) return;
    if (!m_timeline) return;
    Track* seedTrack = m_timeline->track(seed.trackIndex);
    if (!seedTrack) return;
    size_t seedIdx = seedTrack->findClipIndexById(seed.clipId);
    if (seedIdx >= seedTrack->clipCount()) return;
    const uint64_t linkId = seedTrack->clip(seedIdx)->linkId();
    if (linkId == 0) return;  // unlinked clip — nothing to do

    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* tr = m_timeline->track(ti);
        if (!tr) continue;
        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
            const Clip* c = tr->clip(ci);
            if (!c) continue;
            if (c->linkId() != linkId) continue;
            if (c->id() == seed.clipId) continue;  // skip the seed itself
            ClipRef partner{ ti, c->id() };
            if (selected)
                m_selection.selectClip(partner, /*addToSelection*/ true);
            else
                m_selection.deselectClip(partner);
        }
    }
}

} // namespace rt
