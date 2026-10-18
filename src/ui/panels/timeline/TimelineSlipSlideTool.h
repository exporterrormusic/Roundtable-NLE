/*
 * TimelineSlipSlideTool.h — the Slip and Slide tools dragging a clip.
 *
 * Slip shifts which part of the source the clip shows, leaving it in place
 * (the source moves with the mouse, so a drag to the right shows earlier
 * frames, as in Premiere); slide moves the clip between its neighbours,
 * which give or take the difference. Every selected clip (the grabbed one plus link partners) goes
 * by the same whole-frame delta, or none does when any of them can't. Each
 * move records into the undo macro the press opened; the panel closes it
 * (one step) or undoes it (Esc) when the drag ends.
 */
#pragma once

#include "TimelineDragTool.h"
#include "timeline/EditOperations.h"

#include <vector>

namespace rt {

class TimelinePanel;

class TimelineSlipSlideTool final : public TimelineDragTool
{
public:
    enum class Kind { Slip, Slide };

    /// `grabbed` is the clip the press landed on; `clips` every clip that
    /// goes along (it includes `grabbed`). `grabbedOrigin` is the grabbed
    /// clip's source in-point (slip) or timeline in-point (slide).
    TimelineSlipSlideTool(TimelinePanel& panel, Kind kind, ClipRef grabbed,
                          int64_t grabbedOrigin, std::vector<ClipRef> clips);

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& /*event*/) override {}

private:
    TimelinePanel& m_panel;
    Kind m_kind;
    ClipRef m_grabbed;
    int64_t m_grabbedOrigin;
    std::vector<ClipRef> m_clips;
    int64_t m_appliedDelta{0};   ///< how far the grabbed clip has gone
};

} // namespace rt
