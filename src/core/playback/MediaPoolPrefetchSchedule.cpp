/*
 * MediaPoolPrefetchSchedule.cpp — Prefetch scheduling and worker logic
 * extracted from MediaPoolPrefetch.cpp.
 *
 * Contains: schedulePrefetch(), prefetchWorker().
 */

#include "MediaPool.h"
#include "MediaPoolPrefetchInternal.h"
#include "MediaPoolPrefetchGpu.h"
#include "GpuContext.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <chrono>
#include <thread>

#include <volk.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

#include <algorithm>
#include <unordered_map>

namespace rt {

// UPGRADE_PLAN Phase 1 — per-prefetch-worker Vulkan state.
// WorkerGpuState is defined in MediaPoolPrefetchGpu.h so it can be shared
// with the convertDecodedToCacheGpu translation unit (Phase 4).

void MediaPool::scheduleExactPrefetch(MediaHandle handle,
                                      int64_t frameNumber,
                                      ResolutionTier tier)
{
    std::filesystem::path path;
    VideoStreamInfo info;
    bool packed = false;
    bool isLooping = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry) return;
        info = entry->info;
        path = entry->path;
        packed = entry->packedAlpha;
        isLooping = entry->loopPreDecodeStarted;
    }

    if (info.frameCount <= 1) {
        frameNumber = 0;
    } else {
        frameNumber = std::clamp(frameNumber, int64_t(0),
                                 info.frameCount - 1);
    }
    if (m_cache->contains(handle, frameNumber, tier)) return;

    m_scheduler.setPlayhead(frameNumber);

    PrefetchTask task;
    task.handle = handle;
    task.filePath = std::move(path);
    task.frameNumber = frameNumber;
    task.tier = tier;
    task.fps = info.fps > 0.0 ? info.fps : 30.0;
    task.info = info;
    task.packedAlpha = packed;
    task.urgent = true;
    task.exactRequest = true;
    task.isLoop = isLooping;

    bool inserted = false;
    {
        std::lock_guard lock(m_prefetchMutex);
        m_handlePlayheadFrame[handle] = task.frameNumber;
        inserted = prefetch_detail::mergeExactTask(m_prefetchQueue,
                                                    std::move(task));
    }
    if (inserted) {
        m_perf.prefetchScheduled.fetch_add(1, std::memory_order_relaxed);
    }
    m_prefetchCv.notify_all();
}

int MediaPool::decodeAheadStride(double sourceFps) const
{
    const double projFps = m_projectFps.load(std::memory_order_relaxed);
    // Source-fps stride: a 60 fps clip in a 30 fps project only needs every
    // 2nd source frame.
    const int srcStride = (sourceFps > projFps * 1.4)
        ? std::max(1, static_cast<int>(std::round(sourceFps / projFps)))
        : 1;
    // Playback-speed stride: at N× the playhead advances ~N source frames
    // per displayed frame, so decode ONLY those — never the intermediate
    // frames the player skips over.  This is what lets heavy videos keep
    // up at 4× (decode load stays ~1× instead of 4×).  No effect at 1×.
    const double absSpeed = std::abs(m_playbackSpeed.load(std::memory_order_relaxed));
    const int speedStride = (absSpeed > 1.4)
        ? std::max(1, static_cast<int>(std::lround(absSpeed)))
        : 1;
    return std::max(1, srcStride * speedStride);
}

void MediaPool::topUpDecodeAhead(MediaHandle handle, int64_t frameNumber,
                                 ResolutionTier tier, double sourceFps,
                                 int64_t frameCount)
{
    // Exact hits used to schedule nothing, so steady playback drained the
    // decode-ahead window and only refilled it after a frame had already
    // missed: bursts of decoding, an idle worker, then a stutter. Keep the
    // window at least half full instead.
    const int stride = decodeAheadStride(sourceFps);
    const int window = m_scheduler.maxLookahead();
    const int64_t probe = frameNumber + static_cast<int64_t>(std::max(2, window / 2)) * stride;
    if (frameCount > 1 && probe >= frameCount)
        return;   // the file ends inside the window; nothing left to fetch
    if (m_cache->contains(handle, probe, tier))
        return;
    schedulePrefetch(handle, frameNumber + stride, window, /*urgent=*/false, tier);
}

