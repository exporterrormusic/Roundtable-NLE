/*
 * TimelineTrimTool.cpp — see TimelineTrimTool.h.
 */

#include "panels/timeline/TimelineTrimTool.h"
#include "panels/timeline/TimelinePanel.h"
#include "widgets/TimelineTrackWidget.h"

#include "command/Command.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QMouseEvent>

#include <algorithm>
#include <limits>
#include <utility>

namespace rt {

TimelineTrimTool::TimelineTrimTool(TimelinePanel& panel, ClipEdge edge,
                                   ClipEdgeStart primary,
                                   std::vector<ClipEdgeStart> others, bool ripple)
    : m_panel(panel), m_edge(edge), m_primary(primary), m_others(std::move(others)),
      m_ripple(ripple)
{
}

int64_t TimelineTrimTool::frameAnchor() const
{
    return m_edge == ClipEdge::Head ? m_primary.in : m_primary.in + m_primary.duration;
}

void TimelineTrimTool::move(const QMouseEvent& event, int64_t tickDelta)
{
    // The edit-point bracket is a static "you clicked this edge" affordance;
    // once the user actually drags to trim it disappears (Premiere hides it
    // during the trim). Cheap to call every tick.
    m_panel.clearEditPointSelection();
    if (m_ripple)
        moveRipple(event, tickDelta);
    else if (m_edge == ClipEdge::Head)
        moveHead(event, tickDelta);
    else
        moveTail(event, tickDelta);
    m_panel.onScrollChanged();
    m_panel.updateClipDragAutoScroll(event.position());
}

int64_t TimelineTrimTool::snapEdge(const QMouseEvent& event, int64_t tick)
{
    if (event.modifiers() & Qt::ControlModifier) {   // Ctrl: no snapping
        m_panel.m_snapEngine.resetHysteresis();
        m_panel.setSnapIndicator(-1);
        return tick;
    }
    const auto result = m_panel.m_snapEngine.snap(tick);
    m_panel.setSnapIndicator(result.didSnap ? result.snappedTick : -1);
    return result.didSnap ? result.snappedTick : tick;
}

void TimelineTrimTool::moveRipple(const QMouseEvent& event, int64_t tickDelta)
{
    Timeline& timeline = *m_panel.m_timeline;
    const ClipRef& ref = m_primary.ref;
    Track* track = timeline.track(ref.trackIndex);
    const size_t index = track ? track->findClipIndexById(ref.clipId) : SIZE_MAX;
    if (!track || index >= track->clipCount()) return;
    const Clip* clip = track->clip(index);
    const int64_t origTail = m_primary.in + m_primary.duration;

    int64_t edgeTick = m_primary.in;
    if (m_edge == ClipEdge::Tail) {
        // The tail follows the mouse; what follows goes with it.
        int64_t newTail = snapEdge(event, origTail + tickDelta);
        if (newTail <= m_primary.in) newTail = m_primary.in + 1;
        m_panel.executeCommand(EditOperations::rippleTrim(
            timeline, ref.trackIndex, ref.clipId, ClipEdge::Tail, newTail));
        edgeTick = clip->timelineOut();
    } else {
        // The head stays put: the drag says how much source to cut at the
        // head (negative: restore), and rippleTrim applies the difference
        // from what is cut already. Nothing to snap: the edge doesn't move.
        m_panel.m_snapEngine.resetHysteresis();
        m_panel.setSnapIndicator(-1);
        const int64_t cut = std::min(tickDelta, m_primary.duration - 1);
        const int64_t alreadyCut = m_primary.duration - clip->duration();
        if (cut != alreadyCut)
            m_panel.executeCommand(EditOperations::rippleTrim(
                timeline, ref.trackIndex, ref.clipId, ClipEdge::Head,
                clip->timelineIn() + (cut - alreadyCut)));
    }

    for (auto tw : m_panel.m_trackWidgets)
        tw->setHoverEdgeTick(tw->trackIndex() == ref.trackIndex ? edgeTick : -1);
}

void TimelineTrimTool::moveHead(const QMouseEvent& event, int64_t tickDelta)
{
    Timeline& timeline = *m_panel.m_timeline;
    int64_t newHead = snapEdge(event, m_primary.in + tickDelta);

    if (Track* track = timeline.track(m_primary.ref.trackIndex)) {
        const int64_t origTail = m_primary.in + m_primary.duration;
        int64_t leftLimit = std::numeric_limits<int64_t>::min();
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* other = track->clip(ci);
            if (!other || other->id() == m_primary.ref.clipId) continue;
            if (other->timelineOut() <= m_primary.in && other->timelineOut() > leftLimit)
                leftLimit = other->timelineOut();
        }
        if (newHead < leftLimit) newHead = leftLimit;
        if (newHead >= origTail) newHead = origTail - 1;
    }
    const int64_t headDelta = newHead - m_primary.in;

