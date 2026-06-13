/*
 * CompositeEngine.cpp -- GPU compositing pipeline.
 *
 * Encapsulates the single-submit GPU pipeline: upload -> effects ->
 * transitions -> composite -> readback, plus all associated GPU resources.
 */

// Must come before any header that pulls in vulkan.h so volk can
// define VK_NO_PROTOTYPES first.
#include <volk.h>

#include "CompositeEngine.h"
#include "render_graph/GpuRenderGraph.h"
#include "cache/CachePolicy.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "CompositeServiceBlend.h"       // rasterizeMasks
#include "Compositor.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuWorkSubmission.h"
#include "GpuUploadManager.h"
#include "vulkan/Texture.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"
#include "cache/FrameCache.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "timeline/Transition.h"
#include "timeline/Clip.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

using namespace rt;

// s_useRenderGraph was removed in T3.14 — the DAG path is now the only path.


// ============================================================================
// Lifecycle
// ============================================================================

CompositeEngine::CompositeEngine()
{
    m_compositeLru.resize(kCacheSize);
}

CompositeEngine::~CompositeEngine()
{
    shutdown();
}

void CompositeEngine::init(VkDevice device)
{
    m_device = device;

    // The inter-queue semaphore pool now lives on GpuContext (so the
    // presenter can return semaphores to it across threads).  We don't
    // pre-populate; the first acquire allocates on demand and the pool
    // self-warms within the first 3-4 frames.

    m_stagingRing = std::make_unique<StagingRing>();
    m_uploadManager = std::make_unique<GpuUploadManager>(
        GpuContext::get(), *m_stagingRing);

    initTimingPools(device);
}

// ── GPU timing query pools (per-ring-slot) ──────────────────────────────────