void MediaPool::schedulePrefetch(MediaHandle handle, int64_t afterFrame, int count, bool urgent, ResolutionTier tier)
{
    const bool interactivePlayback = urgent && isInteractivePlaybackActive(m_interactivePlaybackUntilMs);

    // Cooldown: skip queue rebuild if we recently scheduled for this handle.
    {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_prefetchMutex);
        m_handlePlayheadFrame[handle] = afterFrame;
        auto it = m_lastScheduleTime.find(handle);
        if (it != m_lastScheduleTime.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
            const int cooldownMs = interactivePlayback ? 120 : 40;
            if (elapsed.count() < cooldownMs) {
                m_prefetchCv.notify_all();
                return;
            }
        }
        m_lastScheduleTime[handle] = now;
    }

    std::filesystem::path path;
    VideoStreamInfo info;
    bool packed = false;
    bool isLooping = false;
    {
        std::lock_guard lock(m_mutex);
        auto* entry = findEntry(handle);
        if (!entry) return;
        info = entry->info;
        path = entry->path;
        packed = entry->packedAlpha;
        isLooping = entry->loopPreDecodeStarted;
    }
    const int64_t maxFrame = info.frameCount > 0 ? info.frameCount - 1 : INT64_MAX;
    const int64_t fc = info.frameCount;

    if (info.frameCount <= 1) {
        if (m_cache->contains(handle, 0, tier))
            return;
        count = 1;
        // CONSUMERS (CompositeServiceLayerBuild, …) always
        // look up still images at frame 0.  If we left afterFrame as-is
        // (e.g. 20 because the timeline playhead is on tick 20), the task
        // below sets task.frameNumber = afterFrame, the decoded result is
        // stored under (handle, 20, tier), and every subsequent consumer
        // lookup at frame 0 misses the cache and triggers another full
        // PNG software-decode (~150 ms for a 4K still).  Force-align the
        // prefetch index to 0 so the producer and consumer agree.
        afterFrame = 0;
    }

    // Skip if loop pre-decode already finished for this handle.
    {
        std::lock_guard lock(m_loopPreDecodeMutex);
        if (m_loopPreDecodeDone.count(handle)) {
            return;
        }
    }

    {
        std::lock_guard lock(m_prefetchMutex);

        // Frame numbers only compare within one file, so every queued task
        // is measured against its OWN file's last requested frame.
        const auto filePlayhead = [this](MediaHandle h) {
            const auto it = m_handlePlayheadFrame.find(h);
            return it != m_handlePlayheadFrame.end() ? it->second
                                                     : INT64_MIN + 4;
        };

        // Remove tasks for this handle + prune each file's stale frames
        // behind that file's own playhead.
        m_prefetchQueue.erase(
            std::remove_if(m_prefetchQueue.begin(), m_prefetchQueue.end(),
                [&](const PrefetchTask& t) {
                    return prefetch_detail::replaceableByDecodeAheadRebuild(
                        t, handle, filePlayhead(t.handle));
                }),
            m_prefetchQueue.end());

        const size_t kMaxQueueSize = interactivePlayback ? 32u : 64u;
        while (m_prefetchQueue.size() > kMaxQueueSize) {
            const auto replaceable = std::find_if(
                m_prefetchQueue.rbegin(), m_prefetchQueue.rend(),
                [](const PrefetchTask& task) { return !task.exactRequest; });
            if (replaceable == m_prefetchQueue.rend()) break;
            m_prefetchQueue.erase(std::next(replaceable).base());
        }

        const int stride = decodeAheadStride(info.fps);
        // Bound the lookahead window in DISPLAYED frames, not source frames, so
        // a strided prefetch still buffers ~maxLookahead frames ahead (without
        // this, stride N would shrink the buffer to maxLookahead/N → stutter).
        const int64_t lookaheadWindow =
            static_cast<int64_t>(m_scheduler.maxLookahead()) * stride;
        const int effectiveCount = interactivePlayback ? std::min(count, 16) : count;
        // Update scheduler playhead for lookahead bounding.
        m_scheduler.setPlayhead(afterFrame);
        for (int i = 0; i < effectiveCount; ++i) {
            int64_t fn = afterFrame + i * stride;

            if (fn > maxFrame) {
                if (isLooping && fc > 1) {
                    fn = ((fn % fc) + fc) % fc;
                } else {
                    break;
                }
            }

            if (m_cache->contains(handle, fn, tier))
                continue;

            // Lookahead gate (stride-scaled): skip frames outside the bounded
            // displayed-frame window (unless this is the urgent target frame).
            if ((fn - afterFrame) > lookaheadWindow && !(urgent && i == 0)) {
                if (i > 0) break;
                continue;
            }

            PrefetchTask task;
            task.handle      = handle;
            task.filePath    = path;
            task.frameNumber = fn;
            task.tier        = tier;
            task.fps         = info.fps > 0 ? info.fps : 30.0;
            task.info        = info;
            task.packedAlpha = packed;
            task.urgent      = (urgent && i == 0);
            task.isLoop      = isLooping;
            // A protected exact endpoint may already occupy this key. Do not
            // add a strided lookahead duplicate behind it.
            if (prefetch_detail::containsTaskKey(m_prefetchQueue, task))
                continue;
            if (m_prefetchQueue.size() >=
                prefetch_detail::kMaxExactQueueDepth) {
                continue;
            }
            if (task.urgent) {
                m_prefetchQueue.push_front(task);
            } else {
                // Interleave files by how far each frame is from its own
                // file's playhead, so every visible clip's next frame is
                // decoded before any clip's far lookahead.
                const int64_t dist = prefetch_detail::frameDistanceFromPlayhead(
                    fn, afterFrame);
                auto it = m_prefetchQueue.begin();
                while (it != m_prefetchQueue.end() && it->urgent) ++it;
                while (it != m_prefetchQueue.end()) {
                    const int64_t existDist =
                        prefetch_detail::frameDistanceFromPlayhead(
                            it->frameNumber, filePlayhead(it->handle));
                    if (existDist > dist) break;
                    ++it;
                }
                m_prefetchQueue.insert(it, task);
            }
            m_perf.prefetchScheduled.fetch_add(1, std::memory_order_relaxed);
        }
    }
    m_prefetchCv.notify_all();
}

