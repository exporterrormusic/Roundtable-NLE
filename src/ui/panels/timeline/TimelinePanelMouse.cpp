/*
 * TimelinePanelMouse.cpp - Mouse event handlers for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"
#include "widgets/TimelineRuler.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/TransitionCmds.h"

#include <QMouseEvent>
#include <QToolTip>
#include <QMenu>
#include <QRubberBand>

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

namespace rt {
void TimelinePanel::mousePressEvent(QMouseEvent* event)
{
    // Reset any stale drag mode from a previous incomplete interaction
    // (e.g., lost mouse capture, interrupted drag). This prevents being
    // stuck in a mode that ignores subsequent input.  Also clean up any
    // visible artifacts left behind by an interrupted drag.
    if (m_dragMode != DragMode::None) {
        // Close any dangling undo macro from the interrupted drag so
        // future commands don't accumulate into a stale macro.
        if (m_commandStack && m_commandStack->isMacroActive()) {
            m_commandStack->endMacro();
        }
        m_dragMode = DragMode::None;
        m_dragClipRef = {};
        m_dragSelectedClips.clear();
        m_dragTargetTrack = SIZE_MAX;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();
        if (m_marqueeScrollTimer) m_marqueeScrollTimer->stop();
        m_marqueeLastMovePos = QPointF();
        if (m_clipDragScrollTimer) m_clipDragScrollTimer->stop();
        m_clipDragLastMovePos = QPointF();
        if (m_rubberBand) m_rubberBand->hide();
        setCursor(Qt::ArrowCursor);
        for (auto tw : m_trackWidgets)
            tw->setHoverEdgeTick(-1);
    }
    m_dragMode = DragMode::None;

    // The Zoom tool is a pure view operation (like in Premiere): a
    // zoom-in/out click must NOT disturb any selection.  Clip selection
    // already survives it (press-start never clears m_selection); the
    // edit-point bracket and transition highlight must survive it too,
    // otherwise zooming silently wipes the "Ctrl+T will add a transition
    // here" indicator even though the edit point is still selected.
    const bool viewOnlyPress = (m_activeTool == EditTool::Zoom);

    if (!viewOnlyPress) {
        // Wipe any prior "between clips" edit-point selection at the start
        // of every press; specific branches below re-set it when needed.
        clearEditPointSelection();

        // Also wipe any stale snap-indicator line (the white dashed
        // vertical line drawn at a snap target during a trim/roll drag).
        // It is only cleared inside the drag-move handlers, so without
        // this a snapped roller/trim leaves the dashed line stuck at the
        // cut point until the next drag — it reads like a phantom
        // selection.  Drag branches below re-set it as the mouse moves.
        setSnapIndicator(-1);

        // Likewise wipe any prior transition selection at press-start; the
        // transition-body-click and transition-trim branches below
        // re-select the relevant transition when appropriate.  Without
        // this, click paths that return early (the transition-trim
        // handle, a clip-edge bracket click, etc.) leave the previously
        // selected transition highlighted forever — so clicking a second
        // transition never moves the indicator and it stays stuck on the
        // first one.
        clearTransitionSelection();
    }

    if (!m_timeline || event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    QPointF pos = event->position();
    m_dragStart = pos;

    switch (m_activeTool)
    {
    case EditTool::Razor:
    {
        // Split at click position
        bool didSplit = false;
        size_t ti = hitTestTrack(pos.y());
        if (ti < m_timeline->trackCount())
        {
            double px = pos.x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            Track* track = m_timeline->track(ti);
            for (size_t ci = 0; ci < track->clipCount(); ++ci)
            {
                const Clip* clip = track->clip(ci);
                if (tick > clip->timelineIn() && tick < clip->timelineOut())
                {
                    auto cmd = EditOperations::splitClip(*m_timeline, ti, clip->id(), tick);
                    executeCommand(std::move(cmd));
                    refreshTrackContents();
                    didSplit = true;
                    break;
                }
            }
        }
        if (!didSplit) {
            // Empty-space click — deselect on release.
            m_dragMode = DragMode::PendingMarquee;
        }
        event->accept();
        return;
    }
    case EditTool::Selection:
    {
        // â”€â”€ Check for transition-edge drag FIRST â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        {
            double px = pos.x() - headerWidth();
            size_t ti = hitTestTrack(pos.y());
            if (ti < m_timeline->trackCount()) {
                Track* track = m_timeline->track(ti);
                constexpr double kHandleThreshold = 8.0;  // pixels
                for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                    const Transition* trans = track->transition(trI);
                    if (!trans) continue;
                    int64_t tStart, tEnd;
                    trans->getRange(tStart, tEnd);
                    double pxStart = m_layoutEngine.timeToPixelX(tStart);
                    double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);

                    // Determine which edges are draggable
                    bool canDragStart = (trans->rightClipId == 0)    // fade-out: start edge
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve
                    bool canDragEnd   = (trans->leftClipId == 0)     // fade-in: end edge
                                     || (trans->leftClipId != 0 && trans->rightClipId != 0); // cross-dissolve

                    if (canDragStart && std::abs(px - pxStart) < kHandleThreshold) {
                        m_dragMode = DragMode::TransitionTrim;
                        m_transTrimTrackIndex  = ti;
                        m_transTrimIndex       = trI;
                        m_transTrimIsStart     = true;
                        m_transTrimOrigDuration   = trans->duration;
                        m_transTrimOrigEditPoint  = trans->editPointTick;
                        // Re-select the grabbed transition (press-start
                        // cleared it) so the indicator follows the click
                        // to this transition instead of staying stuck.
                        m_selection.clear();
                        m_selectedTransitionTrack = ti;
                        m_selectedTransitionIndex = trI;
                        emit selectionChanged();
                        event->accept();
                        return;
                    }
                    if (canDragEnd && std::abs(px - pxEnd) < kHandleThreshold) {
                        m_dragMode = DragMode::TransitionTrim;
                        m_transTrimTrackIndex  = ti;
                        m_transTrimIndex       = trI;
                        m_transTrimIsStart     = false;
                        m_transTrimOrigDuration   = trans->duration;
                        m_transTrimOrigEditPoint  = trans->editPointTick;
                        // Re-select the grabbed transition (press-start
                        // cleared it) so the indicator follows the click
                        // to this transition instead of staying stuck.
                        m_selection.clear();
                        m_selectedTransitionTrack = ti;
                        m_selectedTransitionIndex = trI;
                        emit selectionChanged();
                        event->accept();
                        return;
                    }
                }
            }
        }

        // â”€â”€ Check for transition body click (select transition) â”€â”€â”€â”€â”€â”€â”€â”€
        // SKIP when the click is within the edge-grab zone of any clip's
        // head or tail on this track: otherwise a clip with a transition
        // on its edge becomes un-trimmable — the transition body covers
        // the clip edge.  The clip-edge bracket branch below handles
        // those clicks (Premiere-style: edge wins, drag to trim).
        // The transition's own start/end handles (TransitionTrim branch
        // above) and the body away from the seam are still selectable.
        {
            double px = pos.x() - headerWidth();
            size_t ti = hitTestTrack(pos.y());
            if (ti < m_timeline->trackCount()) {
                Track* track = m_timeline->track(ti);

                bool nearClipEdge = false;
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    const Clip* c = track->clip(ci);
                    if (!c) continue;
                    double cl = m_layoutEngine.timeToPixelX(c->timelineIn());
                    double cr = m_layoutEngine.timeToPixelX(c->timelineOut());
                    double ez = edgeGrabPx(cr - cl);
                    if (std::abs(px - cl) < ez || std::abs(px - cr) < ez) {
                        nearClipEdge = true;
                        break;
                    }
                }

                if (!nearClipEdge) {
                    for (size_t trI = 0; trI < track->transitionCount(); ++trI) {
                        const Transition* trans = track->transition(trI);
                        if (!trans) continue;
                        int64_t tStart, tEnd;
                        trans->getRange(tStart, tEnd);
                        double pxStart = m_layoutEngine.timeToPixelX(tStart);
                        double pxEnd   = m_layoutEngine.timeToPixelX(tEnd);
                        if (px >= pxStart && px <= pxEnd) {
                            m_selection.clear();
                            m_selectedTransitionTrack = ti;
                            m_selectedTransitionIndex = trI;
                            // Update track widgets
                            for (size_t w = 0; w < m_trackWidgets.size(); ++w) {
                                m_trackWidgets[w]->setSelectedClips({});
                                m_trackWidgets[w]->setSelectedTransition(w == ti ? trI : SIZE_MAX);
                            }
                            emit selectionChanged();
                            emit transitionSelected(ti, trI);
                            event->accept();
                            return;
                        }
                    }
                }
            }
        }

        auto hitRef = hitTestClip(pos);

        // Edge-halo fallback: when zoomed out, a clip can be only a few
        // pixels wide, so the press lands just outside the clip's tick
        // range (hitTestClip returns no match) yet still within the edge
        // grab zone. Scan the pressed track for any clip edge within
        // edgeGrabPx of the cursor so the user can still grab + trim it.
        if (!hitRef) {
            size_t tiScan = hitTestTrack(pos.y());
            if (tiScan < m_timeline->trackCount()) {
                const Track* trkScan = m_timeline->track(tiScan);
                double pxScan = pos.x() - headerWidth();
                for (size_t ci = 0; ci < trkScan->clipCount(); ++ci) {
                    const Clip* c = trkScan->clip(ci);
                    if (!c) continue;
                    double l = m_layoutEngine.timeToPixelX(c->timelineIn());
                    double r = m_layoutEngine.timeToPixelX(c->timelineOut());
                    double zone = edgeGrabPx(r - l);
                    if (std::abs(pxScan - l) < zone
                            || std::abs(pxScan - r) < zone) {
                        hitRef = ClipRef{ tiScan, c->id() };
                        break;
                    }
                }
            }
        }

        if (hitRef)
        {
            // Clear any gap selection when clicking a clip
            if (m_gapSelection.active) {
                m_gapSelection.active = false;
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
            }

            // ── Edge / seam click detection (Premiere-style) ────────────
            // Clicking within the edge-grab zone of either the head or
            // tail of the hit clip selects an EDIT POINT (single bracket
            // for an isolated edge, two facing brackets for a connected
            // seam) and primes a trim drag of THAT clip's edge.  The
            // whole-clip highlight is suppressed — only the bracket
            // shows, like Premiere.  This also enables Ctrl+T to add a
            // transition at the clicked edge (m_lastClickedEdge).
            {
                Track* hitTrack = m_timeline->track(hitRef->trackIndex);
                size_t hitIdx   = hitTrack ? hitTrack->findClipIndexById(hitRef->clipId)
                                           : SIZE_MAX;
                if (hitTrack && hitIdx < hitTrack->clipCount())
                {
                    const Clip* hitClip = hitTrack->clip(hitIdx);
                    const double pxLocal  = pos.x() - headerWidth();
                    const double clipL    = m_layoutEngine.timeToPixelX(hitClip->timelineIn());
                    const double clipR    = m_layoutEngine.timeToPixelX(hitClip->timelineOut());
                    const double edgeZone = edgeGrabPx(clipR - clipL);

                    const bool nearLeft  = std::abs(pxLocal - clipL) < edgeZone;
                    const bool nearRight = !nearLeft
                                        && std::abs(pxLocal - clipR) < edgeZone;

                // Clicking within the edge-grab zone of a clip
                // immediately selects the edit point and primes a
                // trim/transition operation, but ONLY if that clip
                // (or its touching neighbour at a seam) is already
                // selected.  The first click on an unselected
                // isolated clip always selects the whole clip,
                // even near its edge — Premiere-style.
                // This also lets Ctrl+T add a transition with a
                // second edge click + shortcut.
                const bool shiftClick = event->modifiers() & Qt::ShiftModifier;
                bool edgeModeOk = false;
                if (!shiftClick && (nearLeft || nearRight)) {
                    // The hit clip itself is already selected — edge mode.
                    if (m_selection.isSelected(*hitRef)) {
                        edgeModeOk = true;
                    } else {
                        // At a seam, hitTestClip always returns the RIGHT
                        // clip.  If the LEFT clip is selected, the user
                        // clicked the seam expecting to trim — enter edge
                        // mode so we keep the selection instead of
                        // deselecting the left clip and selecting the
                        // right one (which reads as "a different corner").
                        for (size_t ci = 0; ci < hitTrack->clipCount(); ++ci) {
                            const Clip* n = hitTrack->clip(ci);
                            if (!n || n->id() == hitClip->id()) continue;
                            if (nearLeft && n->timelineOut() == hitClip->timelineIn()
                                && m_selection.isSelected(ClipRef{hitRef->trackIndex, n->id()})) {
                                edgeModeOk = true;
                                break;
                            }
                            if (nearRight && n->timelineIn() == hitClip->timelineOut()
                                && m_selection.isSelected(ClipRef{hitRef->trackIndex, n->id()})) {
                                edgeModeOk = true;
                                break;
                            }
                        }
                    }
                }
                if (edgeModeOk)
                    {
                        // Look for a touching neighbour on the relevant side.
                        const Clip* leftNeighbour  = nullptr;
                        const Clip* rightNeighbour = nullptr;
                        if (nearLeft) {
                            for (size_t ci = 0; ci < hitTrack->clipCount(); ++ci) {
                                const Clip* n = hitTrack->clip(ci);
                                if (n->id() != hitClip->id() &&
                                    n->timelineOut() == hitClip->timelineIn()) {
                                    leftNeighbour = n;
                                    break;
                                }
                            }
                        } else { // nearRight
                            for (size_t ci = 0; ci < hitTrack->clipCount(); ++ci) {
                                const Clip* n = hitTrack->clip(ci);
                                if (n->id() != hitClip->id() &&
                                    n->timelineIn() == hitClip->timelineOut()) {
                                    rightNeighbour = n;
                                    break;
                                }
                            }
                        }

                        // At a connected seam, hitTestClip is biased: it
                        // uses [in, out) ranges so a click *at* the seam
                        // tick always lands on the RIGHT clip.  If the
                        // user actually clicked just left of the seam
                        // pixel they wanted to trim the LEFT clip's tail
                        // — never the right clip's head.  Use the click's
                        // pixel position relative to the seam to pick the
                        // correct side; for an isolated edge fall back
                        // to the hit clip's edge.
                        const Clip* trimClip = hitClip;
                        ClipEdge    trimEdge;
                        int64_t     seamTick;
                        EditPointSide side;
                        if (leftNeighbour) {
                            // Seam between leftNeighbour and hitClip at clipL.
                            const double seamPx = clipL;
                            if (pxLocal < seamPx) {
                                trimClip = leftNeighbour;
                                trimEdge = ClipEdge::Tail;
                            } else {
                                trimClip = hitClip;
                                trimEdge = ClipEdge::Head;
                            }
                            seamTick = hitClip->timelineIn();
                            side = EditPointSide::Both;
                        } else if (rightNeighbour) {
                            // Seam between hitClip and rightNeighbour at clipR.
                            const double seamPx = clipR;
                            if (pxLocal < seamPx) {
                                trimClip = hitClip;
                                trimEdge = ClipEdge::Tail;
                            } else {
                                trimClip = rightNeighbour;
                                trimEdge = ClipEdge::Head;
                            }
                            seamTick = hitClip->timelineOut();
                            side = EditPointSide::Both;
                        } else {
                            // Isolated edge — no neighbour, hit clip's edge.
                            trimClip = hitClip;
                            trimEdge = nearLeft ? ClipEdge::Head : ClipEdge::Tail;
                            seamTick = nearLeft ? hitClip->timelineIn()
                                                : hitClip->timelineOut();
                            side = nearLeft ? EditPointSide::HeadOnly
                                            : EditPointSide::TailOnly;
                        }

                        // Locate the chosen clip's owning track index.
                        // Always the same as the hit clip's track since
                        // neighbours come from the same hitTrack.
                        const size_t trimTrackIdx = hitRef->trackIndex;

                        // If the user grabbed the edge of a clip that's
                        // part of a current multi-selection (e.g. a V/A
                        // pair, or several clips across tracks), preserve
                        // the selection so the trim drag extends every
                        // selected clip's matching edge by the same delta.
                        // Otherwise fall back to the Premiere-style edit-
                        // point behaviour: clear selection, show the
                        // bracket, single-clip trim.
                        ClipRef trimRef{ trimTrackIdx, trimClip->id() };
                        const bool keepSelection =
                            (m_selection.count() > 1 &&
                             m_selection.isSelected(trimRef));

                        if (!keepSelection) {
                            // Single-clip edge click — Premiere-style edit
                            // point behaviour: clear selection, show the
                            // bracket, single-clip trim.  m_lastClickedEdge
                            // is recorded so Ctrl+T can add a transition.
                            // Also drop any selected transition so its
                            // highlight doesn't linger.
                            m_selection.clear();
                            m_selectedTransitionTrack = SIZE_MAX;
                            m_selectedTransitionIndex = SIZE_MAX;
                            emit selectionChanged();
                        }
                        // Show the edit-point bracket in either case so the
                        // user gets visual confirmation that an edge was
                        // grabbed — multi-selection trim previously had no
                        // bracket at all, which read as "nothing happened".
                        setEditPointSelection(trimTrackIdx, seamTick, side);

                        m_dragClipRef          = trimRef;
                        m_dragOriginalIn       = trimClip->timelineIn();
                        m_dragOriginalSourceIn = trimClip->sourceIn();
                        m_dragOriginalDuration = trimClip->duration();
                        m_dragOriginalTrack    = trimTrackIdx;
                        m_dragMode = (trimEdge == ClipEdge::Tail)
                                       ? DragMode::ClipTrimTail
                                       : DragMode::ClipTrimHead;
                        m_lastClickedEdge = { m_dragClipRef, trimEdge, true };

                        // Wrap all trim-move commands into a single undo
                        // step so one drag = one Ctrl+Z, not hundreds.
                        if (m_commandStack)
                            m_commandStack->beginMacro(
                                (m_dragMode == DragMode::ClipTrimTail)
                                    ? "Trim clip tail" : "Trim clip head");

                        // Snapshot every selected clip's original in/
                        // duration so the trim handlers can extend each
                        // one by the same delta. For the single-clip case
                        // (no multi-selection) the snapshot is just the
                        // grabbed edge's clip.
                        m_dragSelectedClips.clear();
                        m_dragTargetTrack = trimTrackIdx;
                        if (keepSelection) {
                            for (const auto& sel : m_selection.clips()) {
                                Track* selTrack = m_timeline->track(sel.trackIndex);
                                if (!selTrack) continue;
                                size_t si = selTrack->findClipIndexById(sel.clipId);
                                if (si >= selTrack->clipCount()) continue;
                                DragClipState dcs;
                                dcs.ref = sel;
                                dcs.originalIn = selTrack->clip(si)->timelineIn();
                                dcs.originalDuration = selTrack->clip(si)->duration();
                                dcs.originalSourceIn = selTrack->clip(si)->sourceIn();
                                dcs.originalTrack = sel.trackIndex;
                                m_dragSelectedClips.push_back(dcs);
                            }
                        } else {
                            DragClipState dcs;
                            dcs.ref = m_dragClipRef;
                            dcs.originalIn = m_dragOriginalIn;
                            dcs.originalDuration = m_dragOriginalDuration;
                            dcs.originalSourceIn = m_dragOriginalSourceIn;
                            dcs.originalTrack = m_dragOriginalTrack;
                            m_dragSelectedClips.push_back(dcs);
                        }

                        m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
                        std::vector<uint64_t> excludeIds;
                        excludeIds.reserve(m_dragSelectedClips.size() + 2);
                        for (const auto& dcs : m_dragSelectedClips)
                            excludeIds.push_back(dcs.ref.clipId);

                        // Also exclude clip edges that are directly
                        // adjacent on the same track.  After a split
                        // the two halves share the same edge position;
                        // without this exclusion the user must fight
                        // snapping to the other half just to make a
                        // tiny frame adjustment.
                        for (const auto& dcs : m_dragSelectedClips) {
                            Track* adjTrack = m_timeline->track(dcs.originalTrack);
                            if (!adjTrack) continue;
                            size_t idx = adjTrack->findClipIndexById(dcs.ref.clipId);
                            if (idx >= adjTrack->clipCount()) continue;
                            // Clip immediately before the dragged clip
                            if (idx > 0) {
                                const Clip* prevClip = adjTrack->clip(idx - 1);
                                if (prevClip && prevClip->timelineOut() == dcs.originalIn)
                                    excludeIds.push_back(prevClip->id());
                            }
                            // Clip immediately after the dragged clip
                            if (idx + 1 < adjTrack->clipCount()) {
                                const Clip* nextClip = adjTrack->clip(idx + 1);
                                if (nextClip && nextClip->timelineIn() == dcs.originalIn + dcs.originalDuration)
                                    excludeIds.push_back(nextClip->id());
                            }
                        }

                        m_snapEngine.buildTargets(*m_timeline, m_playheadTick,
                                                  0.0, excludeIds);

                        event->accept();
                        return;
                    }
                }
            }

            bool isShift = event->modifiers() & Qt::ShiftModifier;
            // Alt held at press = "disable smart link" — Premiere uses Alt
            // to grab just one side of a linked A/V pair for an isolated
            // drag/select. Alt also triggers the copy-on-release path, so
            // we just check it here without consuming.
            bool isAlt = event->modifiers() & Qt::AltModifier;

            // If clicking on an already-selected clip WITHOUT shift,
            // defer the decision: if the user drags, move all selected clips;
            // if the user releases without moving, select just this one clip
            // (Premiere Pro behaviour).
            bool alreadySelected = m_selection.isSelected(*hitRef);
            if (isShift) {
                m_selection.toggleClip(*hitRef);
                // Carry the link partner along with the toggle: if the
                // clicked clip is now selected, add its partners; if it
                // was just deselected, drop them too. Alt skips this so
                // shift+alt+click peels off just one side of the pair.
                if (!isAlt)
                    setLinkPartnersSelected(*hitRef, m_selection.isSelected(*hitRef));
                emit selectionChanged();
                // Shift-click only toggles selection - don't initiate drag/trim
                m_dragMode = DragMode::None;
                event->accept();
                return;
            }

            if (alreadySelected) {
                // Defer: keep multi-selection for potential drag,
                // wait for release to decide click vs drag.
                m_dragClipRef = *hitRef;
                m_dragMode = DragMode::PendingClipClick;
                event->accept();
                return;
            }

            // Not already selected - immediately select just this clip
            m_selection.clear();
            m_selection.selectClip(*hitRef, false);
            // Premiere A/V link: clicking one side of a linked pair selects
            // both, so dragging moves them together. Alt at press disables
            // this so the user can grab just one side.
            if (!isAlt)
                setLinkPartnersSelected(*hitRef, true);
            emit selectionChanged();

            // Emit clipSelected so the Properties Panel updates
            {
                Track* trk = m_timeline->track(hitRef->trackIndex);
                size_t clipIdx = trk->findClipIndexById(hitRef->clipId);
                if (clipIdx < trk->clipCount())
                    emit clipSelected(hitRef->trackIndex, clipIdx);
            }

            // Body of the clip was clicked — defer the drag commit through
            // PendingClipClick so the cursor must move past ~5 px before the
            // clip starts following. Edge clicks were intercepted by the
            // bracket branch above. The promotion path in mouseMoveEvent
            // does the snap-engine + per-clip snapshot work on first real
            // move; doing it lazily avoids per-press cost on simple clicks
            // and lets us share one deadzone with the already-selected case.
            m_dragClipRef = *hitRef;
            m_dragMode    = DragMode::PendingClipClick;
        }
        else
        {
            // Clicked empty space — defer deselect until we know if the
            // user intends a click (deselect) or a drag (marquee select).
            // This is the Premiere Pro pattern: mouse-down starts a
            // potential marquee; deselect only fires on release without
            // meaningful movement.

            // Detect gap between clips on the clicked track
            size_t ti = hitTestTrack(pos.y());
            double px = pos.x() - headerWidth();
            int64_t clickTick = m_layoutEngine.pixelXToTime(px);
            bool gapFound = false;

            if (ti < m_timeline->trackCount() && clickTick >= 0) {
                Track* track = m_timeline->track(ti);

                // Collect and sort clips by position
                std::vector<const Clip*> sorted;
                sorted.reserve(track->clipCount());
                for (size_t ci = 0; ci < track->clipCount(); ++ci)
                    sorted.push_back(track->clip(ci));
                std::sort(sorted.begin(), sorted.end(),
                    [](const Clip* a, const Clip* b) {
                        return a->timelineIn() < b->timelineIn();
                    });

                // Check gap before first clip (from tick 0 to first clip start)
                if (!sorted.empty() && clickTick < sorted.front()->timelineIn()
                    && sorted.front()->timelineIn() > 0) {
                    m_gapSelection = { ti, 0, sorted.front()->timelineIn(), true };
                    gapFound = true;
                }

                // Check gaps between consecutive clips
                if (!gapFound) {
                    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
                        int64_t gapStart = sorted[i]->timelineOut();
                        int64_t gapEnd   = sorted[i + 1]->timelineIn();
                        if (gapEnd > gapStart &&
                            clickTick >= gapStart && clickTick < gapEnd) {
                            m_gapSelection = { ti, gapStart, gapEnd, true };
                            gapFound = true;
                            break;
                        }
                    }
                }
            }

            if (gapFound) {
                // Show gap highlight on the appropriate track widget
                if (!(event->modifiers() & Qt::ShiftModifier)) {
                    m_selection.clear();
                    m_selectedTransitionTrack = SIZE_MAX;
                    m_selectedTransitionIndex = SIZE_MAX;
                    for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                        m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
                }
                // Clear stale edge-hover state so it doesn't interfere
                m_lastClickedEdge.valid = false;
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
                if (m_gapSelection.trackIndex < m_trackWidgets.size())
                    m_trackWidgets[m_gapSelection.trackIndex]->setGapHighlight(
                        m_gapSelection.startTick, m_gapSelection.endTick);
                // Stay in PendingMarquee so a click-and-drag from inside
                // the gap can still produce a marquee selection box.
                // If the user releases without moving, the gap selection
                // is preserved (mouseRelease's PendingMarquee branch only
                // clears m_selection, which is already empty here).
                // If they drag, the PendingMarquee→MarqueeSelect transition
                // will clear the gap highlight via m_gapSelection.active.
                m_dragMode = DragMode::PendingMarquee;
                emit selectionChanged();
            } else {
                // Clear any existing gap selection
                m_gapSelection.active = false;
                for (size_t w = 0; w < m_trackWidgets.size(); ++w)
                    m_trackWidgets[w]->setGapHighlight(-1, -1);
                m_lastClickedEdge.valid = false;
                // Don't clear selection yet — wait to see if user drags
                m_dragMode = DragMode::PendingMarquee;
            }
        }
        event->accept();
        return;
    }
    case EditTool::Slip:
    {
        auto hitRef = hitTestClip(pos);
        if (hitRef)
        {
            m_dragClipRef = *hitRef;
            m_dragMode = DragMode::SlipTool;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount())
                m_dragOriginalSourceIn = track->clip(idx)->sourceIn();

            // Wrap all slip commands into a single undo step.
            if (m_commandStack)
                m_commandStack->beginMacro("Slip clip");
        } else {
            // Empty-space click — deselect on release (Premiere style).
            m_dragMode = DragMode::PendingMarquee;
        }
        event->accept();
        return;
    }
    case EditTool::Slide:
    {
        auto hitRef = hitTestClip(pos);
        if (hitRef)
        {
            m_dragClipRef = *hitRef;
            m_dragMode = DragMode::SlideTool;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount())
                m_dragOriginalIn = track->clip(idx)->timelineIn();

            // Wrap all slide commands into a single undo step.
            if (m_commandStack)
                m_commandStack->beginMacro("Slide clip");
        } else {
            m_dragMode = DragMode::PendingMarquee;
        }
        event->accept();
        return;
    }
    case EditTool::Rolling:
    {
        // Find the nearest edit point between two adjacent clips
        size_t ti = hitTestTrack(pos.y());
        if (ti < m_timeline->trackCount()) {
            double px = pos.x() - headerWidth();
            int64_t clickTick = m_layoutEngine.pixelXToTime(px);
            Track* track = m_timeline->track(ti);

            // Look for the closest edit point (where one clip ends and another begins)
            int64_t bestDist = INT64_MAX;
            uint64_t bestLeft = 0, bestRight = 0;
            int64_t bestEditPt = 0;

            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                int64_t outTick = clip->timelineOut();

                // Find any clip that starts at or very near this clip's out
                for (size_t ci2 = 0; ci2 < track->clipCount(); ++ci2) {
                    if (ci2 == ci) continue;
                    const Clip* next = track->clip(ci2);
                    int64_t gap = std::abs(next->timelineIn() - outTick);
                    if (gap <= 1600) { // ~1 frame tolerance
                        int64_t editPt = outTick;
                        int64_t dist = std::abs(clickTick - editPt);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestLeft = clip->id();
                            bestRight = next->id();
                            bestEditPt = editPt;
                        }
                    }
                }
            }

            // Always engage the nearest edit point on the clicked track —
            // the Rolling tool would otherwise silently no-op when the user
            // clicked even slightly off the seam (Premiere doesn't gate
            // this on a pixel radius; whatever edit point is closest wins).
            // Without this the tool felt like it had "reverted to selection
            // mode" — actually nothing happened, but the user couldn't tell.
            if (bestLeft != 0) {
                m_dragMode = DragMode::RollingEdit;
                m_rollLeftClipId = bestLeft;
                m_rollRightClipId = bestRight;
                m_rollTrackIndex = ti;
                m_rollOriginalEditPoint = bestEditPt;
                // Capture original clip states for direct manipulation
                size_t li2 = track->findClipIndexById(bestLeft);
                size_t ri2 = track->findClipIndexById(bestRight);
                if (li2 < track->clipCount() && ri2 < track->clipCount()) {
                    const Clip* lc = track->clip(li2);
                    const Clip* rc = track->clip(ri2);
                    m_rollLeftOrigIn     = lc->timelineIn();
                    m_rollLeftOrigDur    = lc->duration();
                    m_rollLeftOrigSrcIn  = lc->sourceIn();
                    m_rollRightOrigIn    = rc->timelineIn();
                    m_rollRightOrigDur   = rc->duration();
                    m_rollRightOrigSrcIn = rc->sourceIn();

                    // Precompute live-drag bounds that match the commit
                    // clamp — so the seam stops at exactly the tick it
                    // will land on, not somewhere past and snap back.
                    int64_t minEP = m_rollLeftOrigIn;
                    int64_t maxEP = m_rollRightOrigIn + m_rollRightOrigDur;

                    // Right clip's head can't roll past its source's
                    // start (sourceIn must stay >= 0). Skip for video
                    // characters which have no finite source media.
                    bool rightHasSrcLimit = false;
                    if (auto* vc = dynamic_cast<const VideoClip*>(rc))
                        rightHasSrcLimit = !vc->isVideoCharacter();
                    else if (dynamic_cast<const AudioClip*>(rc))
                        rightHasSrcLimit = true;
                    if (rightHasSrcLimit && m_rollRightOrigSrcIn > 0)
                        minEP = std::max(minEP, m_rollRightOrigIn - m_rollRightOrigSrcIn);

                    // Left clip's tail can't extend past its source media.
                    int64_t leftSrcDur = 0;
                    if (auto* vc = dynamic_cast<const VideoClip*>(lc)) {
                        if (!vc->isVideoCharacter()) leftSrcDur = vc->sourceDuration();
                    } else if (auto* ac = dynamic_cast<const AudioClip*>(lc)) {
                        leftSrcDur = ac->sourceDuration();
                    }
                    if (leftSrcDur > 0)
                        maxEP = std::min(maxEP, m_rollLeftOrigIn + leftSrcDur - m_rollLeftOrigSrcIn);

                    // Allow the seam to travel all the way to the far edges
                    // of the adjacent clips — rollingEdit() handles full
                    // consumption of one clip by removing it and extending
                    // the other.  No kMinClipDuration floor here so the
                    // user can completely eliminate a clip they roll into.

                    // Degenerate (no valid roll range) shouldn't crash —
                    // collapse to the original seam.
                    if (minEP > maxEP) {
                        minEP = m_rollOriginalEditPoint;
                        maxEP = m_rollOriginalEditPoint;
                    }
                    m_rollMinEditPoint = minEP;
                    m_rollMaxEditPoint = maxEP;
                }

                // Show the edit-point brackets immediately at the seam so
                // the user has a visible "you grabbed this edit point"
                // affordance from the first frame of the drag.
                setEditPointSelection(m_rollTrackIndex, m_rollOriginalEditPoint,
                                       EditPointSide::Both);

                // Initialize snap engine for rolling edit
                m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
                m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0,
                                          {m_rollLeftClipId, m_rollRightClipId});
            }
        }
        if (m_dragMode != DragMode::RollingEdit) {
            // No edit point on the clicked track (or click was outside any
            // track).  Fall through to PendingMarquee so the empty-space
            // release deselects any currently-selected clip.  This is what
            // resolves the "clips stay selected after rolling / clicking
            // empty space doesn't deselect" complaint.
            m_dragMode = DragMode::PendingMarquee;
        }
        event->accept();
        return;
    }
    case EditTool::Ripple:
    {
        // Ripple tool: trim head/tail with ripple (shift subsequent clips)
        auto hitRef = hitTestClip(pos);
        if (hitRef) {
            m_dragClipRef = *hitRef;
            Track* track = m_timeline->track(hitRef->trackIndex);
            size_t idx = track->findClipIndexById(hitRef->clipId);
            if (idx < track->clipCount()) {
                const Clip* clip = track->clip(idx);
                m_dragOriginalIn = clip->timelineIn();
                m_dragOriginalDuration = clip->duration();

                double px = pos.x() - headerWidth();
                double clipLeft  = m_layoutEngine.timeToPixelX(clip->timelineIn());
                double clipRight = m_layoutEngine.timeToPixelX(clip->timelineOut());

                const double kEdgeThreshold = edgeGrabPx(clipRight - clipLeft);
                if (std::abs(px - clipRight) < kEdgeThreshold)
                    m_dragMode = DragMode::ClipTrimTail;
                else
                    m_dragMode = DragMode::ClipTrimHead;

                // Wrap all ripple-trim commands into a single undo step.
                if (m_commandStack && (m_dragMode == DragMode::ClipTrimTail
                                       || m_dragMode == DragMode::ClipTrimHead))
                    m_commandStack->beginMacro("Ripple trim clip");
            }
        } else {
            // Empty-space click — deselect on release.
            m_dragMode = DragMode::PendingMarquee;
        }
        event->accept();
        return;
    }
    case EditTool::Zoom:
    {
        // Zoom in at click position; Alt+click zooms out (like Premiere Pro).
        // After zooming, pan so the clicked point becomes the viewport center.
        double px = pos.x() - headerWidth();
        bool zoomOut = (event->modifiers() & Qt::AltModifier);
        double factor = zoomOut ? (1.0 / 2.0) : 2.0;
        m_layoutEngine.zoomAt(px, factor);
        double centerPx = std::max(m_ruler->width(), 100) / 2.0;
        double newScroll = m_layoutEngine.scrollX() + (px - centerPx);
        m_layoutEngine.setScrollX(std::max(newScroll, 0.0));
        onScrollChanged();
        event->accept();
        return;
    }
    case EditTool::Text:
    {
        if (m_timeline && pos.x() >= headerWidth())
        {
            double px = pos.x() - headerWidth();
            int64_t tick = m_layoutEngine.pixelXToTime(px);
            if (tick < 0) tick = 0;

            // Builds a 5-second text GraphicClip at `tick`.
            auto makeTextClip = [tick]() {
                auto gc = std::make_unique<GraphicClip>();
                gc->setTimelineIn(tick);
                gc->setDuration(kTicksPerSecond * 5);
                gc->setSourceIn(0);
                gc->setLabel("Text");
                gc->addTextLayer("Text Layer 1");
                return gc;
            };

            size_t ti = hitTestTrack(pos.y());
            if (ti < m_timeline->trackCount())
            {
                // Clicked on an existing track row — add to it if free.
                Track* track = m_timeline->track(ti);
                bool occupied = false;
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    const Clip* c = track->clip(ci);
                    if (tick >= c->timelineIn() && tick < c->timelineOut()) {
                        occupied = true;
                        break;
                    }
                }

                if (!occupied) {
                    if (m_commandStack) {
                        m_commandStack->execute(
                            std::make_unique<AddClipCommand>(track, makeTextClip()));
                    } else {
                        track->addClip(makeTextClip());
                    }
                    refreshTrackContents();
                    emit clipCreated();
                }
            }
            else
            {
                // Clicked empty space (e.g. above the top track) — create a
                // new video track and drop the text clip on it, as a single
                // undo step (Premiere Pro behavior).
                if (m_commandStack) {
                    auto compound =
                        std::make_unique<CompoundCommand>("Add Text Layer");

                    auto trackCmd = std::make_unique<AddTrackCommand>(
                        m_timeline, TrackType::Video);
                    trackCmd->execute();
                    Track* newTrack = trackCmd->track();
                    compound->addExecuted(std::move(trackCmd));

                    if (newTrack) {
                        auto clipCmd = std::make_unique<AddClipCommand>(
                            newTrack, makeTextClip());
                        clipCmd->execute();
                        compound->addExecuted(std::move(clipCmd));
                    }
                    m_commandStack->pushWithoutExecute(std::move(compound));
                } else {
                    Track* newTrack = m_timeline->addVideoTrack();
                    if (newTrack) newTrack->addClip(makeTextClip());
                }
                refreshTrackContents();
                emit clipCreated();
            }
        }
        event->accept();
        return;
    }
    default:
        break;
    }

    QWidget::mousePressEvent(event);
}


} // namespace rt