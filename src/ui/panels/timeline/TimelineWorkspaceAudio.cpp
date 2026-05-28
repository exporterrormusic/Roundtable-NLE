/*
 * TimelineWorkspaceAudio.cpp â€” Thin delegation to AudioPlaybackService +
 * Qt-specific scheduling + video media pre-opening.
 *
 * The heavy audio decode/cache/prefetch logic now lives in
 * core/media/AudioPlaybackService.  This file keeps only Qt-dependent
 * scheduling (QTimer::singleShot) and the video-media pre-opening that
 * is shared with the composite pipeline.
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "media/AudioPlaybackService.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "panels/monitors/ProgramMonitor.h"  // for requestRefresh() after warmup

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
#include <QApplication>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <spdlog/spdlog.h>

namespace rt {

// â”€â”€ Thin delegation wrappers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

// â”€â”€ Qt-dependent scheduling (cannot live in core/) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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
#ifdef ROUNDTABLE_HAS_SPINE
        warmNewSpineClips();
#endif
        ensureAudioSourcesLoaded();
        // Re-arm the live file-swap watcher: a clip may have just been
        // added/relinked (e.g. media dragged onto the timeline), so its
        // source file isn't watched yet.  rescanMediaWatch() diffs against
        // the already-watched set, so this is cheap when nothing changed.
        rescanMediaWatch();
    });
}

// â”€â”€ Video media pre-opening (shared with composite pipeline) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void TimelineWorkspace::preOpenVideoMedia()
{
    if (!m_timeline || !m_mediaPool || !m_compositeService) return;

    // Collect (path, isCharacter) for every clip on the timeline that
    // references a media file:
    //   - VideoClip (video tracks) + SpineClip pre-rendered mp4/webm
    //     ("character" clips).  Cold first-frame on a character clip
    //     paid 100-170ms of NVDEC init on the UI thread historically.
    //   - ImageClip (still graphics on video tracks).  PNG opens via
    //     FFmpeg's image2 demuxer are quick individually but stack at
    //     clip boundaries when multiple stills enter the active region
    //     simultaneously.
    //   - AudioClip (audio tracks).  Cold AudioFile open + sndfile/
    //     ffmpeg probe at a clip boundary is the trigger for the
    //     FrameClock JUMP we tracked down in 2026-05-21.
    struct Entry {
        bool isCharacter{false};
        bool predecode{true};
        bool isVideoFile{false};   // video tracks: pre-decode-eligible
    };
    std::map<std::string, Entry> paths;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        auto* track = m_timeline->track(ti);
        if (!track) continue;
        const bool isVideoTrack = (track->type() == TrackType::Video);
        const bool isAudioTrack = (track->type() == TrackType::Audio);
        if (!isVideoTrack && !isAudioTrack) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip) continue;
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                const auto& path = videoClip->mediaPath();
                if (path.empty()) continue;
                auto& e = paths[path];
                if (videoClip->isVideoCharacter())
                    e.isCharacter = true;
                e.isVideoFile = true;
                continue;
            }
            if (auto* imageClip = dynamic_cast<ImageClip*>(clip)) {
                const auto& path = imageClip->mediaPath();
                if (path.empty()) continue;
                // Stills don't need loop-pre-decode or NVDEC, just an
                // open() to warm the MediaPool entry.
                paths[path];   // insert with default Entry (isVideoFile=false)
                continue;
            }
            if (auto* audioClip = dynamic_cast<AudioClip*>(clip)) {
                const auto& path = audioClip->mediaPath();
                if (path.empty()) continue;
                paths[path];   // open warms the AudioFile/sndfile probe
                continue;
            }
#ifdef ROUNDTABLE_HAS_SPINE
            if (auto* spineClip = dynamic_cast<SpineClip*>(clip)) {
                const auto* cache = m_compositeService->animVideoCache();
                if (!cache) continue;
                const std::string& chr    = spineClip->characterName();
                const std::string& outfit = spineClip->outfit();
                const std::string& anim   = spineClip->animationName();

                // Open BOTH mute and talk variants so the first
                // isTalking() toggle during playback doesn't pay a
                // synchronous MediaPool::open on the UI thread.
                //
                // But only full-loop pre-decode the variant this clip
                // is actually configured to use.  Pre-decoding every
                // talk variant as well doubles the cache footprint and,
                // once the cache is full, forces LRU to evict loop
                // frames of currently-playing clips — producing the
                // "previously-smooth animations now stutter" symptom.
                // The unused variant just sits at opened state; it'll
                // be prefetched on demand if talk is toggled mid-
                // playback (one-time ~80ms hit).
                const bool wantsTalk = spineClip->isTalking();
                const bool animIsAlreadyTalk =
                    (anim.size() >= 5 &&
                     anim.compare(anim.size() - 5, 5, "_talk") == 0);
                const std::string muteName = animIsAlreadyTalk
                    ? anim.substr(0, anim.size() - 5) : anim;
                const std::string talkName = animIsAlreadyTalk
                    ? anim : (anim + "_talk");
                auto setFor = [&](const std::string& animName, bool pred) {
                    const auto* entry = cache->getEntry(chr, outfit, animName);
                    if (!entry) return;
                    const std::string p = entry->videoPath.string();
                    if (p.empty()) return;
                    auto it = paths.find(p);
                    if (it == paths.end()) {
                        paths[p] = Entry{/*isCharacter=*/true, /*predecode=*/pred};
                    } else {
                        it->second.isCharacter = true;
                        // OR semantics: predecode if any referencing
                        // clip wants the warm cache.
                        if (pred) it->second.predecode = true;
                    }
                };
                setFor(muteName, /*pred=*/!wantsTalk);
                setFor(talkName, /*pred=*/ wantsTalk);
            }
