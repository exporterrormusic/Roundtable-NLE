/*
 * TimelineWorkspaceAudio.cpp — Thin delegation to AudioPlaybackService +
 * Qt-specific scheduling + video media pre-opening.
 *
 * The heavy audio decode/cache/prefetch logic now lives in
 * core/audio/AudioPlaybackService.  This file keeps only Qt-dependent
 * scheduling (QTimer::singleShot) and the video-media pre-opening that
 * is shared with the composite pipeline.
 */

#include "panels/timeline/TimelineWorkspace.h"
#include "PathUtils.h"

#include "CompositeService.h"
#include "audio/AudioPlaybackService.h"
#include "decode/VideoFrameMapping.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "panels/monitors/ProgramMonitor.h"  // for requestRefresh() after warmup
#include "panels/timeline/MediaWarmupController.h"
#include "panels/timeline/MediaWatchController.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/VideoClip.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "timeline/SpineClip.h"
#include "spine/AnimationVideoCache.h"
#endif

#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::cancelBackgroundMediaWarmups() noexcept
{
    m_mediaWarmup->cancel();
}

void TimelineWorkspace::cancelAndJoinBackgroundMediaWarmups() noexcept
{
    m_mediaWarmup->cancelAndJoin();
}

bool TimelineWorkspace::isBackgroundWarmupActive() const noexcept
{
    return m_mediaWarmup->isActive();
}

// ── Thin delegation wrappers ────────────────────────────────────────────────

void TimelineWorkspace::invalidateAudioSources()
{
    if (m_audioPlayback) m_audioPlayback->invalidateSources();
}

void TimelineWorkspace::loadAudioSources(bool allowBlockingMisses)
{
    if (m_audioPlayback) m_audioPlayback->loadSources(allowBlockingMisses);
}

void TimelineWorkspace::ensureAudioSourcesLoaded()
{
    if (m_audioPlayback) m_audioPlayback->ensureSourcesLoaded();
}

void TimelineWorkspace::warmAudioCacheAsync()
{
    if (m_audioPlayback) m_audioPlayback->warmCacheAsync();
}

void TimelineWorkspace::logTimelineAudioPerfSnapshot(const char* reason)
{
    if (m_audioPlayback) m_audioPlayback->logPerfSnapshot(reason);
}

// ── Qt-dependent scheduling (cannot live in core/) ──────────────────────────

void TimelineWorkspace::scheduleAudioPlaybackWindowRefresh()
{
    if (!m_audioPlayback || !m_audioPlayback->needsPlaybackWindowRefresh())
        return;
    if (m_audioWindowRefreshScheduled)
        return;

    m_audioWindowRefreshScheduled = true;
    m_audioPlayback->warmCacheAsync();
    QTimer::singleShot(0, this, [this]() {
        m_audioWindowRefreshScheduled = false;
        if (m_audioPlayback && m_audioPlayback->needsPlaybackWindowRefresh())
            m_audioPlayback->loadSources(/*allowBlockingMisses=*/false);
    });
}

// Coalesces audio-source reload and spine warm-up into a single deferred
// call on the next event-loop iteration so that split / delete / paste
// handlers return immediately without freezing the UI.
void TimelineWorkspace::schedulePostEditWork()
{
    if (m_postEditScheduled) return;
    m_postEditScheduled = true;

    QTimer::singleShot(0, this, [this]() {
        m_postEditScheduled = false;
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
#ifdef ROUNDTABLE_HAS_SPINE
        warmNewSpineClips();
#endif
        auto t1 = clk::now();
        ensureAudioSourcesLoaded();
        auto t2 = clk::now();
        // Re-arm the live file-swap watcher: a clip may have just been
        // added/relinked (e.g. media dragged onto the timeline), so its
        // source file isn't watched yet.  rescan() diffs against the
        // already-watched set, so this is cheap when nothing changed.
        m_mediaWatch->rescan();
        auto t3 = clk::now();
        auto ms = [](clk::time_point a, clk::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const double total = ms(t0, t3);
        if (total > 200.0)  // only surface genuine post-edit stalls, not normal edits
            spdlog::warn("[EDIT-PERF] schedulePostEditWork: warmSpine={:.1f}ms "
                         "audioSources={:.1f}ms rescanWatch={:.1f}ms TOTAL={:.1f}ms",
                         ms(t0, t1), ms(t1, t2), ms(t2, t3), total);
    });
}

// ── Video media pre-opening (MediaWarmupController) ─────────────────────────

void TimelineWorkspace::preOpenVideoMedia()
{
    m_mediaWarmup->start();
}

} // namespace rt
