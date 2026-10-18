/*
 * TimelineRollTool.h — the Rolling tool dragging a cut between two clips.
 *
 * The cut moves live (the left clip's tail and the right clip's head with
 * any transition anchored on it), snapping unless Ctrl is held, within the
 * range both clips' media allow. With a selected clip at the grabbed cut,
 * the nearest selected cut on every other track rolls by the same delta
 * (Premiere's multi-track roll); the range is then what every seam allows.
 *
 * Nothing is recorded while dragging: the release puts the seams back and
 * records the roll as rollingEdit commands, one undo step. Esc or an
 * interrupted drag just puts them back.
 */
#pragma once

#include "TimelineDragTool.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt {

class TimelinePanel;

class TimelineRollTool final : public TimelineDragTool
{
public:
    /// One cut, as it was when the roll began.
    struct Seam
    {
        size_t  trackIndex{0};
        uint64_t leftClipId{0}, rightClipId{0};
        int64_t origEditPoint{0};
        int64_t leftOrigIn{0}, leftOrigDur{0}, leftOrigSrcIn{0};
        int64_t rightOrigIn{0}, rightOrigDur{0}, rightOrigSrcIn{0};
    };

    /// `minEditPoint`/`maxEditPoint` bound the grabbed cut (already narrowed
    /// so every extra seam stays within its own limits).
    TimelineRollTool(TimelinePanel& panel, Seam primary, std::vector<Seam> extras,
                     int64_t minEditPoint, int64_t maxEditPoint);

    void move(const QMouseEvent& event, int64_t tickDelta) override;
    void release(const QMouseEvent& event) override;
    void cancel() override { restoreOrigins(); }
    void interrupt() override { restoreOrigins(); }
    [[nodiscard]] int64_t frameAnchor() const override { return m_primary.origEditPoint; }

private:
    void applySeamLive(const Seam& seam, int64_t newEditPoint);
    void restoreOrigins();

    TimelinePanel& m_panel;
    Seam m_primary;
    std::vector<Seam> m_extras;
    int64_t m_minEditPoint;
    int64_t m_maxEditPoint;
};

} // namespace rt
