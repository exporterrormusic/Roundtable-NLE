/*
 * TimelineWorkspaceIntegration.cpp — Timeline integration functions
 * extracted from TimelineWorkspace.cpp.
 *
 * Contains: setTimeline(), invalidateCompositeCache(), refreshAfterUndoRedo().
 */

#include "panels/timeline/TimelineWorkspace.h"
#include "PathUtils.h"

#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Settings.h"

#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/captions/CaptionsPanel.h"
#include "panels/tierlist/TierListPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelinePanel.h"

#include "command/CommandStack.h"
#include "audio/AudioPlaybackService.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "playback/PlaybackController.h"
#include "spine/ModelManager.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"

#include "panels/timeline/MediaWatchController.h"

#include <QTimer>

#include <chrono>
#include <filesystem>

#include <spdlog/spdlog.h>

#include "panels/monitors/SourceMonitor.h"

#include <unordered_set>

namespace rt {

void TimelineWorkspace::setTimeline(Timeline* timeline) {
    // Reset audio service state for the new timeline
    if (m_audioPlayback) {
        m_audioPlayback->cancelWarm();
        m_audioPlayback->waitForWarm();
        m_audioPlayback->reset();
        m_audioPlayback->setTimeline(timeline);
    }

    m_timeline = timeline;
    if (m_compositeService) {
        m_compositeService->setTimeline(timeline);
        m_compositeService->setMediaPool(m_mediaPool);
        m_compositeService->setModelManager(m_modelManager);
        m_compositeService->clearMediaHandles();
        // The composite LRU is keyed by (tick, w, h) only — it has no notion
        // of which sequence produced a frame. Swapping the active timeline
        // (e.g. opening a nested sequence) would otherwise let checkLru()
        // return the previous sequence's cached frame at the same tick, so
        // the Program Monitor shows the old sequence's content. Flush it.
        m_compositeService->requestCacheInvalidation();
#ifdef ROUNDTABLE_HAS_SPINE
        m_compositeService->setSpineLoadScheduler(
            [this](const std::string& c, const std::string& o, int s, const std::string& a) {
                scheduleSpineSharedLoad(c, o, s, a);
            });
#endif

        // Safe-mode callback wiring removed in P2 of CLAUDE_IMPROVEMENT_PLAN.
    }

    // Forward to TimelinePanel so its track widgets and ensureDefaultTracks
    // operate on the correct timeline (e.g. after project open).
    // Also forward nullptr to clear the dangling reference when a project
    // is deleted while open, preventing use-after-free crashes.
    if (m_timelinePanel)
        m_timelinePanel->setTimeline(timeline);

    // Forward to PropertiesPanel so shot switching can find group clips.
    if (m_propertiesPanel)
        m_propertiesPanel->setTimeline(timeline);

    // Forward to EffectControlsPanel
    if (m_effectControlsPanel)
        m_effectControlsPanel->setTimeline(timeline);

    // Forward to CaptionsPanel so the caption list rebuilds for the new sequence.
    if (m_captionsPanel)
        m_captionsPanel->setTimeline(timeline);

    // Forward to TierListPanel so Set Start/End can read the playhead and the
    // panel edits the clips of the active sequence.
    if (m_tierListPanel)
        m_tierListPanel->setTimeline(timeline);

    // Forward to ProgramMonitor so its mini-timeline gets the correct
    // duration, in/out points, and playhead range.
    if (m_programMonitor) {
        m_programMonitor->setTimeline(timeline);

        if (timeline) {
            // Re-wire the composite callback — setCurrentProject() in
            // MainWindow calls setCompositeCallback(nullptr) during cleanup,
            // so we must re-establish it when a new timeline is set.
            m_programMonitor->setCompositeCallback(
                [this](int64_t tick, uint32_t w, uint32_t h, bool scrubMode)
                    -> std::shared_ptr<CachedFrame> {
                    return compositeFrame(tick, w, h, scrubMode);
                });

            // Re-start polling so the Program Monitor updates on every tick.
            m_programMonitor->startPolling();
        }
    }

#ifdef ROUNDTABLE_HAS_SPINE
    // Pre-warm the spine cache so first compositeFrame doesn't block on
    // disk I/O (skel parse + PNG decode).  ~100-200ms moved from first
    // render to project-open time where it's imperceptible.
    if (timeline)
        preloadSpineAssets();
#endif

    // Force an initial composite so the Program Monitor shows the frame
    // at the current playhead as soon as the project opens, rather than
    // waiting for the user to scrub or press play.
    if (m_programMonitor && timeline) {
        QTimer::singleShot(100, this, [this]() {
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // Pre-warm the audio decode cache in a background thread so the first
    // play starts instantly instead of blocking on file I/O + resampling.
    if (timeline)
        warmAudioCacheAsync();

    // Pre-open video/image media handles so the first compositeFrame
    // doesn't block on decoder initialization + file probing.
    if (timeline)
        preOpenVideoMedia();

    // Arm the live file-swap watcher for the new project's media.
    if (timeline)
        m_mediaWatch->rescan();
}

void TimelineWorkspace::invalidateCompositeCache()
{
    if (m_compositeService) m_compositeService->requestCacheInvalidation();
}

void TimelineWorkspace::invalidateCompositeCacheRange(int64_t fromTick, int64_t toTick)
{
    if (m_compositeService)
        m_compositeService->requestCacheInvalidationRange(fromTick, toTick);
}

void TimelineWorkspace::refreshChangedMedia(const std::filesystem::path& path)
{
    if (path.empty()) return;

    spdlog::warn("[LIVE-RELOAD] refreshChangedMedia '{}' — invalidate + "
                 "forget + recomposite", pathToUtf8(path));

    // 0. Release the OS file HANDLE and evict caches WITHOUT reopening
    //    the decoder.  On Windows, a delete-pending file's name cannot be
    //    reused until ALL handles close — even FILE_SHARE_DELETE handles.
    //    By leaving the decoder closed, Explorer can immediately recreate
    //    the file.  The decoder reopens on demand in the next getFrame()
    //    call below.
    MediaHandle changed = InvalidMedia;
    if (m_mediaPool)
        changed = m_mediaPool->invalidatePath(path, /*reopenDecoder=*/false);

    // 1. Prime the cache synchronously so the first composite hits with
    //    new pixels immediately.  invalidatePath released the HANDLE above;
    //    getFrame reopens the decoder briefly (ms for PNG), decodes the
    //    new file, and for still images closes the decoder again.
    //    Without this, the composite at 150ms finds an empty CPU+GPU cache,
    //    tryGetFrame returns nullptr, the layer is skipped, and the old
    //    composite output is shown — the "distorted until scrub" symptom.
    if (m_mediaPool && changed != InvalidMedia) {
        m_mediaPool->schedulePrefetch(changed, /*afterFrame=*/0,
                                      /*count=*/1, /*urgent=*/true,
                                      ResolutionTier::Full);
        (void)m_mediaPool->getFrame(changed, /*frame=*/0,
                                    ResolutionTier::Full,
                                    /*scrubMode=*/false);
    }

    // 2. Evict that media's GPU textures. GpuTextureCache is keyed by
    //    (mediaId, frame, tier); since the handle is preserved that key is
    //    unchanged and the compositor would otherwise keep drawing the
    //    stale uploaded texture even though MediaPool now decodes the new
    //    file (this was the "updates in Source Monitor but not the
    //    timeline when scrubbed" symptom).
    if (m_compositeService) {
        if (changed != InvalidMedia)
            m_compositeService->invalidateMediaTextures(changed);
        // Also drop the compositor's cached path→handle mapping (harmless
        // belt-and-suspenders now that the handle is preserved).
        m_compositeService->forgetMediaPath(pathToUtf8(path));
    }

    // 2b. Evict the Project Bin's thumbnail cache and queue a fresh
    //     generation so the icon/grid view updates live — assets in the
    //     bin that aren't yet on a timeline would otherwise show a stale
    //     thumbnail until the next app restart.
    if (m_projectBin)
        m_projectBin->refreshFileThumbnail(path);

    // 3. Flush composited output and refresh the visible monitor so every
    //    timeline instance shows the new content immediately.
    invalidateCompositeCache();
    if (m_timelinePanel) m_timelinePanel->rebuildTracks();
    if (m_programMonitor) m_programMonitor->requestRefresh();

    // 4. Deferred multi-shot refresh — the new file decode is async (the
    //    still is re-opened+decoded on the next getFrame), and the first
    //    refresh often races a still-in-progress write (perf_log shows
    //    'self-test FAILED GetLastError=32 — FILE_SHARE_DELETE not active'
    //    on the immediate reopen, then a clean reopen ~250ms later).  A
    //    single synchronous requestRefresh() composites BEFORE the new
    //    pixels exist, so the user sees blank until they scrub.  Schedule
    //    a few follow-up refreshes so the freshly-decoded frame is
    //    presented WITHOUT any user interaction.  Each shot also reloads
    //    the Source Monitor in place if it happens to be showing the
    //    changed media (otherwise the user has to re-double-click).
    auto kickRefresh = [this, changed](int shot) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        spdlog::warn("[LIVE-RELOAD] kick#{} firing — invalidating composite + "
                     "requestRefresh (handle={})", shot, changed);
        invalidateCompositeCache();
        if (m_programMonitor) m_programMonitor->requestRefresh();
        if (m_sourceMonitor && m_mediaPool && changed != InvalidMedia &&
            m_sourceMonitor->mediaHandle() == changed) {
            m_sourceMonitor->loadClip(changed, m_mediaPool);
        }
    };
    QTimer::singleShot(150,  this, [kickRefresh]() { kickRefresh(1); });
    QTimer::singleShot(450,  this, [kickRefresh]() { kickRefresh(2); });
    QTimer::singleShot(1000, this, [kickRefresh]() { kickRefresh(3); });

    // QFileSystemWatcher stops watching a path once the file is replaced
    // (Explorer's "overwrite" is a delete+create / rename). Force a re-arm
    // so a second swap of the same file is still detected, plus a delayed
    // one for network drives where the new file may not exist yet
    // (rationale documented in MediaWatchController).
    m_mediaWatch->forceRescan();
    m_mediaWatch->scheduleDelayedForceRescan(3000);
}

void TimelineWorkspace::refreshAfterUndoRedo()
{
    invalidateCompositeCache();
    // Undo/redo can add, remove, or retime transitions and audio clips, and
    // audio crossfades are baked into the mixed source.  schedulePostEditWork()
    // (called below) only *loads* missing sources — it won't rebuild an
    // already-cached mix — so drop the cache here or an undone/redone audio
    // cross-dissolve keeps playing its stale (pre-edit) mix.
    invalidateAudioSources();

    // Sync playhead from the Timeline model to the panel and playback
    // controller.  Paste/duplicate commands now move the model's playhead
    // as part of their undoable LambdaCommand, so Ctrl+Z restores the
    // pre-paste position.
    if (m_timelinePanel && m_timeline) {
        int64_t modelTick = m_timeline->playheadPosition();
        m_timelinePanel->setPlayheadPosition(modelTick);
        if (m_playbackController)
            m_playbackController->seekTo(modelTick);
    }

    // After undo/redo, clips may have been added or removed. Clear any
    // selected-clip / ShotPanel pointer that might now be dangling.
    if (m_selection.clip && m_timeline) {
        bool found = false;
        for (size_t ti = 0; ti < m_timeline->trackCount() && !found; ++ti) {
            auto* track = m_timeline->track(ti);
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (track->clip(ci) == m_selection.clip) { found = true; break; }
            }
        }
        if (!found) {
            m_selection.clip = nullptr;
            m_selection.graphicLayerIdx = -1;
            if (m_propertiesPanel) m_propertiesPanel->clearClip();
            if (m_effectControlsPanel) m_effectControlsPanel->clearClip();
        }
    }

    // Rebuild the timeline track widgets so split/merged clips are visible.
    if (m_timelinePanel) m_timelinePanel->rebuildTracks();

#ifdef ROUNDTABLE_HAS_SPINE
    // Purge spine cache entries for clip IDs that no longer exist on the
    // timeline (prevents use-after-free when compositing references a
    // deleted clip's spine state).
    if (m_compositeService && m_timeline) {
        std::unordered_set<uint64_t> liveIds;
        for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
            auto* track = m_timeline->track(ti);
            if (!track) continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* clip = track->clip(ci);
                liveIds.insert(clip->id());
                // Undo/redo mutates clip data directly; the cached spine
                // engine was configured at creation and would otherwise
                // keep playing the pre-undo animation/talk/speed even
                // though the clip (and its label) reverted.
                if (auto* sc = dynamic_cast<SpineClip*>(clip))
                    m_compositeService->resyncSpineClip(sc);
            }
        }
        m_compositeService->purgeDeadSpineStates(liveIds);
    }
#endif

