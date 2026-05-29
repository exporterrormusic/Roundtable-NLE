/*
 * TimelineWorkspaceOverlay.cpp - Shot switching and transform overlay for TimelineWorkspace.
 * Split from TimelineWorkspace.cpp for maintainability.
 */
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SpineClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/Position2D.h"
#include "timeline/GraphicLayer.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include <QFileInfo>
#include <algorithm>
#include <functional>

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ShotPreset.h"
#endif

#include "effects/Blur.h"

#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QTimer>

#include <spdlog/spdlog.h>

#include <filesystem>
namespace rt {

// Transform overlay ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â update the Program Monitor handle overlay for the
// currently selected clip so the user can drag-to-move / drag-to-scale.
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â

// â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”
// applyShotSwitch â€” shared undo-aware shot switch used by both
// PropertiesPanel and ShotPanel signal handlers.
// â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”â”

void TimelineWorkspace::applyShotSwitch(uint64_t groupId, const std::string& newShotName)
{
    if (!m_timeline || !m_shotPresetManager || groupId == 0) {
        if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        return;
    }

    auto presetOpt = m_shotPresetManager->load(newShotName);
    if (!presetOpt) {
        spdlog::warn("TimelineWorkspace: shot preset '{}' not found", newShotName);
        if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        return;
    }

    const auto& preset = *presetOpt;
    spdlog::info("TimelineWorkspace: applying shot '{}' to group {}", newShotName, groupId);

    // â”€â”€ Snapshot old clips for undo â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    struct ClipSnapshot {
        size_t trackIndex;
        std::unique_ptr<Clip> clip;
    };
    auto oldClips = std::make_shared<std::vector<ClipSnapshot>>();
    auto oldShotName = std::make_shared<std::string>();

