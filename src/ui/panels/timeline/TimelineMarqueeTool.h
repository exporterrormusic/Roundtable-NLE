/*
 * TimelineMarqueeTool.h — a press on empty timeline space.
 *
 * Released without moving, it clears the selection (Premiere). Dragged past
 * the 5 px deadzone, it draws a rubber band and selects every clip it
 * touches; with Shift held when the band starts, it adds to the selection
 * that was there. Pegged against the left or right edge it scrolls the
 * timeline, keeping its anchor on the same tick.
 */
#pragma once

#include "TimelineDragTool.h"
#include "timeline/EditOperations.h"

#include <QPointF>

#include <memory>
#include <vector>

class QRubberBand;
class QTimer;

namespace rt {

class TimelinePanel;

class TimelineMarqueeTool final : public TimelineDragTool
{
public:
    TimelineMarqueeTool(TimelinePanel& panel, QPointF pressPos);
    ~TimelineMarqueeTool() override;

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& event) override;
    void finish() override;

private:
    void start(const QMouseEvent& event);
    void updateBand();
    void refreshSelection();
    void autoScroll();

    TimelinePanel& m_panel;
    QPointF m_anchor;     ///< where the band starts; shifted by auto-scroll
    QPointF m_cursor;     ///< latest cursor position
    bool m_started{false};
    bool m_additive{false};
    std::vector<ClipRef> m_baseSelection;
    std::unique_ptr<QRubberBand> m_band;
    std::unique_ptr<QTimer> m_scrollTimer;
};

} // namespace rt