    // If selected clip still exists, refresh property panels to reflect undo/redo
    if (m_selection.clip) {
        if (m_propertiesPanel) m_propertiesPanel->refreshEffects();
        if (m_effectControlsPanel) m_effectControlsPanel->refresh();
    } else if (m_effectControlsPanel && m_effectControlsPanel->clip()) {
        // The panel still has a clip bound (e.g. selection was transiently
        // cleared by rebuildTracks) — refresh it so keyframe display is current.
        m_effectControlsPanel->refresh();
    }

    updateTransformOverlay();
    if (m_programMonitor) {
        // requestRefresh() forces a re-composite of the current playhead
        // without wiping the displayed frame first — the new frame
        // replaces the old in a single present, so there is no blank
        // flicker.  resetViewState() used to be called here as a defence
        // against dock-layout corruption, but it calls clearFrame() on
        // both viewports which produces a one-frame black flash on every
        // undo/redo even when nothing on-screen actually changed.  The
        // real dock-layout recovery already happens in showEvent() when
        // the panel becomes visible.
        m_programMonitor->requestRefresh();
    }

    // Clips may have been added/removed/relinked — keep the live file-swap
    // watcher in sync with the timeline's current media set.
    m_mediaWatch->rescan();

    schedulePostEditWork();
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
