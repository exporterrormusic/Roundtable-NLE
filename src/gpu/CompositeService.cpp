/*
 * CompositeService.cpp - Frame compositing pipeline (extracted from TimelineWorkspace).
 * No Qt dependency.
 */

// **Must** come before any header that pulls in <vulkan/vulkan.h> so
// volk can define VK_NO_PROTOTYPES first.  Without this, calls like
// vkQueueWaitIdle resolve to a direct function symbol instead of a
// function-pointer variable, causing an ACCESS_VIOLATION crash.
#include <volk.h>

#include "CompositeService.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "ClipRenderers.h"

// Media / timeline
#include "cache/CachePolicy.h"
#include "cache/FrameCache.h"
#include "cache/SegmentRenderCache.h"
#include "playback/MediaPool.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
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
#include "spine/AnimationVideoCache.h"
#include "spine/ModelManager.h"
#include "spine/ShotPreset.h"
#include "stb_image.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

// GPU compositing
#include "CompositeEngine.h"
#include "RenderGpuResources.h"
#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

#ifdef _WIN32
#include <windows.h>
#endif


namespace rt {

std::atomic<bool> CompositeService::s_modalDialogActive{false};
// UPGRADE_PLAN: GPU-resident decode + CUDA↔Vulkan zero-copy default-on.
// Set ROUNDTABLE_GPU_RESIDENT_DECODE=0 (or "false"/"off"/"no") in the
// environment to force the legacy CPU upload path — the env var is
// now a kill switch, not an opt-in.  The flag lives in
// playback/GpuResidentDecode.h (core) so the decode path can read it.

namespace {
// Env-var kill switch.  Read once at first CompositeService construction.
// Empty / unset → leave at compile-time default (ON).
// "0"/"false"/"FALSE"/"off"/"no" → force OFF.
// Anything else → leave at compile-time default (ON).
void initGpuResidentDecodeFromEnv()
{
    static std::once_flag once;
    std::call_once(once, []() {
#ifdef _WIN32
        char buf[16] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf),
                     "ROUNDTABLE_GPU_RESIDENT_DECODE") != 0 || len == 0)
            return;  // unset → keep default (ON)
        const std::string v(buf);
#else
        const char* e = std::getenv("ROUNDTABLE_GPU_RESIDENT_DECODE");
        if (!e) return;  // unset → keep default (ON)
        const std::string v(e);
#endif
        const bool kill = v == "0" || v == "false" || v == "FALSE"
                       || v == "off" || v == "OFF"
                       || v == "no"  || v == "NO";
        if (kill) {
            CompositeService::setGpuResidentDecode(false);
            // warn so it survives the warn+ logger filter — flag changes
            // at startup are operationally important.
            spdlog::warn("UPGRADE_PLAN: GPU-resident decode DISABLED "
                         "via ROUNDTABLE_GPU_RESIDENT_DECODE={} "
                         "(legacy CPU upload path)", v);
        } else {
            spdlog::warn("UPGRADE_PLAN: GPU-resident decode ENABLED "
                         "(ROUNDTABLE_GPU_RESIDENT_DECODE={}; "
                         "set =0 to force the legacy path)", v);
        }
    });
}
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CompositeService::CompositeService(GpuResourceMode gpuResourceMode)
{
    initGpuResidentDecodeFromEnv();

    // §4.6 slice 2c: composited-output cache for "Render In to Out".
    m_segmentRenderCache = std::make_unique<SegmentRenderCache>();

    // Export rendering owns private mutable helpers and per-thread command
    // pools. The device, allocator, scheduler, and queues remain shared.
    if (gpuResourceMode == GpuResourceMode::Isolated) {
        m_renderGpuResources = std::make_unique<RenderGpuResources>();
        if (GpuContext::get().isInitialized() &&
            !m_renderGpuResources->init()) {
            spdlog::error("CompositeService: isolated GPU resources unavailable; "
                          "GPU compositing will remain disabled for this session");
        }
    }

    // Create the composite engine (owns GPU compositing pipeline).
    m_engine = std::make_unique<CompositeEngine>();
    if (m_renderGpuResources) {
        m_engine->setSequentialRenderMode(true);
        m_engine->setEffectProcessorResolver(
            [this](uint32_t width, uint32_t height) {
                return m_renderGpuResources
                    ? m_renderGpuResources->effectProcessor(width, height)
                    : nullptr;
            });
    }
    if (GpuContext::get().isInitialized()) {
        VkDevice device = GpuContext::get().vkDevice();
        if (device != VK_NULL_HANDLE &&
            (!m_renderGpuResources || m_renderGpuResources->isInitialized())) {
            if (m_renderGpuResources) {
                m_engine->init(device,
                    &m_renderGpuResources->graphicsCommandPool(),
                    m_renderGpuResources->graphicsQueue());
            } else {
                m_engine->init(device);
            }
        }
    }

    // ── Start the background prewarm thread ───────────────────────────
    // prewarmPlaybackResources() now enqueues work on this thread instead
    // of blocking the UI thread at play-start.  The thread loop waits on
    // a condition variable and wakes up when a new prewarm request arrives
    // or when m_destroying is set.
    m_prewarmThread = std::thread(&CompositeService::prewarmThreadLoop, this);
}

