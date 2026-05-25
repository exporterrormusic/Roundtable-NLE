/*
 * CompositeServiceFramePrewarm.cpp - Background prewarm & timeline lookahead.
 * Extracted from CompositeServiceFrame.cpp (Step 3.1 of modularization plan).
 *
 * Contains:
 *   - prewarmPlaybackResources()   — dispatch prewarm work to background thread
 *   - prewarmThreadLoop()          — background thread entry point
 *   - doPrewarmPlaybackResources() — open media, warm caches, preroll frames
 *   - prewarmUpcomingShots()       — timeline lookahead scan (~2s window)
 */

#include "CompositeService.h"
#include "ClipRenderers.h"
#include "CompositeServiceBlend.h"
#include "CompositeServiceLayerBuild.h"

// Media / timeline
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "Constants.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/TitleClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"
#include "timeline/OpacityMask.h"

#include "project/Project.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <thread>
#include <unordered_set>

// GPU compositing
#include "CompositeEngine.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {


// ─────────────────────────────────────────────────────────────────────────────
// Background-thread prewarm dispatch
// ─────────────────────────────────────────────────────────────────────────────

void CompositeService::prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;

    std::lock_guard lock(m_prewarmMutex);
    m_prewarmRequest = {tick, outW, outH};
    m_prewarmPending = true;
    m_prewarmCv.notify_one();
}

void CompositeService::prewarmThreadLoop()
{
    while (true) {
        PrewarmRequest req;
        {
            std::unique_lock lock(m_prewarmMutex);
            m_prewarmCv.wait(lock, [this]() {
                return m_prewarmPending || m_destroying.load(std::memory_order_acquire);
            });
            if (m_destroying.load(std::memory_order_acquire))
                break;
            req = m_prewarmRequest;
            m_prewarmPending = false;
        }

        // Run the actual prewarm work on this background thread.
        // Uses m_openMediaHandlesMutex (not m_compositeMutex) so it
        // never blocks compositeFrame() on the FrameProducer thread.
        try {
            doPrewarmPlaybackResources(req.tick, req.outW, req.outH);
        } catch (const std::exception& e) {
            spdlog::error("[PREWARM] Exception in background prewarm: {}",
                          e.what());
        } catch (...) {
            spdlog::error("[PREWARM] Unknown exception in background prewarm");
        }
    }
}

