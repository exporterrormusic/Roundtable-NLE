/*
 * TimelinePanelMouseDragRelease.cpp — mouseReleaseEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"

#include <spdlog/spdlog.h>

#include <unordered_map>
#include <unordered_set>

namespace rt {

void TimelinePanel::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragMode == DragMode::ClipMove && m_timeline && !m_dragSelectedClips.empty()) {
        // ── Ghost track: create a new track if the user released in the ghost zone ──
        size_t newTrackIndex = SIZE_MAX;
        float sourceTrackHeight = 80.0f;
        if (m_ghostTrackVisible) {
            // Copy height from the source track of the primary dragged clip
            {
                size_t srcTrackIdx = m_dragOriginalTrack;
                if (srcTrackIdx < m_timeline->trackCount())
                    sourceTrackHeight = m_timeline->track(srcTrackIdx)->height();
            }
            if (m_ghostTrackIsAbove) {
                // Insert ABOVE the top-most REAL video track (but below a pinned
                // caption track), not blindly at index 0 — matches where the
                // drag-move ghost preview anchored the band.
                size_t insertIdx = 0;
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    Track* t = m_timeline->track(i);
                    if (!t || t->isDivider() || t->isCaptionTrack()) continue;
                    if (t->type() == TrackType::Video) { insertIdx = i; break; }
                }
                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                newTrack->setHeight(sourceTrackHeight);
                m_timeline->insertTrack(insertIdx, std::move(newTrack));
                newTrackIndex = insertIdx;
                for (auto& dcs : m_dragSelectedClips) {
                    if (dcs.originalTrack  >= insertIdx) dcs.originalTrack  += 1;
                    if (dcs.ref.trackIndex >= insertIdx) dcs.ref.trackIndex += 1;
                }
                if (m_dragOriginalTrack >= insertIdx) m_dragOriginalTrack += 1;
            } else {
                Track* at = m_timeline->addAudioTrack("");
                at->setHeight(sourceTrackHeight);
                (void)at;
                newTrackIndex = m_timeline->trackCount() - 1;
            }
            m_ghostTrackVisible = false;
            if (m_ghostOverlay) m_ghostOverlay->hide();
        }

        // ── Commit the drag as a SINGLE undoable operation ──────────────
        int trackDelta = 0;
        if (newTrackIndex != SIZE_MAX) {
            trackDelta = static_cast<int>(newTrackIndex)
                       - static_cast<int>(m_dragOriginalTrack);
        } else if (m_dragTargetTrack != SIZE_MAX && !m_dragSelectedClips.empty()) {
            trackDelta = static_cast<int>(m_dragTargetTrack)
                       - static_cast<int>(m_dragOriginalTrack);
        }

        if (newTrackIndex == SIZE_MAX) {
            constexpr double kTrackShiftDeadzone = 18.0;
            if (std::abs(event->position().y() - m_dragStart.y()) < kTrackShiftDeadzone)
                trackDelta = 0;
        }
        if (trackDelta != 0) {
            size_t minOrig = SIZE_MAX, maxOrig = 0;
            for (const auto& dcs : m_dragSelectedClips) {
                minOrig = std::min(minOrig, dcs.originalTrack);
                maxOrig = std::max(maxOrig, dcs.originalTrack);
            }
            if (minOrig != SIZE_MAX) {
                const int lastTrack = static_cast<int>(m_timeline->trackCount()) - 1;
                const int minAllowed = -static_cast<int>(minOrig);
                const int maxAllowed = lastTrack - static_cast<int>(maxOrig);
                trackDelta = std::clamp(trackDelta, minAllowed, maxAllowed);
            }
        }

        // Capture final positions
        struct FinalPos {
            size_t srcTrack; uint64_t clipId;
            int64_t finalIn; int64_t originalIn;
            size_t dstTrack;
        };
        std::vector<FinalPos> finals;
        for (const auto& dcs : m_dragSelectedClips) {
            int64_t curPos = dcs.originalIn;
            size_t currentTrackIdx = dcs.ref.trackIndex;
            {
                Track* tr = m_timeline->track(currentTrackIdx);
                if (tr) {
                    size_t idx = tr->findClipIndexById(dcs.ref.clipId);
                    if (idx < tr->clipCount())
                        curPos = tr->clip(idx)->timelineIn();
                }
            }

            int dst = static_cast<int>(dcs.originalTrack) + trackDelta;
            if (dst < 0) dst = 0;
            if (dst >= static_cast<int>(m_timeline->trackCount())) dst = static_cast<int>(m_timeline->trackCount()) - 1;
            size_t dstTrack = static_cast<size_t>(dst);

            Track* origTr = m_timeline->track(dcs.originalTrack);
            if (!origTr) continue;
            Track* dstTr = m_timeline->track(dstTrack);
            // Dividers are TrackType::Video but reject clips — a plain type
            // check would land the move on the divider row and Track::addClip
            // would silently drop the clip. Fall back to the source track.
            if (!dstTr || dstTr->isDivider() || dstTr->type() != origTr->type())
                dstTrack = dcs.originalTrack;

            if (curPos != dcs.originalIn || dstTrack != dcs.originalTrack)
                finals.push_back({dcs.originalTrack, dcs.ref.clipId, curPos, dcs.originalIn, dstTrack});
        }

        spdlog::info("[OVERLAP-DIAG] mouseRelease: {} finals from {} dragSelectedClips, trackDelta={}",
                     finals.size(), m_dragSelectedClips.size(), trackDelta);

        // Restore clips to their original tracks and positions.  The
        // live-drag preview may have moved each clip between tracks via
        // Track::removeClip()+addClip(), which silently drops every
        // transition referencing that clip on its source track.  Once
        // each clip is back on its original track, re-add the
        // transitions captured at drag-start so the about-to-run
        // moveClipToTrack command sees them and can carry them over.
        for (const auto& fp : finals) {
            Track* curTr = nullptr;
            size_t curIdx = SIZE_MAX;

            for (size_t st = 0; st < m_timeline->trackCount(); ++st) {
                Track* t = m_timeline->track(st);
                size_t idx = t->findClipIndexById(fp.clipId);
                if (idx < t->clipCount()) {
                    curTr = t;
                    curIdx = idx;
                    break;
                }
            }
            if (!curTr || curIdx >= curTr->clipCount()) continue;

            Track* srcTr = m_timeline->track(fp.srcTrack);
            if (!srcTr) continue;

            if (curTr != srcTr) {
                auto clipPtr = curTr->removeClip(curIdx);
                clipPtr->setTimelineIn(fp.originalIn);
                srcTr->addClip(std::move(clipPtr));
            } else {
                curTr->moveClip(curIdx, fp.originalIn);
            }
        }
        // Replay transitions onto the source track for each dragged clip
        // so the upcoming move command can capture them (the live drag
        // preview removed them when it moved the clip between tracks).
        for (const auto& dcs : m_dragSelectedClips) {
            if (dcs.originalTransitions.empty()) continue;
            Track* origTr = m_timeline->track(dcs.originalTrack);
            if (!origTr) continue;
            for (const auto& orig : dcs.originalTransitions) {
                // Skip if a transition with the same edit point + clip
                // ids is already present (same-track moves leave them).
                bool dup = false;
                for (size_t i = 0; i < origTr->transitionCount(); ++i) {
                    const Transition* ex = origTr->transition(i);
                    if (ex && ex->editPointTick == orig.editPointTick
                           && ex->leftClipId   == orig.leftClipId
                           && ex->rightClipId  == orig.rightClipId) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) origTr->addTransition(orig);
            }
        }

        bool hasGhostTrack = (newTrackIndex != SIZE_MAX);
        bool didMove = false;

        // Alt+drag = COPY clips
        bool altCopy = (event->modifiers() & Qt::AltModifier) && !finals.empty();

        if (altCopy) {
            auto masterCompound = std::make_unique<CompoundCommand>("Copy clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

            for (const auto& fp : finals) {
                Track* srcTr = m_timeline->track(fp.srcTrack);
                if (!srcTr) continue;
                size_t idx = srcTr->findClipIndexById(fp.clipId);
                if (idx >= srcTr->clipCount()) continue;

                auto cloned = srcTr->clip(idx)->clone();
                cloned->setTimelineIn(fp.finalIn);

                Track* dstTr = m_timeline->track(fp.dstTrack);
                if (!dstTr) continue;

                auto cmd = std::make_unique<AddClipCommand>(dstTr, std::move(cloned));
                cmd->execute();
                masterCompound->addCommand(std::move(cmd));
            }

            // Collect all newly-added clip IDs so resolveOverlaps won't
            // cascade-delete them (they're from the same multi-clip drag
            // and are meant to coexist on the destination track).
            {
                std::unordered_set<uint64_t> movedIds;
                for (const auto& fp : finals) {
                    Track* dstTr = m_timeline->track(fp.dstTrack);
                    if (!dstTr) continue;
                    for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                        if (dstTr->clip(ci)->timelineIn() == fp.finalIn) {
                            movedIds.insert(dstTr->clip(ci)->id());
                            break;
                        }
                    }
                }

                for (const auto& fp : finals) {
                    Track* dstTr = m_timeline->track(fp.dstTrack);
                    if (!dstTr) continue;
                    for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                        if (dstTr->clip(ci)->timelineIn() == fp.finalIn &&
                            dstTr->clip(ci)->id() != fp.clipId) {
                            auto overwrite = EditOperations::resolveOverlaps(
                                *m_timeline, fp.dstTrack, dstTr->clip(ci)->id(),
                                movedIds);
                            if (overwrite) {
                                overwrite->execute();
                                masterCompound->addCommand(std::move(overwrite));
                            }
                            break;
                        }
                    }
                }
            }

            if (masterCompound->size() > 0) {
                m_commandStack->pushWithoutExecute(std::move(masterCompound));
                onScrollChanged();
                emit contentChanged();
                didMove = true;
            }
        }
        else if (!finals.empty() || hasGhostTrack) {
            auto masterCompound = std::make_unique<CompoundCommand>("Move clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_timeline, newTrackIndex));
            }

            // ── Joint-move transition preservation ───────────────────────
            // If both endpoints of a two-sided dissolve are in the move
            // set AND headed to the same destination track, the
            // per-clip moveClipToTrack path would discard it (one side
            // moves, dissolve drops; second side has no partner to
            // re-link).  Pull these "shared" transitions out of the
            // source track BEFORE the per-clip moves so they survive,
            // then re-add them on the destination after.  We also need
            // the delta so the new editPointTick lines up with the
            // moved clips on the destination.
            struct SharedTrans {
                Transition trans;            // original transition data
                size_t     srcTrack;
                size_t     dstTrack;
                int64_t    timeDelta;        // shift applied to editPointTick
            };
            std::vector<SharedTrans> sharedTransitions;
            {
                // (clipId → finalPos & dstTrack) for every moving clip.
                std::unordered_map<uint64_t, const FinalPos*> movingById;
                for (const auto& fp : finals)
                    movingById.emplace(fp.clipId, &fp);

                // Walk every source track involved in the move, looking
                // for transitions that fully bridge two moving clips
                // that share a destination track.
                std::unordered_set<size_t> srcTracksTouched;
                for (const auto& fp : finals)
                    srcTracksTouched.insert(fp.srcTrack);
                for (size_t srcIdx : srcTracksTouched) {
                    Track* srcTr = m_timeline->track(srcIdx);
                    if (!srcTr) continue;
                    for (size_t ti = srcTr->transitionCount(); ti-- > 0; ) {
                        const Transition* t = srcTr->transition(ti);
                        if (!t) continue;
                        // Both endpoints must reference a moving clip
                        // (excludes single-sided fades, which the
                        // existing per-clip path already carries over).
                        if (t->leftClipId == 0 || t->rightClipId == 0) continue;
                        auto itL = movingById.find(t->leftClipId);
                        auto itR = movingById.find(t->rightClipId);
                        if (itL == movingById.end() || itR == movingById.end())
                            continue;
                        if (itL->second->dstTrack != itR->second->dstTrack)
                            continue;
                        // The two moving clips travel together — capture
                        // the dissolve so we can replant it post-move.
                        SharedTrans s;
                        s.trans     = *t;
                        s.srcTrack  = srcIdx;
                        s.dstTrack  = itL->second->dstTrack;
                        s.timeDelta = itL->second->finalIn - itL->second->originalIn;
                        sharedTransitions.push_back(s);
                        // Yank it out of the source track now so the per-clip
                        // moveClipToTrack doesn't see it as a dangling two-sided
                        // transition. Do this through an UNDOABLE command added
                        // to the compound: a raw srcTr->removeTransition(ti) is
                        // never reversed on Ctrl+Z, so undoing the move (which
                        // removes the replanted copy from dst and moves the clips
                        // back to src) used to leave the dissolve gone for good —
                        // the reported "cross dissolve vanishes on undo" bug.
                        // Identify the transition by its clip-id edit point (not
                        // a vector index) so a redo after undo lifts the right
                        // one even though re-adding appends it at a new index.
                        {
                            const Transition lifted = s.trans;
                            Track* srcTrPtr = srcTr;
                            auto liftCmd = std::make_unique<LambdaCommand>(
                                "Lift shared transition",
                                [srcTrPtr, lifted]() {
                                    size_t i = srcTrPtr->findTransition(
                                        lifted.leftClipId, lifted.rightClipId);
                                    if (i != Track::kNoTransition)
                                        srcTrPtr->removeTransition(i);
                                },
                                [srcTrPtr, lifted]() {
                                    srcTrPtr->addTransition(lifted);
                                });
                            liftCmd->execute();
                            masterCompound->addCommand(std::move(liftCmd));
                        }
                    }
                }
            }

            for (const auto& fp : finals) {
                std::unique_ptr<Command> cmd;
                if (fp.srcTrack != fp.dstTrack) {
                    cmd = EditOperations::moveClipToTrack(
                        *m_timeline, fp.srcTrack, fp.dstTrack, fp.clipId, fp.finalIn);
                } else {
                    cmd = EditOperations::moveClip(
                        *m_timeline, fp.srcTrack, fp.clipId, fp.finalIn);
                }
                if (cmd) {
                    cmd->execute();
                    masterCompound->addCommand(std::move(cmd));
                }
            }

            // Replant the joint-move transitions on the destination
            // track, shifted by the move delta.  Clip IDs are preserved
            // by moveClipToTrack so leftClipId / rightClipId still
            // resolve to the correct (moved) clips on dst.
            for (auto& s : sharedTransitions) {
                Transition replanted = s.trans;
                replanted.editPointTick += s.timeDelta;
                Track* dstTr = m_timeline->track(s.dstTrack);
                if (!dstTr) continue;
                auto addCmd = std::make_unique<AddTransitionCommand>(
                    dstTr, 0, 0, replanted);
                addCmd->execute();
                masterCompound->addCommand(std::move(addCmd));
            }

            // Collect all moved clip IDs so resolveOverlaps won't
            // cascade-delete fellow clips from this same multi-clip drag.
            std::unordered_set<uint64_t> movedIds;
            for (const auto& fp : finals)
                movedIds.insert(fp.clipId);

            for (const auto& fp : finals) {
                uint64_t resolveId = fp.clipId;
                if (fp.srcTrack != fp.dstTrack) {
                    Track* dstTr = m_timeline->track(fp.dstTrack);
                    if (dstTr) {
                        for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                            if (dstTr->clip(ci)->timelineIn() == fp.finalIn) {
                                resolveId = dstTr->clip(ci)->id();
                                break;
                            }
                        }
                    }
                }
                auto overwrite = EditOperations::resolveOverlaps(
                    *m_timeline, fp.dstTrack, resolveId, movedIds);
                if (overwrite) {
                    overwrite->execute();
                    masterCompound->addCommand(std::move(overwrite));
                }
            }

            if (masterCompound->size() > 0) {
                m_commandStack->pushWithoutExecute(std::move(masterCompound));
                onScrollChanged();
                emit contentChanged();
                didMove = true;
            }
        }

        m_dragSelectedClips.clear();
        m_dragTargetTrack = SIZE_MAX;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();

        // Clear per-widget drag state on every track. Drag-move set
        // m_draggedSet + m_ghostDragActive on the source track's widget so
        // its paintEvent could hide the live preview; if we don't clear it
        // here a subsequent reuse (e.g. after a ghost-track drop the source
        // widget gets repointed to the new top track) keeps skipping the
        // clip in both paint passes and the clip stays invisible.
        for (auto& tw : m_trackWidgets) {
            if (!tw) continue;
            tw->setDraggedClips({});
            tw->setGhostDragActive(false);
        }

        if (didMove) {
            // Always do a full rebuild after a ghost-track drop.
            // insertTrackWidgetIncremental() reassigned every existing
            // widget to the POST-insert track index before inserting the
            // new widget, so all existing widgets ended up off-by-one —
            // one of them showed the new track's clip too, which looked
            // like a duplicate clip on a phantom track at the very top.
            // A full rebuild keeps widgets perfectly in sync with the
            // model (a brief flash is far better than a phantom track).
            rebuildTracks();
            emit selectionChanged();
        }
    }

    // ── Rolling edit: commit as a single undoable command ────────────────
    if (m_dragMode == DragMode::RollingEdit && m_timeline) {
        Track* rollTrack = m_timeline->track(m_rollTrackIndex);
        if (rollTrack) {
            size_t li = rollTrack->findClipIndexById(m_rollLeftClipId);
            size_t ri = rollTrack->findClipIndexById(m_rollRightClipId);
            if (li < rollTrack->clipCount() && ri < rollTrack->clipCount()) {
                int64_t finalEditPoint = rollTrack->clip(li)->timelineOut();

                Clip* lc = rollTrack->clip(li);
                Clip* rc = rollTrack->clip(ri);
                lc->setTimelineIn(m_rollLeftOrigIn);
                lc->setDuration(m_rollLeftOrigDur);
                lc->setSourceIn(m_rollLeftOrigSrcIn);
                rc->setTimelineIn(m_rollRightOrigIn);
                rc->setDuration(m_rollRightOrigDur);
                rc->setSourceIn(m_rollRightOrigSrcIn);

                if (finalEditPoint != m_rollOriginalEditPoint) {
                    auto cmd = EditOperations::rollingEdit(
                        *m_timeline, m_rollTrackIndex,
                        m_rollLeftClipId, m_rollRightClipId, finalEditPoint);
                    if (cmd) executeCommand(std::move(cmd));
                }
            }
        }
    }

    // ── PendingClipClick: user clicked an already-selected clip without dragging ──
    if (m_dragMode == DragMode::PendingClipClick) {
        // If this was an edge click (m_lastClickedEdge.valid is true),
        // the edit-point bracket is already showing and the user wants
        // to keep it for Ctrl+T — don't select the whole clip.
        if (!m_lastClickedEdge.valid) {
            // Body click: select just the clicked clip (Premiere Pro
            // behaviour) — but carry its link partner along unless Alt
            // was held, so clicking a linked video doesn't silently drop
            // its companion audio.
            const bool isAlt = event->modifiers() & Qt::AltModifier;
            m_selection.clear();
            m_selection.selectClip(m_dragClipRef, false);
            if (!isAlt)
                setLinkPartnersSelected(m_dragClipRef, true);
            emit selectionChanged();
            // Emit clipSelected so panels update
            Track* trk = m_timeline ? m_timeline->track(m_dragClipRef.trackIndex) : nullptr;
            if (trk) {
                size_t clipIdx = trk->findClipIndexById(m_dragClipRef.clipId);
                if (clipIdx < trk->clipCount())
                    emit clipSelected(m_dragClipRef.trackIndex, clipIdx);
            }
        }
    }

    // ── PendingMarquee without drag: clear selection on empty-space click ──
    if (m_dragMode == DragMode::PendingMarquee) {
        m_selection.clear();
        m_selectedTransitionTrack = SIZE_MAX;
        m_selectedTransitionIndex = SIZE_MAX;
        for (size_t w = 0; w < m_trackWidgets.size(); ++w)
            m_trackWidgets[w]->setSelectedTransition(SIZE_MAX);
        emit selectionChanged();
    }

    // ── Reset all drag-related state ─────────────────────────────────────
    if (m_dragMode != DragMode::None) {
        // Close any open undo macro for trim/slip/slide drags so the
        // buffered mouse-move commands collapse into a single undo entry.
        if (m_commandStack && m_commandStack->isMacroActive()) {
            switch (m_dragMode) {
            case DragMode::ClipTrimHead:
            case DragMode::ClipTrimTail:
            case DragMode::SlipTool:
            case DragMode::SlideTool:
            case DragMode::TransitionTrim:
                m_commandStack->endMacro();
                break;
            default:
                break;
            }
        }

        m_dragMode = DragMode::None;
        m_dragClipRef = {};
        m_dragSelectedClips.clear();
        m_dragTargetTrack = SIZE_MAX;
        m_ghostTrackVisible = false;
        if (m_ghostOverlay) m_ghostOverlay->hide();
        m_snapEngine.resetHysteresis();

        if (m_marqueeScrollTimer) m_marqueeScrollTimer->stop();
        m_marqueeLastMovePos = QPointF();
        if (m_clipDragScrollTimer) m_clipDragScrollTimer->stop();
        m_clipDragLastMovePos = QPointF();

        if (m_rubberBand) m_rubberBand->hide();
        setCursor(Qt::ArrowCursor);
        for (auto tw : m_trackWidgets)
            tw->setHoverEdgeTick(-1);

        // The snap-indicator (white dashed line) is a transient drag
        // affordance — it must disappear the moment the drag/roll/trim
        // ends, not linger until the next mouse press (a snapped
        // roller/trim otherwise looked like a stuck selection at the cut).
        setSnapIndicator(-1);

        // NOTE: the edit-point bracket is deliberately NOT cleared here.
        // A plain edge/seam click (no drag) primes a ClipTrim* drag and
        // paints the bracket; that bracket IS the persistent "this edit
        // point is selected — Ctrl+T will add a transition here" visual.
        // It is already cleared (a) at press-start on the next click and
        // (b) the moment a trim actually moves (in the drag-move handler),
        // so clearing it on release would only kill the indicator while
        // the user is reading it.

        // Gap selection is preserved on release so it remains visible.
        // It will be cleared on the next mouse press (when clicking on a
        // clip or non-gap empty space), matching Premiere Pro behavior.
    }

    QWidget::mouseReleaseEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
