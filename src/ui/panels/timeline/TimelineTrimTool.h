/*
 * TimelineTrimTool.h — dragging a clip's head or tail edge.
 *
 * The grabbed edge follows the mouse (snapping unless Ctrl is held) and
 * stops at the neighbouring clip and one tick short of the other edge.
 * Every other selected clip's same edge moves by the same delta, each
 * clamped to its own neighbour. Each move records a trim command into the
 * undo macro the panel opened; the panel closes it (one step) or undoes it
 * (Esc) when the drag ends.
 */
#pragma once

#include "TimelineDragTool.h"
#include "timeline/EditOperations.h"

#include <vector>

namespace rt {

class TimelinePanel;

class TimelineTrimTool final : public TimelineDragTool
{
public:
    /// A clip's edge as it was when the trim began.
    struct ClipEdgeStart
    {
        ClipRef ref;
        int64_t in{0};
        int64_t duration{0};
    };

    /// `others` may include the primary clip; it is skipped.
    TimelineTrimTool(TimelinePanel& panel, ClipEdge edge, ClipEdgeStart primary,
                     std::vector<ClipEdgeStart> others);

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& /*event*/) override {}
    [[nodiscard]] int64_t frameAnchor() const override;

private:
    void moveHead(const QMouseEvent& event, int64_t tickDelta);
    void moveTail(const QMouseEvent& event, int64_t tickDelta);
    int64_t snapEdge(const QMouseEvent& event, int64_t tick);

    TimelinePanel& m_panel;
    ClipEdge m_edge;
    ClipEdgeStart m_primary;
    std::vector<ClipEdgeStart> m_others;
};

} // namespace rt
