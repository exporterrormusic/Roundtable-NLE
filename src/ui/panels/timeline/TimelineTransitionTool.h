/*
 * TimelineTransitionTool.h — dragging a transition's edge, or a clip edge's
 * transition handle.
 *
 * Edge: the transition's duration follows the cursor (a fade from its edit
 * point, a dissolve symmetrically about its cut), at least one frame and
 * never past the clips under it or a neighbouring transition, with a
 * tooltip showing the change and the length.
 *
 * Handle: a press on the top inner corner of an edge with no transition. A
 * plain click only selects that edge. Dragged inward past a small deadzone,
 * it adds the default transition sized to the drag and from then on drags
 * that transition's inner edge as above.
 *
 * Both record into one undo macro (opened on the first change); the panel
 * closes it (one step) or undoes it (Esc) when the drag ends.
 */
#pragma once

#include "TimelineDragTool.h"
#include "timeline/EditOperations.h"

#include <QPointF>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rt {

class TimelinePanel;

class TimelineTransitionTool final : public TimelineDragTool
{
public:
    /// Dragging the edge of the transition at `transitionIndex` on the track.
    static std::unique_ptr<TimelineTransitionTool> edge(
        TimelinePanel& panel, size_t trackIndex, size_t transitionIndex,
        int64_t originalDuration);
    /// Pressed on `edge` of `clipId`'s transition handle at `pressPos`.
    static std::unique_ptr<TimelineTransitionTool> handle(
        TimelinePanel& panel, size_t trackIndex, uint64_t clipId, ClipEdge edge,
        QPointF pressPos);

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& /*event*/) override {}

private:
    explicit TimelineTransitionTool(TimelinePanel& panel) : m_panel(panel) {}
    void dragEdge(QPointF pos);
    void dragHandle(QPointF pos);
    void showDurationTip(QPointF pos, int64_t duration);

    TimelinePanel& m_panel;
    bool m_handlePending{false};   ///< a handle press not yet turned into a transition
    size_t m_trackIndex{0};
    size_t m_transitionIndex{0};
    int64_t m_originalDuration{0};
    // Handle press.
    uint64_t m_handleClipId{0};
    ClipEdge m_handleEdge{ClipEdge::Head};
    QPointF m_pressPos;
};

} // namespace rt