void CompositeService::doPrewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;
    if (!m_timeline || !m_mediaPool || outW == 0 || outH == 0)
        return;

    // Prevent overlapping prewarm runs
    // (Thread_local guard is sufficient since this is called from the
    //  background prewarm thread, not the UI thread)
    static thread_local bool s_inPrewarm = false;
    if (s_inPrewarm)
        return;
    s_inPrewarm = true;

    // Lock only m_openMediaHandles — the prewarm thread does NOT need
    // m_compositeMutex (GPU compositing).  It only manages media handles
    // and schedules prefetches.  Using a dedicated mutex eliminates the
    // old try_to_lock contention with compositeFrame().
    std::unique_lock lock(m_openMediaHandlesMutex);

    auto& gpu = GpuContext::get();
    if (gpu.isInitialized()) {
        auto* comp = gpu.compositor(outW, outH);
        if (comp && comp->isInitialized()) {
            m_compositorReady.store(true);
            static std::atomic<int> s_prewarmLog{0};
            if (++s_prewarmLog <= 10 || s_prewarmLog % 30 == 0) {
                spdlog::info("[PERF] prewarmPlaybackResources: compositor ready {}x{} at tick={}",
                             outW, outH, tick);
            }
        }
    }

    // Program monitor preview tier
    const ResolutionTier previewTier = playbackTier();
    bool warmedEffectProcessor = false;

    struct PrerollTarget { uint64_t handle; int64_t frame; ResolutionTier tier; };
    std::vector<PrerollTarget> prerollTargets;
    prerollTargets.reserve(8);

    for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
        auto* track = m_timeline->track(ti - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        auto active = track->clipsAtTime(tick);
        for (Clip* clip : active) {
            if (!clip)
                continue;

            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                const auto& mediaPath = videoClip->mediaPath();
                if (mediaPath.empty())
                    continue;

                uint64_t handle = 0;
                auto it = m_openMediaHandles.find(mediaPath);
                if (it != m_openMediaHandles.end()) {
                    handle = it->second;
                } else if (m_mediaPool->isPathOpen(mediaPath)) {
                    handle = m_mediaPool->open(mediaPath);
                    if (handle != 0)
                        m_openMediaHandles[mediaPath] = handle;
                } else {
                    m_mediaPool->openAsync(mediaPath);
                    handle = 0;
                }

                if (handle == 0)
                    continue;

                double fps = videoClip->sourceFps();
                if (fps <= 0.0)
                    fps = 24.0;

                int64_t srcTick = tick - clip->timelineIn() + clip->sourceIn();
                if (srcTick < 0)
                    srcTick = 0;

                int64_t frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                const auto* mediaInfo = m_mediaPool->getInfo(handle);
                if (!mediaInfo)
                    continue;

                if (videoClip->sourceFps() <= 0.0 && mediaInfo->fps > 0.0) {
                    fps = mediaInfo->fps;
                    frameNum = static_cast<int64_t>(ticksToSeconds(srcTick) * fps);
                }

                if (mediaInfo->frameCount <= 1) {
                    frameNum = 0;
                } else if (videoClip->isVideoCharacter()) {
                    frameNum = ((frameNum % mediaInfo->frameCount) + mediaInfo->frameCount)
                               % mediaInfo->frameCount;
                } else {
                    frameNum = std::clamp(frameNum, int64_t(0), mediaInfo->frameCount - 1);
                }

                const auto warmTier = videoClip->isVideoCharacter()
                    ? ResolutionTier::Half : previewTier;
                (void)m_mediaPool->tryGetFrame(handle, frameNum, warmTier);

                if (mediaInfo->frameCount > 1) {
                    m_mediaPool->schedulePrefetch(
                        handle, frameNum + 1, 60, /*urgent=*/true, warmTier);
                    prerollTargets.push_back({handle, frameNum + 1, warmTier});
                }

                if (videoClip->isVideoCharacter() &&
                    mediaInfo->frameCount > 1 &&
                    mediaInfo->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                    m_mediaPool->startLoopPreDecode(handle, warmTier, clip->timelineIn());
                }

                if (!warmedEffectProcessor && gpu.isInitialized() &&
                    clip->effects().hasActiveEffects()) {
                    uint32_t fxW = mediaInfo->width;
                    uint32_t fxH = mediaInfo->height;
                    constexpr uint32_t kPreviewMaxDim = 960;
                    if (fxW > kPreviewMaxDim || fxH > kPreviewMaxDim) {
                        const float scale = std::min(
                            static_cast<float>(kPreviewMaxDim) / static_cast<float>(fxW),
                            static_cast<float>(kPreviewMaxDim) / static_cast<float>(fxH));
                        fxW = std::max<uint32_t>(2u, static_cast<uint32_t>(fxW * scale) & ~1u);
                        fxH = std::max<uint32_t>(2u, static_cast<uint32_t>(fxH * scale) & ~1u);
                    }
                    auto* fx = gpu.effectProcessor(fxW, fxH);
                    warmedEffectProcessor = fx && fx->isInitialized();
                }
            } else if (!warmedEffectProcessor && gpu.isInitialized() && clip->effects().hasActiveEffects()) {
                auto* fx = gpu.effectProcessor(outW, outH);
                warmedEffectProcessor = fx && fx->isInitialized();
            }
            // SpineClips render via GPU live Spine (the H264 pre-rendered
            // path at CompositeServiceLayerBuild.cpp is hardcoded off).
            // Prefetching their AnimVideoCache entries here was burning
            // NVDEC sessions + queueing 30 urgent prefetches per Spine
            // character per shot for frames nothing samples — visible as
            // COMPOSITE-SLOW / DIAG-CLOCK JUMP cascades on constrained
            // GPUs (GTX 1070) once a project had been converted.
        }
    }

    // ── Seed m_lastActiveClipIds ────────────────────────────────────────
    {
        std::unordered_set<uint64_t> activeIds;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (auto* clip : track->clipsAtTime(tick)) {
                if (clip && clip->isEnabled())
                    activeIds.insert(clip->id());
            }
        }
        m_lastActiveClipIds = std::move(activeIds);
    }

    // ── Pre-open ALL media handles on ALL video tracks (background) ─────
    {
        std::vector<std::string> pathsToOpen;
        for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
            auto* track = m_timeline->track(ti - 1);
            if (!track || track->type() != TrackType::Video || track->isMuted())
                continue;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                auto* clip = track->clip(ci);
                if (!clip) continue;
                auto* videoClip = dynamic_cast<VideoClip*>(clip);
                if (!videoClip) continue;
                const auto& mediaPath = videoClip->mediaPath();
                if (mediaPath.empty()) continue;
                // Outer `lock` at the top of doPrewarmPlaybackResources()
                // ALREADY holds m_openMediaHandlesMutex on this thread.
                // A nested std::lock_guard on the same non-recursive
                // mutex throws std::system_error("resource deadlock would
                // occur").  When that exception unwinds, the prewarm
                // thread leaves the timeline scan half-finished and
                // (worse) the spawned background bulk-open thread keeps
                // calling MediaPool::open in parallel with the
                // FrameProducer's composite path — over the next second
                // a partially-decoded frame slips into the GPU pipeline
                // and the NVIDIA driver returns VK_ERROR_DEVICE_LOST.
                if (m_openMediaHandles.count(mediaPath)) continue;
                pathsToOpen.push_back(mediaPath);
            }
        }

        if (!pathsToOpen.empty()) {
            std::sort(pathsToOpen.begin(), pathsToOpen.end());
            pathsToOpen.erase(std::unique(pathsToOpen.begin(), pathsToOpen.end()),
                              pathsToOpen.end());

            auto* pool = m_mediaPool;
            // Release mutex during background bulk-open so FrameProducer
            // is not starved
            lock.unlock();
            // Join any previous bulk-open thread before spawning a new one.
            if (m_bulkOpenThread.joinable())
                m_bulkOpenThread.join();
            m_bulkOpenThread = std::thread([pool, paths = std::move(pathsToOpen)]() {
                for (const auto& mediaPath : paths) {
                    namespace fs = std::filesystem;
                    fs::path p(mediaPath);
                    uint64_t handle = 0;
                    if (p.extension() == ".mov" || p.extension() == ".webm") {
                        fs::path mp4Path = fs::path(mediaPath).replace_extension(".mp4");
                        std::error_code ec;
                        if (fs::exists(mp4Path, ec))
                            handle = pool->open(mp4Path);
                        if (handle == 0) {
                            fs::path mp4InVideos = fs::path("assets") / "videos" / mp4Path.filename();
                            if (fs::exists(mp4InVideos, ec))
                                handle = pool->open(mp4InVideos);
                        }
                    }
                    if (handle == 0) {
                        uint64_t fallbackHandle = pool->open(mediaPath);
                        (void)fallbackHandle;
                    }
                }
                spdlog::info("[PERF] background bulk-open finished ({} paths)", paths.size());
            });
            lock.lock();
        }
    }

    // ── Bounded play-start preroll ──────────────────────────────────────
    if (!prerollTargets.empty()) {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(400);
        int pollCount = 0;
        size_t lastReady = 0;
        bool allWarm = false;
        do {
            // Release the mutex so the FrameProducer thread can composite
            // frames and submit GPU work.
            lock.unlock();
            std::this_thread::sleep_for(milliseconds(2));
            lock.lock();

            lastReady = 0;
            for (const auto& t : prerollTargets) {
                if (m_mediaPool->isFrameCached(t.handle, t.frame, t.tier))
                    ++lastReady;
            }
            if (lastReady == prerollTargets.size()) {
                allWarm = true;
                break;
            }
            ++pollCount;
        } while (steady_clock::now() < deadline);

        static std::atomic<int> s_prerollLog{0};
        if (++s_prerollLog <= 10 || s_prerollLog % 30 == 0) {
            if (allWarm) {
                spdlog::info("[PERF] play-start preroll: {}/{} frames warm after {} polls",
                             lastReady, prerollTargets.size(), pollCount);
            } else {
                spdlog::warn("[PERF] play-start preroll TIMEOUT: only {}/{} frames warm after {} polls",
                             lastReady, prerollTargets.size(), pollCount);
            }
        }
    }

    s_inPrewarm = false;
}


