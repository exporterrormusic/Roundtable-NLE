/*
 * TimelineClipMoveTool.cpp — see TimelineClipMoveTool.h.
 */

#include "panels/timeline/TimelineClipMoveTool.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "command/Command.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/ClipCommands.h"
#include "edit/EditSession.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <QApplication>
#include <QMouseEvent>
#include <QScrollArea>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace rt {

TimelineClipMoveTool::TimelineClipMoveTool(TimelinePanel& panel, int64_t originalIn,
                                           int64_t originalDuration, size_t originalTrack,
                                           std::vector<TimelineDragClip> clips)
    : m_panel(panel), m_originalIn(originalIn), m_originalDuration(originalDuration),
      m_originalTrack(originalTrack), m_targetTrack(originalTrack),
      m_clips(std::move(clips))
{
}

void TimelineClipMoveTool::move(const QMouseEvent& event, int64_t tickDelta)
{
    const QPointF pos = event.position();
    moveTo(event, pos, tickDelta);
}

void TimelineClipMoveTool::moveTo(const QMouseEvent& event, QPointF pos, int64_t tickDelta)
{
    int64_t newIn = m_originalIn + tickDelta;
    if (newIn < 0) newIn = 0;

    // Snap every head + tail of every dragged clip (the primary alone for
    // a single-clip drag; all of them for a multi-clip / linked A/V drag,
    // so clips on other tracks magnetise too). snapEdges is STICKY: once
    // an edge seats against a target it holds until the nearest edge is
    // dragged past the release zone, so the lock no longer drops the
    // moment a tail edge (or a linked clip's edge) is the one that snapped.
    // Holding Ctrl suppresses magnetism entirely for precision moves.
    const bool ctrlHeld = (event.modifiers() & Qt::ControlModifier);
    if (!ctrlHeld) {
        const int64_t baseDelta = newIn - m_originalIn;
        std::vector<int64_t> edges;
        if (m_clips.size() > 1) {
            edges.reserve(m_clips.size() * 2);
            for (const auto& dcs : m_clips) {
                const int64_t cIn = dcs.originalIn + baseDelta;
                edges.push_back(cIn);
                edges.push_back(cIn + dcs.originalDuration);
            }
        } else {
            edges = { newIn, newIn + m_originalDuration };
        }
        auto result = m_panel.m_snapEngine.snapEdges(edges);
        if (result.didSnap) {
            newIn += result.delta;
            if (newIn < 0) newIn = 0;
            m_panel.setSnapIndicator(result.snappedTick);
        } else {
            m_panel.setSnapIndicator(-1);
        }
    } else {
        // Ctrl held: suppress magnetism, reset sticky state so
        // releasing Ctrl doesn't cause a sudden jump to a stale target.
        m_panel.m_snapEngine.resetHysteresis();
        m_panel.setSnapIndicator(-1);
    }

    // Compute the actual delta applied to the primary clip
    int64_t actualDelta = newIn - m_originalIn;

    // EDIT INVARIANT (see CommandStack.h "EDIT DISCIPLINE"):
    // Group-floor clamp: when multiple clips are dragged left as a
    // unit, raise the GROUP delta so the leftmost selected clip lands at
    // tick 0 instead of every clip individually clamping to 0 and
    // collapsing into an overlap (rightmost ends up overwriting the
    // leftmost).
    {
        int64_t minSelOrig = std::numeric_limits<int64_t>::max();
        for (const auto& dcs : m_clips)
            minSelOrig = std::min(minSelOrig, dcs.originalIn);
        if (minSelOrig != std::numeric_limits<int64_t>::max()
                && actualDelta < -minSelOrig) {
            actualDelta = -minSelOrig;
            newIn = m_originalIn + actualDelta;
        }
    }

    // Determine the dragged clip's track type up front — it drives both
    // the cross-track target and the new-track ghost zones below.
    TrackType dragType = TrackType::Video;
    if (!m_clips.empty()) {
        Track* srcTr = m_panel.m_timeline->track(m_clips[0].ref.trackIndex);
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
    for (size_t i = 0; i < m_panel.m_timeline->trackCount(); ++i) {
        Track* t = m_panel.m_timeline->track(i);
        if (!t || t->isDivider() || t->isCaptionTrack()) continue;
        if (t->type() == TrackType::Video) {
            topVideoIdx = i;
            if (i < m_panel.m_trackWidgets.size()) {
                topVideoTop = m_panel.m_trackWidgets[i]->mapTo(&m_panel, QPoint(0, 0)).y();
                topVideoH   = m_panel.m_trackWidgets[i]->height();
            }
            break;
        }
    }
    const int  newTrackBand = std::min(14, std::max(6, topVideoH / 3));
    const bool videoAboveZone =
        (dragType == TrackType::Video && topVideoTop >= 0.0 &&
         pos.y() < topVideoTop + newTrackBand);

    // Detect cross-track target
    size_t targetTrack = m_panel.hitTestTrack(pos.y());
    if (!videoAboveZone && targetTrack < m_panel.m_timeline->trackCount()) {
        m_targetTrack = targetTrack;
        m_panel.m_ghostTrackVisible = false;
        if (m_panel.m_ghostOverlay) m_panel.m_ghostOverlay->hide();
    } else if (!m_panel.m_trackWidgets.empty()) {
        // ── Ghost track detection (Premiere Pro-style) ──────────────
        // Cursor is outside any track, OR inside the new-video-track-above
        // band at the top edge of the top video track.
        auto lastTw  = m_panel.m_trackWidgets.back();
        QPoint lastBot  = lastTw->mapTo(&m_panel, QPoint(0, lastTw->height()));

        // Last audio track index (for the audio-below zone). Skip divider
        // rows — they're TrackType::Video but not real audio tracks.
        size_t lastAudioIdx = SIZE_MAX;
        for (size_t i = 0; i < m_panel.m_timeline->trackCount(); ++i) {
            Track* t = m_panel.m_timeline->track(i);
            if (!t || t->isDivider()) continue;
            if (t->type() != TrackType::Video)
                lastAudioIdx = i;
        }

        // Scroll area X position
        QPoint scrollOrig = m_panel.m_verticalScroll->mapTo(&m_panel, QPoint(0, 0));
        int ghostX = scrollOrig.x();
        int ghostW = m_panel.m_verticalScroll->width();

        if (videoAboveZone && topVideoIdx < m_panel.m_trackWidgets.size()) {
            m_panel.m_ghostTrackVisible = true;
            m_panel.m_ghostTrackIsAbove = true;
            m_panel.m_ghostTrackHeight = m_panel.m_trackWidgets[topVideoIdx]->height();
            m_panel.m_ghostTrackY = static_cast<int>(topVideoTop) - m_panel.m_ghostTrackHeight;
            if (m_panel.m_ghostOverlay) {
                m_panel.m_ghostOverlay->isAbove = true;
                m_panel.m_ghostOverlay->onExistingTrack = false;  // new-track preview
                m_panel.m_ghostOverlay->setGeometry(ghostX, m_panel.m_ghostTrackY, ghostW, m_panel.m_ghostTrackHeight);
                m_panel.m_ghostOverlay->raise();
                m_panel.m_ghostOverlay->show();
                m_panel.m_ghostOverlay->update();
            }
        } else if (dragType == TrackType::Audio && pos.y() > lastBot.y() &&
                   lastAudioIdx < m_panel.m_trackWidgets.size()) {
            m_panel.m_ghostTrackVisible = true;
            m_panel.m_ghostTrackIsAbove = false;
            m_panel.m_ghostTrackHeight = lastTw->height();
            m_panel.m_ghostTrackY = lastBot.y();
            if (m_panel.m_ghostOverlay) {
                m_panel.m_ghostOverlay->isAbove = false;
                m_panel.m_ghostOverlay->onExistingTrack = false;  // new-track preview
                m_panel.m_ghostOverlay->setGeometry(ghostX, m_panel.m_ghostTrackY, ghostW, m_panel.m_ghostTrackHeight);
                m_panel.m_ghostOverlay->raise();
                m_panel.m_ghostOverlay->show();
                m_panel.m_ghostOverlay->update();
            }
        } else {
            m_panel.m_ghostTrackVisible = false;
            if (m_panel.m_ghostOverlay) m_panel.m_ghostOverlay->hide();
        }
    }

    // Compute per-clip cross-track delta from the primary clip.
    int trackDeltaLive = 0;
    if (m_targetTrack != SIZE_MAX && !m_clips.empty()) {
        trackDeltaLive = static_cast<int>(m_targetTrack)
                       - static_cast<int>(m_originalTrack);
    }

    // ── Y deadzone for cross-track shifts ───────────────────────────
    constexpr double kTrackShiftDeadzone = 18.0;
    if (std::abs(pos.y() - m_panel.m_dragStart.y()) < kTrackShiftDeadzone)
        trackDeltaLive = 0;

    // ── Shift held = constrain to current track (Premiere-style) ───
    // Lets the user nudge a clip horizontally without worrying about
    // accidentally bumping it to a neighbouring track. Checked live
    // so toggling Shift mid-drag also toggles the constraint.
    if (event.modifiers() & Qt::ShiftModifier) {
        trackDeltaLive = 0;
        m_panel.m_ghostTrackVisible = false;
        if (m_panel.m_ghostOverlay) m_panel.m_ghostOverlay->hide();
    }

    // ── Group-safety clamp ──────────────────────────────────────────
    if (trackDeltaLive != 0) {
        size_t minOrig = SIZE_MAX, maxOrig = 0;
        for (const auto& dcs : m_clips) {
            minOrig = std::min(minOrig, dcs.originalTrack);
            maxOrig = std::max(maxOrig, dcs.originalTrack);
        }
        if (minOrig != SIZE_MAX) {
            const int lastTrack = static_cast<int>(m_panel.m_timeline->trackCount()) - 1;
            const int minAllowed = -static_cast<int>(minOrig);
            const int maxAllowed = lastTrack - static_cast<int>(maxOrig);
            trackDeltaLive = std::clamp(trackDeltaLive, minAllowed, maxAllowed);
        }
    }

    // Move clips DIRECTLY in the data model (no commands) for live visual feedback.
    for (auto& dcs : m_clips) {
        // No per-clip max(0,...): actualDelta is already group-floored
        // above so the leftmost clip lands at tick 0 without others
        // collapsing onto it.
        int64_t clipNewIn = dcs.originalIn + actualDelta;
        if (clipNewIn < 0) clipNewIn = 0;

        int dst = static_cast<int>(dcs.originalTrack) + trackDeltaLive;

        Track* srcTr = m_panel.m_timeline->track(dcs.ref.trackIndex);
        if (!srcTr) continue;
        // Clamp onto the nearest track that can host this clip's kind. A
        // video clip dragged down into the audio section (or onto the V/A
        // divider) lands on the lowest video track instead of being
        // rejected — and this matches where the release commits it, so the
        // preview and the committed result agree. clampTrackToHostTrack
        // already excludes dividers + the caption track and keeps the
        // index in array bounds.
        size_t dstTrack = m_panel.clampTrackToHostTrack(dst, srcTr);
        if (dstTrack == SIZE_MAX) dstTrack = dcs.ref.trackIndex;
        Track* dstTr = m_panel.m_timeline->track(dstTrack);
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
            Track* curTr = m_panel.m_timeline->track(curTrack);
            size_t idx = curTr->findClipIndexById(dcs.ref.clipId);
            if (idx >= curTr->clipCount()) continue;
            auto clipPtr = curTr->removeClip(idx);
            if (!clipPtr) continue;
            clipPtr->setTimelineIn(clipNewIn);
            dstTr->addClip(std::move(clipPtr));
            dcs.ref.trackIndex = dstTrack;
        } else {
            Track* tr = m_panel.m_timeline->track(curTrack);
            size_t idx = tr->findClipIndexById(dcs.ref.clipId);
            if (idx >= tr->clipCount()) continue;
            tr->moveClip(idx, clipNewIn);
        }
    }
    m_panel.onScrollChanged();

    // Update ghost overlay clip previews when ghost track is visible.
    // Filter by the GHOST's direction so a linked A/V pair doesn't
    // stuff both clips into the same row: the audio-below ghost shows
    // only the audio companion, and video-above shows only the video.
    // The other side stays drawn on its existing track via normal clip
    // rendering (it didn't actually move, so the rendering is correct).
    if (m_panel.m_ghostTrackVisible && m_panel.m_ghostOverlay) {
        const TrackType ghostType = m_panel.m_ghostTrackIsAbove
                                    ? TrackType::Video
                                    : TrackType::Audio;
        std::vector<GhostTrackOverlay::GhostClipPreview> previews;
        for (const auto& dcs : m_clips) {
            Track* trk = m_panel.m_timeline->track(dcs.ref.trackIndex);
            if (!trk) continue;
            if (trk->type() != ghostType) continue;  // wrong side, skip
            if (trk->isDivider()) continue;
            size_t idx = trk->findClipIndexById(dcs.ref.clipId);
            if (idx >= trk->clipCount()) continue;
            const Clip* clip = trk->clip(idx);
            double px = m_panel.m_layoutEngine.timeToPixelX(clip->timelineIn());
            double pw = m_panel.m_layoutEngine.timeToPixelX(clip->timelineIn() + clip->duration()) - px;
            if (pw <= 0) continue;

            GhostTrackOverlay::GhostClipPreview gp;
            gp.x = static_cast<int>(px);
            gp.width = static_cast<int>(pw);
            gp.color = (ghostType == TrackType::Video) ? 0x4A90D9FF : 0x3CA05AFF;
            gp.label = QString::fromStdString(clip->label());
            previews.push_back(gp);
        }
        m_panel.m_ghostOverlay->setClipPreviews(previews);
    } else if (m_panel.m_ghostOverlay) {
        m_panel.m_ghostOverlay->setClipPreviews({});
    }

    // Sync selection + drag state
    {
        std::vector<std::vector<size_t>> perTrack(m_panel.m_trackWidgets.size());
        for (const auto& dcs : m_clips) {
            size_t ti = dcs.ref.trackIndex;
            if (ti < m_panel.m_trackWidgets.size()) {
                Track* trk = m_panel.m_timeline->track(ti);
                if (trk) {
                    size_t idx = trk->findClipIndexById(dcs.ref.clipId);
                    if (idx < trk->clipCount())
                        perTrack[ti].push_back(idx);
                }
            }
        }
        for (size_t ti = 0; ti < m_panel.m_trackWidgets.size(); ++ti) {
            m_panel.m_trackWidgets[ti]->setSelectedClips(perTrack[ti]);
            m_panel.m_trackWidgets[ti]->setDraggedClips(perTrack[ti]);
            m_panel.m_trackWidgets[ti]->setGhostDragActive(m_panel.m_ghostTrackVisible && !perTrack[ti].empty());
        }
    }
    m_panel.updateClipDragAutoScroll(pos);
}

