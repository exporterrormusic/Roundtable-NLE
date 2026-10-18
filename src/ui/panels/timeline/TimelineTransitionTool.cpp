/*
 * TimelineTransitionTool.cpp — see TimelineTransitionTool.h.
 */

#include "panels/timeline/TimelineTransitionTool.h"
#include "panels/timeline/TimelinePanel.h"
#include "widgets/TimelineTrackWidget.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "command/commands/TransitionCmds.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <QMouseEvent>
#include <QString>
#include <QToolTip>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace rt {

std::unique_ptr<TimelineTransitionTool> TimelineTransitionTool::edge(
    TimelinePanel& panel, size_t trackIndex, size_t transitionIndex,
    int64_t originalDuration)
{
    std::unique_ptr<TimelineTransitionTool> tool(new TimelineTransitionTool(panel));
    tool->m_trackIndex = trackIndex;
    tool->m_transitionIndex = transitionIndex;
    tool->m_originalDuration = originalDuration;
    return tool;
}

std::unique_ptr<TimelineTransitionTool> TimelineTransitionTool::handle(
    TimelinePanel& panel, size_t trackIndex, uint64_t clipId, ClipEdge edge,
    QPointF pressPos)
{
    std::unique_ptr<TimelineTransitionTool> tool(new TimelineTransitionTool(panel));
    tool->m_handlePending = true;
    tool->m_trackIndex = trackIndex;
    tool->m_handleClipId = clipId;
    tool->m_handleEdge = edge;
    tool->m_pressPos = pressPos;
    return tool;
}

void TimelineTransitionTool::move(const QMouseEvent& event, int64_t /*tickDelta*/)
{
    if (m_handlePending)
        dragHandle(event.position());
    else
        dragEdge(event.position());
}