    // Find existing group's time position/duration AND the video tracks
    // the group currently occupies (so the new preset's layers land on
    // those same tracks instead of being re-routed elsewhere).
    //
    // Time span is computed as [min(timelineIn), max(timelineOut)] over
    // every visual clip in the group. For a normal shot group all members
    // are aligned, so this matches the first clip's range. For a freshly-
    // grouped multi-clip selection (where the user picked unrelated clips
    // at different times) it correctly covers the bounding span the user
    // selected — the new shot fills the whole selection rather than just
    // the first clip's slot.
    int64_t groupStart = 0;
    int64_t groupEnd = 48000; // 1s fallback (used only if no group clips found)
    std::vector<size_t> groupVideoTracks; // ascending; front (top) -> back (bottom)
    bool foundGroupTime = false;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* trk = m_timeline->track(ti);
        if (!trk) continue;
        bool trackHasGroupVisual = false;
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio) {
                const int64_t cIn  = c->timelineIn();
                const int64_t cOut = c->timelineOut();
                if (!foundGroupTime) {
                    groupStart = cIn;
                    groupEnd   = cOut;
                    foundGroupTime = true;
                } else {
                    if (cIn  < groupStart) groupStart = cIn;
                    if (cOut > groupEnd)   groupEnd   = cOut;
                }
                trackHasGroupVisual = true;
            }
        }
        if (trackHasGroupVisual && trk->type() == TrackType::Video)
            groupVideoTracks.push_back(ti);
    }
    int64_t groupDuration = std::max<int64_t>(1, groupEnd - groupStart);

    // Clone old visual clips in this group (for undo).
    // IMPORTANT: clone() assigns a fresh global ID, so we explicitly copy
    // the original id onto the snapshot. insertSnapshots() will then keep
    // that id stable across every undo/redo cycle. Without this, undoing a
    // shot switch resurrects the old clips with brand-new ids and any
    // earlier undo command on the stack that referenced them by id (a
    // prior MoveClipCommand, trim, etc.) silently fails to apply.
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* trk = m_timeline->track(ti);
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            Clip* c = trk->clip(ci);
            if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio) {
                if (oldShotName->empty() && !c->shotName().empty())
                    *oldShotName = c->shotName();
                auto snapClone = c->clone();
                snapClone->setId(c->id()); // preserve original id for undo correctness
                oldClips->push_back({ti, std::move(snapClone)});
            }
        }
    }

    // â”€â”€ Build list of new clips â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto newClips = std::make_shared<std::vector<ClipSnapshot>>();

    const auto& order = preset.layerOrder();

    // â”€â”€ Ensure enough video tracks exist â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Count visible layers to determine the required track count.
    size_t neededTracks = 0;
    for (size_t li = 0; li < order.size(); ++li) {
        const auto& lr = order[li];
        bool vis = false;
        if (lr.type == LayerType::Background) {
            auto* bg = preset.background(lr.index);
            vis = (bg && bg->visible);
        } else {
            auto* ch = preset.character(lr.index);
            vis = (ch && ch->visible);
        }
        if (vis) ++neededTracks;
    }

    // ---- Pick the video tracks each new layer should occupy ----------
    // Constraint: replace the OLD shot's clips on the SAME video tracks
    // the group is currently using. The back/BG layer keeps its bottom
    // track, front layers keep theirs. Only when the new preset has
    // MORE layers than the old shot do we add tracks -- and we add them
    // ABOVE the top-most existing group track so the BG track index
    // doesn't shift.
    //
    // Track-index convention here: smaller index = HIGHER in the video
    // stack (renders on top). So:
    //   groupVideoTracks.back()  = bottom of group = BG track
    //   groupVideoTracks.front() = top    of group = front character
    //
    // layerIdx counts from 0 = back (BG) upward to N-1 = front (top char).
    //
    // IMPORTANT: this planning phase is PURE -- it does NOT modify the
    // timeline. Any track inserts/adds are deferred to the redo lambda
    // so they can be reversed cleanly on undo.
    std::vector<size_t> layerTracks(neededTracks, SIZE_MAX);

    // Track-insert plan (consumed by the redo/undo lambdas):
    //   insertPlanAt  : index at which to insert new tracks (all stacked
    //                   at the same position; smaller index = top of stack)
    //   numInserts    : how many new video tracks to add
    //   insertHeight  : height to give each inserted track (inherits from
    //                   whatever track is currently at insertPlanAt so the
    //                   user's customised heights aren't visually disrupted)
    //   appendPlan    : count of extra tracks appended to the bottom of
    //                   the video stack (used when the group is empty)
    size_t insertPlanAt = SIZE_MAX;
    size_t numInserts   = 0;
    float  insertHeight = 80.0f;
    size_t appendPlan   = 0;

    if (!groupVideoTracks.empty() && neededTracks > 0) {
        const size_t reusableN = std::min(neededTracks, groupVideoTracks.size());

        // ── Reuse empty tracks before creating new ones ──────────────
        // A track is "empty" if it has no clips that overlap the group's
        // time span [groupStart, groupEnd].  Tracks with clips elsewhere
        // on the timeline are still usable when they're clear at this spot.
        auto trackEmptyInRange = [this](size_t ti, int64_t start, int64_t end) -> bool {
            Track* t = m_timeline->track(ti);
            if (!t) return false;
            for (size_t ci = 0; ci < t->clipCount(); ++ci) {
                Clip* c = t->clip(ci);
                if (!c) continue;
                // Overlap check: clip [cIn, cOut) vs range [start, end)
                if (c->timelineIn() < end && c->timelineOut() > start)
                    return false; // overlaps — not empty here
            }
            return true; // clear in this time range
        };

        size_t shortage = (neededTracks > groupVideoTracks.size())
            ? neededTracks - groupVideoTracks.size() : 0;
        std::vector<size_t> emptyCandidates;
        if (shortage > 0) {
            // Scan every video track, checking for clips that overlap
            // the group's time span (not the entire track lifetime).
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                Track* t = m_timeline->track(ti);
                if (!t || t->isDivider() || t->type() != TrackType::Video)
                    continue;
                if (!trackEmptyInRange(ti, groupStart, groupEnd)) continue;
                bool inGroup = false;
                for (size_t g : groupVideoTracks)
                    if (g == ti) { inGroup = true; break; }
                if (inGroup) continue;
                emptyCandidates.push_back(ti);
            }
            // Sort by distance to group for compactness
            const size_t gf = groupVideoTracks.front();
            const size_t gr = groupVideoTracks.back();
            std::sort(emptyCandidates.begin(), emptyCandidates.end(),
                [gf, gr](size_t a, size_t b) {
                    auto d = [gf, gr](size_t x) {
                        if (x < gf) return gf - x;
                        if (x > gr) return x - gr;
                        return size_t{0};
                    };
                    size_t da = d(a), db = d(b);
                    if (da != db) return da < db;
                    return a < b;
                });
        }

        // Consume empty candidates in proximity order (closest first for
        // compact placement).  Only create new tracks when there are
        // genuinely no tracks clear in this time range anywhere.
        size_t shift = 0;
        std::vector<size_t> extraTracks;
        if (shortage > 0) {
            size_t usedEmpty = 0;
            for (size_t ei = 0; ei < emptyCandidates.size() && usedEmpty < shortage; ++ei) {
                extraTracks.push_back(emptyCandidates[ei]);
                ++usedEmpty;
            }
            shortage -= usedEmpty;

            // Only insert new tracks as a last resort
            if (shortage > 0) {
                numInserts   = shortage;
                insertPlanAt = groupVideoTracks.front();
                if (insertPlanAt < m_timeline->trackCount()) {
                    Track* refTrack = m_timeline->track(insertPlanAt);
                    if (refTrack && refTrack->height() >= 1.0f)
                        insertHeight = refTrack->height();
                }
                shift = numInserts;
            }

            // Map the extra (reused empty) track indices into layerTracks.
            // If new tracks are being inserted at insertPlanAt, any reused
            // track at-or-after insertPlanAt shifts down by numInserts.
            for (size_t e = 0; e < extraTracks.size(); ++e) {
                size_t idx = extraTracks[e];
                if (idx >= insertPlanAt && insertPlanAt != SIZE_MAX)
                    idx += shift;
                layerTracks[reusableN + e] = idx;
            }
        }

        // Reused-group layer indices, accounting for the upcoming shift:
        // existing group tracks at-or-after insertPlanAt move down by `shift`.
        for (size_t k = 0; k < reusableN; ++k) {
            size_t orig = groupVideoTracks[groupVideoTracks.size() - 1 - k];
            layerTracks[k] = (orig >= insertPlanAt && insertPlanAt != SIZE_MAX)
                ? orig + shift : orig;
        }

        // New tracks all land at insertPlanAt (each insert shifts the
        // previous one to insertPlanAt+1, etc.). Map the topmost (front)
        // layer to the topmost (smallest index) new track.
        for (size_t e = 0; e < numInserts; ++e) {
            layerTracks[reusableN + extraTracks.size() + e] = insertPlanAt + (numInserts - 1 - e);
        }

        spdlog::warn("applyShotSwitch: layerTracks=[{}] reusableN={} extraTracks={} "
                     "numInserts={} insertPlanAt={} shift={}",
            [&]{
                std::string s;
                for (size_t i = 0; i < layerTracks.size(); ++i) {
                    if (i) s += ",";
                    s += std::to_string(layerTracks[i]);
                }
                return s;
            }(),
            reusableN, extraTracks.size(), numInserts,
            insertPlanAt == SIZE_MAX ? std::string{"NONE"} : std::to_string(insertPlanAt),
            shift);
    } else if (neededTracks > 0) {
        // No existing group on timeline -- place layers at the bottom of
        // the video stack, appending tracks (via addVideoTrack) as needed.
        std::vector<size_t> videoIndices;
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti)
            if (m_timeline->track(ti)->type() == TrackType::Video)
                videoIndices.push_back(ti);
        if (videoIndices.size() < neededTracks)
            appendPlan = neededTracks - videoIndices.size();
        // After the appends, video indices run from
        // [old front .. old back, new1, new2, ...] -- where new tracks
        // sit just after the existing video tracks. Their indices come
        // immediately after videoIndices.back() (or 0 if no video yet).
        std::vector<size_t> finalVideo = videoIndices;
        size_t nextIdx = videoIndices.empty() ? 0 : videoIndices.back() + 1;
        for (size_t e = 0; e < appendPlan; ++e)
            finalVideo.push_back(nextIdx + e);
        for (size_t k = 0; k < neededTracks; ++k)
            layerTracks[k] = finalVideo[finalVideo.size() - 1 - k];
    }

    // â”€â”€ Map layers to tracks by position â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // layerOrder[0] = FRONT (top of UI â†’ lowest index â†’ V3)
    // layerOrder[last] = BACK  (bottom of UI â†’ highest index â†’ V1)
    // Iterate back-to-front (oi from lastâ†’0), assign track positions
    // so that:
    //   back layer (BG)    â†’ highest video index â†’ V1 (bottom)
    //   front layer (char) â†’ lowest video index  â†’ VN (top)

    int layerIdx = 0;
    for (int oi = static_cast<int>(order.size()) - 1; oi >= 0; --oi) {
        const auto& ref = order[static_cast<size_t>(oi)];

        // Target track is whatever was assigned for this layerIdx above.
        size_t targetTrack = (static_cast<size_t>(layerIdx) < layerTracks.size())
            ? layerTracks[static_cast<size_t>(layerIdx)]
            : 0;

        if (ref.type == LayerType::Background) {
            auto* bg = preset.background(ref.index);
            if (!bg || !bg->visible) continue;

            auto vc = std::make_unique<VideoClip>();
            vc->setMediaPath(bg->path);
            vc->setTimelineIn(groupStart);
            vc->setDuration(groupDuration);
            // Use original filename (no extension) as label
            QString bgFile = QString::fromStdString(bg->path);
            QString bgLabel = QFileInfo(bgFile).baseName();
            vc->setLabel(bgLabel.toStdString());
            vc->setGroupId(groupId);
            vc->setLayerId("background_" + std::to_string(ref.index));
            vc->setShotName(newShotName);
            constexpr float outW2 = 1920.0f, outH2 = 1080.0f;
            vc->positionX().setDefaultValue((bg->posX - 0.5f) * outW2);
            vc->positionY().setDefaultValue((bg->posY - 0.5f) * outH2);
            vc->scaleX().setDefaultValue(bg->scale);
            vc->scaleY().setDefaultValue(bg->scale);
            vc->opacity().setDefaultValue(bg->opacity);
            if (bg->cropLeft > 0 || bg->cropRight > 0 || bg->cropTop > 0 || bg->cropBottom > 0)
                vc->setCrop(bg->cropLeft, bg->cropRight, bg->cropTop, bg->cropBottom);
            if (bg->blur > 0.0f) {
                auto fx = std::make_unique<Blur>();
                fx->param(Blur::Radius).track.setDefaultValue(bg->blur);
                vc->effects().addEffect(std::move(fx));
            }
            newClips->push_back({targetTrack, std::move(vc)});
        } else { // Character
            auto* ch = preset.character(ref.index);
            if (!ch || !ch->visible) continue;

            if (ch->isVideoCharacter()) {
                const std::string& videoPath = ch->activeVideoPath();
                if (videoPath.empty()) { ++layerIdx; continue; }
                auto vc = std::make_unique<VideoClip>();
                vc->setMediaPath(videoPath);
                vc->setTimelineIn(groupStart);
                vc->setDuration(groupDuration);
                // Use "CHARACTER - ANIMATION" as label
                std::string animLabel = ch->characterName;
                if (!ch->animation.empty())
                    animLabel += " - " + ch->animation;
                vc->setLabel(animLabel);
                vc->setShotName(newShotName);
                vc->setGroupId(groupId);
                vc->setLayerId("char_" + std::to_string(ref.index));
                // Character metadata for Properties panel controls
                vc->setCharacterName(ch->characterName);
                vc->setTalking(ch->isTalking);
                vc->setVideoMutePath(ch->videoMutePath);
                vc->setVideoTalkPath(ch->videoTalkPath);
                constexpr float cW = 1920.0f, cH = 1080.0f;
                vc->positionX().setDefaultValue((ch->posX - 0.5f) * cW);
                vc->positionY().setDefaultValue((ch->posY - 0.5f) * cH);
                // Flip is encoded as the sign of the scale so the
                // PropertiesPanel flip checkboxes round-trip cleanly.
                // The compositor applies the 0.85× COMPOSE character-fit
                // dynamically via srcH→outH normalization.
                vc->scaleX().setDefaultValue(ch->flipX ? -ch->scale : ch->scale);
                vc->scaleY().setDefaultValue(ch->flipY ? -ch->scale : ch->scale);
                vc->opacity().setDefaultValue(ch->opacity);
                if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                    vc->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);
                if (ch->blur > 0.0f) {
                    auto fx = std::make_unique<Blur>();
                    fx->param(Blur::Radius).track.setDefaultValue(ch->blur);
                    vc->effects().addEffect(std::move(fx));
                }
                newClips->push_back({targetTrack, std::move(vc)});
            } else {
                auto sc = std::make_unique<SpineClip>();
                sc->setCharacterName(ch->characterName);
                sc->setOutfit(ch->outfit);
                sc->setStance(ch->stance);
                sc->setAnimationName(ch->animation);
                sc->setTalking(ch->isTalking);
                sc->setTimelineIn(groupStart);
                sc->setDuration(groupDuration);
                // Use "CHARACTER - ANIMATION" as label
                std::string animLabel = ch->characterName;
                if (!ch->animation.empty())
                    animLabel += " - " + ch->animation;
                sc->setLabel(animLabel);
                sc->setShotName(newShotName);
                sc->setGroupId(groupId);
                sc->setLayerId("char_" + std::to_string(ref.index));
                constexpr float sW = 1920.0f, sH = 1080.0f;
                // The COMPOSE 0.85 base-fit factor is applied dynamically
                // in the compositor (compositeFrame) via the GPU spine
                // FBO's native 0.85× fit, matching COMPOSE exactly.
                sc->positionX().setDefaultValue((ch->posX - 0.5f) * sW);
                sc->positionY().setDefaultValue((ch->posY - 0.5f) * sH);
                // Flip is encoded as the sign of the scale so the
                // PropertiesPanel flip checkboxes round-trip cleanly.
                sc->scaleX().setDefaultValue(ch->flipX ? -ch->scale : ch->scale);
                sc->scaleY().setDefaultValue(ch->flipY ? -ch->scale : ch->scale);
                sc->opacity().setDefaultValue(ch->opacity);
                if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                    sc->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);
                if (ch->blur > 0.0f) {
                    auto fx = std::make_unique<Blur>();
                    fx->param(Blur::Radius).track.setDefaultValue(ch->blur);
                    sc->effects().addEffect(std::move(fx));
                }
                newClips->push_back({targetTrack, std::move(sc)});
            }
        }
        ++layerIdx;
    }

    // ---- Helpers ------------------------------------------------------
    // Clear panel pointers that may reference clips we're about to free.
    // A deferred paint event firing between removeClip() and the rebuild
    // would otherwise dereference freed std::string members (label,
    // shotName) and crash inside QString::fromStdString.
    auto clearPanelSelections = [this]() {
        m_selectedClip = nullptr;
        if (m_propertiesPanel)     m_propertiesPanel->clearClip();
        if (m_effectControlsPanel) m_effectControlsPanel->setClip(nullptr, nullptr);
        if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->setClip(nullptr, nullptr);
        if (m_ColorGradingPanel)   m_ColorGradingPanel->setClip(nullptr, nullptr);
        if (m_timelinePanel)       m_timelinePanel->selection().clear();
    };

    auto removeGroupVisualClips = [this, groupId]() {
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            Track* trk = m_timeline->track(ti);
            if (!trk) continue;
            for (int ci = static_cast<int>(trk->clipCount()) - 1; ci >= 0; --ci) {
                Clip* c = trk->clip(static_cast<size_t>(ci));
                if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio)
                    trk->removeClip(static_cast<size_t>(ci));
            }
        }
    };

    auto insertSnapshots = [this, groupId](
            const std::vector<ClipSnapshot>& source,
            const std::string& shotNameToPropagate) {
        for (const auto& snap : source) {
            if (!snap.clip) continue;
            Track* trk = nullptr;
            size_t idx = snap.trackIndex;
            if (idx < m_timeline->trackCount()) {
                trk = m_timeline->track(idx);
                if (trk && trk->type() != TrackType::Video) trk = nullptr;
            }
            if (!trk) {
                // Fallback: nearest video track upward, then downward.
                for (size_t si = idx; si < m_timeline->trackCount(); ++si) {
                    Track* t = m_timeline->track(si);
                    if (t && t->type() == TrackType::Video) { trk = t; break; }
                }
                if (!trk) {
                    for (size_t si = idx; si > 0; --si) {
                        Track* t = m_timeline->track(si - 1);
                        if (t && t->type() == TrackType::Video) { trk = t; break; }
                    }
                }
                if (!trk) trk = m_timeline->addVideoTrack("");
            }
            if (trk) {
                // Preserve the snapshot's clip id on the live re-inserted
                // clone. snap.clip already holds the canonical id (set at
                // capture time for old clips, the original creation id for
                // new shot-layer clips) — without restoring it here, every
                // redo/undo would mint a new global id and stale undo
                // commands referencing the prior id would silently no-op.
                auto reClone = snap.clip->clone();
                reClone->setId(snap.clip->id());
                trk->addClip(std::move(reClone));
            }
        }
        // Propagate the shot name to remaining group members (audio).
        if (!shotNameToPropagate.empty()) {
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                Track* trk = m_timeline->track(ti);
                if (!trk) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    Clip* c = trk->clip(ci);
                    if (c && c->groupId() == groupId)
                        c->setShotName(shotNameToPropagate);
                }
            }
        }
    };

    // Finalise: rebuild track widgets (so newly inserted tracks get widgets),
    // invalidate the composite cache, force the Program Monitor to recompose
    // the current frame (notifyScrub + requestRefresh is what the seek/scrub
    // path uses -- requestRefresh alone is sometimes coalesced into the same
    // frame and the user sees stale/black until they nudge the playhead),
    // and reselect a representative clip in the group.
    auto finaliseAndReselect = [this, groupId]() {
        invalidateCompositeCache();
        if (m_timelinePanel) m_timelinePanel->rebuildTracks();
        // Kick off background opens for the new shot's media (NVDEC init
        // + FFmpeg probe is 100-170ms per character clip). preOpenVideoMedia
        // posts a second requestRefresh() back to the UI thread once the
        // background opens finish, so the composite that finally samples
        // the new clips finds the media warm.
        preOpenVideoMedia();
        if (m_programMonitor) {
            m_programMonitor->notifyScrub();
            m_programMonitor->requestRefresh();
        }

        Clip* picked = nullptr;
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            Track* trk = m_timeline->track(ti);
            if (!trk) continue;
            for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                Clip* c = trk->clip(ci);
                if (c && c->groupId() == groupId && c->clipType() != ClipType::Audio) {
                    picked = c; break;
                }
            }
            if (picked) break;
        }
        if (picked) {
            m_selectedClip = picked;
            if (m_propertiesPanel) m_propertiesPanel->setClip(picked);
        }
    };

    // ---- Execute via undo command ------------------------------------
    // The redo lambda inserts the planned tracks (so undo can remove
    // them); the undo lambda undoes both the clip swap AND the inserts.
    if (m_commandStack) {
        auto cmd = std::make_unique<LambdaCommand>(
            "Switch Shot to " + newShotName,
            // REDO
            [this, newClips, newShotName, insertPlanAt, numInserts, insertHeight,
             appendPlan, clearPanelSelections, removeGroupVisualClips,
             insertSnapshots, finaliseAndReselect]() {
                clearPanelSelections();
                removeGroupVisualClips();
                // Insert planned tracks ABOVE existing top-of-group.
                for (size_t e = 0; e < numInserts; ++e) {
                    auto t = std::make_unique<Track>(TrackType::Video, std::string{});
                    t->setHeight(insertHeight);
                    m_timeline->insertTrack(insertPlanAt, std::move(t));
                }
                // Append planned tracks at the bottom of the video stack.
                for (size_t e = 0; e < appendPlan; ++e)
                    (void)m_timeline->addVideoTrack("");
                insertSnapshots(*newClips, newShotName);
                finaliseAndReselect();
            },
            // UNDO
            [this, oldClips, oldShotName, insertPlanAt, numInserts, appendPlan,
             clearPanelSelections, removeGroupVisualClips, insertSnapshots,
             finaliseAndReselect]() {
                clearPanelSelections();
                removeGroupVisualClips();
                // Remove the tracks that REDO inserted (top of group).
                // They occupy [insertPlanAt .. insertPlanAt+numInserts-1];
                // remove from the highest index down so each takeTrack uses
                // an index that's still valid.
                for (size_t e = 0; e < numInserts; ++e) {
                    size_t removeAt = insertPlanAt + (numInserts - 1 - e);
                    if (removeAt < m_timeline->trackCount())
                        (void)m_timeline->takeTrack(removeAt);
                }
                // Remove the tracks that REDO appended. These should be
                // the last `appendPlan` video tracks. Walk from the end.
                for (size_t e = 0; e < appendPlan; ++e) {
                    for (size_t i = m_timeline->trackCount(); i-- > 0; ) {
                        Track* t = m_timeline->track(i);
                        if (t && t->type() == TrackType::Video
                                && t->clipCount() == 0) {
                            (void)m_timeline->takeTrack(i);
                            break;
                        }
                    }
                }
                insertSnapshots(*oldClips, *oldShotName);
                finaliseAndReselect();
            });
        cmd->execute();
        m_commandStack->pushWithoutExecute(std::move(cmd));
    } else {
        // No command stack: run the redo body inline.
        clearPanelSelections();
        removeGroupVisualClips();
        for (size_t e = 0; e < numInserts; ++e) {
            auto t = std::make_unique<Track>(TrackType::Video, std::string{});
            t->setHeight(insertHeight);
            m_timeline->insertTrack(insertPlanAt, std::move(t));
        }
        for (size_t e = 0; e < appendPlan; ++e)
            (void)m_timeline->addVideoTrack("");
        insertSnapshots(*newClips, newShotName);
        finaliseAndReselect();
    }

    spdlog::info("TimelineWorkspace: shot switch complete");
}

