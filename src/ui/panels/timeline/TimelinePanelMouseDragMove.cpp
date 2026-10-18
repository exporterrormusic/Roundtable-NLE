/*
 * TimelinePanelMouseDragMove.cpp — mouseMoveEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelineTrimTool.h"
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
            int64_t anchor = m_dragTool ? m_dragTool->frameAnchor() : -1;
            switch (m_dragMode) {
            case DragMode::ClipMove:
                anchor = m_dragOriginalIn;
                break;
            default:
                break;
            }
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
        continueClipMoveDrag(pos, tickDelta);
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
        continueTransitionTrimDrag(pos);
        break;
    case DragMode::TransitionHandle:
        continueTransitionHandleDrag(pos);
        break;
    default:
        break;
    }

    event->accept();
}

void TimelinePanel::continueClipMoveDrag(QPointF pos, int64_t tickDelta)
{
    int64_t newIn = m_dragOriginalIn + tickDelta;
    if (newIn < 0) newIn = 0;

    // Snap every head + tail of every dragged clip (the primary alone for
    // a single-clip drag; all of them for a multi-clip / linked A/V drag,
    // so clips on other tracks magnetise too). snapEdges is STICKY: once
    // an edge seats against a target it holds until the nearest edge is
    // dragged past the release zone, so the lock no longer drops the
    // moment a tail edge (or a linked clip's edge) is the one that snapped.
    // Holding Ctrl suppresses magnetism entirely for precision moves.
    const bool ctrlHeld = (QApplication::keyboardModifiers() & Qt::ControlModifier);
    if (!ctrlHeld) {
        const int64_t baseDelta = newIn - m_dragOriginalIn;
        std::vector<int64_t> edges;
        if (m_dragSelectedClips.size() > 1) {
            edges.reserve(m_dragSelectedClips.size() * 2);
            for (const auto& dcs : m_dragSelectedClips) {
                const int64_t cIn = dcs.originalIn + baseDelta;
                edges.push_back(cIn);
                edges.push_back(cIn + dcs.originalDuration);
            }
        } else {
            edges = { newIn, newIn + m_dragOriginalDuration };
        }
        auto result = m_snapEngine.snapEdges(edges);
        if (result.didSnap) {
            newIn += result.delta;
            if (newIn < 0) newIn = 0;
            setSnapIndicator(result.snappedTick);
        } else {
            setSnapIndicator(-1);
        }
    } else {
        // Ctrl held: suppress magnetism, reset sticky state so
        // releasing Ctrl doesn't cause a sudden jump to a stale target.
        m_snapEngine.resetHysteresis();
        setSnapIndicator(-1);
    }

    // Compute the actual delta applied to the primary clip
    int64_t actualDelta = newIn - m_dragOriginalIn;

    // EDIT INVARIANT (see CommandStack.h "EDIT DISCIPLINE"):
    // Group-floor clamp: when multiple clips are dragged left as a
    // unit, raise the GROUP delta so the leftmost selected clip lands at
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

    // Determine the dragged clip's track type up front — it drives both
    // the cross-track target and the new-track ghost zones below.
    TrackType dragType = TrackType::Video;
    if (!m_dragSelectedClips.empty()) {
        Track* srcTr = m_timeline->track(m_dragSelectedClips[0].ref.trackIndex);
        if (srcTr) dragType = srcTr->type();
    }

    // ── New-VIDEO-track-above zone ──────────────────────────────────────
    // The track column is top-aligned with a bottom stretch (see
    // TimelinePanelTracks.cpp), and the ruler is a separate widget, so there
    // is cursor-reachable empty space BELOW the bottom track (the audio-
    // below ghost zone) but NONE above the top track — a strict "cursor
    // above the first track" test can never be reached.  So treat a thin
    // band at the TOP EDGE of the top-most real video track as the
    // new-track-above zone when dragging a video clip.  (Skip dividers + the
    // pinned caption track so the band anchors to a real, media-hosting
    // video track.)
    double topVideoTop = -1.0;
    size_t topVideoIdx = SIZE_MAX;
    int    topVideoH   = 80;
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* t = m_timeline->track(i);
        if (!t || t->isDivider() || t->isCaptionTrack()) continue;
        if (t->type() == TrackType::Video) {
            topVideoIdx = i;
            if (i < m_trackWidgets.size()) {
                topVideoTop = m_trackWidgets[i]->mapTo(this, QPoint(0, 0)).y();
                topVideoH   = m_trackWidgets[i]->height();
            }
            break;
        }
    }
    const int  newTrackBand = std::min(14, std::max(6, topVideoH / 3));
    const bool videoAboveZone =
        (dragType == TrackType::Video && topVideoTop >= 0.0 &&
         pos.y() < topVideoTop + newTrackBand);

    // Detect cross-track target
    size_t targetTrack = hitTestTrack(pos.y());
    if (!videoAboveZone && targetTrack < m_timeline->trackCount()) {
        m_dragTargetTrack = targetTrack;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();
    } else if (!m_trackWidgets.empty()) {
        // ── Ghost track detection (Premiere Pro-style) ──────────────
        // Cursor is outside any track, OR inside the new-video-track-above
        // band at the top edge of the top video track.
        auto lastTw  = m_trackWidgets.back();
        QPoint lastBot  = lastTw->mapTo(this, QPoint(0, lastTw->height()));

        // Last audio track index (for the audio-below zone). Skip divider
        // rows — they're TrackType::Video but not real audio tracks.
        size_t lastAudioIdx = SIZE_MAX;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            Track* t = m_timeline->track(i);
            if (!t || t->isDivider()) continue;
            if (t->type() != TrackType::Video)
                lastAudioIdx = i;
        }

        // Scroll area X position
        QPoint scrollOrig = m_verticalScroll->mapTo(this, QPoint(0, 0));
        int ghostX = scrollOrig.x();
        int ghostW = m_verticalScroll->width();

        if (videoAboveZone && topVideoIdx < m_trackWidgets.size()) {
            m_ghostTrackVisible = true;
            m_ghostTrackIsAbove = true;
            m_ghostTrackHeight = m_trackWidgets[topVideoIdx]->height();
            m_ghostTrackY = static_cast<int>(topVideoTop) - m_ghostTrackHeight;
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

        Track* srcTr = m_timeline->track(dcs.ref.trackIndex);
        if (!srcTr) continue;
        // Clamp onto the nearest track that can host this clip's kind. A
        // video clip dragged down into the audio section (or onto the V/A
        // divider) lands on the lowest video track instead of being
        // rejected — and this matches where the release commits it, so the
        // preview and the committed result agree. clampTrackToHostTrack
        // already excludes dividers + the caption track and keeps the
        // index in array bounds.
        size_t dstTrack = clampTrackToHostTrack(dst, srcTr);
        if (dstTrack == SIZE_MAX) dstTrack = dcs.ref.trackIndex;
        Track* dstTr = m_timeline->track(dstTrack);
        // Never relinquish the source clip unless the destination can
        // actually own it. Track::addClip takes unique ownership even when
        // it rejects a divider, which previously destroyed the clip while
        // the pointer crossed an organizational divider row.
        if (!dstTr || dstTr->isDivider() || dstTr->isCaptionTrack()
                || dstTr->type() != srcTr->type()) {
            continue;
        }

        size_t curTrack = dcs.ref.trackIndex;

        if (dstTrack != curTrack) {
            Track* curTr = m_timeline->track(curTrack);
            size_t idx = curTr->findClipIndexById(dcs.ref.clipId);
            if (idx >= curTr->clipCount()) continue;
            auto clipPtr = curTr->removeClip(idx);
            if (!clipPtr) continue;
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
        m_dragOriginalTrack = m_dragClipRef.trackIndex;

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
            if (!selTrack || selTrack->isLocked()) continue;
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
        if (trimEdge)
            beginTrim(*trimEdge);
    }
}

void TimelinePanel::beginTrim(ClipEdge edge)
{
    std::vector<TimelineTrimTool::ClipEdgeStart> others;
    others.reserve(m_dragSelectedClips.size());
    for (const auto& state : m_dragSelectedClips)
        others.push_back({state.ref, state.originalIn, state.originalDuration});
    m_dragTool = std::make_unique<TimelineTrimTool>(
        *this, edge,
        TimelineTrimTool::ClipEdgeStart{m_dragClipRef, m_dragOriginalIn, m_dragOriginalDuration},
        std::move(others));
}

void TimelinePanel::continueTransitionTrimDrag(QPointF pos)
{
    Track* track = m_timeline->track(m_transTrimTrackIndex);
    if (track && !track->isLocked()
            && m_transTrimIndex < track->transitionCount()) {
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

        // ── Clamp: don't overlap clip boundaries or other transitions ──
        int64_t maxDur = std::numeric_limits<int64_t>::max();
        if (t.leftClipId == 0) {
            // Fade-in: extends right into rightClip.
            // Clip boundary: cannot extend past right clip's end.
            size_t ri = track->findClipIndexById(t.rightClipId);
            if (ri < track->clipCount())
                maxDur = track->clip(ri)->timelineOut() - t.editPointTick;

            // Adjacent transitions: stop at the nearest transition whose
            // start is beyond our edit point.
            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transTrimIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (os > t.editPointTick) {
                    int64_t limit = os - t.editPointTick;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        } else if (t.rightClipId == 0) {
            // Fade-out: extends left into leftClip.
            size_t li = track->findClipIndexById(t.leftClipId);
            if (li < track->clipCount())
                maxDur = t.editPointTick - track->clip(li)->timelineIn();

            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transTrimIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (oe < t.editPointTick) {
                    int64_t limit = t.editPointTick - oe;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        } else {
            // Cross-dissolve: extends both ways.
            size_t li = track->findClipIndexById(t.leftClipId);
            size_t ri = track->findClipIndexById(t.rightClipId);
            int64_t leftLimit = std::numeric_limits<int64_t>::max();
            int64_t rightLimit = std::numeric_limits<int64_t>::max();
            if (li < track->clipCount())
                leftLimit = t.editPointTick - track->clip(li)->timelineIn();
            if (ri < track->clipCount())
                rightLimit = track->clip(ri)->timelineOut() - t.editPointTick;
            maxDur = std::min(leftLimit, rightLimit) * 2;  // dur/2 each side

            for (size_t oi = 0; oi < track->transitionCount(); ++oi) {
                if (oi == m_transTrimIndex) continue;
                const Transition* ot = track->transition(oi);
                int64_t os, oe;
                ot->getRange(os, oe);
                if (os > t.editPointTick) {
                    int64_t limit = (os - t.editPointTick) * 2;
                    if (limit < maxDur) maxDur = limit;
                }
                if (oe < t.editPointTick) {
                    int64_t limit = (t.editPointTick - oe) * 2;
                    if (limit < maxDur) maxDur = limit;
                }
            }
        }
        if (maxDur < kMinTransDur) maxDur = kMinTransDur;
        if (newDur > maxDur) newDur = maxDur;

        t.duration = newDur;

        // Execute through command stack so the project is marked
        // modified during the drag (same pattern as clip trim).
        // A macro wraps all drag ticks into a single undo step.
        if (m_commandStack && !m_commandStack->isMacroActive())
            m_commandStack->beginMacro("Trim transition");
        auto cmd = std::make_unique<SetTransitionPropertyCommand>(
            track, m_transTrimIndex, t);
        executeCommand(std::move(cmd));
        showTransitionDurationTip(pos, newDur);
    }
    onScrollChanged();
}

void TimelinePanel::continueTransitionHandleDrag(QPointF pos)
{
    // Deadzone: a plain click on a handle only selects the edge.
    if (std::abs(pos.x() - m_dragStart.x()) < 4.0) return;

    Track* track = m_timeline->track(m_handleDrag.trackIndex);
    const size_t idx = track ? track->findClipIndexById(m_handleDrag.clipId)
                             : SIZE_MAX;
    if (!track || track->isLocked() || idx >= track->clipCount()) return;
    if (EditOperations::clipEdgeHasTransition(
            *track, m_handleDrag.clipId, m_handleDrag.edge))
        return;
    const Clip* clip = track->clip(idx);

    // Only an inward drag makes a transition.
    const bool head = m_handleDrag.edge == ClipEdge::Head;
    const int64_t dragTick =
        m_layoutEngine.pixelXToTime(pos.x() - headerWidth());
    const int64_t inward = head ? dragTick - clip->timelineIn()
                                : clip->timelineOut() - dragTick;
    if (inward <= 0) return;

    // The transition's inner edge follows the cursor: a one-sided fade spans
    // the whole drag, a dissolve centered on a cut spans it on both sides.
    Transition trans = EditOperations::defaultTransitionAtEdge(
        *track, *clip, m_handleDrag.edge, inward);
    if (trans.leftClipId != 0 && trans.rightClipId != 0)
        trans.duration = inward * 2;
    if (!EditOperations::fitTransitionToAvailableDuration(*track, trans))
        return;

    // One undo step for the whole gesture: the add plus every live resize
    // (continueTransitionTrimDrag reuses this macro; release closes it).
    if (m_commandStack && !m_commandStack->isMacroActive())
        m_commandStack->beginMacro("Add Transition");
    executeCommand(std::make_unique<AddTransitionCommand>(
        track, idx, idx, trans));

    size_t transIdx = SIZE_MAX;
    for (size_t i = 0; i < track->transitionCount(); ++i) {
        const Transition* t = track->transition(i);
        if (t && t->leftClipId == trans.leftClipId
            && t->rightClipId == trans.rightClipId) {
            transIdx = i;
            break;
        }
    }
    if (transIdx == SIZE_MAX) return;

    // From here on it is an ordinary drag of the new transition's inner edge.
    clearEditPointSelection();
    m_transTrimTrackIndex    = m_handleDrag.trackIndex;
    m_transTrimIndex         = transIdx;
    m_transTrimIsStart       = !head;
    m_transTrimOrigDuration  = 0;
    m_transTrimOrigEditPoint = trans.editPointTick;
    m_selectedTransitionTrack = m_handleDrag.trackIndex;
    m_selectedTransitionIndex = transIdx;
    for (auto tw : m_trackWidgets) {
        tw->setTransitionHandleHover(0, -1);
        tw->setSelectedTransition(
            tw->trackIndex() == m_selectedTransitionTrack ? transIdx : SIZE_MAX);
    }
    m_dragMode = DragMode::TransitionTrim;
    setCursor(Qt::SizeHorCursor);
    emit selectionChanged();
    continueTransitionTrimDrag(pos);
}

void TimelinePanel::showTransitionDurationTip(QPointF pos, int64_t duration)
{
    // Premiere shows the change and the total length while a transition
    // edge is dragged.
    const int64_t delta = duration - m_transTrimOrigDuration;
    const QString text = QStringLiteral("%1%2   Duration %3")
        .arg(delta < 0 ? QStringLiteral("-") : QStringLiteral("+"),
             QString::fromStdString(m_layoutEngine.formatTimecode(
                 delta < 0 ? -delta : delta)),
             QString::fromStdString(m_layoutEngine.formatTimecode(duration)));
    QToolTip::showText(mapToGlobal(pos.toPoint()) + QPoint(14, -36), text, this);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