CompositeService::~CompositeService()
{
    // Belt-and-braces: App.cpp Phase 1 calls shutdown() before MediaPool
    // is destroyed, but if anyone constructs a CompositeService outside
    // App (tests, future code), the destructor still does the full
    // teardown.  shutdown() is idempotent.
    shutdown();

    // Destroy the composite engine — it drains GPU queues and frees all
    // GPU resources (submission, staging ring, texture cache, layers).
    m_engine.reset();
    m_renderGpuResources.reset();
}

std::shared_ptr<Compositor> CompositeService::renderCompositor(
    uint32_t width, uint32_t height)
{
    if (m_renderGpuResources)
        return m_renderGpuResources->compositor(width, height);
    return GpuContext::get().compositor(width, height);
}

std::shared_ptr<EffectProcessor> CompositeService::renderEffectProcessor(
    uint32_t width, uint32_t height)
{
    if (m_renderGpuResources)
        return m_renderGpuResources->effectProcessor(width, height);
    return GpuContext::get().effectProcessor(width, height);
}

std::shared_ptr<TransitionRenderer> CompositeService::renderTransitionRenderer(
    uint32_t width, uint32_t height)
{
    if (m_renderGpuResources)
        return m_renderGpuResources->transitionRenderer(width, height);
    return GpuContext::get().transitionRenderer(width, height);
}

std::shared_ptr<SpineRenderer> CompositeService::renderSpineRenderer(
    uint32_t width, uint32_t height, const std::string& contentKey)
{
    if (m_renderGpuResources)
        return m_renderGpuResources->spineRenderer(width, height, contentKey);
    return GpuContext::get().spineRenderer(width, height, contentKey);
}

std::shared_ptr<Nv12Converter> CompositeService::renderNv12Converter(
    uint32_t width, uint32_t height)
{
    if (m_renderGpuResources)
        return m_renderGpuResources->nv12Converter(width, height);
    return GpuContext::get().nv12Converter(width, height);
}

void CompositeService::reset()
{
    invalidateCacheDirect();
    clearMediaHandles();
    if (m_engine) {
        m_engine->clearLru();
    }
    {
        std::lock_guard<std::mutex> g(m_lastActiveClipIdsMutex);
        m_lastActiveClipIds.clear();
    }
    {
        // Same lock prewarmUpcomingShots holds while mutating these — see
        // m_lookaheadMutex docs in CompositeService.h.
        std::lock_guard lg(m_lookaheadMutex);
        m_prewarmedClipIds.clear();
        m_headWarmedClipIds.clear();
        m_lastLookaheadScan = {};
        m_lastLookaheadTick = INT64_MIN;
    }
    m_stickyLastClipFrame.clear();
    m_stickyLastCharFrame.clear();
#ifdef ROUNDTABLE_HAS_SPINE
    m_spineSharedCache.clear();
    {
        // Renderers holding a page keep it alive past this (fence-guarded).
        std::lock_guard lg(m_spineGpuPagesMutex);
        m_spineGpuPages.clear();
    }
    m_animNameCache.clear();
    m_gpuSpinePreparedRenderers.clear();
    m_spineCache.clear();
    m_lastPreRenderedSpineFrame.clear();
    {
        std::lock_guard lg(m_spinePendingMutex);
        m_spinePendingKeys.clear();
    }
    m_spineLoadFutures.clear();
    m_animVideoCache.reset();
#endif
}