void TimelineWorkspace::scheduleOverlayRefresh()
{
    // Bump generation so stale timer callbacks are ignored.
    uint32_t gen = ++m_overlayRefreshGen;

    // Also force a recomposite so the displayed frame matches compositeWidth.
    invalidateCompositeCache();
    if (m_programMonitor) m_programMonitor->requestRefresh();

    // Schedule deferred overlay updates at increasing intervals.
    // By the time the later callbacks fire, the pipeline will have
    // composited and displayed the new frame, so srcWidth() is current.
    for (int delayMs : {0, 50, 150}) {
        QTimer::singleShot(delayMs, this, [this, gen]() {
            if (gen == m_overlayRefreshGen)
                updateTransformOverlay();
        });
    }
}

void TimelineWorkspace::updateTransformOverlay()
{
    if (!m_programMonitor || !m_programMonitor->viewport()) return;
    auto* vp = m_programMonitor->viewport();

    if (!m_selectedClip || !m_timeline) {
        vp->clearTransformOverlay();
        if (m_programMonitor->transformOverlay())
            m_programMonitor->transformOverlay()->clearTransformOverlay();
        return;
    }

    // Hide overlay when playhead is outside the clip's time range.
    int64_t clipEnd = m_selectedClip->timelineOut();
    int64_t clipIn = m_selectedClip->timelineIn();
    if (m_playbackController) {
        int64_t curTick = m_playbackController->currentTick();
        if (curTick < clipIn || curTick > clipEnd) {
            vp->clearTransformOverlay();
            if (m_programMonitor->transformOverlay())
                m_programMonitor->transformOverlay()->clearTransformOverlay();
            return;
        }
    }

    TransformOverlayInfo info;
    info.visible  = true;

    // Evaluate at the current playhead position relative to clip start
    const int64_t relTick = m_playbackController
        ? std::max(int64_t{0}, m_playbackController->currentTick() - m_selectedClip->timelineIn())
        : int64_t{0};

    // Per-layer transform for GraphicClip: when a specific layer is selected
    // in Essential Graphics, show the overlay sized around that layer only.
    if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
        if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
            auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
            const auto& xf = layer->transform();
            info.posX     = xf.posX.evaluate(relTick);
            info.posY     = xf.posY.evaluate(relTick);
            info.scaleX   = xf.scaleX.evaluate(relTick);
            info.scaleY   = xf.scaleY.evaluate(relTick);
            info.rotation = xf.rotation.evaluate(relTick);
            info.anchorX  = xf.anchorX.evaluate(relTick);
            info.anchorY  = xf.anchorY.evaluate(relTick);

            // Content-rect mode: the overlay will apply the EXACT same
            // QPainter transform that renderGraphicClip() uses, so the
            // bounding box matches the rendered content pixel-perfectly.
            // posX/posY/scale/rotation are the layer transform values;
            // contentL/T/R/B are the pre-transform canvas-space bounds.
            info.useContentRect = true;

            if (layer->layerType() == GraphicLayerType::Text) {
                auto* tl = static_cast<TextLayer*>(layer);
                QFont font(QString::fromStdString(tl->fontFamily()),
                           static_cast<int>(tl->fontSize()));
                font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                font.setItalic(tl->isItalic());
                float tracking = tl->tracking().evaluate(relTick);
                font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));

                QString text = QString::fromStdString(tl->text());
                if (tl->allCaps()) text = text.toUpper();

                // Output dimensions — the project/sequence resolution.
                // renderGraphicClip() renders at full project res then
                // downscales and adds posX raw in that space, so the
                // overlay must measure/place in the SAME space. This is
                // NOT m_programMonitor->outputWidth() (the preview res,
                // which can be a 1920 preview of a 4K project — that
                // mismatch drifted the box from the text proportionally
                // to distance from center).
                uint32_t outW = 0, outH = 0;
                graphicCanvasRes(outW, outH);

                // Use a very large rect so text is never clipped/wrapped by canvas bounds
                double bigW = static_cast<double>(outW) * 10.0;
                double bigH = static_cast<double>(outH) * 10.0;
                QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                bigW, bigH);

                // Same alignment as renderGraphicClip
                int hAlign = Qt::AlignHCenter;
                switch (tl->alignment()) {
                    case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                    case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                    case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                    case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
                }
                int vAlign = Qt::AlignVCenter;
                switch (tl->vAlignment()) {
                    case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                    case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                    case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
                }

                // Measure text bounds using QPainter::drawText — this is the
                // exact same code path as renderGraphicClip(), guaranteeing
                // pixel-perfect bounding box matching.
                QImage metricsCanvas(static_cast<int>(outW), static_cast<int>(outH),
                                     QImage::Format_ARGB32_Premultiplied);
                QPainter metricsP(&metricsCanvas);
                metricsP.setRenderHint(QPainter::Antialiasing, true);
                metricsP.setRenderHint(QPainter::TextAntialiasing, true);
                metricsP.setFont(font);
                QRectF textBounds;
                metricsP.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap,
                                  text, &textBounds);
                metricsP.end();

                // Premiere-style breathing room around the glyphs. Two parts:
                //   • Horizontal: side-bearing slack so the box doesn't kiss
                //     the leftmost/rightmost ink (which would put the corner
                //     scale handles ON the glyphs).
                //   • Vertical: a fraction of font height — text ascenders/
                //     descenders vary, but ~25% of one line height matches
                //     Premiere's clear margin above caps and below descenders.
                const float horizPad = tl->fontSize() * 0.45f;
                const float vertPad  = tl->fontSize() * 0.40f;
                info.contentL = static_cast<float>(textBounds.left())   - horizPad;
                info.contentT = static_cast<float>(textBounds.top())    - vertPad;
                info.contentR = static_cast<float>(textBounds.right())  + horizPad;
                info.contentB = static_cast<float>(textBounds.bottom()) + vertPad;

                spdlog::info("[Overlay] text='{}' outW={} outH={} textBounds=({:.1f},{:.1f},{:.1f},{:.1f}) "
                             "content=({:.1f},{:.1f},{:.1f},{:.1f}) posX={:.1f} posY={:.1f} scX={:.2f} scY={:.2f}",
                             tl->text(), outW, outH,
                             textBounds.left(), textBounds.top(), textBounds.width(), textBounds.height(),
                             info.contentL, info.contentT, info.contentR, info.contentB,
                             info.posX, info.posY, info.scaleX, info.scaleY);

                info.contentCanvasW = static_cast<float>(outW);
                info.contentCanvasH = static_cast<float>(outH);
            } else {
                auto* sl = static_cast<ShapeLayer*>(layer);
                float sw = sl->shapeWidth();
                float sh = sl->shapeHeight();
                // Project/sequence resolution — same space renderGraphicClip
                // composites shapes in (NOT the monitor preview res).
                uint32_t outW = 0, outH = 0;
                graphicCanvasRes(outW, outH);
                // Shapes are centered in the canvas
                float cx = static_cast<float>(outW) * 0.5f;
                float cy = static_cast<float>(outH) * 0.5f;
                info.contentL = cx - sw * 0.5f;
                info.contentT = cy - sh * 0.5f;
                info.contentR = cx + sw * 0.5f;
                info.contentB = cy + sh * 0.5f;
                info.contentCanvasW = static_cast<float>(outW);
                info.contentCanvasH = static_cast<float>(outH);
            }

            // Clip-level (outer) transform — applied by compositor on top of layer transform.
            {
                auto p2 = evaluatePosition2D(m_selectedClip->positionX(),
                                             m_selectedClip->positionY(), relTick);
                info.clipPosX = p2.first;
                info.clipPosY = p2.second;
            }
            info.clipScaleX   = m_selectedClip->scaleX().evaluate(relTick);
            info.clipScaleY   = m_selectedClip->scaleY().evaluate(relTick);
            info.clipRotation = m_selectedClip->rotation().evaluate(relTick);
        }
    } else {
        {
            auto p2 = evaluatePosition2D(m_selectedClip->positionX(),
                                         m_selectedClip->positionY(), relTick);
            info.posX = p2.first;
            info.posY = p2.second;
        }
        info.scaleX   = m_selectedClip->scaleX().evaluate(relTick);
        info.scaleY   = m_selectedClip->scaleY().evaluate(relTick);
        info.rotation = m_selectedClip->rotation().evaluate(relTick);
        info.anchorX  = m_selectedClip->anchorX().evaluate(relTick);
        info.anchorY  = m_selectedClip->anchorY().evaluate(relTick);
    }

    // Determine source dimensions for the bounding box.
    // For VideoClip / SpineClipÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢video fallback, look up the media info.
    // For GraphicClip, use the output resolution (graphics fill the canvas).
    // For GraphicClip without per-layer bounding box, use output resolution
    if (m_selectedClip->clipType() == ClipType::Graphic && info.srcW == 0 && info.srcH == 0) {
        info.srcW = m_programMonitor->outputWidth();
        info.srcH = m_programMonitor->outputHeight();
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<ImageClip*>(m_selectedClip)) {
        auto* imageClip = dynamic_cast<ImageClip*>(m_selectedClip);
        // Use stored source dimensions first
        if (imageClip->sourceWidth() > 0 && imageClip->sourceHeight() > 0) {
            info.srcW = imageClip->sourceWidth();
            info.srcH = imageClip->sourceHeight();
        } else if (m_mediaPool) {
            uint64_t handle = m_compositeService->findMediaHandle(imageClip->mediaPath());
            if (handle == 0) {
                handle = m_mediaPool->open(imageClip->mediaPath());
                if (handle != 0)
                    m_compositeService->registerMediaHandle(imageClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_mediaPool->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                }
            }
        }
    }

    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<VideoClip*>(m_selectedClip)) {
        auto* videoClip = dynamic_cast<VideoClip*>(m_selectedClip);
        if (m_mediaPool) {
            // Try cached handle first
            uint64_t handle = m_compositeService->findMediaHandle(videoClip->mediaPath());
            if (handle == 0) {
                // Media not opened yet ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â open it now so we get correct dimensions.
                // compositeFrame() will find this handle on its next pass.
                handle = m_mediaPool->open(videoClip->mediaPath());
                if (handle == 0) {
                    // Try alternate extension (.webm ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬Â .mov)
                    namespace fs = std::filesystem;
                    fs::path vidPath(videoClip->mediaPath());
                    fs::path alt = vidPath;
                    if (vidPath.extension() == ".webm") alt.replace_extension(".mov");
                    else if (vidPath.extension() == ".mov") alt.replace_extension(".webm");
                    if (alt != vidPath) {
                        handle = m_mediaPool->open(alt);
                        if (handle == 0) {
                            fs::path candidate = fs::path("assets") / "videos" / alt.filename();
                            if (fs::exists(candidate))
                                handle = m_mediaPool->open(candidate);
                        }
                    }
                    if (handle == 0) {
                        fs::path candidate = fs::path("assets") / "videos" / vidPath.filename();
                        if (fs::exists(candidate))
                            handle = m_mediaPool->open(candidate);
                    }
                }
                if (handle != 0)
                    m_compositeService->registerMediaHandle(videoClip->mediaPath(), handle);
            }
            if (handle != 0) {
                const auto* mi = m_mediaPool->getInfo(handle);
                if (mi) {
                    info.srcW = mi->width;
                    info.srcH = mi->height;
                }
            }
        }
    }
