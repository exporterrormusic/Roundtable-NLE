/*
 * MediaPool.cpp â€” manages decoders and shared frame cache
 */

#include "MediaPool.h"
#include "PrefetchTexturePool.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>
#include <chrono>

#include "GpuContext.h"
#include "Nv12Converter.h"
#include "cuda/CudaVulkanInterop.h"

// volk provides Vulkan function pointers (VK_NO_PROTOTYPES is defined globally)
#include <volk.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rt {

namespace {

constexpr int64_t kInteractivePlaybackDeferMs = 2500;

int64_t steadyClockMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void extendInteractivePlaybackWindow(std::atomic<int64_t>& untilMs)
{
    const int64_t candidate = steadyClockMillis() + kInteractivePlaybackDeferMs;
    int64_t current = untilMs.load(std::memory_order_relaxed);
    while (current < candidate &&
           !untilMs.compare_exchange_weak(current, candidate,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
    }
}

} // namespace

// â”€â”€â”€ Packed-alpha unpack helper â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// Unpack a packed-alpha frame in-place.  Packed layout: top half = RGB
/// (A=255), bottom half = alpha as greyscale.  After unpack, the frame
/// Check if a codec name corresponds to ProRes.
static bool isProResCodec(const std::string& codecName)
{
    return codecName.find("prores") != std::string::npos;
}

// â”€â”€â”€ Construction â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

MediaPool::MediaPool(std::shared_ptr<FrameCache> cache)
    : m_cache(cache ? std::move(cache) : std::make_shared<FrameCache>())
    , m_pixelPool(std::make_shared<PixelBufferPool>())
{
    // Configure the scheduler lookahead to match the existing prefetch window.
    m_scheduler.setMaxLookahead(perfProfile().prefetchAheadFrames);
    m_scheduler.setMaxWorkers(perfProfile().prefetchThreadCount);

    // UPGRADE_PLAN Phase 3: try to allocate the GPU-resident texture pool
    // here.  In the typical App startup MediaPool is constructed BEFORE
    // GpuContext::init() runs (App.cpp), so this branch is rarely taken
    // — App calls onGpuContextReady() once init succeeds to retry the
    // allocation.  Keeping the eager attempt for headless test paths
    // that init GpuContext earlier.
    if (GpuContext::get().isInitialized()) {
        m_prefetchTexPool = std::make_unique<PrefetchTexturePool>();
    }

    startPrefetchThread();
    startOpenWorker();
}

void MediaPool::onGpuContextReady()
{
    if (m_prefetchTexPool) return;
    if (!GpuContext::get().isInitialized()) return;
    m_prefetchTexPool = std::make_unique<PrefetchTexturePool>();

    // UPGRADE_PLAN Path C: shared producer-side timeline semaphore.
    // Created once here, owned by MediaPool for the rest of its
    // lifetime, signalled by every convert+copy submission, waited on
    // GPU-side by the compositor.
    if (m_prefetchTimelineSem == 0) {
        VkSemaphoreTypeCreateInfo timelineType{};
        timelineType.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineType.initialValue  = 0;

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semInfo.pNext = &timelineType;

        VkSemaphore sem = VK_NULL_HANDLE;
        const VkResult sr = vkCreateSemaphore(
            GpuContext::get().vkDevice(), &semInfo, nullptr, &sem);
        if (sr == VK_SUCCESS) {
            m_prefetchTimelineSem = reinterpret_cast<uint64_t>(sem);
            spdlog::warn("MediaPool: prefetch timeline semaphore created — "
                         "cross-queue compositor wait now active");
        } else {
            spdlog::warn("MediaPool: failed to create prefetch timeline "
                         "semaphore (vk={}); cross-queue path will fall "
                         "back to per-submit fence wait",
                         static_cast<int>(sr));
        }
    }

    spdlog::warn("MediaPool: PrefetchTexturePool allocated post-GpuContext init "
                 "— GPU-resident prefetch decode path now armed");
}

uint64_t MediaPool::prefetchTimelineSem() const noexcept
{
    return m_prefetchTimelineSem;
}

uint64_t MediaPool::nextPrefetchTimelineValue() noexcept
{
    // fetch_add returns the OLD value; we want the NEW one (so the
    // first signal is value 1, not 0 — value 0 is the initial value
    // of the timeline semaphore and waiting on 0 is a no-op).
    return m_prefetchTimelineValue.fetch_add(1, std::memory_order_relaxed) + 1;
}

MediaPool::~MediaPool()
{
    stopOpenWorker();
    stopPrefetchThread();
    closeAll();

    // Destroy the shared timeline semaphore.  All prefetch work has
    // been joined by stopPrefetchThread() above, and the compositor's
    // composite ring's fences have been drained as part of GpuContext
    // shutdown, so no one can be waiting on this anymore.
    if (m_prefetchTimelineSem != 0 && GpuContext::get().isInitialized()) {
        vkDestroySemaphore(GpuContext::get().vkDevice(),
                           reinterpret_cast<VkSemaphore>(m_prefetchTimelineSem),
                           nullptr);
        m_prefetchTimelineSem = 0;
    }
}
} // namespace rt