    m_panel.executeCommand(EditOperations::trimClip(
        timeline, m_primary.ref.trackIndex, m_primary.ref.clipId, ClipEdge::Head, newHead));

    // The other selected clips' heads move by the same delta — including
    // clips on other tracks (an A/V pair). Each is clamped to its own left
    // neighbour and its own original tail, so a shorter clip is never
    // over-trimmed.
    for (const auto& other : m_others) {
        if (other.ref.clipId == m_primary.ref.clipId &&
            other.ref.trackIndex == m_primary.ref.trackIndex) continue;
        Track* track = timeline.track(other.ref.trackIndex);
        if (!track || track->findClipIndexById(other.ref.clipId) >= track->clipCount())
            continue;
        int64_t head = other.in + headDelta;
        const int64_t origTail = other.in + other.duration;
        if (head < 0) head = 0;
        if (head >= origTail) head = origTail - 1;
        int64_t leftLimit = std::numeric_limits<int64_t>::min();
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* c = track->clip(ci);
            if (!c || c->id() == other.ref.clipId) continue;
            if (c->timelineOut() <= other.in && c->timelineOut() > leftLimit)
                leftLimit = c->timelineOut();
        }
        if (head < leftLimit) head = leftLimit;
        m_panel.executeCommand(EditOperations::trimClip(
            timeline, other.ref.trackIndex, other.ref.clipId, ClipEdge::Head, head));
    }

    for (auto tw : m_panel.m_trackWidgets)
        tw->setHoverEdgeTick(tw->trackIndex() == m_primary.ref.trackIndex ? newHead : -1);
}

void TimelineTrimTool::moveTail(const QMouseEvent& event, int64_t tickDelta)
{
    Timeline& timeline = *m_panel.m_timeline;
    const int64_t origTail = m_primary.in + m_primary.duration;
    int64_t newTail = snapEdge(event, origTail + tickDelta);

    if (Track* track = timeline.track(m_primary.ref.trackIndex)) {
        int64_t rightLimit = std::numeric_limits<int64_t>::max();
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* other = track->clip(ci);
            if (!other || other->id() == m_primary.ref.clipId) continue;
            if (other->timelineIn() >= origTail && other->timelineIn() < rightLimit)
                rightLimit = other->timelineIn();
        }
        if (newTail > rightLimit) newTail = rightLimit;
        if (newTail <= m_primary.in) newTail = m_primary.in + 1;
    }
    const int64_t tailDelta = newTail - origTail;

    m_panel.executeCommand(EditOperations::trimClip(
        timeline, m_primary.ref.trackIndex, m_primary.ref.clipId, ClipEdge::Tail, newTail));

    // The other selected clips' tails move by the same delta, across tracks
    // too, each clamped to its own right neighbour and its own original head.
    for (const auto& other : m_others) {
        if (other.ref.clipId == m_primary.ref.clipId &&
            other.ref.trackIndex == m_primary.ref.trackIndex) continue;
        Track* track = timeline.track(other.ref.trackIndex);
        if (!track || track->findClipIndexById(other.ref.clipId) >= track->clipCount())
            continue;
        const int64_t otherTail = other.in + other.duration;
        int64_t tail = otherTail + tailDelta;
        int64_t rightLimit = std::numeric_limits<int64_t>::max();
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* c = track->clip(ci);
            if (!c || c->id() == other.ref.clipId) continue;
            if (c->timelineIn() >= otherTail && c->timelineIn() < rightLimit)
                rightLimit = c->timelineIn();
        }
        if (tail > rightLimit) tail = rightLimit;
        if (tail <= other.in) tail = other.in + 1;
        m_panel.executeCommand(EditOperations::trimClip(
            timeline, other.ref.trackIndex, other.ref.clipId, ClipEdge::Tail, tail));
    }

    for (auto tw : m_panel.m_trackWidgets)
        tw->setHoverEdgeTick(tw->trackIndex() == m_primary.ref.trackIndex ? newTail : -1);
}

} // namespace rt