#ifdef ROUNDTABLE_HAS_SPINE
    // For SpineClip: use shared spine bounds for the overlay size.
    if ((info.srcW == 0 || info.srcH == 0) && dynamic_cast<SpineClip*>(m_selectedClip)) {
        auto* spineClip = static_cast<SpineClip*>(m_selectedClip);
        if (m_compositeService) {
            const auto* shared = m_compositeService->getSpineSharedDataForOverlay(
                spineClip->characterName(), spineClip->outfit(),
                static_cast<int>(spineClip->stance()));
            // Match the renderer: it frames the character to THIS animation's
            // box (setup pose ∪ the animation's envelope), so the overlay must
            // use the same bounds or it won't line up with the visible
            // character (especially for taller "action" poses).
            float boundsW = shared ? shared->stableBoundsW : 0.0f;
            float boundsH = shared ? shared->stableBoundsH : 0.0f;
            if (shared) {
                auto abIt = shared->animBounds.find(spineClip->animationName());
                if (abIt != shared->animBounds.end()) {
                    boundsW = abIt->second.w;
                    boundsH = abIt->second.h;
                }
            }
            if (shared && boundsW > 1.0f && boundsH > 1.0f) {
                // Scale: same as compositor: fit height with 0.9 padding
                float refH = 1080.0f;
                float fitZoom = (refH / boundsH) * 0.9f;
                info.srcW = static_cast<uint32_t>(boundsW * fitZoom);
                info.srcH = static_cast<uint32_t>(boundsH * fitZoom);
            }
        }
    }
