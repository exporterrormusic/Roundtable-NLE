/*
 * TimelinePanelMouseDragMove.cpp — mouseMoveEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QRubberBand>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>

namespace rt {

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
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
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
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
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
                    for (size_t trI = 0; trI < hTrack->transitionCount(); ++trI) {
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
            if (!hoveringTransEdge) {
            auto hitRef = hitTestClip(event->position());

            // Edge-halo fallback: when zoomed out, a clip can be only a
            // few pixels wide, so the cursor sits just outside the clip's
            // tick range (where hitTestClip returns no match) yet still
            // within the edge grab zone. Scan the hovered track for any
            // clip edge within edgeGrabPx of the cursor and treat that as
            // a hit so the trim cursor still appears.
            if (!hitRef) {
                size_t ti = hitTestTrack(event->position().y());
                if (ti < m_timeline->trackCount()) {
                    const Track* trk = m_timeline->track(ti);
                    double pxScan = event->position().x() - headerWidth();
                    for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                        const Clip* c = trk->clip(ci);
                        if (!c) continue;
                        double l = m_layoutEngine.timeToPixelX(c->timelineIn());
                        double r = m_layoutEngine.timeToPixelX(c->timelineOut());
                        double zone = edgeGrabPx(r - l);
                        if (std::abs(pxScan - l) < zone
                                || std::abs(pxScan - r) < zone) {
                            hitRef = ClipRef{ ti, c->id() };
                            break;
                        }
                    }
                }
            }

            if (hitRef)
            {
                // Check edge proximity for trim cursor
                Track* track = m_timeline->track(hitRef->trackIndex);
                size_t idx = track ? track->findClipIndexById(hitRef->clipId) : SIZE_MAX;
                if (idx < track->clipCount())
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
    {
        const double fps = m_layoutEngine.frameRate();
        if (fps > 0.0) {
            const int64_t tpf = static_cast<int64_t>(48000.0 / fps + 0.5);
            if (tpf > 0) {
                const int64_t half = (tickDelta >= 0) ? tpf / 2 : -(tpf / 2);
                tickDelta = ((tickDelta + half) / tpf) * tpf;
            }
        }
    }

    switch (m_dragMode)
    {
    case DragMode::ClipMove:
    {
        int64_t newIn = m_dragOriginalIn + tickDelta;
        if (newIn < 0) newIn = 0;

        // Snap both edges of the dragged clip(s). For a single-clip drag
        // the primary's head + tail are tested; for a multi-clip drag we
        // walk EVERY dragged clip's head + tail and pick whichever yields
        // the smallest snap delta — otherwise only the primary's edges
        // could snap, so clips on other tracks would silently pass over
        // target edges and never magnetise.
        // Holding Ctrl suppresses magnetism entirely for precision moves.
        const bool ctrlHeld = (QApplication::keyboardModifiers() & Qt::ControlModifier);
        if (!ctrlHeld) {
        const bool multiDrag = (m_dragSelectedClips.size() > 1);
        if (!multiDrag) {
            int64_t newOut = newIn + m_dragOriginalDuration;
            auto result = m_snapEngine.snapPair(newIn, newOut);
            if (result.didSnap) {
                newIn = result.snappedTick;
                // Show indicator at whichever edge actually snapped
                auto headCheck = m_snapEngine.snap(newIn);
                if (headCheck.didSnap && headCheck.delta == 0)
                    setSnapIndicator(newIn);  // head edge snapped
                else
                    setSnapIndicator(newIn + m_dragOriginalDuration);  // tail edge
            } else {
                setSnapIndicator(-1);
            }
        } else {
            // findNearestAttract is the non-hysteretic primitive — using
            // it here lets us probe every dragged edge independently
            // without the snap engine's stuck-target state interfering
            // across clips.
            const int64_t attract = m_snapEngine.thresholdTicks();
            int64_t bestAbs = std::numeric_limits<int64_t>::max();
            int64_t bestDelta = 0;
            int64_t bestEdgeTick = 0;
            for (const auto& dcs : m_dragSelectedClips) {
                const int64_t cIn  = dcs.originalIn + tickDelta;
                const int64_t cOut = cIn + dcs.originalDuration;
                auto hitIn  = m_snapEngine.findNearestAttract(cIn,  attract);
                if (hitIn.found && hitIn.dist < bestAbs) {
                    bestAbs = hitIn.dist;
                    bestDelta = hitIn.tick - cIn;
                    bestEdgeTick = hitIn.tick;
                }
                auto hitOut = m_snapEngine.findNearestAttract(cOut, attract);
                if (hitOut.found && hitOut.dist < bestAbs) {
                    bestAbs = hitOut.dist;
                    bestDelta = hitOut.tick - cOut;
                    bestEdgeTick = hitOut.tick;
                }
            }
            if (bestAbs != std::numeric_limits<int64_t>::max()) {
                newIn += bestDelta;
                if (newIn < 0) newIn = 0;
                setSnapIndicator(bestEdgeTick);
            } else {
                setSnapIndicator(-1);
            }
        }
        } else {
            // Ctrl held: suppress magnetism, reset sticky state so
            // releasing Ctrl doesn't cause a sudden jump to a stale target.
            m_snapEngine.resetHysteresis();
            setSnapIndicator(-1);
        }

        // Compute the actual delta applied to the primary clip
        int64_t actualDelta = newIn - m_dragOriginalIn;

        // Group-floor clamp: when multiple clips are dragged left as a
        // unit, raise the delta so the leftmost selected clip lands at
        // tick 0 instead of every clip individually clamping to 0 and
        // collapsing into an overlap (rightmost ends up overwriting the
        // leftmost).
        {
            int64_t minSelOrig = std::numeric_limits<int64_t>::max();
            for (const auto& dcs : m_dragSelectedClips)
                minSelOrig = std::min(minSelOrig, dcs.originalIn);
            if (minSelOrig != std::numeric_limits<int64_t>::max()
                    && actualDelta < -minSelOrig) {
                actualDelta = -minSelOrig;
                newIn = m_dragOriginalIn + actualDelta;
            }
        }

        // Detect cross-track target
        size_t targetTrack = hitTestTrack(pos.y());
        if (targetTrack < m_timeline->trackCount()) {
            m_dragTargetTrack = targetTrack;
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        } else if (!m_trackWidgets.empty()) {
            // ── Ghost track detection (Premiere Pro-style) ──────────────
            // Cursor is OUTSIDE any existing track.
            auto firstTw = m_trackWidgets.front();
            auto lastTw  = m_trackWidgets.back();
            QPoint firstTop = firstTw->mapTo(this, QPoint(0, 0));
            QPoint lastBot  = lastTw->mapTo(this, QPoint(0, lastTw->height()));

            // Determine dragged clip's track type
            TrackType dragType = TrackType::Video;
            if (!m_dragSelectedClips.empty()) {
                Track* srcTr = m_timeline->track(m_dragSelectedClips[0].ref.trackIndex);
                if (srcTr) dragType = srcTr->type();
            }

            // Find first/last track indices of each type. Skip divider rows
            // — they're TrackType::Video but not real video tracks, so
            // including them in lastVideoIdx misclassifies the V/A boundary.
            size_t firstVideoIdx = SIZE_MAX, lastVideoIdx = SIZE_MAX;
            size_t firstAudioIdx = SIZE_MAX, lastAudioIdx = SIZE_MAX;
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                Track* t = m_timeline->track(i);
                if (!t || t->isDivider()) continue;
                if (t->type() == TrackType::Video) {
                    if (firstVideoIdx == SIZE_MAX) firstVideoIdx = i;
                    lastVideoIdx = i;
                } else {
                    if (firstAudioIdx == SIZE_MAX) firstAudioIdx = i;
                    lastAudioIdx = i;
                }
            }

            // Scroll area X position
            QPoint scrollOrig = m_verticalScroll->mapTo(this, QPoint(0, 0));
            int ghostX = scrollOrig.x();
            int ghostW = m_verticalScroll->width();

            if (dragType == TrackType::Video && pos.y() < firstTop.y() &&
                firstVideoIdx < m_trackWidgets.size()) {
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = true;
                m_ghostTrackHeight = firstTw->height();
                m_ghostTrackY = firstTop.y() - m_ghostTrackHeight;
                if (m_ghostOverlay) {
                    m_ghostOverlay->isAbove = true;
                    m_ghostOverlay->onExistingTrack = false;  // new-track preview
                    m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                    m_ghostOverlay->raise();
                    m_ghostOverlay->show();
                    m_ghostOverlay->update();
                }
            } else if (dragType == TrackType::Audio && pos.y() > lastBot.y() &&
                       lastAudioIdx < m_trackWidgets.size()) {
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = false;
                m_ghostTrackHeight = lastTw->height();
                m_ghostTrackY = lastBot.y();
                if (m_ghostOverlay) {
                    m_ghostOverlay->isAbove = false;
                    m_ghostOverlay->onExistingTrack = false;  // new-track preview
                    m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                    m_ghostOverlay->raise();
                    m_ghostOverlay->show();
                    m_ghostOverlay->update();
                }
            } else {
                m_ghostTrackVisible = false;
                if (m_ghostOverlay) m_ghostOverlay->hide();
            }
        }

        // Compute per-clip cross-track delta from the primary clip.
        int trackDeltaLive = 0;
        if (m_dragTargetTrack != SIZE_MAX && !m_dragSelectedClips.empty()) {
            trackDeltaLive = static_cast<int>(m_dragTargetTrack)
                           - static_cast<int>(m_dragOriginalTrack);
        }

        // ── Y deadzone for cross-track shifts ───────────────────────────
        constexpr double kTrackShiftDeadzone = 18.0;
        if (std::abs(pos.y() - m_dragStart.y()) < kTrackShiftDeadzone)
            trackDeltaLive = 0;

        // ── Shift held = constrain to current track (Premiere-style) ───
        // Lets the user nudge a clip horizontally without worrying about
        // accidentally bumping it to a neighbouring track. Checked live
        // so toggling Shift mid-drag also toggles the constraint.
        if (QApplication::keyboardModifiers() & Qt::ShiftModifier) {
            trackDeltaLive = 0;
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        }

        // ── Group-safety clamp ──────────────────────────────────────────
        if (trackDeltaLive != 0) {
            size_t minOrig = SIZE_MAX, maxOrig = 0;
            for (const auto& dcs : m_dragSelectedClips) {
                minOrig = std::min(minOrig, dcs.originalTrack);
                maxOrig = std::max(maxOrig, dcs.originalTrack);
            }
            if (minOrig != SIZE_MAX) {
                const int lastTrack = static_cast<int>(m_timeline->trackCount()) - 1;
                const int minAllowed = -static_cast<int>(minOrig);
                const int maxAllowed = lastTrack - static_cast<int>(maxOrig);
                trackDeltaLive = std::clamp(trackDeltaLive, minAllowed, maxAllowed);
            }
        }

        // Move clips DIRECTLY in the data model (no commands) for live visual feedback.
        for (auto& dcs : m_dragSelectedClips) {
            // No per-clip max(0,...): actualDelta is already group-floored
            // above so the leftmost clip lands at tick 0 without others
            // collapsing onto it.
            int64_t clipNewIn = dcs.originalIn + actualDelta;
            if (clipNewIn < 0) clipNewIn = 0;

            int dst = static_cast<int>(dcs.originalTrack) + trackDeltaLive;
            // Per-clip bounds: never let a clip go outside the valid track
            // range.  The group-wide trackDelta clamp only guarantees the
            // min/max original tracks stay in range — intermediate clips
            // could still overshoot if the group spans multiple tracks.
            size_t dstTrack;
            if (dst < 0 || dst >= static_cast<int>(m_timeline->trackCount()))
                dstTrack = dcs.ref.trackIndex;
            else
                dstTrack = static_cast<size_t>(dst);

            Track* srcTr = m_timeline->track(dcs.ref.trackIndex);
            if (!srcTr) continue;
            Track* dstTr = m_timeline->track(dstTrack);
            // Dividers are TrackType::Video but reject clips. A plain type
            // check would let the live preview "move" the clip to the divider
            // row, where Track::addClip silently drops it (returns nullptr
            // after std::move has already consumed the unique_ptr — the clip
            // is permanently lost). Reject the divider so the clip stays on
            // its current track for this drag tick.
            if (!dstTr || dstTr->isDivider() || dstTr->type() != srcTr->type())
                dstTrack = dcs.ref.trackIndex;

            size_t curTrack = dcs.ref.trackIndex;

            if (dstTrack != curTrack) {
                Track* curTr = m_timeline->track(curTrack);
                size_t idx = curTr->findClipIndexById(dcs.ref.clipId);
                if (idx >= curTr->clipCount()) continue;
                auto clipPtr = curTr->removeClip(idx);
                clipPtr->setTimelineIn(clipNewIn);
                dstTr->addClip(std::move(clipPtr));
                dcs.ref.trackIndex = dstTrack;
            } else {
                Track* tr = m_timeline->track(curTrack);
                size_t idx = tr->findClipIndexById(dcs.ref.clipId);
                if (idx >= tr->clipCount()) continue;
                tr->moveClip(idx, clipNewIn);
            }
        }
        onScrollChanged();

        // Update ghost overlay clip previews when ghost track is visible.
        // Filter by the GHOST's direction so a linked A/V pair doesn't
        // stuff both clips into the same row: the audio-below ghost shows
        // only the audio companion, and video-above shows only the video.
        // The other side stays drawn on its existing track via normal clip
        // rendering (it didn't actually move, so the rendering is correct).
        if (m_ghostTrackVisible && m_ghostOverlay) {
            const TrackType ghostType = m_ghostTrackIsAbove
                                        ? TrackType::Video
                                        : TrackType::Audio;
            std::vector<GhostTrackOverlay::GhostClipPreview> previews;
            for (const auto& dcs : m_dragSelectedClips) {
                Track* trk = m_timeline->track(dcs.ref.trackIndex);
                if (!trk) continue;
                if (trk->type() != ghostType) continue;  // wrong side, skip
                if (trk->isDivider()) continue;
                size_t idx = trk->findClipIndexById(dcs.ref.clipId);
                if (idx >= trk->clipCount()) continue;
                const Clip* clip = trk->clip(idx);
                double px = m_layoutEngine.timeToPixelX(clip->timelineIn());
                double pw = m_layoutEngine.timeToPixelX(clip->timelineIn() + clip->duration()) - px;
                if (pw <= 0) continue;

                GhostTrackOverlay::GhostClipPreview gp;
                gp.x = static_cast<int>(px);
                gp.width = static_cast<int>(pw);
                gp.color = (ghostType == TrackType::Video) ? 0x4A90D9FF : 0x3CA05AFF;
                gp.label = QString::fromStdString(clip->label());
                previews.push_back(gp);
            }
            m_ghostOverlay->setClipPreviews(previews);
        } else if (m_ghostOverlay) {
            m_ghostOverlay->setClipPreviews({});
        }

        // Sync selection + drag state
        {
            std::vector<std::vector<size_t>> perTrack(m_trackWidgets.size());
            for (const auto& dcs : m_dragSelectedClips) {
                size_t ti = dcs.ref.trackIndex;
                if (ti < m_trackWidgets.size()) {
                    Track* trk = m_timeline->track(ti);
                    if (trk) {
                        size_t idx = trk->findClipIndexById(dcs.ref.clipId);
                        if (idx < trk->clipCount())
                            perTrack[ti].push_back(idx);
                    }
                }
            }
            for (size_t ti = 0; ti < m_trackWidgets.size(); ++ti) {
                m_trackWidgets[ti]->setSelectedClips(perTrack[ti]);
                m_trackWidgets[ti]->setDraggedClips(perTrack[ti]);
                m_trackWidgets[ti]->setGhostDragActive(m_ghostTrackVisible && !perTrack[ti].empty());
            }
        }
        updateClipDragAutoScroll(pos);
        break;
    }
    case DragMode::ClipTrimHead:
    {
        // The edit-point bracket is a static "you clicked this edge"
        // affordance; once the user actually drags to trim it should
        // disappear (Premiere hides it during the trim).  Cheap + safe
        // to call every tick — setEditPointTick early-returns at -1.
        clearEditPointSelection();
        int64_t newHead = m_dragOriginalIn + tickDelta;
        const bool ctrlHeld = (QApplication::keyboardModifiers() & Qt::ControlModifier);
        if (!ctrlHeld) {
        auto result = m_snapEngine.snap(newHead);
        if (result.didSnap) {
            newHead = result.snappedTick;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }
        } else {
            m_snapEngine.resetHysteresis();
            setSnapIndicator(-1);
        }

        if (Track* tr = m_timeline->track(m_dragClipRef.trackIndex)) {
            const int64_t origTail = m_dragOriginalIn + m_dragOriginalDuration;
            int64_t leftLimit = std::numeric_limits<int64_t>::min();
            for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                const Clip* other = tr->clip(ci);
                if (!other || other->id() == m_dragClipRef.clipId) continue;
                if (other->timelineOut() <= m_dragOriginalIn &&
                    other->timelineOut() > leftLimit)
                    leftLimit = other->timelineOut();
            }
            if (newHead < leftLimit) newHead = leftLimit;
            if (newHead >= origTail) newHead = origTail - 1;
        }

        int64_t headDelta = newHead - m_dragOriginalIn;

        // Trim primary clip
        {
            auto cmd = EditOperations::trimClip(*m_timeline, m_dragClipRef.trackIndex,
                                                m_dragClipRef.clipId,
                                                ClipEdge::Head, newHead);
            executeCommand(std::move(cmd));
        }

        // Trim other selected clips by the same delta — including clips on
        // OTHER tracks (e.g. an audio clip on A1 paired with a video clip
        // on V1).  Each clip is clamped independently to its own neighbour
        // on the left and to its own original tail, so a shorter clip in
        // the selection won't be over-trimmed.
        for (const auto& dcs : m_dragSelectedClips) {
            if (dcs.ref.clipId == m_dragClipRef.clipId &&
                dcs.ref.trackIndex == m_dragClipRef.trackIndex) continue;
            Track* selTr = m_timeline->track(dcs.ref.trackIndex);
            if (!selTr) continue;
            size_t si = selTr->findClipIndexById(dcs.ref.clipId);
            if (si >= selTr->clipCount()) continue;
            int64_t selNewHead = dcs.originalIn + headDelta;
            int64_t selOrigTail = dcs.originalIn + dcs.originalDuration;
            if (selNewHead < 0) selNewHead = 0;
            if (selNewHead >= selOrigTail) selNewHead = selOrigTail - 1;
            // also keep away from adjacent clip to the left
            int64_t selLeftLimit = std::numeric_limits<int64_t>::min();
            for (size_t ci = 0; ci < selTr->clipCount(); ++ci) {
                const Clip* other = selTr->clip(ci);
                if (!other || other->id() == dcs.ref.clipId) continue;
                if (other->timelineOut() <= dcs.originalIn &&
                    other->timelineOut() > selLeftLimit)
                    selLeftLimit = other->timelineOut();
            }
            if (selNewHead < selLeftLimit) selNewHead = selLeftLimit;
            auto selCmd = EditOperations::trimClip(*m_timeline, dcs.ref.trackIndex,
                                                    dcs.ref.clipId,
                                                    ClipEdge::Head, selNewHead);
            executeCommand(std::move(selCmd));
        }

        for (auto tw : m_trackWidgets)
            tw->setHoverEdgeTick(tw->trackIndex() == m_dragClipRef.trackIndex ? newHead : -1);
        onScrollChanged();
        updateClipDragAutoScroll(pos);
        break;
    }
    case DragMode::ClipTrimTail:
    {
        // See ClipTrimHead: hide the edit-point bracket once trimming.
        clearEditPointSelection();
        int64_t newTail = m_dragOriginalIn + m_dragOriginalDuration + tickDelta;
        const bool ctrlHeld = (QApplication::keyboardModifiers() & Qt::ControlModifier);
        if (!ctrlHeld) {
        auto result = m_snapEngine.snap(newTail);
        if (result.didSnap) {
            newTail = result.snappedTick;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }
        } else {
            m_snapEngine.resetHysteresis();
            setSnapIndicator(-1);
        }

        if (Track* tr = m_timeline->track(m_dragClipRef.trackIndex)) {
            const int64_t origTail = m_dragOriginalIn + m_dragOriginalDuration;
            int64_t rightLimit = std::numeric_limits<int64_t>::max();
            for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                const Clip* other = tr->clip(ci);
                if (!other || other->id() == m_dragClipRef.clipId) continue;
                if (other->timelineIn() >= origTail &&
                    other->timelineIn() < rightLimit)
                    rightLimit = other->timelineIn();
            }
            if (newTail > rightLimit) newTail = rightLimit;
            if (newTail <= m_dragOriginalIn) newTail = m_dragOriginalIn + 1;
        }

        int64_t tailDelta = newTail - (m_dragOriginalIn + m_dragOriginalDuration);

        // Trim primary clip
        {
            auto cmd = EditOperations::trimClip(*m_timeline, m_dragClipRef.trackIndex,
                                                m_dragClipRef.clipId,
                                                ClipEdge::Tail, newTail);
            executeCommand(std::move(cmd));
        }

        // Trim other selected clips by the same delta — across tracks too
        // (e.g. an A/V pair, or a V1+A1+A2 multi-selection).  Each clip is
        // clamped to its own right-side neighbour and its own original head.
        for (const auto& dcs : m_dragSelectedClips) {
            if (dcs.ref.clipId == m_dragClipRef.clipId &&
                dcs.ref.trackIndex == m_dragClipRef.trackIndex) continue;
            Track* selTr = m_timeline->track(dcs.ref.trackIndex);
            if (!selTr) continue;
            size_t si = selTr->findClipIndexById(dcs.ref.clipId);
            if (si >= selTr->clipCount()) continue;
            int64_t selOrigTail = dcs.originalIn + dcs.originalDuration;
            int64_t selNewTail = selOrigTail + tailDelta;
            // keep away from adjacent clip to the right
            int64_t selRightLimit = std::numeric_limits<int64_t>::max();
            for (size_t ci = 0; ci < selTr->clipCount(); ++ci) {
                const Clip* other = selTr->clip(ci);
                if (!other || other->id() == dcs.ref.clipId) continue;
                if (other->timelineIn() >= selOrigTail &&
                    other->timelineIn() < selRightLimit)
                    selRightLimit = other->timelineIn();
            }
            if (selNewTail > selRightLimit) selNewTail = selRightLimit;
            if (selNewTail <= dcs.originalIn) selNewTail = dcs.originalIn + 1;
            auto selCmd = EditOperations::trimClip(*m_timeline, dcs.ref.trackIndex,
                                                    dcs.ref.clipId,
                                                    ClipEdge::Tail, selNewTail);
            executeCommand(std::move(selCmd));
        }

        for (auto tw : m_trackWidgets)
            tw->setHoverEdgeTick(tw->trackIndex() == m_dragClipRef.trackIndex ? newTail : -1);
        onScrollChanged();
        updateClipDragAutoScroll(pos);
        break;
    }
    case DragMode::SlipTool:
    {
        auto cmd = EditOperations::slipClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId, tickDelta);
        executeCommand(std::move(cmd));
        onScrollChanged();
        break;
    }
    case DragMode::SlideTool:
    {
        auto cmd = EditOperations::slideClip(*m_timeline, m_dragClipRef.trackIndex,
                                            m_dragClipRef.clipId, tickDelta);
        executeCommand(std::move(cmd));
        onScrollChanged();
        break;
    }
    case DragMode::RollingEdit:
    {
        // Snap first, but respect the precomputed clamp — never let the
        // snap drag the seam past a source-content limit, otherwise the
        // indicator would appear at a tick the seam can't actually reach.
        const int64_t rawEditPoint = m_rollOriginalEditPoint + tickDelta;
        int64_t newEditPoint = rawEditPoint;
        const bool ctrlHeld = (QApplication::keyboardModifiers() & Qt::ControlModifier);
        if (!ctrlHeld) {
        auto snapResult = m_snapEngine.snap(rawEditPoint);
        if (snapResult.didSnap
            && snapResult.snappedTick >= m_rollMinEditPoint
            && snapResult.snappedTick <= m_rollMaxEditPoint)
        {
            newEditPoint = snapResult.snappedTick;
            setSnapIndicator(snapResult.snappedTick);
        }
        else
        {
            setSnapIndicator(-1);
            newEditPoint = std::clamp(rawEditPoint,
                                      m_rollMinEditPoint, m_rollMaxEditPoint);
        }
        } else {
            m_snapEngine.resetHysteresis();
            setSnapIndicator(-1);
            newEditPoint = std::clamp(rawEditPoint,
                                      m_rollMinEditPoint, m_rollMaxEditPoint);
        }

        Track* rollTrack = m_timeline->track(m_rollTrackIndex);
        if (rollTrack) {
            size_t li = rollTrack->findClipIndexById(m_rollLeftClipId);
            size_t ri = rollTrack->findClipIndexById(m_rollRightClipId);
            if (li < rollTrack->clipCount() && ri < rollTrack->clipCount()) {
                const int64_t rightEnd = m_rollRightOrigIn + m_rollRightOrigDur;
                int64_t leftNewDur = newEditPoint - m_rollLeftOrigIn;
                int64_t rightNewDur = rightEnd - newEditPoint;
                int64_t rightSrcDelta = newEditPoint - m_rollRightOrigIn;

                Clip* lc = rollTrack->clip(li);
                Clip* rc = rollTrack->clip(ri);
                lc->setDuration(leftNewDur);
                rc->setTimelineIn(newEditPoint);
                rc->setDuration(rightNewDur);
                rc->setSourceIn(m_rollRightOrigSrcIn + rightSrcDelta);

                // Live-update any transition anchored to this edit point
                // (cross-dissolve between L/R, or single-sided fade on
                // either side) so the user sees the transition slide
                // along with the seam — not stay at the original tick
                // until the drag is committed.
                for (auto& t : rollTrack->transitions()) {
                    const bool touchesLeft  = (t.leftClipId  == m_rollLeftClipId);
                    const bool touchesRight = (t.rightClipId == m_rollRightClipId);
                    const bool leftFadeOut  = touchesLeft  && t.rightClipId == 0;
                    const bool rightFadeIn  = touchesRight && t.leftClipId  == 0;
                    const bool dissolve     = touchesLeft  && touchesRight;
                    if (dissolve || leftFadeOut || rightFadeIn)
                        t.editPointTick = newEditPoint;
                }

                // Slide the edit-point brackets along with the seam so the
                // user has a stable visual anchor for where the cut is
                // landing. setEditPointTick has an internal early-out, so
                // calling this every move tick is cheap.
                setEditPointSelection(m_rollTrackIndex, newEditPoint,
                                       EditPointSide::Both);
            }
        }
        onScrollChanged();
        break;
    }
    case DragMode::PendingClipClick:
    {
        QPointF delta = pos - m_dragStart;
        if (delta.manhattanLength() < 5.0)
            break;

        // User started dragging — determine mode based on where the
        // original click landed relative to clip edges.
        Track* track = m_timeline->track(m_dragClipRef.trackIndex);
        size_t idx = track->findClipIndexById(m_dragClipRef.clipId);
        if (idx < track->clipCount()) {
            const Clip* clip = track->clip(idx);
            m_dragOriginalIn = clip->timelineIn();
            m_dragOriginalSourceIn = clip->sourceIn();
            m_dragOriginalDuration = clip->duration();
            m_dragOriginalTrack = m_dragClipRef.trackIndex;

            // Use the drag-start position (where user first clicked) to
            // determine edge proximity — not the current mouse position.
            double clickPx = m_dragStart.x() - headerWidth();
            double clipLeft  = m_layoutEngine.timeToPixelX(clip->timelineIn());
            double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

            const double kEdgeThreshold = edgeGrabPx(clipRight - clipLeft);
            if (std::abs(clickPx - clipLeft) < kEdgeThreshold) {
                m_dragMode = DragMode::ClipTrimHead;
                m_lastClickedEdge = { m_dragClipRef, ClipEdge::Head, true };
                // Wrap trim-move commands into a single undo step so one
                // drag = one Ctrl+Z (edge-click path in mousePressEvent
                // does this too; PendingClipClick→trim needs its own).
                if (m_commandStack)
                    m_commandStack->beginMacro("Trim clip head");
            } else if (std::abs(clickPx - clipRight) < kEdgeThreshold) {
                m_dragMode = DragMode::ClipTrimTail;
                m_lastClickedEdge = { m_dragClipRef, ClipEdge::Tail, true };
                if (m_commandStack)
                    m_commandStack->beginMacro("Trim clip tail");
            } else {
                m_dragMode = DragMode::ClipMove;
                m_lastClickedEdge.valid = false;
            }

            // Record original positions of ALL selected clips
            // (used by both ClipMove and multi-clip trim).  Also snapshot
            // any transitions referencing each clip — see mouse-press
            // path for rationale (live-drag drops them on cross-track).
            m_dragSelectedClips.clear();
            m_dragTargetTrack = m_dragClipRef.trackIndex;
            for (const auto& sel : m_selection.clips()) {
                Track* selTrack = m_timeline->track(sel.trackIndex);
                if (!selTrack) continue;
                size_t si = selTrack->findClipIndexById(sel.clipId);
                if (si < selTrack->clipCount()) {
                    DragClipState dcs;
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
                    m_dragSelectedClips.push_back(dcs);
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
        }
        break;
    }

    case DragMode::PendingMarquee:
    {
        QPointF delta = pos - m_dragStart;
        if (delta.manhattanLength() < 5.0)
            break;

        const bool shiftHeld =
            (QApplication::keyboardModifiers() & Qt::ShiftModifier);

        if (shiftHeld) {
            m_marqueeBaseSelection = m_selection.clips();
        } else {
            m_marqueeBaseSelection.clear();
            m_selection.clear();
            m_selectedTransitionTrack = SIZE_MAX;
            m_selectedTransitionIndex = SIZE_MAX;
            for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
            emit selectionChanged();
        }
        if (m_gapSelection.active) {
            m_gapSelection.active = false;
            for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                m_trackWidgets[w]->setGapHighlight(-1, -1);
        }
        m_dragMode = DragMode::MarqueeSelect;
        [[fallthrough]];
    }
    case DragMode::MarqueeSelect:
    {
        m_marqueeEnd = pos;
        m_marqueeLastMovePos = pos;

        if (!m_marqueeScrollTimer) {
            m_marqueeScrollTimer = new QTimer(this);
            m_marqueeScrollTimer->setInterval(16);
            connect(m_marqueeScrollTimer, &QTimer::timeout,
                    this, [this]() {
                if (m_dragMode != DragMode::MarqueeSelect) {
                    m_marqueeScrollTimer->stop();
                    return;
                }
                const int hdr = headerWidth();
                constexpr int kEdge   = 40;
                constexpr double kMaxV = 18.0;
                double dx = 0.0;
                const double cx = m_marqueeLastMovePos.x();
                if (cx < hdr + kEdge)
                    dx = -kMaxV * std::clamp(
                        (hdr + kEdge - cx) / kEdge, 0.0, 1.0);
                else if (cx > width() - kEdge)
                    dx =  kMaxV * std::clamp(
                        (cx - (width() - kEdge)) / kEdge, 0.0, 1.0);
                if (dx == 0.0) return;
                double oldScroll = m_layoutEngine.scrollX();
                double newScroll = std::max(0.0, oldScroll + dx);
                if (newScroll == oldScroll) return;
                m_layoutEngine.setScrollX(newScroll);
                m_dragStart.setX(m_dragStart.x() - (newScroll - oldScroll));
                onScrollChanged();
                refreshMarqueeSelection();
            });
        }
        const int hdr = headerWidth();
        constexpr int kEdge = 40;
        const bool atEdge = (pos.x() < hdr + kEdge) ||
                            (pos.x() > width() - kEdge);
        if (atEdge && !m_marqueeScrollTimer->isActive())
            m_marqueeScrollTimer->start();
        else if (!atEdge && m_marqueeScrollTimer->isActive())
            m_marqueeScrollTimer->stop();

        if (!m_rubberBand)
            m_rubberBand = new QRubberBand(QRubberBand::Rectangle, this);

        const int rightLimit = width();
        double rbX1 = std::clamp(m_dragStart.x(),
                                 static_cast<double>(hdr),
                                 static_cast<double>(rightLimit));
        double rbX2 = std::clamp(pos.x(),
                                 static_cast<double>(hdr),
                                 static_cast<double>(rightLimit));
        QRect marqueeRect = QRect(
            QPoint(static_cast<int>(std::min(rbX1, rbX2)),
                   static_cast<int>(std::min(m_dragStart.y(), pos.y()))),
            QPoint(static_cast<int>(std::max(rbX1, rbX2)),
                   static_cast<int>(std::max(m_dragStart.y(), pos.y()))));
        m_rubberBand->setGeometry(marqueeRect);
        m_rubberBand->show();

        refreshMarqueeSelection();
        update();
        break;
    }
    case DragMode::TransitionTrim:
    {
        Track* track = m_timeline->track(m_transTrimTrackIndex);
        if (track && m_transTrimIndex < track->transitionCount()) {
            Transition t = *track->transition(m_transTrimIndex);
            double px = pos.x() - headerWidth();
            int64_t dragTick = m_layoutEngine.pixelXToTime(px);

            constexpr int64_t kMinTransDur = 1600;
            int64_t newDur = t.duration;

            if (t.leftClipId == 0) {
                newDur = std::max(kMinTransDur, dragTick - t.editPointTick);
            } else if (t.rightClipId == 0) {
                newDur = std::max(kMinTransDur, t.editPointTick - dragTick);
            } else {
                int64_t dist = std::abs(dragTick - t.editPointTick);
                newDur = std::max(kMinTransDur, dist * 2);
            }

            t.duration = newDur;
            track->setTransition(m_transTrimIndex, t);
        }
        onScrollChanged();
        break;
    }
    default:
        break;
    }

    event->accept();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