void TimelineTransitionTool::dragEdge(QPointF pos)
{
    Track* track = m_panel.m_timeline->track(m_trackIndex);
    if (track && !track->isLocked()
            && m_transitionIndex < track->transitionCount()) {
        Transition t = *track->transition(m_transitionIndex);
        double px = pos.x() - m_panel.headerWidth();
        int64_t dragTick = m_panel.m_layoutEngine.pixelXToTime(px);

        constexpr int64_t kMinTransDur = 1600;
        int64_t newDur = t.duration;

        if (t.leftClipId == 0) {
            newDur = std::max(kMinTransDur, dragTick - t.editPointTick);
        } else if (t.rightClipId == 0) {
            newDur = std::max(kMinTransDur, t.editPointTick - dragTick);
        } else {
            int64_t dist = std::abs(dragTick - t.editPointTick);
            newDur = std::max(kMinTransDur, dist * 2);
        }

        // ── Clamp: don't overlap clip boundaries or other transitions ──
        int64_t maxDur = std::numeric_limits<int64_t>::max();
        if (t.leftClipId == 0) {
            // Fade-in: extends right into rightClip.
            // Clip boundary: cannot extend past right clip's end.
            size_t ri = track->findClipIndexById(t.rightClipId);
            if (ri < track->clipCount())
                maxDur = track->clip(ri)->timelineOut() - t.editPointTick;

            // Adjacent transitions: stop at the nearest transition whose
            // start is beyond our edit point.
            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transitionIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (os > t.editPointTick) {
                    int64_t limit = os - t.editPointTick;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        } else if (t.rightClipId == 0) {
            // Fade-out: extends left into leftClip.
            size_t li = track->findClipIndexById(t.leftClipId);
            if (li < track->clipCount())
                maxDur = t.editPointTick - track->clip(li)->timelineIn();

            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transitionIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (oe < t.editPointTick) {
                    int64_t limit = t.editPointTick - oe;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        } else {
            // Cross-dissolve: extends both ways.
            size_t li = track->findClipIndexById(t.leftClipId);
            size_t ri = track->findClipIndexById(t.rightClipId);
            int64_t leftLimit = std::numeric_limits<int64_t>::max();
            int64_t rightLimit = std::numeric_limits<int64_t>::max();
            if (li < track->clipCount())
                leftLimit = t.editPointTick - track->clip(li)->timelineIn();
            if (ri < track->clipCount())
                rightLimit = track->clip(ri)->timelineOut() - t.editPointTick;
            maxDur = std::min(leftLimit, rightLimit) * 2;  // dur/2 each side

            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transitionIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (os > t.editPointTick) {
                    int64_t limit = (os - t.editPointTick) * 2;
                    if (limit < maxDur) maxDur = limit;
                }
                if (oe < t.editPointTick) {
                    int64_t limit = (t.editPointTick - oe) * 2;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        }
        if (maxDur < kMinTransDur) maxDur = kMinTransDur;
        if (newDur > maxDur) newDur = maxDur;

        t.duration = newDur;

        // Execute through command stack so the project is marked
        // modified during the drag (same pattern as clip trim).
        // A macro wraps all drag ticks into a single undo step.
        if (m_panel.m_commandStack && !m_panel.m_commandStack->isMacroActive())
            m_panel.m_commandStack->beginMacro("Trim transition");
        auto cmd = std::make_unique<SetTransitionPropertyCommand>(
            track, m_transitionIndex, t);
        m_panel.executeCommand(std::move(cmd));
        showDurationTip(pos, newDur);
    }
    m_panel.onScrollChanged();
}

void TimelineTransitionTool::dragHandle(QPointF pos)
{
    // Deadzone: a plain click on a handle only selects the edge.
    if (std::abs(pos.x() - m_pressPos.x()) < 4.0) return;

    Track* track = m_panel.m_timeline->track(m_trackIndex);
    const size_t idx = track ? track->findClipIndexById(m_handleClipId)
                             : SIZE_MAX;
    if (!track || track->isLocked() || idx >= track->clipCount()) return;
    if (EditOperations::clipEdgeHasTransition(
            *track, m_handleClipId, m_handleEdge))
        return;
    const Clip* clip = track->clip(idx);

    // Only an inward drag makes a transition.
    const bool head = m_handleEdge == ClipEdge::Head;
    const int64_t dragTick =
        m_panel.m_layoutEngine.pixelXToTime(pos.x() - m_panel.headerWidth());
    const int64_t inward = head ? dragTick - clip->timelineIn()
                                : clip->timelineOut() - dragTick;
    if (inward <= 0) return;

    // The transition's inner edge follows the cursor: a one-sided fade spans
    // the whole drag, a dissolve centered on a cut spans it on both sides.
    Transition trans = EditOperations::defaultTransitionAtEdge(
        *track, *clip, m_handleEdge, inward);
    if (trans.leftClipId != 0 && trans.rightClipId != 0)
        trans.duration = inward * 2;
    if (!EditOperations::fitTransitionToAvailableDuration(*track, trans))
        return;

    // One undo step for the whole gesture: the add plus every live resize
    // (dragEdge reuses this macro; the panel closes it).
    if (m_panel.m_commandStack && !m_panel.m_commandStack->isMacroActive())
        m_panel.m_commandStack->beginMacro("Add Transition");
    m_panel.executeCommand(std::make_unique<AddTransitionCommand>(
        track, idx, idx, trans));

    size_t transIdx = SIZE_MAX;
    for (size_t i = 0; i < track->transitionCount(); ++i) {
        const Transition* t = track->transition(i);
        if (t && t->leftClipId == trans.leftClipId
            && t->rightClipId == trans.rightClipId) {
            transIdx = i;
            break;
        }
    }
    if (transIdx == SIZE_MAX) return;

    // From here on it is an ordinary drag of the new transition's inner edge.
    m_panel.clearEditPointSelection();
    m_transitionIndex = transIdx;
    m_originalDuration = 0;
    m_panel.m_selectedTransitionTrack = m_trackIndex;
    m_panel.m_selectedTransitionIndex = transIdx;
    for (auto tw : m_panel.m_trackWidgets) {
        tw->setTransitionHandleHover(0, -1);
        tw->setSelectedTransition(
            tw->trackIndex() == m_panel.m_selectedTransitionTrack ? transIdx : SIZE_MAX);
    }
    m_panel.m_dragMode = TimelinePanel::DragMode::TransitionTrim;
    m_handlePending = false;
    m_panel.setCursor(Qt::SizeHorCursor);
    emit m_panel.selectionChanged();
    dragEdge(pos);
}

void TimelineTransitionTool::showDurationTip(QPointF pos, int64_t duration)
{
    // Premiere shows the change and the total length while a transition
    // edge is dragged.
    const int64_t delta = duration - m_originalDuration;
    const QString text = QStringLiteral("%1%2   Duration %3")
        .arg(delta < 0 ? QStringLiteral("-") : QStringLiteral("+"),
             QString::fromStdString(m_panel.m_layoutEngine.formatTimecode(
                 delta < 0 ? -delta : delta)),
             QString::fromStdString(m_panel.m_layoutEngine.formatTimecode(duration)));
    QToolTip::showText(m_panel.mapToGlobal(pos.toPoint()) + QPoint(14, -36), text, &m_panel);
}

} // namespace rt
