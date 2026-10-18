/*
 * TimelineSlipSlideTool.cpp — see TimelineSlipSlideTool.h.
 */

#include "panels/timeline/TimelineSlipSlideTool.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/Command.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <memory>
#include <utility>

namespace rt {

TimelineSlipSlideTool::TimelineSlipSlideTool(TimelinePanel& panel, Kind kind,
                                             ClipRef grabbed, int64_t grabbedOrigin,
                                             std::vector<ClipRef> clips)
    : m_panel(panel), m_kind(kind), m_grabbed(grabbed),
      m_grabbedOrigin(grabbedOrigin), m_clips(std::move(clips))
{
}

void TimelineSlipSlideTool::move(const QMouseEvent& /*event*/, int64_t tickDelta)
{
    Timeline& timeline = *m_panel.m_timeline;
    const int64_t snappedDelta = m_panel.m_layoutEngine.snapToFrame(tickDelta);
    const int64_t step = snappedDelta - m_appliedDelta;

    // Slip moves the source under the clip with the mouse (Premiere): a drag
    // to the right shows EARLIER source frames, so the source in-point moves
    // against the drag.
    const int64_t sourceStep = -step;

    // All or nothing: one clip that can't go keeps every clip where it is.
    std::vector<std::unique_ptr<Command>> commands;
    for (const auto& ref : m_clips) {
        auto command = m_kind == Kind::Slip
            ? EditOperations::slipClip(timeline, ref.trackIndex, ref.clipId, sourceStep)
            : EditOperations::slideClip(timeline, ref.trackIndex, ref.clipId, step);
        if (!command) { commands.clear(); break; }
        commands.push_back(std::move(command));
    }
    if (!commands.empty()) {
        for (auto& command : commands) m_panel.executeCommand(std::move(command));
        // How far the grabbed clip really went (an operation may clamp), in
        // drag terms.
        if (const Track* track = timeline.track(m_grabbed.trackIndex)) {
            const size_t index = track->findClipIndexById(m_grabbed.clipId);
            if (index < track->clipCount()) {
                const Clip* clip = track->clip(index);
                m_appliedDelta = m_kind == Kind::Slip
                    ? m_grabbedOrigin - clip->sourceIn()
                    : clip->timelineIn() - m_grabbedOrigin;
            }
        }
    }
    m_panel.onScrollChanged();
}

} // namespace rt