#ifdef ROUNDTABLE_HAS_SPINE
void CompositeService::initAnimVideoCache(MediaPool* pool)
{
    if (!m_animVideoCache && pool) {
        // Cache directory is always under the program's assets/ folder
        // (resolved relative to the working directory in dev, or to the
        //  app directory in installed builds — the same as how assets/
        //  itself is resolved).
        m_animVideoCache = std::make_unique<AnimationVideoCache>(
            pool, "assets/converted", "assets");
        m_animVideoCache->scanCacheDirectory();
    }
}

void CompositeService::drainCompletedSpineFutures()
{
    m_spineLoadFutures.erase(
        std::remove_if(m_spineLoadFutures.begin(), m_spineLoadFutures.end(),
                       [](const std::future<void>& f) {
                           return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                       }),
        m_spineLoadFutures.end());
}

void CompositeService::addSpineFuture(std::future<void> f)
{
    m_spineLoadFutures.push_back(std::move(f));
}

std::shared_ptr<CompositeService::SpineSharedData>
CompositeService::findSpineSharedData(const std::string& key) const
{
    auto it = m_spineSharedCache.find(key);
    return (it != m_spineSharedCache.end()) ? it->second : nullptr;
}

void CompositeService::storeSpineSharedData(const std::string& key,
                                            std::shared_ptr<SpineSharedData> data)
{
    m_spineSharedCache[key] = std::move(data);
}

const std::vector<std::string>*
CompositeService::findAnimNames(const std::string& key) const
{
    auto it = m_animNameCache.find(key);
    return (it != m_animNameCache.end()) ? &it->second : nullptr;
}

void CompositeService::storeAnimNames(const std::string& key,
                                      std::vector<std::string> names)
{
    m_animNameCache[key] = std::move(names);
}

void CompositeService::evictSpineState(uint64_t clipId)
{
    m_spineCache.erase(clipId);
}

void CompositeService::purgeDeadSpineStates(const std::unordered_set<uint64_t>& liveIds)
{
    for (auto it = m_spineCache.begin(); it != m_spineCache.end(); ) {
        if (liveIds.find(it->first) == liveIds.end())
            it = m_spineCache.erase(it);
        else
            ++it;
    }
}
#endif

void CompositeService::invalidateCacheDirect()
{
    m_liveCacheGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (m_engine) {
        m_engine->clearLru();
    }
    {
        std::lock_guard lg(m_lastCompositeMtx);
        m_lastGoodComposite.reset();
        m_lastGoodCompositeTick = -1;
        m_lastGoodCompositeClipIds.clear();
        m_lastStaticComposite.reset();
        m_lastStaticCompositeKey = 0;
        // Re-arm the A1 settle window so a project switch / cache flush
        // gets the same first-view grace as cold startup, instead of
        // inheriting the previous project's timestamp.
        m_lastFullCompositeAt = std::chrono::steady_clock::time_point{};
        m_lastFullLayerCount  = 0;
    }
    m_cacheInvalidateRequested.store(false, std::memory_order_release);
}

void CompositeService::invalidateMediaTextures(uint64_t mediaId)
{
    if (mediaId == 0) return;
    if (m_engine) {
        if (auto* texCache = m_engine->textureCache()) {
            texCache->evictMedia(mediaId);
            spdlog::warn("[LIVE-RELOAD] CompositeService: evicted GPU "
                         "textures for mediaId={} (force re-upload)", mediaId);
        }
        // Also reset GpuUploadManager's per-layer-slot dirty-tracking keys.
        // For non-cacheable layers (stills outside loop/scrub), the upload
        // path keys by (mediaId, frameNumber) per pool slot; if those match
        // the request it SKIPS the CPU→GPU upload entirely.  After live
        // replacement the (mediaId, frame) tuple is unchanged but pixels
        // differ — without this reset the pool slot keeps drawing the OLD
        // uploaded texture (the exact "needs scrub" symptom).
        m_engine->invalidateMediaPoolSlots(mediaId);
    }
    // Also drop the cached composite output so the next frame rebuilds the
    // layer with the freshly-uploaded texture instead of a cached blend.
    requestCacheInvalidation();
}