#endif
        }
    }

    if (paths.empty()) return;

    // warn so the user can confirm the pre-open ran (their logger
    // filter is warn+).  Counts include audio + image clips since
    // the 2026-05-21 extension covered the clip-boundary lag cause.
    int videoCount = 0, audioImageCount = 0;
    for (const auto& [p, e] : paths) {
        if (e.isVideoFile) ++videoCount; else ++audioImageCount;
    }
    spdlog::warn("preOpenVideoMedia: pre-opening {} handle(s) ({} video, "
                 "{} audio/image) on background thread",
                 paths.size(), videoCount, audioImageCount);

    // Dispatch the actual open()+loop-pre-decode work to a detached
    // worker thread so the UI thread (and any pending compositeFrame
    // calls) are not blocked by FFmpeg probe + NVDEC init, which for a
    // modern H.264 packed-alpha character clip costs 100-170ms each.
    //
    // MediaPool::open is thread-safe and dedups by canonical path, so
    // subsequent UI-thread calls to open() for the same path will hit
    // the fast cache path (<1ms) once this background pass completes.
    //
    // We intentionally do NOT write to CompositeService::m_openMediaHandles
    // from this thread (it isn't locked).  The compositor's first access
    // on the UI thread will call m_mediaPool->open() which will then be
    // cheap, and the CompositeService map is populated there.
    auto* pool    = m_mediaPool;
    auto  pathMap = std::move(paths);

    // Track that a background warmup thread is active so playback can be
    // deferred until it completes.  This prevents use-after-free crashes
    // when the compositor evaluates clip keyframes while the timeline is
    // still being populated during project-open.
    m_backgroundWarmupActive.fetch_add(1, std::memory_order_release);

    std::thread([this, pool, pathMap = std::move(pathMap)]() {
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        int loopWarmCount = 0;
        int headWarmCount = 0;
        int opened = 0;
        for (const auto& [path, info] : pathMap) {
            if (!pool) break;
            uint64_t handle = pool->open(path);
            if (handle == 0) {
                spdlog::warn("preOpenVideoMedia(bg): failed to open '{}'", path);
                continue;
            }
            ++opened;
            const auto* mediaInfo = pool->getInfo(handle);
            if (!mediaInfo) continue;

            // Non-video files (stills / audio): the open() call above is
            // all we need.  Skip loop-pre-decode / prefetch (those are
            // video-only paths that would waste work on stills and don't
            // apply to AudioClip media).
            if (!info.isVideoFile) continue;

            if (info.isCharacter) {
                // Cap check removed — startLoopPreDecode handles it
                // internally AND sets the loop wrap-around flag for
                // over-cap clips (Wells's 880-frame ProRes loop) so
                // the prefetch can pre-fetch frame 0 at the loop
                // boundary.
                if (info.predecode && mediaInfo->frameCount > 1) {
                    pool->startLoopPreDecode(handle, ResolutionTier::Half);
                    ++loopWarmCount;
                }
            } else {
                if (mediaInfo->frameCount > 1 &&
                    mediaInfo->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                    pool->startLoopPreDecode(handle, ResolutionTier::Full);
                    ++loopWarmCount;
                } else if (mediaInfo->frameCount > 1) {
                    pool->schedulePrefetch(handle, /*afterFrame=*/0,
                                           /*count=*/60,
                                           /*urgent=*/false,
                                           ResolutionTier::Full);
                    ++headWarmCount;
                }
            }
        }
        auto ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        spdlog::warn("preOpenVideoMedia(bg): opened={} loopWarm={} headWarm={} in {:.0f}ms",
                     opened, loopWarmCount, headWarmCount, ms);

        // Signal completion on the UI thread so isBackgroundWarmupActive()
        // returns false and playback can proceed.  Also re-kick the Program
        // Monitor so the just-warmed media actually paints: the 100ms
        // post-setTimeline requestRefresh fires a 15-cycle (~240ms) settle
        // window that typically expires before this background open
        // completes (NVDEC init + FFmpeg probe is 100-170ms per character
        // clip), so the cold-start composite returned nothing and the
        // tick-dedup gate then froze the monitor blank until the user
        // scrubbed or played.  A second requestRefresh here resets the
        // settle window with the cache warm, so the saved-playhead frame
        // composites for real on the next poll tick.
        QMetaObject::invokeMethod(qApp, [this]() {
            m_backgroundWarmupActive.fetch_sub(1, std::memory_order_release);
            if (m_programMonitor) m_programMonitor->requestRefresh();
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace rt
