/*
 * MediaPoolPerf.cpp — Performance metrics logging for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuUploadManager.h"
#include "cache/PrefetchTexturePool.h"
#include "playback/FrameProducer.h"
#include "cuda/CudaVulkanInterop.h"

#include <spdlog/spdlog.h>

namespace rt {

// ─── prefetchStats ──────────────────────────────────────────────────────────

MediaPool::PrefetchStats MediaPool::prefetchStats() const
{
    PrefetchStats out;
    {
        std::lock_guard lk(m_prefetchMutex);
        out.queueDepth   = m_prefetchQueue.size();
        out.ownedHandles = m_prefetchPackedOwner.size();
    }
    // m_scrubDecoders is touched on the UI thread but the perf report
    // runs on the FrameProducer thread; a stale read here is acceptable
    // — the count is purely diagnostic and we don't want to add UI-thread
    // contention to a logging path.
    out.scrubDecoders = m_scrubDecoders.size();
    return out;
}

// ─── Performance report ─────────────────────────────────────────────────────

void MediaPool::logPerfReport()
{
    auto hits     = m_perf.cacheHits.exchange(0, std::memory_order_relaxed);
    auto nearby   = m_perf.nearbyHits.exchange(0, std::memory_order_relaxed);
    auto stale    = m_perf.staleReturns.exchange(0, std::memory_order_relaxed);
    auto inlined  = m_perf.inlineDecodes.exchange(0, std::memory_order_relaxed);
    auto prefetch = m_perf.prefetchDeliveries.exchange(0, std::memory_order_relaxed);
    auto misses   = m_perf.totalMisses.exchange(0, std::memory_order_relaxed);
    auto total    = m_perf.totalRequests.exchange(0, std::memory_order_relaxed);
    auto sched    = m_perf.prefetchScheduled.exchange(0, std::memory_order_relaxed);
    auto avgUs    = m_perf.avgDecodeUs.load(std::memory_order_relaxed);

    if (total == 0) return; // nothing to report

    double hitPct   = total > 0 ? (hits * 100.0 / total) : 0.0;
    double dropPct  = total > 0 ? ((nearby + stale + misses) * 100.0 / total) : 0.0;

    auto cacheStats = m_cache->stats();

    spdlog::info("====== [PERF] MediaPool Frame Delivery Report ======");
    spdlog::info("  Total requests:      {}", total);
    spdlog::info("  Cache hits (exact):  {} ({:.1f}%)", hits, hitPct);
    spdlog::info("  Nearby-frame drops:  {}", nearby);
    spdlog::info("  Stale-frame returns: {}", stale);
    spdlog::info("  Inline decodes:      {}", inlined);
    spdlog::info("  Total misses (null): {}", misses);
    spdlog::info("  Frame drop rate:     {:.1f}%", dropPct);
    spdlog::info("  Prefetch scheduled:  {} tasks", sched);
    spdlog::info("  Prefetch delivered:  {} frames", prefetch);
    spdlog::info("  Avg decode time:     {} us", avgUs);
    spdlog::info("  FrameCache: {} entries, {:.0f}/{:.0f} MB, hit={:.1f}%",
                 cacheStats.frameCount,
                 cacheStats.memoryUsed / 1048576.0,
                 cacheStats.memoryCapacity / 1048576.0,
                 cacheStats.hitRate() * 100.0);

    // UPGRADE_PLAN Phase 0 D.1: cold-frame composite-upload cost.  Should
    // drop sharply once the GPU-resident decode path (Phase 4-5) lands.
    const uint64_t firstUploadUs    = GpuUploadManager::firstUploadTotalUs();
    const uint64_t firstUploadCount = GpuUploadManager::firstUploadCount();
    if (firstUploadCount > 0) {
        spdlog::info("  GPU first uploads:   {} ({:.2f} ms total, {:.2f} ms avg)",
                     firstUploadCount,
                     firstUploadUs / 1000.0,
                     (firstUploadUs / 1000.0) / firstUploadCount);
    } else {
        spdlog::info("  GPU first uploads:   0");
    }
    GpuUploadManager::resetFirstUploadStats();

    // UPGRADE_PLAN Phase 9: dispatch ratio so an operator can see at a
    // glance whether GPU-resident is actually firing.  Bumped to warn so
    // it survives the warn+ logger filter — these counters are the main
    // signal that the pipeline change is doing what it claims.
    const uint64_t gpuResident = m_perf.gpuResidentDecoded.exchange(0, std::memory_order_relaxed);
    const uint64_t zeroCopy    = m_perf.zeroCopyDecoded.exchange(0, std::memory_order_relaxed);
    const uint64_t cpuConvert  = m_perf.cpuConvertDecoded.exchange(0, std::memory_order_relaxed);
    const uint64_t convertTotal = gpuResident + zeroCopy + cpuConvert;
    if (convertTotal > 0) {
        const double gpuPct = 100.0 * (gpuResident + zeroCopy) / convertTotal;
        const double zcPct  = 100.0 * zeroCopy / convertTotal;
        spdlog::warn("[UPGRADE_PLAN PERF] decode dispatch: GPU={} ZC={} CPU={} "
                     "({:.1f}% GPU-resident, {:.1f}% zero-copy) | "
                     "first-uploads={} ({:.2f}ms avg)",
                     gpuResident, zeroCopy, cpuConvert, gpuPct, zcPct,
                     firstUploadCount,
                     firstUploadCount > 0
                         ? (firstUploadUs / 1000.0) / firstUploadCount
                         : 0.0);
    }

    // ── [CACHE-DUMP] warn-level cache-size snapshot ─────────────────
    //
    // Sustained growth in any of these counters across consecutive
    // reports while playback is steady-state points to a leak.
    // Specifically:
    //   - frameMB → FrameCache (CPU pixel pool) growing past its cap
    //   - gpuTexMB / gpuTexN → GpuTextureCache (VRAM) growing past its
    //                          budget; eviction stalled or budget too high
    //   - prefetchQ → MediaPool's prefetch task queue (capped at 32/64;
    //                  if pegged at cap, prefetch is backlogged)
    //   - zcLive → in-flight CUDA↔Vulkan SharedAllocations.  Should
    //               match in-flight prefetch work (≤ 2 NVDEC workers ×
    //               kMaxPendingPerWorker=3 = 6).  Growth past that is
    //               the canonical leak the new mutex fix targets.
    //   - zcPool → idle SharedAllocations awaiting reuse (capped at
    //               pool capacity, typically 16 on 24 GB VRAM).
    //   - scrubDec → MediaPool::m_scrubDecoders (one per handle ever
    //                 scrubbed; should plateau at clip count).
    //
    // Single line, warn level so it survives the warn+ logger filter.
    PrefetchStats pf = prefetchStats();
    size_t gpuTexBytes = 0, gpuTexEntries = 0, gpuTexBudget = 0;
    size_t zcLive = 0, zcPool = 0;
    auto& ctx = GpuContext::get();
    if (ctx.isInitialized()) {
        if (auto* interop = ctx.cudaVulkanInterop()) {
            auto s = interop->stats();
            zcLive = s.live;
            zcPool = s.pooled;
        }
        // GpuTextureCache lives in CompositeEngine; CompositeEngine
        // registers it with GpuContext on creation for diagnostic
        // visibility.  Surfaced in CACHE-DUMP so an operator can spot
        // VRAM growth in the GPU cache as a cause of submit-slow
        // regressions that show layers=0 submit=high (the 49 s
        // pattern in 21:14:09+ perf logs).
        if (auto* gtex = ctx.gpuTextureCacheDiag()) {
            gpuTexBytes   = gtex->memoryUsed();
            gpuTexEntries = gtex->entryCount();
            gpuTexBudget  = gtex->budget();
        }
    }

    // PrefetchTexturePool: count of recycled VkImages awaiting reuse
    // across all (W,H,format,usage) buckets.  Each pooled texture is
    // ~8 MB VRAM at 1080p BGRA, so 100 pooled = ~800 MB held aside for
    // recycling.  Growth past kMaxPerShape × shape-count means the
    // pool overflow path is destroying textures every frame
    // (vmaDestroyImage) — the symptom the kMaxPerShape=64 bump
    // targeted.
    size_t poolEntries = m_prefetchTexPool ? m_prefetchTexPool->totalEntries() : 0;

    // UPGRADE_PLAN Path C (2026-05-22): per-family submission counts.
    // gx (graphics) should be advancing once Path C is live — every
    // composite frame submits there.  cmp (compute) advances for every
    // prefetch convert+copy.  A perf log that shows gx growing in step
    // with the composite rate confirms the queue split is firing.  If
    // gx stays at zero while playback is active, something redirected
    // the composite submission back to compute and Path C isn't
    // engaged.  Counters are running totals since GpuScheduler init.
    uint64_t schedGx = 0, schedCmp = 0, schedXfer = 0;
    if (ctx.isInitialized() && ctx.scheduler().isInitialized()) {
        schedGx   = ctx.scheduler().submissionsOn(GpuQueueKind::Graphics);
        schedCmp  = ctx.scheduler().submissionsOn(GpuQueueKind::Compute);
        schedXfer = ctx.scheduler().submissionsOn(GpuQueueKind::Transfer);
    }

    // gpuOwn = entries in FrameCache that still hold a gpuTextureOwner
    // shared_ptr — each ≈ 8 MB VRAM held outside GpuTextureCache.  If
    // this counter climbs unbounded while playback is steady, the
    // entry-cap eviction is not keeping up.  frameN ≤ frameNCap is
    // the invariant the entry-count cap enforces.
    const size_t frameNCap = m_cache ? m_cache->maxEntries() : 0;
    const size_t gpuOwn    = m_cache ? m_cache->gpuCoOwnedCount() : 0;

    // diskQ = DiskFrameCache write-behind queue depth.  Each entry
    // pins a shared_ptr<CachedFrame> whose lazyReadback captures the
    // source GPU texture — so this is effectively "GPU textures
    // pinned by the disk writer".  Capped at DiskFrameCache::
    // kMaxWriteQueue; the cap drops oldest on overflow to bound VRAM.
    const size_t diskQ = m_diskCache ? m_diskCache->writeQueueSize() : 0;

    // VMA snapshot — total device-local allocation usage tracked by the
    // allocator.  If gpuTex stays well below its budget but vmaUsedMB
    // climbs anyway, fragmentation or other VMA pools (non-cache
    // allocations) are the leak source.
    //
    // vmaAllocs:   total VMA allocation count.  If this grows while
    //              gpuTexN / texPool / frameN are stable, something
    //              outside our caches is allocating GPU resources
    //              without releasing them.
    // vmaBlocksMB: VMA's TOTAL block memory reserved from the driver
    //              (deviceLocalUsedBytes only counts live allocations).
    //              If vmaBlocksMB >> vmaMB, VMA is holding onto big
    //              memory blocks but they have lots of free space —
    //              fragmentation.
    size_t vmaUsed = 0, vmaBudget = 0, vmaBlocks = 0;
    uint32_t vmaAllocs = 0;
    if (ctx.isInitialized()) {
        auto m = ctx.allocator().queryStats();
        vmaUsed   = m.deviceLocalUsedBytes;
        vmaBudget = m.deviceLocalBudgetBytes;
        vmaBlocks = m.totalAllocatedBytes;
        vmaAllocs = m.allocationCount;
    }

    // UPGRADE_PLAN item 1 STEP 3: adaptive playback resolution tier.
    // divisor   = the FrameProducer's current effective divisor (1, 2, 4
    //             or 8 — only 1/2/4 are reachable from adaptive logic;
    //             8 only via the manual dropdown).
    // adaptive  = '*' when the producer is auto-managing the divisor,
    //             ' ' for manual mode.
    int  prodDivisor = 0;
    char adaptiveMark = ' ';
    if (auto* prod = FrameProducer::activeForDiag()) {
        prodDivisor  = prod->currentDivisor();
        adaptiveMark = prod->isAdaptiveEnabled() ? '*' : ' ';
    }

    spdlog::warn("[CACHE-DUMP] frameMB={:.0f}/{:.0f} frameN={}/{} gpuOwn={} "
                 "gpuTexMB={:.0f}/{:.0f} gpuTexN={} "
                 "vmaMB={:.0f}/{:.0f} vmaAllocs={} vmaBlocksMB={:.0f} "
                 "prefetchQ={} ownedH={} scrubDec={} diskQ={} "
                 "zcLive={} zcPool={} texPool={} "
                 "submits[gx={} cmp={} xfer={}] "
                 "tier=1/{}{}",
                 cacheStats.memoryUsed / 1048576.0,
                 cacheStats.memoryCapacity / 1048576.0,
                 cacheStats.frameCount, frameNCap, gpuOwn,
                 gpuTexBytes / 1048576.0, gpuTexBudget / 1048576.0,
                 gpuTexEntries,
                 vmaUsed / 1048576.0, vmaBudget / 1048576.0,
                 vmaAllocs, vmaBlocks / 1048576.0,
                 pf.queueDepth, pf.ownedHandles, pf.scrubDecoders, diskQ,
                 zcLive, zcPool, poolEntries,
                 schedGx, schedCmp, schedXfer,
                 prodDivisor, adaptiveMark);

    spdlog::info("====================================================");
}

} // namespace rt
