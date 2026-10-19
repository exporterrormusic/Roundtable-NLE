/*
 * MediaWarmupController.cpp — background pre-opening of the timeline's video
 * and image media (see MediaWarmupController.h).
 */

#include "panels/timeline/MediaWarmupController.h"
#include "threading/WorkerThread.h"

#include "decode/VideoFrameMapping.h"
#include "playback/MediaPool.h"
#include "timeline/ImageClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <spdlog/spdlog.h>

namespace rt {

struct MediaWarmupController::Batch {
    Batch(uint64_t generationValue, int workerCount)
        : generation(generationValue)
        , workersRemaining(workerCount)
    {}

    uint64_t generation{0};
    std::atomic<bool> finished{false};
    std::atomic<int> workersRemaining{0};
    std::vector<WorkerThread> workers;
};

void MediaWarmupController::cancel() noexcept
{
    m_generation.fetch_add(1, std::memory_order_acq_rel);
    for (const auto& batch : m_batches) {
        for (auto& worker : batch->workers)
            worker.requestStop();
    }
}

void MediaWarmupController::cancelAndJoin() noexcept
{
    cancel();
    for (const auto& batch : m_batches) {
        for (auto& worker : batch->workers)
            worker.stop();
    }
    m_batches.clear();
    if (m_cfg.destroying())
        m_active.store(0, std::memory_order_release);
}

void MediaWarmupController::reapFinished()
{
    std::erase_if(m_batches, [](const auto& batch) {
        return batch->finished.load(std::memory_order_acquire);
    });
}

int MediaWarmupController::migrateDeferredLegacyMediaMasks()
{
    Timeline* timeline = m_cfg.timeline();
    MediaPool* mediaPool = m_cfg.mediaPool();
    if (!timeline || !mediaPool) return 0;
    const auto resolution = timeline->settings().resolution();
    if (resolution.width == 0 || resolution.height == 0) return 0;

    int migrated = 0;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* track = timeline->track(ti);
        if (!track) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            Clip* clip = track->clip(ci);
            if (!clip) continue;

            if (auto* video = dynamic_cast<VideoClip*>(clip)) {
                if (video->mediaPath().empty()) continue;
                const MediaHandle handle = mediaPool->open(video->mediaPath());
                if (handle == InvalidMedia) continue;
                const VideoStreamInfo* info = mediaPool->getInfo(handle);
                if (info && info->width > 0 && info->height > 0) {
                    video->setSourceMetadata(
                        info->width, info->height, info->rotation);
                    migrated += video->migrateLegacyMasksToSourceLocal(
                        resolution.width, resolution.height,
                        info->width, info->height, info->rotation);
                }
                // Balance this metadata lookup. The warmup's own open keeps
                // its shared decoder alive for playback.
                mediaPool->release(handle);
                continue;
            }

            if (auto* image = dynamic_cast<ImageClip*>(clip)) {
                uint32_t sourceW = image->sourceWidth();
                uint32_t sourceH = image->sourceHeight();
                MediaHandle handle = InvalidMedia;
                if ((sourceW == 0 || sourceH == 0) &&
                    !image->mediaPath().empty()) {
                    handle = mediaPool->open(image->mediaPath());
                    if (handle != InvalidMedia) {
                        const VideoStreamInfo* info = mediaPool->getInfo(handle);
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
                if (handle != InvalidMedia) mediaPool->release(handle);
            }
        }
    }

    if (migrated > 0) {
        spdlog::info(
            "Migrated {} deferred legacy media mask(s) after metadata warmup",
            migrated);
        m_cfg.invalidateCompositeCache();
    }
    return migrated;
}

void MediaWarmupController::start()
{
    Timeline* timeline = m_cfg.timeline();
    MediaPool* mediaPool = m_cfg.mediaPool();
    // A new warmup supersedes work collected from an earlier timeline/edit.
    // Cancellation is non-blocking here; completed batches are reaped without
    // keeping their worker thread objects around indefinitely.
    cancel();
    reapFinished();
    if (!timeline || !mediaPool || !m_cfg.canWarm()) return;
    const uint64_t generation =
        m_generation.load(std::memory_order_acquire);

    // Collect (path, isCharacter) for every clip on the timeline that
    // references a media file:
    //   - VideoClip (video tracks). Cold first-frame on a video character
    //     paid 100-170ms of NVDEC init on the UI thread historically.
    //   - ImageClip (still graphics on video tracks).  PNG opens via
    //     FFmpeg's image2 demuxer are quick individually but stack at
    //     clip boundaries when multiple stills enter the active region
    //     simultaneously.
    // Audio is deliberately absent: warmAudioCacheAsync owns a bounded
    // playhead window. Whole-sequence probing made long projects wait for
    // hundreds of distant dialogue files before becoming interactive.
    struct Entry {
        bool isCharacter{false};
        bool predecode{true};
        bool isVideoFile{false};   // video tracks: pre-decode-eligible
        // Timeline distance from the playhead to this file's nearest use
        // (0 = under the playhead). Files are warmed nearest-first.
        int64_t distance{INT64_MAX};
        // Source frame shown at that nearest use. Mapped here without stream
        // info, so a clip with no stored fps used the 24 fps fallback and the
        // worker rescales it to the file's real fps once open.
        int64_t firstFrame{0};
        bool mappedAt24Fallback{false};
        // Loop pre-decode priority on the compositor prewarm's scale (the
        // tick playback reaches the clip; lower runs sooner).
        int64_t loopPriority{0};
    };
    // The model playhead is the restored project position at open; the
    // playback controller has not been seeked to it yet.
    const int64_t playhead = timeline->playheadPosition();
    std::map<std::string, Entry> paths;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        auto* track = timeline->track(ti);
        if (!track) continue;
        const bool isVideoTrack = (track->type() == TrackType::Video);
        const bool isAudioTrack = (track->type() == TrackType::Audio);
        if (!isVideoTrack && !isAudioTrack) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip) continue;
            // Playback runs forward, so a clip behind the playhead counts
            // double the distance of one the same span ahead.
            int64_t useTick = playhead;
            int64_t distance = 0;
            if (playhead < clip->timelineIn()) {
                useTick = clip->timelineIn();
                distance = useTick - playhead;
            } else if (playhead >= clip->timelineOut()) {
                useTick = clip->timelineIn();
                distance = 2 * (playhead - clip->timelineOut() + 1);
            }
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                const auto& path = videoClip->mediaPath();
                if (path.empty()) continue;
                auto& e = paths[path];
                if (videoClip->isVideoCharacter())
                    e.isCharacter = true;
                e.isVideoFile = true;
                if (distance < e.distance) {
                    const auto mapped =
                        mapTickToSourceFrame(*videoClip, useTick, nullptr);
                    e.distance = distance;
                    e.loopPriority = playhead + distance;
                    e.firstFrame = mapped.frame;
                    e.mappedAt24Fallback = videoClip->sourceFps() <= 0.0;
                }
                continue;
            }
            if (auto* imageClip = dynamic_cast<ImageClip*>(clip)) {
                const auto& path = imageClip->mediaPath();
                if (path.empty()) continue;
                // Stills don't need loop-pre-decode or NVDEC, just an
                // open() to warm the MediaPool entry.
                auto& e = paths[path];   // default Entry (isVideoFile=false)
                e.distance = std::min(e.distance, distance);
                continue;
            }
            // Audio owns a playhead-window warmup path (warmAudioCacheAsync).
            // Opening every distinct audio file in a long sequence here kept
            // the project-loading overlay up for archival media far from the
            // current playhead and duplicated that subsystem's work.
        }
    }

    if (paths.empty()) return;

    // warn so the user can confirm the pre-open ran (their logger
    // filter is warn+).
    int videoCount = 0, imageCount = 0;
    for (const auto& [p, e] : paths) {
        if (e.isVideoFile) ++videoCount; else ++imageCount;
    }
    spdlog::warn("preOpenVideoMedia: pre-opening {} handle(s) ({} video, "
                 "{} image) on background thread",
                 paths.size(), videoCount, imageCount);

    // Dispatch the actual open()+loop-pre-decode work to owned workers so the
    // UI thread (and any pending compositeFrame
    // calls) are not blocked by FFmpeg probe + NVDEC init, which for a
    // modern H.264 packed-alpha character clip costs 100-170ms each.
    //
    // MediaPool::open is thread-safe and dedups by canonical path, so
    // subsequent UI-thread calls to open() for the same path will hit
    // the fast cache path (<1ms) once this background pass completes.
    //
    // We intentionally do NOT write to CompositeService::m_openMediaHandles
    // from this thread (it isn't locked).  The compositor's first access
    // on the UI thread will call mediaPool->open() which will then be
    // cheap, and the CompositeService map is populated there.
    auto* pool = mediaPool;

    // Track that a background warmup batch is active so playback can be
    // deferred until it completes.  This prevents use-after-free crashes
    // when the compositor evaluates clip keyframes while the timeline is
    // still being populated during project-open.  One increment for the whole
    // batch (regardless of worker count); the LAST worker out decrements it.
    m_active.fetch_add(1, std::memory_order_release);

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
    // Workers pull in index order, so the clips under the playhead open (and
    // queue their prefetch) first instead of whichever path sorts first.
    // stable_sort keeps path order between equally distant files.
    std::stable_sort(work->begin(), work->end(),
                     [](const auto& a, const auto& b) {
                         return a.second.distance < b.second.distance;
                     });

    // The frame the nearest use shows, in the opened file's frame numbers
    // (unclamped: loop pre-decode wraps it, prefetch clamps it).
    const auto startFrameFor = [](const Entry& e, const VideoStreamInfo& media) {
        if (!e.mappedAt24Fallback || media.fps <= 0.0) return e.firstFrame;
        return static_cast<int64_t>(std::llround(
            static_cast<double>(e.firstFrame) * media.fps / 24.0));
    };

    // Stills near the playhead are decoded too: an undecoded still costs
    // 60-150 ms (PNG/4K) the first time a composite shows it (perf_log
    // 2026-10-12: 'INTRO 2' at tick 0, 62 ms). Bounded, since a decoded 4K
    // still holds ~33 MB.
    constexpr int kMaxWarmStills = 8;
    const int64_t stillWindow = secondsToTicks(10.0);
    auto stillsWarmed = std::make_shared<std::atomic<int>>(0);

    const int total           = static_cast<int>(work->size());
    auto workIndex            = std::make_shared<std::atomic<int>>(0);
    auto processed            = std::make_shared<std::atomic<int>>(0);
    auto startTime            = std::make_shared<std::chrono::steady_clock::time_point>(
                                    std::chrono::steady_clock::now());

    unsigned hw = std::thread::hardware_concurrency();
    int nThreads = static_cast<int>(hw ? std::min(hw, 6u) : 4u);
    nThreads = std::max(2, std::min(nThreads, total));
    auto batch = std::make_shared<Batch>(generation, nThreads);
    auto* batchPtr = batch.get();
    batch->workers.reserve(static_cast<size_t>(nThreads));
    m_batches.push_back(batch);

    spdlog::warn("preOpenVideoMedia: warming {} handle(s) across {} thread(s)",
                 total, nThreads);

    for (int ti = 0; ti < nThreads; ++ti) {
        WorkerThread worker;
        worker.start("Media warm-up " + std::to_string(ti + 1),
            [this, pool, work, workIndex, processed, batchPtr, generation,
             startTime, total, nThreads, startFrameFor, stillWindow,
             stillsWarmed](std::stop_token stop) {
            for (;;) {
                if (stop.stop_requested()) break;
                int idx = workIndex->fetch_add(1, std::memory_order_relaxed);
                if (idx >= total) break;
                const auto& path = (*work)[idx].first;
                const auto& info = (*work)[idx].second;

                if (pool) {
                    uint64_t handle = pool->open(path);
                    const auto* mediaInfo =
                        handle != 0 ? pool->getInfo(handle) : nullptr;
                    if (handle == 0) {
                        spdlog::warn("preOpenVideoMedia(bg): failed to open '{}'", path);
                    } else if (mediaInfo && mediaInfo->frameCount <= 1) {
                        // A still (ImageClip, or a single-frame file on a
                        // video track) only needs open() unless it is near
                        // the playhead. The compositor reads stills at frame 0.
                        if (info.distance <= stillWindow
                            && stillsWarmed->fetch_add(1, std::memory_order_relaxed)
                                   < kMaxWarmStills) {
                            pool->schedulePrefetch(handle, /*afterFrame=*/0,
                                                   /*count=*/1, /*urgent=*/false,
                                                   ResolutionTier::Full);
                        }
                    } else if (info.isVideoFile) {
                        if (mediaInfo) {
                            if (info.isCharacter) {
                                if (info.predecode && mediaInfo->frameCount > 1)
                                    pool->startLoopPreDecode(
                                        handle, ResolutionTier::Half,
                                        info.loopPriority,
                                        startFrameFor(info, *mediaInfo));
                            } else if (mediaInfo->frameCount > 1) {
                                // Ordinary videos get a short prefetch only.  Full-res
                                // loop pre-decode of every short clip thrashed the 1 GB
                                // RAM cache (~125 frames at 1888x1088), forced CPU
                                // converts over GPU zero-copy, and resumed on every
                                // pause — jittery playback (perf_log 2026-09-26).
                                // It starts at the frame the nearest use shows (the
                                // playhead frame, or a trimmed clip's in point), not
                                // the file's frame 0.
                                const int64_t startFrame = std::clamp(
                                    startFrameFor(info, *mediaInfo),
                                    int64_t{0}, mediaInfo->frameCount - 1);
                                pool->schedulePrefetch(handle, startFrame,
                                                       /*count=*/60, /*urgent=*/false,
                                                       ResolutionTier::Full);
                            }
                        }
                    }
                }

                const int done = processed->fetch_add(1, std::memory_order_acq_rel) + 1;
                QMetaObject::invokeMethod(this, [this, generation, done, total]() {
                    if (m_cfg.destroying()
                        || generation != m_generation.load(
                            std::memory_order_acquire)) {
                        return;
                    }
                    m_cfg.progress(done, total);
                }, Qt::QueuedConnection);
            }

            // The last worker to finish finalizes the batch on the UI thread.
            if (batchPtr->workersRemaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                auto ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - *startTime).count();
                const int completed = processed->load(std::memory_order_acquire);
                spdlog::warn("preOpenVideoMedia(bg): warmed {} of {} handle(s) on {} thread(s) in {:.0f}ms",
                             completed, total, nThreads, ms);
                // Re-kick the Program Monitor so the just-warmed media paints,
                // decrement the batch counter, and signal MainWindow to drop
                // the project-loading input lock. The workspace is the Qt
                // receiver, so Qt discards this if destruction has begun.
                QMetaObject::invokeMethod(this, [this, generation]() {
                    const int remaining =
                        m_active.fetch_sub(1, std::memory_order_release) - 1;
                    const bool current =
                        !m_cfg.destroying()
                        && generation == m_generation.load(
                            std::memory_order_acquire);
                    if (current) {
                        migrateDeferredLegacyMediaMasks();
                        m_cfg.warmed();
                    }
                    if (!m_cfg.destroying()
                        && remaining <= 0) {
                        m_cfg.finished();
                    }
                }, Qt::QueuedConnection);
                batchPtr->finished.store(true, std::memory_order_release);
            }
        });
        batch->workers.push_back(std::move(worker));
    }
}

} // namespace rt