void CompositeEngine::initTimingPools(VkDevice device)
{
    if (device == VK_NULL_HANDLE) return;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(GpuContext::get().physicalDevice(), &props);
    m_timestampPeriodNs = static_cast<double>(props.limits.timestampPeriod);

    // Driver may not support GPU timestamps on the compute queue.  If the
    // valid bits for the compute queue family are 0, we just skip telemetry.
    const auto& families = GpuContext::get().device().queueFamilies();
    const uint32_t computeQF = families.compute.value_or(families.graphics.value_or(0));
    VkQueueFamilyProperties qprops{};
    {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(GpuContext::get().physicalDevice(), &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> all(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(GpuContext::get().physicalDevice(), &qCount, all.data());
        if (computeQF < qCount) qprops = all[computeQF];
    }
    if (qprops.timestampValidBits == 0) {
        spdlog::info("[GPU-TIMING] Compute queue does not support timestamps — skipping telemetry");
        return;
    }

    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = kTimingMarkers;

    for (int i = 0; i < kTimingRingSize; ++i) {
        if (vkCreateQueryPool(device, &qpci, nullptr, &m_timingPools[i]) != VK_SUCCESS) {
            spdlog::warn("[GPU-TIMING] vkCreateQueryPool failed for slot {}", i);
            // Tear down any pools already created and bail out — telemetry
            // is best-effort, never fatal.
            destroyTimingPools();
            return;
        }
        m_timingPoolUsed[i] = false;
    }
    m_timingInitialized = true;
    spdlog::info("[GPU-TIMING] Query pools initialized "
                 "(period={:.2f}ns, ring={})",
                 m_timestampPeriodNs, kTimingRingSize);
}

void CompositeEngine::destroyTimingPools()
{
    for (int i = 0; i < kTimingRingSize; ++i) {
        if (m_timingPools[i] != VK_NULL_HANDLE) {
            vkDestroyQueryPool(m_device, m_timingPools[i], nullptr);
            m_timingPools[i] = VK_NULL_HANDLE;
        }
        m_timingPoolUsed[i] = false;
    }
    m_timingInitialized = false;
}

void CompositeEngine::resolveTimingsForSlot(int slotIdx)
{
    if (!m_timingInitialized || slotIdx < 0 || slotIdx >= kTimingRingSize) return;
    if (!m_timingPoolUsed[slotIdx]) return;  // pool not yet populated

    uint64_t ts[kTimingMarkers]{};
    VkResult r = vkGetQueryPoolResults(
        m_device, m_timingPools[slotIdx],
        0, kTimingMarkers,
        sizeof(ts), ts, sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;  // not ready (driver bug?) — skip

    const double tickToMs = m_timestampPeriodNs * 1e-6;
    GpuStageTimings t;
    t.uploadMs  = static_cast<double>(ts[1] - ts[0]) * tickToMs;
    t.effectMs  = static_cast<double>(ts[2] - ts[1]) * tickToMs;
    t.composeMs = static_cast<double>(ts[3] - ts[2]) * tickToMs;
    t.frameMs   = static_cast<double>(ts[3] - ts[0]) * tickToMs;
    t.valid     = true;
    {
        std::lock_guard l(m_timingMtx);
        m_lastTimings = t;
    }

    static int s_logCounter = 0;
    if (++s_logCounter % 30 == 0) {
        spdlog::info("[GPU-TIMING] upload={:.2f}ms effect={:.2f}ms "
                     "compose={:.2f}ms frame={:.2f}ms",
                     t.uploadMs, t.effectMs, t.composeMs, t.frameMs);
    }
}

VkSemaphore CompositeEngine::acquireFrameSemaphore()
{
    // Delegate to the GpuContext-owned pool so the presenter (VulkanViewport,
    // running on the GUI thread) can return semaphores to the same pool we
    // pull from here on the FrameProducer thread.  The previous design had
    // the pool living locally on CompositeEngine, with the presenter pushing
    // consumed semaphores onto an unrelated `m_recycledSemaphores` vector
    // that was never drained — the result was a ~60 VkSemaphore/sec leak
    // over the whole session.
    return GpuContext::get().acquireBinarySemaphore();
}

void CompositeEngine::releaseFrameSemaphore(VkSemaphore sem)
{
    GpuContext::get().releaseBinarySemaphore(sem);
}

void CompositeEngine::shutdown()
{
    if (m_device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_device);

    // Inter-queue semaphores are owned by GpuContext now and destroyed
    // from GpuContext::shutdown after device waitIdle — see the matching
    // block there.  Nothing to do for them here.

    destroyTimingPools();
    destroyCompositeSlot();
    // Unhook the upload manager from the texture cache BEFORE the cache is
    // destroyed: GpuUploadManager holds a raw GpuTextureCache* and has
    // installed a recycle callback capturing `this`.  If the cache dies
    // first, the manager's destructor calls setRecycleFn(nullptr) on a
    // dangling pointer → Mtx_lock on a destroyed std::mutex (the June-8
    // shutdown ACCESS_VIOLATION).
    if (m_uploadManager)
        m_uploadManager->shutdown();
    clearGpuTexCache();
    m_gpuLayerTextures.clear();
    m_gpuMaskTextures.clear();
    m_layerEffectOutputs.clear();
    m_compositeLru.clear();
    m_stagingRing.reset();
}

// ============================================================================
// LRU cache
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::checkLru(
    int64_t tick, uint32_t w, uint32_t h) const
{
    for (const auto& ce : m_compositeLru) {
        if (ce.frame && ce.frame->gpuReady) continue;
        if (ce.tick == tick && ce.w == w && ce.h == h && ce.frame)
            return ce.frame;
    }
    return nullptr;
}

void CompositeEngine::insertLru(int64_t tick, uint32_t w, uint32_t h,
                                std::shared_ptr<CachedFrame> frame)
{
    if (m_compositeLru.size() < kCacheSize)
        m_compositeLru.push_back({tick, w, h, std::move(frame)});
    else {
        m_compositeLru[m_compositeLruIdx] = {tick, w, h, std::move(frame)};
        m_compositeLruIdx = (m_compositeLruIdx + 1) % kCacheSize;
    }
}

void CompositeEngine::flushLruOnResize(uint32_t w, uint32_t h)
{
    if (!m_compositeLru.empty() &&
        (m_compositeLru.front().w != w || m_compositeLru.front().h != h))
    {
        m_compositeLru.clear();
        m_compositeLru.resize(kCacheSize);
        m_compositeLruIdx = 0;
    }
}

void CompositeEngine::clearLru()
{
    m_compositeLru.clear();
    m_compositeLru.resize(kCacheSize);
    m_compositeLruIdx = 0;
}

// A3: Only invalidate composite entries whose tick falls inside [fromTick, toTick].
// Used by edit commands that mutate a known time range — keeps cached
// frames from the rest of the timeline alive so seeking elsewhere doesn't
// trigger a full re-composite chain.
void CompositeEngine::invalidateLruRange(int64_t fromTick, int64_t toTick)
{
    if (fromTick > toTick) std::swap(fromTick, toTick);
    for (auto& ce : m_compositeLru) {
        if (ce.frame && ce.tick >= fromTick && ce.tick <= toTick) {
            ce.frame.reset();
            ce.tick = -1;
            ce.w = 0;
            ce.h = 0;
        }
    }
}

// ============================================================================
// GPU state
// ============================================================================

bool CompositeEngine::isGpuAvailable() const noexcept
{
    return m_gpuCompositeState > 0;
}

void CompositeEngine::notifyDeviceLost() noexcept
{
    m_gpuCompositeState = -1;
}

// resetBackoff was removed in P2 — see CLAUDE_IMPROVEMENT_PLAN.

// ============================================================================
// Texture cache
// ============================================================================

void CompositeEngine::clearTextureCache()
{
    clearGpuTexCache();
}

void CompositeEngine::invalidateMediaPoolSlots(uint64_t mediaId)
{
    if (mediaId == 0) return;
    size_t reset = 0;
    for (auto& key : m_gpuLayerTexKeys) {
        if (key.mediaId == mediaId) {
            key.mediaId     = 0;
            key.frameNumber = -1;
            key.framePtr    = nullptr;
            ++reset;
        }
    }
    if (reset > 0) {
        spdlog::warn("[LIVE-RELOAD] CompositeEngine: reset {} pool-slot "
                     "dirty-tracking key(s) for mediaId={} — next upload "
                     "will re-stage new pixels", reset, mediaId);
    }
}

void CompositeEngine::clearGpuTexCache()
{
    // Clear the diagnostic pointer BEFORE freeing the cache so any
    // concurrent perf-dump reader sees a null (and skips) rather than
    // dereferencing a dangling pointer.
    GpuContext::get().registerGpuTextureCache(nullptr);
    m_gpuTexCache.reset();
}

int CompositeEngine::vramUsagePercent() const noexcept
{
    return m_gpuTexCache ? m_gpuTexCache->usagePercent() : 0;
}

void CompositeEngine::destroyCompositeSlot()
{
    m_gpuSubmission.reset();
    m_gpuCompositeState = 0;
}

// ============================================================================
// Main GPU compositing entry point
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::composite(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool gpuDisplayMode,
    Compositor* compositor,
    EffectProcessor* effectProcessor,
    TransitionRenderer* transitionRenderer,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    std::chrono::high_resolution_clock::time_point& perfTgpuUp,
    std::chrono::high_resolution_clock::time_point& perfTcomp,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount,
    bool allowLruInsert,
    bool forceSyncReadback)
{
    if (m_gpuCompositeState == 0) {
        m_gpuCompositeState = GpuContext::get().isInitialized() ? 1 : -1;
    }

    // -- GPU device-lost / failed --
    // tryRecover() now transitions to Failed and fires the fatal-failure
    // callback exactly once.  We never resume the GPU path on the same
    // process — the user is expected to restart.  Returning nullptr lets
    // CompositeService fall through to safe-mode CPU compositing.
    {
        auto& gpu = GpuContext::get();
        const GpuState st = gpu.gpuState();
        if (st == GpuState::DeviceLost) {
            spdlog::warn("[COMPOSITE] GPU device lost detected");
            gpu.tryRecover();  // marks Failed + fires fatal callback
            m_gpuCompositeState = -1;
            return nullptr;
        }
        if (st == GpuState::Failed) {
            m_gpuCompositeState = -1;
            return nullptr;
        }
    }

    // P2: GPU backoff retry was deleted.  GPU submit failures are now
    // surfaced as device-lost in the submit path itself (see below).
    if (m_gpuCompositeState != 1 || !compositor || !compositor->isInitialized()) {
        return nullptr;
    }

    // The DAG-based path is now the only path.  The legacy monolithic
    // composite() body (~450 lines of inline command-buffer recording) was
    // deleted after the DAG path reached parity (May 2026).  See
    // RENDER_GRAPH_PLAN.txt Phase 6 / Section I.2 ("Feature Flag Lifecycle
    // — Phase 6+: Remove toggle, delete old code").
    return compositeViaRenderGraph(
        layers, outW, outH, tick, scrubMode, gpuDisplayMode,
        compositor, effectProcessor, transitionRenderer,
        perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp,
        effectLayerCount, effectPassCount, transitionCount,
        allowLruInsert, forceSyncReadback);
}
