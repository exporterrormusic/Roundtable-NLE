/*
 * TimelineClipMoveTool.h — dragging selected clips along and across tracks.
 *
 * The selected clips follow the mouse live, snapping unless Ctrl is held,
 * moving as a group (the group stops when its first clip reaches tick 0, so
 * spacing is kept), and across tracks onto tracks that can host each clip.
 * Dragging into the band above the top video track (or below the last audio
 * track) shows a ghost track that the release creates.
 *
 * Nothing is recorded while dragging: the release puts the clips back and
 * records the move (with any new track and Alt-copy) as one undo step. Esc
 * or an interrupted drag just puts them back.
 */
#pragma once

#include "TimelineDragTool.h"
#include "timeline/EditOperations.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt {

class TimelinePanel;

/// A clip taking part in a move or trim drag, as it was when the drag began,
/// with the transitions on its track that reference it (a live cross-track
/// move drops them; the release puts them back before recording the move).
struct TimelineDragClip
{
    ClipRef ref;
    int64_t originalIn{0};
    int64_t originalDuration{0};
    int64_t originalSourceIn{0};
    size_t  originalTrack{0};
    std::vector<Transition> originalTransitions;
};

class TimelineClipMoveTool final : public TimelineDragTool
{
public:
    /// The grabbed clip's in-point, duration and track, and every clip that
    /// moves (the selection, including the grabbed clip).
    TimelineClipMoveTool(TimelinePanel& panel, int64_t originalIn, int64_t originalDuration,
                         size_t originalTrack, std::vector<TimelineDragClip> clips);

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& event) override;
    void cancel() override { restoreOrigins(); }
    void interrupt() override { restoreOrigins(); }
    [[nodiscard]] int64_t frameAnchor() const override { return m_originalIn; }

private:
    void moveTo(const QMouseEvent& event, QPointF pos, int64_t tickDelta);
    int clampTrackDelta(int requestedDelta, TrackType section) const;
    void restoreOrigins();

    TimelinePanel& m_panel;
    int64_t m_originalIn;
    int64_t m_originalDuration;
    size_t m_originalTrack;
    size_t m_targetTrack;   ///< track under the cursor
    std::vector<TimelineDragClip> m_clips;
};

} // namespace rt
