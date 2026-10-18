/*
 * TimelineDragTool.h — one timeline drag in progress.
 *
 * A tool owns its drag's state, follows the mouse, and ends one of three
 * ways (TimelinePanel::DragEnd): released, cancelled by Esc, or interrupted
 * by a press that arrived before the release. TimelinePanel::endDrag calls
 * the matching hook and then finish(), so a tool never has to know which
 * other cleanup the panel does.
 *
 * Drags not yet converted still run on TimelinePanel's DragMode switch
 * (rewrite Stage 9 moves them over one at a time).
 */
#pragma once

class QMouseEvent;

namespace rt {

class TimelineDragTool
{
public:
    virtual ~TimelineDragTool() = default;

    /// Follow the cursor. The event carries the modifiers to use.
    virtual void move(const QMouseEvent& event) = 0;
    /// The button came up; runs before the panel commits the drag.
    virtual void release(const QMouseEvent& event) = 0;
    /// Esc: undo whatever this drag applied.
    virtual void cancel() {}
    /// A press arrived before the release (lost mouse capture).
    virtual void interrupt() {}
    /// Remove on-screen affordances; called once, however the drag ended.
    virtual void finish() {}
};

} // namespace rt
