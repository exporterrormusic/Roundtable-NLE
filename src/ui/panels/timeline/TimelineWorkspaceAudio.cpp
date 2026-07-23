/*
 * TimelineWorkspaceAudio.cpp â€” Thin delegation to AudioPlaybackService +
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
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "panels/monitors/ProgramMonitor.h"  // for requestRefresh() after warmup
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
#include <QApplication>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <vector>
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

// â”€â”€ Video media pre-opening (shared with composite pipeline) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

int TimelineWorkspace::migrateDeferredLegacyMediaMasks()
{
    if (!m_timeline || !m_mediaPool) return 0;
    const auto resolution = m_timeline->settings().resolution();
    if (resolution.width == 0 || resolution.height == 0) return 0;

    int migrated = 0;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        Track* track = m_timeline->track(ti);
        if (!track) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            Clip* clip = track->clip(ci);
            if (!clip) continue;

            if (auto* video = dynamic_cast<VideoClip*>(clip)) {
                if (video->mediaPath().empty()) continue;
                const MediaHandle handle = m_mediaPool->open(video->mediaPath());
                if (handle == InvalidMedia) continue;
                const VideoStreamInfo* info = m_mediaPool->getInfo(handle);
                if (info && info->width > 0 && info->height > 0) {
                    video->setSourceMetadata(
                        info->width, info->height, info->rotation);
                    migrated += video->migrateLegacyMasksToSourceLocal(
                        resolution.width, resolution.height,
                        info->width, info->height, info->rotation);
                }
                // Balance this metadata lookup. The warmup's own open keeps
                // its shared decoder alive for playback.
                m_mediaPool->release(handle);
                continue;
            }

            if (auto* image = dynamic_cast<ImageClip*>(clip)) {
                uint32_t sourceW = image->sourceWidth();
                uint32_t sourceH = image->sourceHeight();
                MediaHandle handle = InvalidMedia;
                if ((sourceW == 0 || sourceH == 0) &&
                    !image->mediaPath().empty()) {
                    handle = m_mediaPool->open(image->mediaPath());
                    if (handle != InvalidMedia) {
                        const VideoStreamInfo* info = m_mediaPool->getInfo(handle);
                        if (info && info->width > 0 && info->height > 0) {
                            sourceW = info->width;
                            sourceH = info->height;
                            image->setSourceResolution(sourceW, sourceH);
                        }
                    }
                }
                if (sourceW > 0 && sourceH > 0)
                    migrated += image->migrateLegacyMasksToSourceLocal(
                        resolution.width, resolution.height,
                        sourceW, sourceH);
                if (handle != InvalidMedia) m_mediaPool->release(handle);
            }
        }
    }

    if (migrated > 0) {
        spdlog::info(
            "Migrated {} deferred legacy media mask(s) after metadata warmup",
            migrated);
        invalidateCompositeCache();
    }
    return migrated;
}

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
                    const std::string p = pathToUtf8(entry->videoPath);
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
    auto* pool = m_mediaPool;

    // Track that a background warmup batch is active so playback can be
    // deferred until it completes.  This prevents use-after-free crashes
    // when the compositor evaluates clip keyframes while the timeline is
    // still being populated during project-open.  One increment for the whole
    // batch (regardless of worker count); the LAST worker out decrements it.
    m_backgroundWarmupActive.fetch_add(1, std::memory_order_release);

    // Flatten the work so several worker threads can pull entries in parallel.
    // The dominant per-file cost is the FFmpeg probe + NVDEC init (100-300ms)
    // and it was previously paid SEQUENTIALLY for every clip on one thread —
    // the single biggest contributor to slow project opens.  open(),
    // startLoopPreDecode() and schedulePrefetch() are all internally locked
    // (m_mutex / m_prefetchMutex), so concurrent calls are safe; and the heavy
    // decode/convert still runs on MediaPool's dedicated prefetch/loop workers
    // (we only enqueue here), so no GPU work happens on these threads.
    auto work = std::make_shared<std::vector<std::pair<std::string, Entry>>>();
    work->reserve(paths.size());
    for (auto& kv : paths) work->emplace_back(kv.first, kv.second);

    const int total           = static_cast<int>(work->size());
    auto workIndex            = std::make_shared<std::atomic<int>>(0);
    auto processed            = std::make_shared<std::atomic<int>>(0);
    auto startTime            = std::make_shared<std::chrono::steady_clock::time_point>(
                                    std::chrono::steady_clock::now());

    unsigned hw = std::thread::hardware_concurrency();
    int nThreads = static_cast<int>(hw ? std::min(hw, 6u) : 4u);
    nThreads = std::max(2, std::min(nThreads, total));
    auto threadsRemaining = std::make_shared<std::atomic<int>>(nThreads);

    spdlog::warn("preOpenVideoMedia: warming {} handle(s) across {} thread(s)",
                 total, nThreads);

    for (int ti = 0; ti < nThreads; ++ti) {
        std::thread([this, pool, work, workIndex, processed,
                     threadsRemaining, startTime, total, nThreads]() {
            for (;;) {
                int idx = workIndex->fetch_add(1, std::memory_order_relaxed);
                if (idx >= total) break;
                const auto& path = (*work)[idx].first;
                const auto& info = (*work)[idx].second;

                if (pool) {
                    uint64_t handle = pool->open(path);
                    if (handle == 0) {
                        spdlog::warn("preOpenVideoMedia(bg): failed to open '{}'", path);
                    } else if (info.isVideoFile) {
                        // Non-video files (stills / audio) only need open().
                        const auto* mediaInfo = pool->getInfo(handle);
                        if (mediaInfo) {
                            if (info.isCharacter) {
                                if (info.predecode && mediaInfo->frameCount > 1)
                                    pool->startLoopPreDecode(handle, ResolutionTier::Half);
                            } else if (mediaInfo->frameCount > 1 &&
                                       mediaInfo->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                                pool->startLoopPreDecode(handle, ResolutionTier::Full);
                            } else if (mediaInfo->frameCount > 1) {
                                pool->schedulePrefetch(handle, /*afterFrame=*/0,
                                                       /*count=*/60, /*urgent=*/false,
                                                       ResolutionTier::Full);
                            }
                        }
                    }
                }

                const int done = processed->fetch_add(1, std::memory_order_acq_rel) + 1;
                QMetaObject::invokeMethod(qApp, [this, done, total]() {
                    emit backgroundWarmupProgress(done, total);
                }, Qt::QueuedConnection);
            }

            // The last worker to finish finalizes the batch on the UI thread.
            if (threadsRemaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                auto ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - *startTime).count();
                spdlog::warn("preOpenVideoMedia(bg): warmed {} handle(s) on {} thread(s) in {:.0f}ms",
                             total, nThreads, ms);
                // Re-kick the Program Monitor so the just-warmed media paints,
                // decrement the batch counter, and signal MainWindow to drop
                // the project-loading input lock.  Posted to qApp so it runs
                // on the UI thread where these are safe to touch.
                QMetaObject::invokeMethod(qApp, [this]() {
                    migrateDeferredLegacyMediaMasks();
                    const int remaining =
                        m_backgroundWarmupActive.fetch_sub(1, std::memory_order_release) - 1;
                    if (m_programMonitor) m_programMonitor->requestRefresh();
                    if (remaining <= 0)
                        emit backgroundWarmupFinished();
                }, Qt::QueuedConnection);
            }
        }).detach();
    }
}

} // namespace rt