void CompositeService::requestCacheInvalidationRange(int64_t fromTick, int64_t toTick)
{
    // Try to acquire m_compositeMutex non-blockingly.  If the composite
    // thread is mid-frame, fall back to the existing atomic-flag mechanism
    // (which drops the full LRU on next compositeFrame entry).  This avoids
    // deadlocking the UI thread on a long composite.
    std::unique_lock lock(m_compositeMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Compositor is busy.  Fall back to full invalidation.
        requestCacheInvalidation();
        return;
    }
    if (m_engine)
        m_engine->invalidateLruRange(m_timeline, fromTick, toTick);
    {
        std::lock_guard lg(m_lastCompositeMtx);
        m_lastStaticComposite.reset();
        m_lastStaticCompositeKey = 0;
        // If the held last-good frame happens to be in the affected range,
        // drop it too — otherwise keep it to bridge the next composite.
        if (m_lastGoodComposite && m_lastGoodCompositeTick >= fromTick &&
            m_lastGoodCompositeTick <= toTick)
        {
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
            m_lastGoodCompositeClipIds.clear();
            // The held composite covered the invalidated range, so
            // re-arm the A1 settle clock too — otherwise the next
            // partial composite would inherit the stale timestamp
            // and skip the first-view hold.
            m_lastFullCompositeAt = std::chrono::steady_clock::time_point{};
            m_lastFullLayerCount  = 0;
        }
    }
}

// ── Cache policy ─────────────────────────────────────────────────────────────

void CompositeService::setCachePolicy(rt::CachePolicy* policy)
{
    m_cachePolicy = policy;
    if (m_engine)
        m_engine->setCachePolicy(policy);
}

// ── Shutdown ────────────────────────────────────────────────────────────────

void CompositeService::shutdown()
{
    // Idempotent: App.cpp Phase 1 calls this, and ~CompositeService
    // calls it again as a safety net.  Use a dedicated guard atomic
    // (not m_shutdown) — requestShutdown() also flips m_shutdown, so
    // a prior requestShutdown call would otherwise make us skip the
    // real teardown here.
    if (m_shutdownDone.exchange(true, std::memory_order_acq_rel))
        return;
    m_shutdown.store(true, std::memory_order_release);

    // 1a. Drain any in-flight spine load futures so we don't leave
    //     background threads running past shutdown.
    {
        for (auto& f : m_spineLoadFutures) {
            if (f.valid())
                f.wait();
        }
        m_spineLoadFutures.clear();
    }

    // 1. Stop the prewarm thread BEFORE we clear the containers it
    //    reads.  Without this, calling reset() below races a live
    //    prewarm iteration that's walking m_stickyLastClipFrame /
    //    m_spineCache / etc.
    m_destroying.store(true);
    {
        std::lock_guard lock(m_prewarmMutex);
        m_prewarmCv.notify_one();
    }
    if (m_prewarmThread.joinable())
        m_prewarmThread.join();
    if (m_bulkOpenThread.joinable())
        m_bulkOpenThread.join();

    // 2. Drop every shared_ptr<CachedFrame> this service holds.
    //    Each CachedFrame's gpuTextureOwner is a shared_ptr<Texture>
    //    issued by MediaPool's PrefetchTexturePool — letting these
    //    refs survive into Phase 3 (when MediaPool is already gone)
    //    fires the recycle-into-dead-pool AV.  reset() clears:
    //    m_lastGoodComposite, m_stickyLastClipFrame,
    //    m_stickyLastCharFrame, m_lastPreRenderedSpineFrame,
    //    m_spineCache (transitively SpineCPUState::cachedFrame),
    //    and the engine's composite LRU.
    reset();

    // 3. Tear down the composite engine — waits for GPU idle, frees
    //    GPU resources (submission, staging ring, texture cache,
    //    layer textures).  After this, the engine is unusable but
    //    its destructor (running from ~CompositeService later) is
    //    a safe no-op because everything is already torn down.
    if (m_engine) {
        m_engine->shutdown(m_renderGpuResources
            ? GpuTeardownMode::SessionScoped
            : GpuTeardownMode::DeviceWide);
    }

    // Private helpers and their command pools depend on the shared Vulkan
    // device. Release them as part of shutdown (not merely destruction) so
    // phased application teardown cannot destroy GpuContext first.
    if (m_renderGpuResources)
        m_renderGpuResources->shutdown(GpuTeardownMode::SessionScoped);
    m_renderGpuResources.reset();

    spdlog::info("CompositeService::shutdown() — complete");
}