void TimelineClipMoveTool::release(const QMouseEvent& event)
{
    if (!m_panel.m_timeline || m_clips.empty())
        return;

        // ── Ghost track: create a new track if the user released in the ghost zone ──
        size_t newTrackIndex = SIZE_MAX;
        float sourceTrackHeight = 80.0f;
        if (m_panel.m_ghostTrackVisible) {
            // Copy height from the source track of the primary dragged clip
            {
                size_t srcTrackIdx = m_originalTrack;
                if (srcTrackIdx < m_panel.m_timeline->trackCount())
                    sourceTrackHeight = m_panel.m_timeline->track(srcTrackIdx)->height();
            }
            if (m_panel.m_ghostTrackIsAbove) {
                // Insert ABOVE the top-most REAL video track (but below a pinned
                // caption track), not blindly at index 0 — matches where the
                // drag-move ghost preview anchored the band.
                size_t insertIdx = 0;
                for (size_t i = 0; i < m_panel.m_timeline->trackCount(); ++i) {
                    Track* t = m_panel.m_timeline->track(i);
                    if (!t || t->isDivider() || t->isCaptionTrack()) continue;
                    if (t->type() == TrackType::Video) { insertIdx = i; break; }
                }
                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                newTrack->setHeight(sourceTrackHeight);
                m_panel.m_timeline->insertTrack(insertIdx, std::move(newTrack));
                newTrackIndex = insertIdx;
                for (auto& dcs : m_clips) {
                    if (dcs.originalTrack  >= insertIdx) dcs.originalTrack  += 1;
                    if (dcs.ref.trackIndex >= insertIdx) dcs.ref.trackIndex += 1;
                }
                if (m_originalTrack >= insertIdx) m_originalTrack += 1;
            } else {
                Track* at = m_panel.m_timeline->addAudioTrack("");
                at->setHeight(sourceTrackHeight);
                (void)at;
                newTrackIndex = m_panel.m_timeline->trackCount() - 1;
            }
            m_panel.m_ghostTrackVisible = false;
            if (m_panel.m_ghostOverlay) m_panel.m_ghostOverlay->hide();
        }

        // ── Commit the drag as a SINGLE undoable operation ──────────────
        int trackDelta = 0;
        if (newTrackIndex != SIZE_MAX) {
            trackDelta = static_cast<int>(newTrackIndex)
                       - static_cast<int>(m_originalTrack);
        } else if (m_targetTrack != SIZE_MAX && !m_clips.empty()) {
            trackDelta = static_cast<int>(m_targetTrack)
                       - static_cast<int>(m_originalTrack);
        }

        if (newTrackIndex == SIZE_MAX) {
            constexpr double kTrackShiftDeadzone = 18.0;
            if (std::abs(event.position().y() - m_panel.m_dragStart.y()) < kTrackShiftDeadzone)
                trackDelta = 0;
        }
        if (trackDelta != 0) {
            size_t minOrig = SIZE_MAX, maxOrig = 0;
            for (const auto& dcs : m_clips) {
                minOrig = std::min(minOrig, dcs.originalTrack);
                maxOrig = std::max(maxOrig, dcs.originalTrack);
            }
            if (minOrig != SIZE_MAX) {
                const int lastTrack = static_cast<int>(m_panel.m_timeline->trackCount()) - 1;
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
        for (const auto& dcs : m_clips) {
            int64_t curPos = dcs.originalIn;
            size_t currentTrackIdx = dcs.ref.trackIndex;
            {
                Track* tr = m_panel.m_timeline->track(currentTrackIdx);
                if (tr) {
                    size_t idx = tr->findClipIndexById(dcs.ref.clipId);
                    if (idx < tr->clipCount())
                        curPos = tr->clip(idx)->timelineIn();
                }
            }

            int dst = static_cast<int>(dcs.originalTrack) + trackDelta;

            Track* origTr = m_panel.m_timeline->track(dcs.originalTrack);
            if (!origTr) continue;
            // Clamp onto the nearest track that can host this clip's kind rather
            // than snapping back to the origin when released over an
            // incompatible section: a video clip dropped down in the audio
            // tracks (or on the V/A divider) commits to the lowest video track,
            // matching the live preview. Excludes dividers + the caption track
            // and keeps the index in array bounds.
            size_t dstTrack = m_panel.clampTrackToHostTrack(dst, origTr);
            if (dstTrack == SIZE_MAX) dstTrack = dcs.originalTrack;
            Track* dstTr = m_panel.m_timeline->track(dstTrack);
            if (!dstTr) continue;

            if (curPos != dcs.originalIn || dstTrack != dcs.originalTrack)
                finals.push_back({dcs.originalTrack, dcs.ref.clipId, curPos, dcs.originalIn, dstTrack});
        }

        spdlog::info("[OVERLAP-DIAG] mouseRelease: {} finals from {} dragSelectedClips, trackDelta={}",
                     finals.size(), m_clips.size(), trackDelta);

        // Back to the drag-start state; the commands below re-apply the
        // move as one undoable step.
        restoreOrigins();

        bool hasGhostTrack = (newTrackIndex != SIZE_MAX);
        bool didMove = false;

        // Alt+drag = COPY clips
        bool altCopy = (event.modifiers() & Qt::AltModifier) && !finals.empty();

        if (altCopy) {
            auto masterCompound = std::make_unique<CompoundCommand>("Copy clips");
            if (m_panel.m_dragEdit.active())
                if (EditSession* edits = m_panel.editSession()) edits->renameOpen("Copy clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_panel.m_timeline, newTrackIndex));
            }

            for (const auto& fp : finals) {
                Track* srcTr = m_panel.m_timeline->track(fp.srcTrack);
                if (!srcTr) continue;
                size_t idx = srcTr->findClipIndexById(fp.clipId);
                if (idx >= srcTr->clipCount()) continue;

                auto cloned = srcTr->clip(idx)->clone();
                cloned->setTimelineIn(fp.finalIn);

                Track* dstTr = m_panel.m_timeline->track(fp.dstTrack);
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
                    Track* dstTr = m_panel.m_timeline->track(fp.dstTrack);
                    if (!dstTr) continue;
                    for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                        if (dstTr->clip(ci)->timelineIn() == fp.finalIn) {
                            movedIds.insert(dstTr->clip(ci)->id());
                            break;
                        }
                    }
                }

                for (const auto& fp : finals) {
                    Track* dstTr = m_panel.m_timeline->track(fp.dstTrack);
                    if (!dstTr) continue;
                    for (size_t ci = 0; ci < dstTr->clipCount(); ++ci) {
                        if (dstTr->clip(ci)->timelineIn() == fp.finalIn &&
                            dstTr->clip(ci)->id() != fp.clipId) {
                            auto overwrite = EditOperations::resolveOverlaps(
                                *m_panel.m_timeline, fp.dstTrack, dstTr->clip(ci)->id(),
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
                m_panel.recordAppliedDragResult(std::move(masterCompound));
                m_panel.onScrollChanged();
                emit m_panel.contentChanged();
                didMove = true;
            }
        }
        else if (!finals.empty() || hasGhostTrack) {
            auto masterCompound = std::make_unique<CompoundCommand>("Move clips");

            if (hasGhostTrack) {
                masterCompound->addCommand(
                    std::make_unique<InsertTrackAtCommand>(m_panel.m_timeline, newTrackIndex));
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
                    Track* srcTr = m_panel.m_timeline->track(srcIdx);
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
                        *m_panel.m_timeline, fp.srcTrack, fp.dstTrack, fp.clipId, fp.finalIn);
                } else {
                    cmd = EditOperations::moveClip(
                        *m_panel.m_timeline, fp.srcTrack, fp.clipId, fp.finalIn);
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
                Track* dstTr = m_panel.m_timeline->track(s.dstTrack);
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
                    Track* dstTr = m_panel.m_timeline->track(fp.dstTrack);
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
                    *m_panel.m_timeline, fp.dstTrack, resolveId, movedIds);
                if (overwrite) {
                    overwrite->execute();
                    masterCompound->addCommand(std::move(overwrite));
                }
            }

            if (masterCompound->size() > 0) {
                m_panel.recordAppliedDragResult(std::move(masterCompound));
                m_panel.onScrollChanged();
                emit m_panel.contentChanged();
                didMove = true;
            }
        }

        m_clips.clear();
        m_targetTrack = SIZE_MAX;
        m_panel.m_ghostTrackVisible = false;
        if (m_panel.m_ghostOverlay) m_panel.m_ghostOverlay->hide();

        // Clear per-widget drag state on every track. Drag-move set
        // m_draggedSet + m_ghostDragActive on the source track's widget so
        // its paintEvent could hide the live preview; if we don't clear it
        // here a subsequent reuse (e.g. after a ghost-track drop the source
        // widget gets repointed to the new top track) keeps skipping the
        // clip in both paint passes and the clip stays invisible.
        for (auto& tw : m_panel.m_trackWidgets) {
            if (!tw) continue;
            tw->setDraggedClips({});
            tw->setGhostDragActive(false);
        }

        if (didMove) {
            // Always do a full rebuild after a ghost-track drop.
            // m_panel.insertTrackWidgetIncremental() reassigned every existing
            // widget to the POST-insert track index before inserting the
            // new widget, so all existing widgets ended up off-by-one —
            // one of them showed the new track's clip too, which looked
            // like a duplicate clip on a phantom track at the very top.
            // A full rebuild keeps widgets perfectly in sync with the
            // model (a brief flash is far better than a phantom track).
            m_panel.rebuildTracks();
            emit m_panel.selectionChanged();
        }
    }

void TimelineClipMoveTool::restoreOrigins()
{
    if (!m_panel.m_timeline) return;
    // The live-drag preview may have moved each clip between tracks via
    // Track::removeClip()+addClip(), which silently drops every transition
    // referencing that clip on its source track.  Once each clip is back on
    // its original track, re-add the transitions captured at drag-start
    // (a following moveClipToTrack command then carries them over).
    for (const auto& dcs : m_clips) {
        Track* curTr = nullptr;
        size_t curIdx = SIZE_MAX;
        for (size_t st = 0; st < m_panel.m_timeline->trackCount(); ++st) {
            Track* t = m_panel.m_timeline->track(st);
            const size_t idx = t->findClipIndexById(dcs.ref.clipId);
            if (idx < t->clipCount()) {
                curTr = t;
                curIdx = idx;
                break;
            }
        }
        if (!curTr) continue;
        Track* srcTr = m_panel.m_timeline->track(dcs.originalTrack);
        if (!srcTr) continue;
        if (curTr != srcTr) {
            auto clipPtr = curTr->removeClip(curIdx);
            clipPtr->setTimelineIn(dcs.originalIn);
            srcTr->addClip(std::move(clipPtr));
        } else if (curTr->clip(curIdx)->timelineIn() != dcs.originalIn) {
            curTr->moveClip(curIdx, dcs.originalIn);
        }
    }
    for (const auto& dcs : m_clips) {
        if (dcs.originalTransitions.empty()) continue;
        Track* origTr = m_panel.m_timeline->track(dcs.originalTrack);
        if (!origTr) continue;
        for (const auto& orig : dcs.originalTransitions) {
            // Skip if a transition with the same edit point + clip ids is
            // already present (same-track moves leave them).
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
}

} // namespace rt