void MediaPool::prefetchWorker(int workerId)
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    spdlog::info("MediaPool: prefetch worker {} started (ABOVE_NORMAL priority)", workerId);

    // Worker-pool partition point: ids [0..nvdecWorkers) are the NVDEC-
    // eligible workers, the rest are software-decode.  Snapshot once per
    // worker so the partition is stable for this thread's lifetime.
    // (Was the PREFETCH_NVDEC_WORKERS constant; now machine-adaptive via
    // PerformanceProfile — see the retired BOOST_MODE_PLAN doc in git history.)
    const int nvdecWorkers = perfProfile().nvdecWorkers;

    // UPGRADE_PLAN: per-worker Vulkan state.  MediaPool is constructed
    // BEFORE GpuContext::init runs in the typical App startup, so this
    // first attempt usually fails (GpuContext not yet initialised).
    // The worker re-tries lazily at the top of each loop iteration
    // until init succeeds, after which the GPU-resident decode path
    // becomes eligible.  This avoids any worker-restart dance.
    WorkerGpuState wgs;
    const std::string wgsOwner = "prefetch[" + std::to_string(workerId) + "]";
    bool wgsWarned = false;
    auto tryInitWgs = [&]() {
        if (wgs.ready() || wgsWarned || !GpuContext::get().isInitialized()) return;
        // A failure here is permanent (a wedged device): warn once, stay CPU.
        if (!initWorkerGpuState(wgs, wgsOwner.c_str()))
            wgsWarned = true;
        else
            spdlog::debug("MediaPool {}: WorkerGpuState ready "
                          "— GPU-resident decode path now eligible", wgsOwner);
    };
    tryInitWgs();

    std::unordered_map<MediaHandle, PrefetchDecoderState> decoders;
    size_t queueSize = 0;
    int skippedCached = 0;
    int decodedCount = 0;

    // Drops this worker's decoder for `handle` (its NVDEC surfaces / frame
    // threads and swscale state) and gives up ownership of the handle.
    const auto releaseDecoder = [&](MediaHandle handle) {
        auto it = decoders.find(handle);
        if (it == decoders.end()) return;
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (it->second.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(it->second.swsCtx));
#endif
        decoders.erase(it);
        std::lock_guard lk(m_prefetchMutex);
        auto oit = m_prefetchPackedOwner.find(handle);
        if (oit != m_prefetchPackedOwner.end() && oit->second == workerId)
            m_prefetchPackedOwner.erase(oit);
    };

    while (m_prefetchRunning) {
        // Lazy retry: if GpuContext became initialised since worker start,
        // arm WorkerGpuState now.  No-op when already ready.
        if (!wgs.ready()) tryInitWgs();

        // Release decoders this worker hasn't used for a while (beyond the
        // few kept warm). They used to live until 8 other files pushed them
        // out, holding VRAM for clips long gone from the timeline window.
        if (decoders.size() > prefetch_detail::kWarmDecodersKept) {
            std::vector<std::pair<MediaHandle,
                                  std::chrono::steady_clock::time_point>> lastUsed;
            lastUsed.reserve(decoders.size());
            for (const auto& [h, s] : decoders) lastUsed.emplace_back(h, s.lastUsed);
            for (const MediaHandle h : prefetch_detail::idleDecodersToRelease(
                     std::move(lastUsed), std::chrono::steady_clock::now(),
                     prefetch_detail::kIdleDecoderRelease,
                     prefetch_detail::kWarmDecodersKept)) {
                releaseDecoder(h);
                spdlog::debug("MediaPool prefetch[{}]: released idle decoder "
                              "for handle {}", workerId, h);
            }
        }

        PrefetchTask task;
        {
            std::unique_lock lock(m_prefetchMutex);
            // Timed so an idle worker still gets back to the release above.
            m_prefetchCv.wait_for(lock, std::chrono::seconds(2), [this] {
                return !m_prefetchQueue.empty() || !m_prefetchRunning;
            });
            if (!m_prefetchRunning) break;
            if (m_prefetchQueue.empty()) continue;

            queueSize = m_prefetchQueue.size();

            // Release stale ownerships.
            for (auto it = m_prefetchPackedOwner.begin(); it != m_prefetchPackedOwner.end();) {
                if (it->second == workerId) {
                    bool hasTask = std::any_of(
                        m_prefetchQueue.begin(), m_prefetchQueue.end(),
                        [h = it->first](const PrefetchTask& qt) { return qt.handle == h; });
                    const bool decoderWarm = decoders.count(it->first) > 0;
                    if (!hasTask && !decoderWarm) { it = m_prefetchPackedOwner.erase(it); continue; }

                    bool anyPackedAlpha = false;
                    bool sawTask = false;
                    for (const auto& qt : m_prefetchQueue) {
                        if (qt.handle == it->first) {
                            anyPackedAlpha = qt.packedAlpha;
                            sawTask = true;
                            break;
                        }
                    }
                    if (sawTask) {
                        // Only NVDEC-vs-packed-alpha is a hard mismatch:
                        // NVDEC cannot decode packed-alpha sources, so an
                        // NVDEC worker that took ownership of one must hand
                        // it back to a SW worker. Plain (non-packed) video
                        // is fine on either pool, so we no longer evict a
                        // SW worker that owns a non-packed handle — that
                        // change is what lets SW workers help on plain
                        // H.264, instead of yielding back to the two NVDEC
                        // workers and starving the cache during cold-start.
                        const bool nvdecHoldsAlpha =
                            (workerId < nvdecWorkers) && anyPackedAlpha;
                        if (nvdecHoldsAlpha) {
                            auto dit = decoders.find(it->first);
                            if (dit != decoders.end()) {
#ifdef ROUNDTABLE_HAS_FFMPEG
                                if (dit->second.swsCtx)
                                    sws_freeContext(static_cast<SwsContext*>(dit->second.swsCtx));
#endif
                                decoders.erase(dit);
                            }
                            it = m_prefetchPackedOwner.erase(it);
                            m_prefetchCv.notify_all();
                            continue;
                        }
                    }
                }
                ++it;
            }

            // Per-handle ownership: one worker per handle for sequential decode.
            // Two-tier affinity (UPGRADE_PLAN A regression fix, 2026-05-21):
            //   - NVDEC workers (id < PREFETCH_NVDEC_WORKERS) refuse packed-
            //     alpha (NVDEC can't decode it).
            //   - SW workers (id >= PREFETCH_NVDEC_WORKERS) refuse FIRST-
            //     OWNERSHIP of plain (non-packed) video tasks so NVDEC
            //     workers get first dibs.  Without this, in a multi-clip
            //     project the 8 SW workers race the 2 NVDEC workers for
            //     handle ownership and usually win — locking each H.264
            //     file to SW decode for its lifetime, which kills the
            //     CUDA↔Vulkan zero-copy path (it needs NVDEC output).
            //
            // Fallback: if the queue is video-only (every acceptable task
            // is a non-packed video and no NVDEC worker has yet grabbed
            // it), SW workers DO take it.  This preserves the "SW workers
            // help fill the cache when NVDEC is saturated" property that
            // the previous relaxed rule was meant to provide.
            auto isNonPackedVideo = [](const PrefetchTask& t) {
                return t.info.frameCount > 1 && !t.packedAlpha;
            };
            // On an uncapped GPU every worker decodes plain video on NVDEC
            // (useHwDecode below), so none needs to defer to workers
            // [0, nvdecWorkers). Deferring let a "SW" worker win the race
            // anyway and decode a 4K H.264 file on two CPU threads at ~5 fps
            // while NVDEC sat idle.
            const bool anyWorkerNvdec =
                m_anyWorkerMayUseNvdec.load(std::memory_order_acquire);
            auto preferredAcceptable = [&](const PrefetchTask& t) -> bool {
                if (workerId < nvdecWorkers && t.packedAlpha)
                    return false;
                auto ownerIt = m_prefetchPackedOwner.find(t.handle);
                if (ownerIt != m_prefetchPackedOwner.end())
                    return ownerIt->second == workerId;
                // No owner yet.  SW workers skip plain video on this pass.
                if (workerId >= nvdecWorkers && isNonPackedVideo(t) && !anyWorkerNvdec)
                    return false;
                return true;
            };

            auto pick = std::find_if(m_prefetchQueue.begin(),
                                     m_prefetchQueue.end(), preferredAcceptable);

            // SW fallback: if the preferred pass found nothing AND the
            // queue does contain at least one unowned non-packed video
            // task, give NVDEC workers a brief window to grab it before
            // we steal it.  Without this wait, the SW worker that happens
            // to enter the locked section first immediately steals the
            // single video task and the NVDEC workers never see it —
            // exactly the regression that turned ZC=100% into ZC=0 on
            // multi-clip projects at load.
            //
            // wait_for releases the queue mutex during the wait, so an
            // NVDEC worker can claim the video task during this window.
            // If they do, the re-check below finds nothing (owned by
            // someone else), we fall through to the normal 2 ms idle
            // wait and try again next iteration.
            if (pick == m_prefetchQueue.end() &&
                workerId >= nvdecWorkers)
            {
                const bool hasUnownedVideo = std::any_of(
                    m_prefetchQueue.begin(), m_prefetchQueue.end(),
                    [&](const PrefetchTask& t) {
                        if (!isNonPackedVideo(t)) return false;
                        auto ownerIt = m_prefetchPackedOwner.find(t.handle);
                        return ownerIt == m_prefetchPackedOwner.end();
                    });

                if (hasUnownedVideo) {
                    m_prefetchCv.wait_for(lock, std::chrono::milliseconds(20));

                    // Re-run preferred — an NVDEC worker may have grabbed
                    // the video while we were waiting, in which case it
                    // now has an owner and the SW worker can pick up any
                    // other already-owned task it might be tracking.
                    pick = std::find_if(m_prefetchQueue.begin(),
                                        m_prefetchQueue.end(),
                                        preferredAcceptable);
                }

                // Final fallback: NVDEC didn't grab during the window
                // (probably saturated).  SW worker takes the video to
                // keep throughput up.
                if (pick == m_prefetchQueue.end()) {
                    auto fallbackAcceptable = [&](const PrefetchTask& t) -> bool {
                        auto ownerIt = m_prefetchPackedOwner.find(t.handle);
                        if (ownerIt == m_prefetchPackedOwner.end()) return true;
                        return ownerIt->second == workerId;
                    };
                    pick = std::find_if(m_prefetchQueue.begin(),
                                        m_prefetchQueue.end(),
                                        fallbackAcceptable);
                }
            }

            if (pick == m_prefetchQueue.end()) {
                m_prefetchCv.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            m_prefetchPackedOwner.emplace(pick->handle, workerId);
            task = *pick;
            m_prefetchQueue.erase(pick);

            // Batch-claim consecutive frames to avoid seek contention.
            constexpr int kBatchClaim = 30;
            if (!task.exactRequest) {
                int64_t claimFn = task.frameNumber + 1;
                const int64_t claimMax = task.frameNumber + kBatchClaim;
                auto it = m_prefetchQueue.begin();
                while (it != m_prefetchQueue.end() && claimFn <= claimMax) {
                    if (!it->exactRequest && it->handle == task.handle &&
                        it->frameNumber == claimFn) {
                        it = m_prefetchQueue.erase(it);
                        ++claimFn;
                    } else {
                        ++it;
                    }
                }
            }
        }

        if (m_cache->contains(task.handle, task.frameNumber, task.tier)) {
            ++skippedCached;
            continue;
        }

        const size_t maxDecoders = (workerId < nvdecWorkers) ? 8 : 4;

        bool isNewHandle = (decoders.find(task.handle) == decoders.end());
        if (isNewHandle && decoders.size() >= maxDecoders) {
            MediaHandle evictHandle = 0;
            auto oldestTime = std::chrono::steady_clock::time_point::max();
            for (auto& [h, s] : decoders) {
                if (s.decoder && s.lastUsed < oldestTime) {
                    oldestTime = s.lastUsed;
                    evictHandle = h;
                }
            }
            if (evictHandle != 0) {
                releaseDecoder(evictHandle);
                spdlog::debug("MediaPool prefetch[{}]: evicted decoder for handle {} (limit={})",
                              workerId, evictHandle, maxDecoders);
            }
        }

        auto& state = decoders[task.handle];
        state.lastUsed = std::chrono::steady_clock::now();
        // Formats the GPU converters don't take (4:2:2, 10-bit planar,
        // non-709) convert on the CPU here; one thread cost ~30-50 ms per 4K
        // frame. Same budget as the software decode.
        state.swsThreads = prefetchSoftwareDecodeThreads();
        if (!state.decoder) {
            auto openT0 = std::chrono::steady_clock::now();
            state.decoder = std::make_unique<VideoDecoder>();
            const bool useHwDecode =
                (workerId < nvdecWorkers ||
                 (m_anyWorkerMayUseNvdec.load(std::memory_order_acquire) &&
                  !task.packedAlpha))
                && !task.info.hasAlpha;
            // Passed on the hardware path too: a codec NVDEC cannot take
            // (ProRes, DNxHR, CineForm) falls back to software inside open(),
            // and with 0 it got slice threads only — one core for CineForm,
            // which threads by frame.
            const int swThreads = prefetchSoftwareDecodeThreads();
            if (!state.decoder->open(task.filePath, /*forceSoftware=*/!useHwDecode, /*maxThreads=*/swThreads)) {
                spdlog::warn("MediaPool prefetch[{}]: failed to open decoder for handle {} '{}'",
                             workerId, task.handle, pathToUtf8(task.filePath.filename()));
                state.decoder.reset();
                continue;
            }
            auto openMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - openT0).count();
            spdlog::info("MediaPool prefetch[{}]: opened {} decoder for handle {} '{}' in {:.0f}ms",
                         workerId, useHwDecode ? "NVDEC" : "SW",
                         task.handle, pathToUtf8(task.filePath.filename()), openMs);
        }

        auto decT0 = std::chrono::steady_clock::now();
        auto frame = decodePrefetchFrame(state, task, &wgs);
        auto decMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - decT0).count();
        if (decMs > 80.0) {
            spdlog::info("[PERF SLOW DECODE] prefetch[{}] handle={} frame={} took {:.0f}ms "
                         "(packedAlpha={} hasAlpha={})",
                         workerId, task.handle, task.frameNumber, decMs,
                         task.packedAlpha, task.info.hasAlpha);
        }
        if (frame) {
            m_cache->put(frame);
            if (m_diskCache) m_diskCache->putAsync(frame);
            m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
            ++decodedCount;
            if (decodedCount % 30 == 1) {
                spdlog::info("[PERF] prefetch[{}]: decoded handle={} frame={} {}x{} "
                             "queueDepth={} skippedCached={} totalDecoded={}",
                             workerId, task.handle, task.frameNumber,
                             frame->width, frame->height,
                             queueSize, skippedCached, decodedCount);
                skippedCached = 0;
            }

            // Sequential follow-up: decode next consecutive frames while decoder is positioned.
            // Exact temporal endpoints are isolated requests. Returning to
            // the queue immediately lets the adjacent partner run next;
            // strided follow-up could otherwise spend 30 decodes on N+2,
            // N+4, ... while urgent N+1 waits on the same owned handle.
            const int kMaxFollowUp = task.exactRequest ? 0 : 30;
            // For stills (frameCount 0 or 1) there is no "next consecutive
            // frame" — the previous version defaulted to INT64_MAX when
            // frameCount <= 1, which made the follow-up loop decode 30
            // phantom frames against an image2 demuxer (each producing the
            // same PNG over and over, hundreds of ms of wasted software
            // decode per still per scheduling cycle).
            const int64_t maxFollow = (task.info.frameCount > 1)
                ? task.info.frameCount - 1
                : (task.info.frameCount == 1 ? 0 : -1);

            ResolutionTier followTier = task.tier;
            {
                std::lock_guard fLock(m_prefetchMutex);
                for (const auto& qt : m_prefetchQueue) {
                    if (qt.handle == task.handle) {
                        followTier = qt.tier;
                        break;
                    }
                }
            }

            // Stride the follow-up batch by playback speed.  At Nx only every
            // Nth source frame is displayed; decodePrefetchFrame decode-skips
            // the gap WITHOUT converting (its delta<=150 fast path), so the
            // decoder still advances frame-by-frame (cheap NVDEC) but the
            // expensive GPU convert+copy runs only on the frames that will be
            // shown.  This cuts convert load ~Nx at high speed — the fix for
            // 4x stutter (the convert+copy, not the decode, saturates the
            // compute queue).  followStride == 1 at normal speed (unchanged).
            const double projFpsFu = m_projectFps.load(std::memory_order_relaxed);
            const int srcStrideFu = (task.info.fps > projFpsFu * 1.4)
                ? std::max(1, static_cast<int>(std::round(task.info.fps / projFpsFu))) : 1;
            const double absSpeedFu = std::abs(m_playbackSpeed.load(std::memory_order_relaxed));
            const int speedStrideFu = (absSpeedFu > 1.4)
                ? std::max(1, static_cast<int>(std::lround(absSpeedFu))) : 1;
            const int followStride = std::max(1, srcStrideFu * speedStrideFu);

            for (int f = 1; f <= kMaxFollowUp; ++f) {
                const int64_t nextFn = task.frameNumber +
                                       static_cast<int64_t>(f) * followStride;
                if (nextFn > maxFollow) break;
                if (m_cache->contains(task.handle, nextFn, followTier)) continue;
                if (!m_prefetchRunning) break;

                // Yield immediately to any exact temporal endpoint, including
                // one for this same handle. Ordinary same-handle urgency can
                // still use the warm sequential decoder after three frames;
                // an adjacent N+1 endpoint cannot wait behind that batch.
                {
                    std::lock_guard uLock(m_prefetchMutex);
                    for (const auto& qt : m_prefetchQueue) {
                        if (qt.urgent &&
                            (qt.exactRequest ||
                             (f > 3 && qt.handle != task.handle))) {
                            goto endFollowUp;
                        }
                    }
                }

                PrefetchTask followTask = task;
                followTask.frameNumber = nextFn;
                followTask.tier        = followTier;
                followTask.urgent      = false;

                auto followFrame = decodePrefetchFrame(state, followTask, &wgs);
                if (followFrame) {
                    m_cache->put(followFrame);
                    if (m_diskCache) m_diskCache->putAsync(followFrame);
                    m_perf.prefetchDeliveries.fetch_add(1, std::memory_order_relaxed);
                    ++decodedCount;
                } else {
                    break;
                }
            }
            endFollowUp:;
        }
    }

    // Cleanup thread-local decoders and sws contexts
#ifdef ROUNDTABLE_HAS_FFMPEG
    for (auto& [h, state] : decoders) {
        if (state.swsCtx)
            sws_freeContext(static_cast<SwsContext*>(state.swsCtx));
    }
#endif
    spdlog::debug("MediaPool: prefetch worker {} exiting", workerId);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
