/*
 * TimelinePanelMouseDragMove.cpp — mouseMoveEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineTrimTool.h"
#include "panels/timeline/TimelineClipMoveTool.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "FrameTime.h"

#include <QApplication>
#include <QToolTip>
#include <QMouseEvent>
#include <QTimer>
#include <QRubberBand>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>
#include <optional>

namespace rt {

// Clamp a desired track index onto the nearest track that can host a clip of
// the same kind as `like` (skipping dividers + the pinned caption track).
// User dividers may appear between two tracks of the same kind, so numeric
// range clamping is not sufficient: the resulting index itself must be a real
// media track. Returns SIZE_MAX only when no compatible host track exists.
size_t TimelinePanel::clampTrackToHostTrack(int desiredIdx, const Track* like) const
{
    if (!m_timeline || !like) return SIZE_MAX;
    return m_timeline->nearestClipHostTrack(desiredIdx, like->type());
}

// Auto-scroll the timeline while a clip drag (move or trim) pegs against
// the left/right edge of the viewport. Same proximity-ramped curve as the
// marquee timer; the only twist is that we have to adjust m_dragStart.x()
// so the cursor-anchored deltaX in mouseMoveEvent stays correct as scroll
// moves underneath us, then re-fire the move pipeline to slide the clip.
void TimelinePanel::updateClipDragAutoScroll(const QPointF& pos)
{
    m_clipDragLastMovePos = pos;

    if (!m_clipDragScrollTimer) {
        m_clipDragScrollTimer = new QTimer(this);
        m_clipDragScrollTimer->setInterval(16);
        connect(m_clipDragScrollTimer, &QTimer::timeout, this, [this]() {
            if (m_dragMode != DragMode::ClipMove
             && m_dragMode != DragMode::ClipTrimHead
             && m_dragMode != DragMode::ClipTrimTail) {
                m_clipDragScrollTimer->stop();
                return;
            }
            const int hdr = headerWidth();
            constexpr int    kEdge = 40;
            constexpr double kMaxV = 18.0;
            double dx = 0.0;
            const double cx = m_clipDragLastMovePos.x();
            if (cx < hdr + kEdge)
                dx = -kMaxV * std::clamp((hdr + kEdge - cx) / kEdge, 0.0, 1.0);
            else if (cx > width() - kEdge)
                dx =  kMaxV * std::clamp((cx - (width() - kEdge)) / kEdge, 0.0, 1.0);
            if (dx == 0.0) return;

            const double oldScroll = m_layoutEngine.scrollX();
            const double newScroll = std::max(0.0, oldScroll + dx);
            if (newScroll == oldScroll) return;
            m_layoutEngine.setScrollX(newScroll);
            // Keep deltaX = pos.x() - m_dragStart.x() consistent as scroll
            // advances under the (stationary) cursor.
            m_dragStart.setX(m_dragStart.x() - (newScroll - oldScroll));
            onScrollChanged();

            // Re-fire the move pipeline so the clip preview slides along
            // with the new scroll position. The cursor's local position is
            // unchanged; the adjusted m_dragStart makes deltaX expand.
            QMouseEvent fake(QEvent::MouseMove,
                             m_clipDragLastMovePos,
                             m_clipDragLastMovePos,
                             mapToGlobal(m_clipDragLastMovePos.toPoint()),
                             Qt::LeftButton, Qt::LeftButton,
                             // The keys still held: a trim reads Ctrl
                             // (no snapping) from the move event.
                             QApplication::keyboardModifiers());
            mouseMoveEvent(&fake);
        });
    }

    const int hdr = headerWidth();
    constexpr int kEdge = 40;
    const bool atEdge = (pos.x() < hdr + kEdge) || (pos.x() > width() - kEdge);
    if (atEdge && !m_clipDragScrollTimer->isActive())
        m_clipDragScrollTimer->start();
    else if (!atEdge && m_clipDragScrollTimer->isActive())
        m_clipDragScrollTimer->stop();
}

void TimelinePanel::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_timeline)
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    // ── Hover cursor feedback when not dragging ─────────────────────────
    if (m_dragMode == DragMode::None)
    {
        updateTransitionHandleHover(event->position());
        if (m_activeTool == EditTool::Razor)
        {
            // Track razor position for the red line indicator
            double px = event->position().x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            size_t ti = hitTestTrack(event->position().y());

            // Check if cursor is over a clip in this track
            bool overClip = false;
            if (ti < m_timeline->trackCount()) {
                Track* trk = m_timeline->track(ti);
                for (size_t ci = 0;
                     trk && !trk->isLocked() && ci < trk->clipCount(); ++ci) {
                    const Clip* c = trk->clip(ci);
                    if (tick > c->timelineIn() && tick < c->timelineOut()) {
                        overClip = true;
                        break;
                    }
                }
            }

            // Set razor tick on the hovered track widget, clear others
            for (auto tw : m_trackWidgets)
                tw->setRazorTick(overClip && tw->trackIndex() == ti ? tick : -1);

            QWidget::mouseMoveEvent(event);
            return;
        }
        if (m_activeTool == EditTool::Selection)
        {
            // Check for transition-edge hover first
            bool hoveringTransEdge = false;
            {
                double hpx = event->position().x() - headerWidth();
                size_t hti = hitTestTrack(event->position().y());
                if (hti < m_timeline->trackCount()) {
                    Track* hTrack = m_timeline->track(hti);
                    constexpr double kHandleThreshold = 8.0;
                    for (size_t trI = 0;
                         hTrack && !hTrack->isLocked()
                         && trI < hTrack->transitionCount(); ++trI) {
                        const Transition* trans = hTrack->transition(trI);
                        if (!trans) continue;
                        int64_t tStart, tEnd;
                        trans->getRange(tStart, tEnd);
                        double pxStart = m_layoutEngine.timeToPixelX(tStart);
                        double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);
                        bool canStart = (trans->rightClipId == 0)
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0);
                        bool canEnd   = (trans->leftClipId == 0)
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0);
                        if ((canStart && std::abs(hpx - pxStart) < kHandleThreshold) ||
                            (canEnd   && std::abs(hpx - pxEnd)   < kHandleThreshold)) {
                            setCursor(Qt::SizeHorCursor);
                            hoveringTransEdge = true;
                            break;
                        }
                    }
                }
            }
            // Transition handle: the top inner corner of an edge with no
            // transition. A distinct cursor from the trim cursor so the two
            // gestures are never confused.
            if (!hoveringTransEdge && hitTestTransitionHandle(event->position())) {
                setCursor(Qt::PointingHandCursor);
                for (auto tw : m_trackWidgets)
                    tw->setHoverEdgeTick(-1);
                hoveringTransEdge = true;
            }
            if (!hoveringTransEdge) {
            auto hitRef = hitTestClip(event->position());

            // Edge-halo fallback: when zoomed out, a clip can be only a
            // few pixels wide, so the cursor sits just outside the clip's
            // tick range (where hitTestClip returns no match) yet still
            // within the edge grab zone. Scan the hovered track for any
            // clip edge within edgeGrabPx of the cursor and treat that as
            // a hit so the trim cursor still appears.
            // Same rule as the press (hitTestEdgeHalo), so the trim
            // cursor only shows where a press really grabs the edge.
            if (!hitRef)
                hitRef = hitTestEdgeHalo(hitTestTrack(event->position().y()),
                                         event->position().x() - headerWidth());

            if (hitRef)
            {
                // Check edge proximity for trim cursor
                Track* track = m_timeline->track(hitRef->trackIndex);
                size_t idx = track ? track->findClipIndexById(hitRef->clipId) : SIZE_MAX;
                if (track && !track->isLocked() && idx < track->clipCount())
                {
                    const Clip* clip = track->clip(idx);
                    double px = event->position().x() - headerWidth();
                    double clipLeft  = m_layoutEngine.timeToPixelX(clip->timelineIn());
                    double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

                    // Match the press-time grab zone (TimelinePanel::edgeGrabPx)
                    // so the resize cursor appears exactly where a trim will
                    // actually start.
                    const double kEdgeThreshold = edgeGrabPx(clipRight - clipLeft);
                    bool nearHead = std::abs(px - clipLeft)  < kEdgeThreshold;
                    bool nearTail = std::abs(px - clipRight) < kEdgeThreshold;
                    if (nearHead || nearTail)
                    {
                        setCursor(Qt::SizeHorCursor);
                        // Highlight the hovered edge on the track widget
                        int64_t edgeTick = nearHead ? clip->timelineIn() : clip->timelineOut();
                        for (auto tw : m_trackWidgets)
                            tw->setHoverEdgeTick(hitRef->trackIndex == tw->trackIndex() ? edgeTick : -1);
                    }
                    else
                    {
                        setCursor(Qt::ArrowCursor);
                        for (auto tw : m_trackWidgets)
                            tw->setHoverEdgeTick(-1);
                    }
                }
                else
                {
                    setCursor(Qt::ArrowCursor);
                    for (auto tw : m_trackWidgets)
                        tw->setHoverEdgeTick(-1);
                }
            }
            else
            {
                setCursor(Qt::ArrowCursor);
                for (auto tw : m_trackWidgets)
                    tw->setHoverEdgeTick(-1);
            }
            } // end if (!hoveringTransEdge)
        }
        QWidget::mouseMoveEvent(event);
        return;
    }

    QPointF pos = event->position();
    double deltaX = pos.x() - m_dragStart.x();
    double pps = m_layoutEngine.pixelsPerSecond();
    int64_t tickDelta = static_cast<int64_t>(deltaX / pps * 48000.0);

    // Quantize the move to whole frames so a clip can never land on a
    // fractional-frame tick. Timeline ticks run at 48000/sec but a frame is
    // 48000/fps ticks; an un-quantized pixel→tick delta drops clips a
    // fraction of a frame off, which is invisible on the timeline but makes
    // a clip's end overhang the frame boundary the compositor samples on —
    // showing a 1-frame ghost at shot cuts. Snap-to-edge (below) still
    // overrides this when the magnet engages.
    // NTSC frames alternate 1601/1602 ticks, so a fixed whole-frame step
    // drifts off the grid playback samples on: land the dragged EDGE on a
    // canonical frame start instead (FrameTime.h).
    {
        const double fps = m_layoutEngine.frameRate();
        if (fps > 0.0) {
            const RationalFrameRate grid = canonicalFrameRate(fps);
            const int64_t anchor = m_dragTool ? m_dragTool->frameAnchor() : -1;
            tickDelta = (anchor >= 0)
                ? snapTickToNearestFrame(anchor + tickDelta, grid) - anchor
                : snapTickToNearestFrame(tickDelta, grid);   // whole frames
        }
    }

    // Open this drag's undo step before its first change (no-op for modes
    // that change nothing, and once open).
    ensureDragEditGesture();

    switch (m_dragMode)
    {
    case DragMode::ClipMove:
        if (m_dragTool) m_dragTool->move(*event, tickDelta);
        break;
    case DragMode::ClipTrimHead:
    case DragMode::ClipTrimTail:
        if (m_dragTool) m_dragTool->move(*event, tickDelta);
        break;
    case DragMode::SlipTool:
    case DragMode::SlideTool:
    case DragMode::RollingEdit:
        if (m_dragTool) m_dragTool->move(*event, tickDelta);
        break;
    case DragMode::PendingClipClick:
        continuePendingClipClick(pos);
        break;

    case DragMode::PendingMarquee:
    case DragMode::MarqueeSelect:
        if (m_dragTool) m_dragTool->move(*event, tickDelta);
        break;
    case DragMode::TransitionTrim:
    case DragMode::TransitionHandle:
        if (m_dragTool) m_dragTool->move(*event, tickDelta);
        break;
    default:
        break;
    }

    event->accept();
}

void TimelinePanel::continuePendingClipClick(QPointF pos)
{
    QPointF delta = pos - m_dragStart;
    if (delta.manhattanLength() < 5.0)
        return;

    // User started dragging — determine mode based on where the
    // original click landed relative to clip edges.
    Track* track = m_timeline->track(m_dragClipRef.trackIndex);
    if (!track || track->isLocked()) {
        m_dragMode = DragMode::None;
        return;
    }
    size_t idx = track->findClipIndexById(m_dragClipRef.clipId);
    if (idx < track->clipCount()) {
        const Clip* clip = track->clip(idx);
        m_dragOriginalIn = clip->timelineIn();
        m_dragOriginalSourceIn = clip->sourceIn();
        m_dragOriginalDuration = clip->duration();

        // Use the drag-start position (where user first clicked) to
        // determine edge proximity — not the current mouse position.
        double clickPx = m_dragStart.x() - headerWidth();
        double clipLeft  = m_layoutEngine.timeToPixelX(clip->timelineIn());
        double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

        const double kEdgeThreshold = edgeGrabPx(clipRight - clipLeft);
        std::optional<ClipEdge> trimEdge;
        if (std::abs(clickPx - clipLeft) < kEdgeThreshold) {
            trimEdge = ClipEdge::Head;
            m_dragMode = DragMode::ClipTrimHead;
            m_lastClickedEdge = { m_dragClipRef, ClipEdge::Head, true };
            // Wrap trim-move commands into a single undo step so one
            // drag = one Ctrl+Z (edge-click path in mousePressEvent
            // does this too; PendingClipClick→trim needs its own).
            if (m_commandStack)
                m_commandStack->beginMacro("Trim clip head");
        } else if (std::abs(clickPx - clipRight) < kEdgeThreshold) {
            trimEdge = ClipEdge::Tail;
            m_dragMode = DragMode::ClipTrimTail;
            m_lastClickedEdge = { m_dragClipRef, ClipEdge::Tail, true };
            if (m_commandStack)
                m_commandStack->beginMacro("Trim clip tail");
        } else {
            m_dragMode = DragMode::ClipMove;
            // Drop the brackets with the edge so they never show a cut
            // Ctrl+T can't act on.
            clearEditPointSelection();
        }

        // Record original positions of ALL selected clips
        // (used by both ClipMove and multi-clip trim).  Also snapshot
        // any transitions referencing each clip — see mouse-press
        // path for rationale (live-drag drops them on cross-track).
        std::vector<TimelineDragClip> clips;
        for (const auto& sel : m_selection.clips()) {
            Track* selTrack = m_timeline->track(sel.trackIndex);
            if (!selTrack || selTrack->isLocked()) continue;
            size_t si = selTrack->findClipIndexById(sel.clipId);
            if (si < selTrack->clipCount()) {
                TimelineDragClip dcs;
                dcs.ref = sel;
                dcs.originalIn = selTrack->clip(si)->timelineIn();
                dcs.originalDuration = selTrack->clip(si)->duration();
                dcs.originalSourceIn = selTrack->clip(si)->sourceIn();
                dcs.originalTrack = sel.trackIndex;
                for (const auto& t : selTrack->transitions()) {
                    if (t.leftClipId == sel.clipId
                     || t.rightClipId == sel.clipId)
                        dcs.originalTransitions.push_back(t);
                }
                clips.push_back(std::move(dcs));
            }
        }

        // Initialize snap engine
        m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
        {
            std::vector<uint64_t> excludeIds;
            for (const auto& sel : m_selection.clips())
                excludeIds.push_back(sel.clipId);
            m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0, excludeIds);
        }
        if (trimEdge)
            beginTrim(*trimEdge, clips);
        else
            m_dragTool = std::make_unique<TimelineClipMoveTool>(
                *this, m_dragOriginalIn, m_dragOriginalDuration,
                m_dragClipRef.trackIndex, std::move(clips));
    }
}

void TimelinePanel::beginTrim(ClipEdge edge, const std::vector<TimelineDragClip>& clips,
                              bool ripple)
{
    std::vector<TimelineTrimTool::ClipEdgeStart> others;
    others.reserve(clips.size());
    for (const auto& clip : clips)
        others.push_back({clip.ref, clip.originalIn, clip.originalDuration});
    m_dragTool = std::make_unique<TimelineTrimTool>(
        *this, edge,
        TimelineTrimTool::ClipEdgeStart{m_dragClipRef, m_dragOriginalIn, m_dragOriginalDuration},
        std::move(others), ripple);
}


// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
