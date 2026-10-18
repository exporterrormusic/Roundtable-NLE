/*
 * TimelineRollTool.cpp — see TimelineRollTool.h.
 */

#include "panels/timeline/TimelineRollTool.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <QMouseEvent>

#include <algorithm>
#include <utility>

namespace rt {

TimelineRollTool::TimelineRollTool(TimelinePanel& panel, Seam primary,
                                   std::vector<Seam> extras,
                                   int64_t minEditPoint, int64_t maxEditPoint)
    : m_panel(panel), m_primary(primary), m_extras(std::move(extras)),
      m_minEditPoint(minEditPoint), m_maxEditPoint(maxEditPoint)
{
}

void TimelineRollTool::move(const QMouseEvent& event, int64_t tickDelta)
{
    // Snap first, but respect the precomputed clamp — never let the snap
    // drag the seam past a source-content limit, otherwise the indicator
    // would appear at a tick the seam can't actually reach.
    const int64_t rawEditPoint = m_primary.origEditPoint + tickDelta;
    int64_t newEditPoint = std::clamp(rawEditPoint, m_minEditPoint, m_maxEditPoint);
    if (event.modifiers() & Qt::ControlModifier) {   // Ctrl: no snapping
        m_panel.m_snapEngine.resetHysteresis();
        m_panel.setSnapIndicator(-1);
    } else {
        const auto snap = m_panel.m_snapEngine.snap(rawEditPoint);
        if (snap.didSnap && snap.snappedTick >= m_minEditPoint
            && snap.snappedTick <= m_maxEditPoint) {
            newEditPoint = snap.snappedTick;
            m_panel.setSnapIndicator(snap.snappedTick);
        } else {
            m_panel.setSnapIndicator(-1);
        }
    }

    applySeamLive(m_primary, newEditPoint);
    // Other selected tracks' seams move by the same delta (the press already
    // narrowed the range so each stays within its own limits).
    const int64_t delta = newEditPoint - m_primary.origEditPoint;
    for (const auto& seam : m_extras)
        applySeamLive(seam, seam.origEditPoint + delta);

    // Slide the edit-point brackets along with the seam so the user has a
    // stable visual anchor for where the cut is landing.
    m_panel.setEditPointSelection(m_primary.trackIndex, newEditPoint, EditPointSide::Both);
    m_panel.onScrollChanged();
}

void TimelineRollTool::applySeamLive(const Seam& seam, int64_t newEditPoint)
{
    Track* track = m_panel.m_timeline->track(seam.trackIndex);
    if (!track || track->isLocked()) return;
    const size_t li = track->findClipIndexById(seam.leftClipId);
    const size_t ri = track->findClipIndexById(seam.rightClipId);
    if (li >= track->clipCount() || ri >= track->clipCount()) return;

    const int64_t rightEnd = seam.rightOrigIn + seam.rightOrigDur;
    Clip* lc = track->clip(li);
    Clip* rc = track->clip(ri);
    lc->setDuration(newEditPoint - seam.leftOrigIn);
    rc->setTimelineIn(newEditPoint);
    rc->setDuration(rightEnd - newEditPoint);
    rc->setSourceIn(seam.rightOrigSrcIn + (newEditPoint - seam.rightOrigIn));

    // Live-update any transition anchored to this edit point (cross-dissolve
    // between L/R, or single-sided fade on either side) so the transition
    // slides with the seam instead of waiting for the commit.
    for (auto& t : track->transitions()) {
        const bool touchesLeft  = t.leftClipId  == seam.leftClipId;
        const bool touchesRight = t.rightClipId == seam.rightClipId;
        const bool leftFadeOut  = touchesLeft  && t.rightClipId == 0;
        const bool rightFadeIn  = touchesRight && t.leftClipId  == 0;
        if ((touchesLeft && touchesRight) || leftFadeOut || rightFadeIn)
            t.editPointTick = newEditPoint;
    }
}

void TimelineRollTool::restoreOrigins()
{
    Timeline* timeline = m_panel.m_timeline;
    if (!timeline) return;
    const auto restore = [timeline](const Seam& seam) {
        Track* track = timeline->track(seam.trackIndex);
        if (!track) return;
        const size_t li = track->findClipIndexById(seam.leftClipId);
        const size_t ri = track->findClipIndexById(seam.rightClipId);
        if (li >= track->clipCount() || ri >= track->clipCount()) return;
        Clip* lc = track->clip(li);
        Clip* rc = track->clip(ri);
        lc->setTimelineIn(seam.leftOrigIn);
        lc->setDuration(seam.leftOrigDur);
        lc->setSourceIn(seam.leftOrigSrcIn);
        rc->setTimelineIn(seam.rightOrigIn);
        rc->setDuration(seam.rightOrigDur);
        rc->setSourceIn(seam.rightOrigSrcIn);
    };
    restore(m_primary);
    for (const auto& seam : m_extras)
        restore(seam);
}

void TimelineRollTool::release(const QMouseEvent& /*event*/)
{
    Timeline* timeline = m_panel.m_timeline;
    Track* track = timeline ? timeline->track(m_primary.trackIndex) : nullptr;
    if (!track) return;
    const size_t li = track->findClipIndexById(m_primary.leftClipId);
    const size_t ri = track->findClipIndexById(m_primary.rightClipId);
    if (li >= track->clipCount() || ri >= track->clipCount()) return;
    const int64_t finalEditPoint = track->clip(li)->timelineOut();

    // Every rolled seam back to the original model, so each rollingEdit
    // below starts from it.
    restoreOrigins();

    const int64_t delta = finalEditPoint - m_primary.origEditPoint;
    if (delta == 0) return;
    // Every seam rolls by the same delta: one undo step.
    CommandStack* stack = m_panel.m_commandStack;
    const bool multi = !m_extras.empty() && stack;
    if (multi) stack->beginMacro("Rolling edit");
    if (auto cmd = EditOperations::rollingEdit(*timeline, m_primary.trackIndex,
                                               m_primary.leftClipId,
                                               m_primary.rightClipId, finalEditPoint))
        m_panel.executeCommand(std::move(cmd));
    for (const auto& seam : m_extras) {
        if (auto extra = EditOperations::rollingEdit(*timeline, seam.trackIndex,
                                                     seam.leftClipId, seam.rightClipId,
                                                     seam.origEditPoint + delta))
            m_panel.executeCommand(std::move(extra));
    }
    if (multi) stack->endMacro();
}

} // namespace rt