// ─────────────────────────────────────────────────────────────────────
// Timeline lookahead prewarm.
//
// Scans clips whose timelineIn() lies in (tick, tick + LOOKAHEAD_SECONDS]
// and proactively opens their media + schedules first-frame prefetch.
// This eliminates the 150-200ms "cold decoder open" stall at every shot
// boundary — the decoder is warm and frame 0 cached before the playhead
// reaches the clip.  VLC doesn't have this problem because it plays a
// single continuous stream; we stitch many files, so we must prewarm.
// Throttled to ~100ms (10 scans/sec) to avoid redundant work at 60fps.
// ─────────────────────────────────────────────────────────────────────
void CompositeService::prewarmUpcomingShots(int64_t tick)
{
    if (m_shutdown.load(std::memory_order_acquire))
        return;
    if (!m_timeline || !m_mediaPool) return;

    using namespace std::chrono;
    auto now = steady_clock::now();
    if (now - m_lastLookaheadScan < milliseconds(100))
        return;
    m_lastLookaheadScan = now;

    constexpr double LOOKAHEAD_SECONDS = 2.0;
    const int64_t lookaheadTicks = static_cast<int64_t>(LOOKAHEAD_SECONDS * kTicksPerSecond);
    const int64_t windowEnd = tick + lookaheadTicks;

    int prewarmedThisScan = 0;
    int openedThisScan = 0;

    for (size_t ti = m_timeline->trackCount(); ti > 0; --ti) {
        auto* track = m_timeline->track(ti - 1);
        if (!track || track->type() != TrackType::Video || track->isMuted())
            continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip || !clip->isEnabled()) continue;

            const int64_t clipIn = clip->timelineIn();
            if (clipIn <= tick || clipIn > windowEnd) continue;
            if (m_lastActiveClipIds.count(clip->id())) continue;
            if (!m_prewarmedClipIds.insert(clip->id()).second) continue;

            // Resolve which media path this clip decodes from.  Only
            // VideoClip has one — SpineClips render via GPU live Spine and
            // do NOT decode the AnimVideoCache .mp4/.webm even when
            // present (see CompositeServiceLayerBuild.cpp `if (false)`).
            std::string mp;
            bool isCharacter = false;
            if (auto* videoClip = dynamic_cast<VideoClip*>(clip)) {
                mp = videoClip->mediaPath();
                isCharacter = videoClip->isVideoCharacter();
            }
            if (mp.empty()) {
                // Either a non-media clip or a Spine clip with no
                // pre-rendered video.  Drop the prewarm mark so it can be
                // re-considered later without duplicate log spam.
                m_prewarmedClipIds.erase(clip->id());
                continue;
            }

            if (!m_mediaPool->isPathOpen(mp)) {
                m_mediaPool->openAsync(mp);
                ++openedThisScan;
                // Re-queue: next scan (~100ms later) will schedule prefetch
                // once the async open completes.
                m_prewarmedClipIds.erase(clip->id());
                continue;
            }

            uint64_t handle = 0;
            {
                std::lock_guard hl(m_openMediaHandlesMutex);
                auto it = m_openMediaHandles.find(mp);
                if (it != m_openMediaHandles.end()) {
                    handle = it->second;
                } else {
                    handle = m_mediaPool->open(mp); // fast: already open
                    if (handle != 0) m_openMediaHandles[mp] = handle;
                }
            }
            if (handle == 0) continue;

            const auto* info = m_mediaPool->getInfo(handle);
            if (!info) continue;

            double fps = (info->fps > 0.0) ? info->fps : 24.0;
            if (auto* vc = dynamic_cast<VideoClip*>(clip)) {
                if (vc->sourceFps() > 0.0) fps = vc->sourceFps();
            }

            const double srcSecs = static_cast<double>(clip->sourceIn()) / static_cast<double>(kTicksPerSecond);
            int64_t srcFrame = static_cast<int64_t>(srcSecs * fps);
            if (info->frameCount > 1) {
                if (isCharacter)
                    srcFrame = ((srcFrame % info->frameCount) + info->frameCount) % info->frameCount;
                else
                    srcFrame = std::clamp(srcFrame, int64_t(0), info->frameCount - 1);
            } else {
                srcFrame = 0;
            }

            // Upcoming-shot prewarm uses the current playback tier.
            // Characters stay Half (they composite tiny regardless).
            const auto warmTier = isCharacter ? ResolutionTier::Half
                                              : playbackTier();

            m_mediaPool->schedulePrefetch(handle, srcFrame,
                                          /*count=*/30, /*urgent=*/true, warmTier);
            ++prewarmedThisScan;

            // For short character loops (≤ LOOP_PREDECODE_MAX_FRAMES),
            // pre-decode the entire file into cache so playback is 100%
            // cache hits for the full duration of the shot.  Without this,
            // a 160-frame 60fps clip decoded reactively at ~12fps looks
            // frozen, updating only every ~80ms.  startLoopPreDecode is
            // idempotent and cheap once the loop is cached.
            if (isCharacter &&
                info->frameCount > 1 &&
                info->frameCount <= MediaPool::LOOP_PREDECODE_MAX_FRAMES) {
                m_mediaPool->startLoopPreDecode(handle, warmTier, clipIn);
            }

            const double leadMs = static_cast<double>(clipIn - tick)
                                  / static_cast<double>(kTicksPerSecond) * 1000.0;
            static std::atomic<int> s_lookaheadLog{0};
            if (++s_lookaheadLog <= 30 || s_lookaheadLog % 20 == 0) {
                spdlog::info("[LOOKAHEAD] prewarm clip {} '{}' frame {} tier={} frames={} (starts in {:.0f}ms)",
                             clip->id(),
                             std::filesystem::path(mp).filename().string(),
                             srcFrame, static_cast<int>(warmTier),
                             info->frameCount, leadMs);
            }
        }
    }

    // Drop prewarm marks for clips that have become active so that if the
    // same clip id reappears later on the timeline (looped section) it
    // gets prewarmed again.
    for (auto it = m_prewarmedClipIds.begin(); it != m_prewarmedClipIds.end();) {
        if (m_lastActiveClipIds.count(*it))
            it = m_prewarmedClipIds.erase(it);
        else
            ++it;
    }

    if (prewarmedThisScan > 0 || openedThisScan > 0) {
        spdlog::debug("[LOOKAHEAD] scan@tick={} window={:.1f}s: {} prewarmed, {} async-opens",
                      tick, LOOKAHEAD_SECONDS, prewarmedThisScan, openedThisScan);
    }
}


} // namespace rt