int CompositeService::vramUsagePercent() const
{
    return m_engine ? m_engine->vramUsagePercent() : 0;
}

#ifdef ROUNDTABLE_HAS_SPINE
void CompositeService::integrateSpineSharedData(const std::string& key,
                                                 std::shared_ptr<SpineSharedData> shared)
{
    m_spineSharedCache[key] = std::move(shared);
}

void CompositeService::preuploadSpineAtlas(const std::string& key,
                                           const SpineSharedData& shared)
{
    if (m_shutdown.load(std::memory_order_acquire) || shared.pagePixels.empty())
        return;
    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized() || !ctx.isOperational())
        return;
    {
        std::lock_guard lg(m_spineGpuPagesMutex);
        if (m_spineGpuPages.count(key))
            return;
    }

    // Private pool: command pools are externally synchronised and this runs
    // on a background thread.  Same queue family as SpineRenderer's uploads.
    const auto& families = ctx.device().queueFamilies();
    CommandPool pool;
    if (!pool.create(ctx.vkDevice(), families.graphics.value_or(0)))
        return;
    pool.setQueueMutex(&ctx.graphicsQueueMutex());

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Texture>> pages(shared.pagePixels.size());
    for (size_t pi = 0; pi < shared.pagePixels.size(); ++pi) {
        if (m_shutdown.load(std::memory_order_acquire))
            return;  // shutdown() waits for this thread; device still alive
        if (pi >= shared.pageWidths.size() || pi >= shared.pageHeights.size() ||
            shared.pagePixels[pi].empty() ||
            shared.pageWidths[pi] <= 0 || shared.pageHeights[pi] <= 0)
            return;  // incomplete set: let the renderer path handle it

        // Must match SpineRenderer::uploadAtlasTexture's configuration.
        TextureConfig cfg;
        cfg.width  = static_cast<uint32_t>(shared.pageWidths[pi]);
        cfg.height = static_cast<uint32_t>(shared.pageHeights[pi]);
        cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
        cfg.filter = VK_FILTER_LINEAR;
        auto tex = std::make_shared<Texture>();
        if (!tex->createFromData(ctx.allocator().handle(), ctx.vkDevice(), cfg,
                                 shared.pagePixels[pi].data(),
                                 static_cast<VkDeviceSize>(shared.pagePixels[pi].size()),
                                 pool, ctx.graphicsQueue())) {
            spdlog::warn("[SPINE-GPU] background atlas upload failed for '{}' page {}",
                         key, pi);
            return;
        }
        pages[pi] = std::move(tex);
    }

    {
        std::lock_guard lg(m_spineGpuPagesMutex);
        if (!m_shutdown.load(std::memory_order_acquire))
            m_spineGpuPages.emplace(key, std::move(pages));
    }
    spdlog::info("[SPINE-GPU] pre-uploaded atlas for '{}' ({} pages, {:.1f} ms, background)",
                 key, shared.pagePixels.size(),
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - t0).count());
}

std::vector<std::shared_ptr<Texture>>
CompositeService::spineGpuPages(const std::string& key) const
{
    std::lock_guard lg(m_spineGpuPagesMutex);
    const auto it = m_spineGpuPages.find(key);
    return it != m_spineGpuPages.end() ? it->second
                                       : std::vector<std::shared_ptr<Texture>>{};
}

void CompositeService::storeSpineGpuPages(const std::string& key,
                                          std::vector<std::shared_ptr<Texture>> pages)
{
    std::lock_guard lg(m_spineGpuPagesMutex);
    if (!m_shutdown.load(std::memory_order_acquire))
        m_spineGpuPages.emplace(key, std::move(pages));  // keeps an existing set
}

void CompositeService::removeSpinePendingKey(const std::string& key)
{
    std::lock_guard lg(m_spinePendingMutex);
    m_spinePendingKeys.erase(key);
}

bool CompositeService::isSpinePending(const std::string& key) const
{
    std::lock_guard lg(m_spinePendingMutex);
    return m_spinePendingKeys.count(key) > 0;
}

bool CompositeService::addSpinePendingKey(const std::string& key)
{
    std::lock_guard lg(m_spinePendingMutex);
    if (m_spinePendingKeys.count(key))
        return false;
    m_spinePendingKeys.insert(key);
    return true;
}
#endif


} // namespace rt