#endif

    // Fallback: if we still don't have dimensions, use the viewport's
    // frame dimensions (the composite output).  This at least makes the
    // bounding box match the visible canvas rather than an arbitrary 16:9.
    if (info.srcW == 0 || info.srcH == 0) {
        if (vp->frameWidth() > 0 && vp->frameHeight() > 0) {
            info.srcW = vp->frameWidth();
            info.srcH = vp->frameHeight();
        } else {
            info.srcW = 1920;
            info.srcH = 1080;
        }
    }

    // Packed-alpha: visible area is top half (same as compositor).
    // Apply this BEFORE pushing to either overlay so both the software
    // viewport and the GPU TransformOverlayWidget receive the same srcH —
    // previously the software path got the un-adjusted (full packed)
    // height, producing a 2×-tall bounding box for packed-alpha clips.
    if (info.srcW > 0 && info.srcH > 0 && !info.directSize && !info.useContentRect) {
        if (auto* vc = dynamic_cast<VideoClip*>(m_selectedClip)) {
            if (m_mediaPool) {
                uint64_t h = m_compositeService->findMediaHandle(vc->mediaPath());
                if (h != 0) {
                    const auto* mi2 = m_mediaPool->getInfo(h);
                    if (mi2 && mi2->packedAlpha) {
                        if (info.srcH > 1) info.srcH /= 2;
                    }
                }
            }
        }
    }

    // Characters (SpineClip and VideoClip flagged as character) are
    // composited with CONTAIN-fit, not cover-fit (see
    // CompositeServiceLayerBuild.cpp where layer.containFit = true is
    // set for isVideoCharClip || isPreRenderedSpine).  The overlay must
    // match that fit mode or the bounding box is grossly oversized
    // (cover-fit overflows for portrait sources, ~2× too big visually).
    {
        bool isCharacter = false;
#ifdef ROUNDTABLE_HAS_SPINE
        if (dynamic_cast<SpineClip*>(m_selectedClip))
            isCharacter = true;
#endif
        if (!isCharacter) {
            if (auto* vc = dynamic_cast<VideoClip*>(m_selectedClip))
                if (vc->isVideoCharacter())
                    isCharacter = true;
        }
        if (isCharacter)
            info.containFit = true;
    }

    vp->setTransformOverlay(info);

    // Build outline-only sibling overlays for every other layer that's
    // multi-selected in the Essential Graphics list. They share the
    // clip-level transform with the focused layer; only the per-layer
    // transform + content rect differ. The overlay widget draws these
    // as thin dashed rectangles (no handles) so the user can see at a
    // glance which layers will travel together in a group-move drag.
    std::vector<TransformOverlayInfo> secondaries;
    if (m_selectedClip
        && m_selectedClip->clipType() == ClipType::Graphic
        && m_selectedGraphicLayerIdxs.size() > 1)
    {
        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
        secondaries.reserve(m_selectedGraphicLayerIdxs.size());
        for (int idx : m_selectedGraphicLayerIdxs) {
            if (idx == m_selectedGraphicLayerIdx) continue;
            if (idx < 0 || idx >= static_cast<int>(gc->layerCount())) continue;
            auto* layer = gc->layer(static_cast<size_t>(idx));
            if (!layer) continue;

            TransformOverlayInfo si;
            si.visible = true;
            si.useContentRect = true;

            const auto& xf = layer->transform();
            si.posX     = xf.posX.evaluate(relTick);
            si.posY     = xf.posY.evaluate(relTick);
            si.scaleX   = xf.scaleX.evaluate(relTick);
            si.scaleY   = xf.scaleY.evaluate(relTick);
            si.rotation = xf.rotation.evaluate(relTick);
            si.anchorX  = xf.anchorX.evaluate(relTick);
            si.anchorY  = xf.anchorY.evaluate(relTick);

            // Shared clip-level (outer) transform.
            si.clipPosX     = info.clipPosX;
            si.clipPosY     = info.clipPosY;
            si.clipScaleX   = info.clipScaleX;
            si.clipScaleY   = info.clipScaleY;
            si.clipRotation = info.clipRotation;

            uint32_t outW = 0, outH = 0;
            graphicCanvasRes(outW, outH);
            si.contentCanvasW = static_cast<float>(outW);
            si.contentCanvasH = static_cast<float>(outH);

            if (layer->layerType() == GraphicLayerType::Text) {
                auto* tl = static_cast<TextLayer*>(layer);
                QFont font(QString::fromStdString(tl->fontFamily()),
                           static_cast<int>(tl->fontSize()));
                font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                font.setItalic(tl->isItalic());
                float tracking = tl->tracking().evaluate(relTick);
                font.setLetterSpacing(QFont::AbsoluteSpacing,
                                      static_cast<qreal>(tracking));
                QString text = QString::fromStdString(tl->text());
                if (tl->allCaps()) text = text.toUpper();

                double bigW = static_cast<double>(outW) * 10.0;
                double bigH = static_cast<double>(outH) * 10.0;
                QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                bigW, bigH);
                int hAlign = Qt::AlignHCenter;
                switch (tl->alignment()) {
                    case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                    case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                    case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                    case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
                }
                int vAlign = Qt::AlignVCenter;
                switch (tl->vAlignment()) {
                    case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                    case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                    case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
                }
                QImage mc(static_cast<int>(outW), static_cast<int>(outH),
                          QImage::Format_ARGB32_Premultiplied);
                QPainter mp(&mc);
                mp.setRenderHint(QPainter::Antialiasing, true);
                mp.setRenderHint(QPainter::TextAntialiasing, true);
                mp.setFont(font);
                QRectF tb;
                mp.drawText(textRect, hAlign | vAlign | Qt::TextWordWrap,
                            text, &tb);
                mp.end();
                const float horizPad = tl->fontSize() * 0.45f;
                const float vertPad  = tl->fontSize() * 0.40f;
                si.contentL = static_cast<float>(tb.left())   - horizPad;
                si.contentT = static_cast<float>(tb.top())    - vertPad;
                si.contentR = static_cast<float>(tb.right())  + horizPad;
                si.contentB = static_cast<float>(tb.bottom()) + vertPad;
            } else {
                auto* sl = static_cast<ShapeLayer*>(layer);
                float sw = sl->shapeWidth();
                float sh = sl->shapeHeight();
                float cx = static_cast<float>(outW) * 0.5f;
                float cy = static_cast<float>(outH) * 0.5f;
                si.contentL = cx - sw * 0.5f;
                si.contentT = cy - sh * 0.5f;
                si.contentR = cx + sw * 0.5f;
                si.contentB = cy + sh * 0.5f;
            }

            secondaries.push_back(si);
        }
    }

    // Also update the GPU overlay widget (TransformOverlayWidget)
    if (auto* overlay = m_programMonitor->transformOverlay()) {
        overlay->setTransformOverlay(info);
        overlay->setSecondaryOverlays(secondaries);

        // Pass mask data for overlay drawing
        if (m_selectedClip && m_selectedClip->maskCount() > 0)
            overlay->setMasks(&m_selectedClip->masks());
        else
            overlay->setMasks(nullptr);

        // Pass Position tracks so the overlay can draw the motion path and
        // expose the right-click "Spatial Interpolation" menu on waypoints.
        if (m_selectedClip
            && m_selectedClip->positionX().keyframeCount() >= 2
            && m_selectedClip->positionY().keyframeCount() >= 2)
        {
            overlay->setMotionPathTracks(&m_selectedClip->positionX(),
                                         &m_selectedClip->positionY(),
                                         m_commandStack);
        } else {
            overlay->clearMotionPath();
        }

        // Tell the overlay the PROJECT/sequence resolution — the same
        // space renderGraphicClip composites text in (and the same one
        // the inline text editor scales its font against). Using
        // ProgramMonitor::outputWidth() here gave the preview resolution,
        // which can be lower than the project (e.g. 1920 preview of a 4K
        // project) and made the inline-edit font sized as if the canvas
        // were 1080-tall — visibly different from the rendered text.
        {
            uint32_t w = 0, h = 0;
            graphicCanvasRes(w, h);
            overlay->setSequenceResolution(w, h);
        }
    }
}

// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â
// TitleClip CPU rendering ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â draw text to a BGRA CachedFrame using QPainter
// ÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚ÂÃƒÂ¢Ã¢â‚¬Â¢Ã‚Â


} // namespace rt
