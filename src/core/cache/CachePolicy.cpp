/*
 * CachePolicy.cpp — System-adaptive cache budget + VRAM pressure mgmt.
 */

#include "CachePolicy.h"
#include "GpuTextureCache.h"
#include "FrameCache.h"
#include "DiskFrameCache.h"
#include "PerformanceProfile.h"

#include <spdlog/spdlog.h>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

CachePolicy::CachePolicy()
{
    m_totalRam = queryTotalPhysicalRam();
    m_lastPressureCheck = std::chrono::steady_clock::now();

    spdlog::info("CachePolicy: system RAM = {:.1f} GB",
                 m_totalRam / (1024.0 * 1024.0 * 1024.0));
}

CachePolicy::~CachePolicy() = default;

// ═════════════════════════════════════════════════════════════════════════════
// Cache registration
// ═════════════════════════════════════════════════════════════════════════════

void CachePolicy::setFrameCache(FrameCache* cache)
{
    m_frameCache = cache;
    if (!m_frameCache) return;

    const size_t budget = recommendedFrameCacheBudget();
    m_frameCache->setCapacity(budget);

    spdlog::info("CachePolicy: FrameCache budget = {:.1f} GB ({:.0f}% of RAM)",
                 budget / (1024.0 * 1024.0 * 1024.0),
                 m_totalRam > 0 ? (budget * 100.0 / m_totalRam) : 50.0);
}

void CachePolicy::setDiskCache(DiskFrameCache* cache)
{
    m_diskCache = cache;
    if (!m_diskCache) return;

    const size_t budget = recommendedDiskCacheBudget();
    m_diskCache->setBudget(budget);

    spdlog::info("CachePolicy: DiskFrameCache budget = {:.1f} GB",
                 budget / (1024.0 * 1024.0 * 1024.0));
}

void CachePolicy::reapplyBudgets()
{
    if (m_frameCache) {
        const size_t budget = recommendedFrameCacheBudget();
        m_frameCache->setCapacity(budget);
        spdlog::info("CachePolicy: FrameCache budget re-applied = {:.1f} GB "
                     "(tier-adaptive profile)",
                     budget / (1024.0 * 1024.0 * 1024.0));
    }
    if (m_diskCache) {
        const size_t budget = recommendedDiskCacheBudget();
        m_diskCache->setBudget(budget);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Budget queries
// ═════════════════════════════════════════════════════════════════════════════

size_t CachePolicy::recommendedFrameCacheBudget() const noexcept
{
    constexpr size_t kMiB = 1024ull * 1024ull;
    constexpr size_t kGiB = 1024ull * kMiB;
    constexpr size_t kMin = 256 * kMiB;
    constexpr size_t kMax = 1 * kGiB;

    // Boost override (PerformanceProfile): when set, it is the authoritative
    // budget — the profile already clamped it to a safe fraction of RAM.
    if (const size_t o = perfProfile().frameCacheBudgetBytes; o != 0)
        return o;

    if (m_totalRam == 0) return 512 * kMiB;

    // Architectural fix (2026-05-22): FrameCache now stores ONLY CPU
    // bytes — CachedFrame::memoryUsage() no longer counts GPU texture
    // VRAM, and the compositor transfers sole ownership of GPU textures
    // to GpuTextureCache on first use (see CompositeServiceLayerBuild's
    // putShared + reset sequence).  With that change, a multi-gigabyte
    // FrameCache budget makes no sense — it'd just be a giant CPU-side
    // metadata pool, since GPU-resident frames have empty pixel
    // vectors.
    //
    // Premiere's in-RAM frame cache is on the order of 256 MB – 2 GB
    // depending on project size; the rest of its working set lives on
    // disk (Media Cache Database) or is re-decoded on demand.  Apply
    // the same shape here: 1 GB ceiling, scaled down on RAM-constrained
    // systems.  At 8 MB per inline-decoded 1080p frame this holds
    // ~128 frames of CPU pixels, plus thousands of metadata-only
    // entries for GPU-resident frames whose textures are tracked by
    // GpuTextureCache.
    //
    // Prior values produced two failure modes:
    //   - 50%/clamp[4 GB, 64 GB] (original) → 32 GB on a 64 GB box →
    //     held ~32 GB of co-owned VRAM references → exceeded 24 GB
    //     RTX 4090 → driver paged textures → playback degraded after
    //     ~30 s.
    //   - 12.5%/clamp[2 GB, 8 GB] (intermediate) → 8 GB FrameCache →
    //     same VRAM-coupling bug but hit cap in ~45 s → constant LRU
    //     churn destroyed GPU textures still referenced by in-flight
    //     compositor command buffers → SEH access violation inside
    //     vkQueueSubmit → VK_ERROR_DEVICE_LOST.
    size_t budget = m_totalRam / 64;  // ~1.5% of RAM
    return std::clamp(budget, kMin, kMax);
}

size_t CachePolicy::recommendedGpuTexCacheBudget(
    size_t deviceVramBytes) const noexcept
{
    // ARCHITECTURAL FIX (UPGRADE_PLAN 2026-05-22 v3): Premiere-style
    // BOUNDED WORKING SET, not a percentage-of-VRAM hoard.
    //
    // Earlier iterations (60% → 40% of VRAM, multi-GB budgets) all
    // tried to keep the cache "as large as VRAM allows" while
    // evicting on pressure.  In practice that pattern grows VMA-
    // tracked VRAM unboundedly until the OS budget is hit and the
    // driver starts paging — submit latencies jump 50-250 ms and
    // playback stutters.  Premiere doesn't use 14 GB of VRAM for a
    // single 1080p timeline and neither should we.
    //
    // GpuTextureCache now caps its entry count (see
    // recommendedGpuTexCacheMaxEntries below), which is the actual
    // ceiling.  This byte budget stays in place as a defence-in-
    // depth lower bound — useful for very-low-VRAM machines where
    // even the small entry cap would exceed available headroom.
    //
    // Defaults: 1 GB on any device with > 4 GB VRAM, scaling down
    // proportionally below that.  An 8 GB card only needs to dedicate
    // ~12% of VRAM to the GPU texture cache; everything else lives
    // in DiskFrameCache or gets re-decoded on demand.
    constexpr size_t kMiB = 1024ull * 1024ull;
    constexpr size_t kGiB = 1024ull * kMiB;
    constexpr size_t kMin = 256 * kMiB;
    constexpr size_t kMax = 2 * kGiB;

    // Boost override: profile already clamped to <=45% of VRAM.
    if (const size_t o = perfProfile().gpuTexCacheBudgetBytes; o != 0)
        return o;

    if (deviceVramBytes == 0) return 1 * kGiB;

    // 12% of device VRAM, clamped [256 MB, 2 GB].
    //   24 GB → 2 GB (cap)
    //   12 GB → 1.44 GB → clamped to 2 GB only if >> 12
    //    8 GB → 960 MB
    //    6 GB → 720 MB
    //    4 GB → 480 MB
    //    2 GB → 256 MB (floor)
    size_t budget = deviceVramBytes * 12 / 100;
    return std::clamp(budget, kMin, kMax);
}

size_t CachePolicy::recommendedGpuTexCacheMaxEntries(
    size_t deviceVramBytes) const noexcept
{
    // The Premiere-style bounded working set.  This is the actual
    // ceiling — the byte budget is just a defence-in-depth check.
    //
    // 1080p BGRA texture ≈ 8 MB.  At 30 fps we have ~3-4 seconds of
    // history before LRU evicts.  That's plenty for typical scrub-
    // back interactions; longer scrubs read from DiskFrameCache
    // (~5 ms / frame on NVMe) or re-decode (~10-30 ms / frame H.264
    // hardware).
    //
    // Scaled by VRAM only mildly because the working set is what
    // matters, not the GPU's capacity to hoard:
    //   24 GB → 180 entries (~1.4 GB working set)
    //   12 GB → 150 entries
    //    8 GB → 120 entries (~960 MB)
    //    6 GB →  90 entries
    //    4 GB →  60 entries
    //    2 GB →  40 entries
    constexpr size_t kFloor = 40;
    constexpr size_t kCeil  = 180;
    // Boost override: the profile raises this ceiling per tier (safe —
    // eviction is fence-gated; pressure relief stays armed).
    if (const size_t o = perfProfile().gpuTexMaxEntries; o != 0)
        return o;
    if (deviceVramBytes == 0) return 120;
    const size_t entries = deviceVramBytes / (160ull * 1024 * 1024); // ~160 MB / entry of VRAM headroom
    return std::clamp(entries, kFloor, kCeil);
}

size_t CachePolicy::recommendedFrameCacheMaxEntries(
    size_t deviceVramBytes) const noexcept
{
    // FrameCache holds CachedFrame metadata; entries with non-null
    // gpuTextureOwner pin an 8 MB VkImage until consumed by the
    // compositor (which then transfers ownership to GpuTextureCache).
    // The cap here bounds the orphan-texture VRAM holding while the
    // GpuTextureCache cap (separate, smaller) bounds the consumed-
    // texture working set.
    //
    // Sized to roughly 2× the GpuTextureCache max entries — enough
    // room for the prefetch lookahead window (12 frames) + follow-up
    // batch (30 frames) per active clip + small slack, with most
    // entries being consumed/transferred shortly after insertion.
    //
    //   24 GB VRAM → ~360 entries (~2.8 GB orphan-texture VRAM)
    //    8 GB VRAM → ~240 entries (~1.9 GB)
    //    4 GB VRAM → ~120 entries (~960 MB)
    constexpr size_t kFloor = 100;
    constexpr size_t kCeil  = 400;
    // Boost override: raised ceiling to match the larger GPU working set.
    if (const size_t o = perfProfile().frameCacheMaxEntries; o != 0)
        return o;
    const size_t gpuTexEntries = recommendedGpuTexCacheMaxEntries(deviceVramBytes);
    const size_t entries = gpuTexEntries * 2;
    return std::clamp(entries, kFloor, kCeil);
}

size_t CachePolicy::recommendedDiskCacheBudget() const noexcept
{
    constexpr size_t kGiB = 1024ull * 1024ull * 1024ull;
    constexpr size_t kMin = 4 * kGiB;
    constexpr size_t kMax = 32 * kGiB;

    // Boost override (currently unused — disk is cheap and already adaptive).
    if (const size_t o = perfProfile().diskCacheBudgetBytes; o != 0)
        return o;

    // 5% of free space on the current drive
    std::error_code ec;
    const auto space = std::filesystem::space(".", ec);
    if (ec || space.available == 0)
        return 8 * kGiB;

    size_t budget = static_cast<size_t>(space.available) / 20;  // 5%
    return std::clamp(budget, kMin, kMax);
}

// ═════════════════════════════════════════════════════════════════════════════
// Per-frame hook
// ═════════════════════════════════════════════════════════════════════════════

void CachePolicy::onFrameCompleted()
{
    // Throttle: check pressure at most every kPressureInterval
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastPressureCheck < kPressureInterval)
        return;
    m_lastPressureCheck = now;

    checkVramPressure();
    checkCpuPressure();
}

// ═════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

void CachePolicy::onGpuAvailable(size_t deviceVramBytes)
{
    m_totalVram = deviceVramBytes;
    m_vramBudget = recommendedGpuTexCacheBudget(deviceVramBytes);

    spdlog::info("CachePolicy: GPU VRAM = {:.1f} GB, tex cache budget = {:.1f} GB",
                 deviceVramBytes / (1024.0 * 1024.0 * 1024.0),
                 m_vramBudget / (1024.0 * 1024.0 * 1024.0));

    // Apply the entry-count cap to FrameCache now that VRAM is known.
    // This is the leak ceiling: without it, GPU-resident CachedFrames
    // accumulate by entry count (each 200 bytes, but each pinning an
    // 8 MB VkImage) until VRAM exhausts and ZC collapses to 0 around
    // the 45 s mark on a 24 GB GPU.
    if (m_frameCache) {
        const size_t maxEntries = recommendedFrameCacheMaxEntries(deviceVramBytes);
        m_frameCache->setMaxEntries(maxEntries);
        spdlog::info("CachePolicy: FrameCache max entries = {} "
                     "(bounds GPU-co-owned VRAM exposure)",
                     maxEntries);
    }
}

void CachePolicy::onGpuLost()
{
    m_totalVram = 0;
    m_vramBudget = 0;
}

void CachePolicy::logBudgets() const
{
    spdlog::info("── Cache budgets ──────────────────────────");

    if (m_frameCache) {
        const auto s = m_frameCache->stats();
        spdlog::info("FrameCache:    {:.1f} GB / {:.1f} GB  ({} frames, {:.1f}% hit)",
                     s.memoryUsed / (1024.0 * 1024.0 * 1024.0),
                     s.memoryCapacity / (1024.0 * 1024.0 * 1024.0),
                     s.frameCount, s.hitRate() * 100.0);
    }

    spdlog::info("GpuTexCache:   budget = {:.1f} GB  (applied by CompositeEngine)",
                 m_vramBudget / (1024.0 * 1024.0 * 1024.0));

    spdlog::info("───────────────────────────────────────────");
}

// ═════════════════════════════════════════════════════════════════════════════
// Private helpers
// ═════════════════════════════════════════════════════════════════════════════

size_t CachePolicy::queryTotalPhysicalRam()
{
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        return static_cast<size_t>(ms.ullTotalPhys);
#endif
    return 0;
}

void CachePolicy::checkVramPressure()
{
    if (!m_frameCache)
        return;

    // Ask CompositeEngine (via the registered callback) whether VRAM is
    // currently under pressure.  Post-2026-05-22 the callback's signal
    // is VMA's used/budget ratio rather than the texture cache's
    // self-pressure — see CompositeEngine's setVramPressureFn lambda
    // for rationale.
    size_t cacheBudget = 0;
    if (!m_vramPressureFn || !m_vramPressureFn(&cacheBudget))
        return;

    // ── Response 1: drop FrameCache GPU-co-owned entries ─────────────
    // Each entry pins an 8 MB VkImage via gpuTextureOwner that VMA
    // can't reclaim until FrameCache drops the shared_ptr.  Target
    // 10% of the current texture-cache budget worth of bytes — a
    // proportionate response that doesn't over-evict on a small
    // pressure spike.
    const size_t targetBytes = cacheBudget / 10;
    spdlog::info("CachePolicy: VRAM pressure detected — "
                 "evicting GPU-co-owned frames from FrameCache");
    const size_t freed = m_frameCache->evictGpuCoOwned(targetBytes);
    if (freed > 0) {
        spdlog::info("CachePolicy: freed {:.1f} MB from FrameCache "
                     "(GPU-co-owned)",
                     freed / (1024.0 * 1024.0));
    }

    // ── Response 2: shrink the GpuTextureCache itself ────────────────
    // FrameCache eviction only releases TEXTURES THAT FRAMECACHE STILL
    // OWNS — i.e. textures whose consumers haven't yet transferred sole
    // ownership into GpuTextureCache.  In the 21:41-42 trace those
    // amount to ~4 GB of orphan textures; the other ~10 GB of texture-
    // tracked VRAM lives in GpuTextureCache itself, which the existing
    // pressure path never touched.  Shrink the cache budget by 15% of
    // its current value here; the callback wired by CompositeEngine
    // calls setBudget on the cache, which triggers eviction down to
    // the new ceiling.  The CPU-pressure path's restore-on-relief logic
    // keeps using m_vramBudget, so the original budget remains the
    // ceiling once VMA pressure subsides.
    if (m_setGpuBudgetFn && cacheBudget > 0) {
        constexpr size_t kMinBudget = 512ull * 1024 * 1024;
        const size_t shrunk = std::max(cacheBudget * 85 / 100, kMinBudget);
        if (shrunk < cacheBudget) {
            spdlog::info("CachePolicy: shrinking GpuTextureCache "
                         "budget {:.1f} GB → {:.1f} GB to relieve VMA pressure",
                         cacheBudget / (1024.0 * 1024.0 * 1024.0),
                         shrunk   / (1024.0 * 1024.0 * 1024.0));
            m_setGpuBudgetFn(shrunk);
        }
    }
}

void CachePolicy::checkCpuPressure()
{
    if (!m_frameCache || !m_setGpuBudgetFn || m_vramBudget == 0)
        return;

    const auto s = m_frameCache->stats();
    if (s.memoryCapacity == 0) return;

    const double usage = static_cast<double>(s.memoryUsed) /
                         static_cast<double>(s.memoryCapacity);

    constexpr double kHighWater = 0.90;
    constexpr double kLowWater  = 0.75;

    if (!m_cpuPressureActive && usage >= kHighWater) {
        // CPU cache is full.  Shrink the GPU texture cache to 60% of
        // its normal budget — this evicts cold GPU textures (typically
        // off-screen layers and pre-roll frames) and lets their
        // CachedFrame::gpuTextureOwner shared_ptrs release, which can
        // in turn free CPU memory if the FrameCache was the last owner.
        const size_t shrunk = m_vramBudget * 6 / 10;
        spdlog::warn("CachePolicy: CPU pressure {:.0f}% — shrinking GPU "
                     "tex cache budget {:.1f}GB → {:.1f}GB",
                     usage * 100.0,
                     m_vramBudget / (1024.0 * 1024.0 * 1024.0),
                     shrunk / (1024.0 * 1024.0 * 1024.0));
        m_setGpuBudgetFn(shrunk);
        m_cpuPressureActive = true;
    } else if (m_cpuPressureActive && usage < kLowWater) {
        spdlog::info("CachePolicy: CPU pressure relieved ({:.0f}%) — "
                     "restoring GPU tex cache budget {:.1f}GB",
                     usage * 100.0,
                     m_vramBudget / (1024.0 * 1024.0 * 1024.0));
        m_setGpuBudgetFn(m_vramBudget);
        m_cpuPressureActive = false;
    }
}


// =====================================================================
// ROLE 2 - Access tracking, playhead windows, eviction policy
// (formerly UnifiedCache.cpp)
// =====================================================================

// ─── Generation ────────────────────────────────────────────────────────────

uint64_t CachePolicy::onFrameStart() noexcept
{
    return ++m_generation;
}

void CachePolicy::markAccess(const Key& key) noexcept
{
    m_lastAccess[key] = m_generation;
}

uint64_t CachePolicy::lastAccess(const Key& key) const noexcept
{
    auto it = m_lastAccess.find(key);
    return it == m_lastAccess.end() ? 0ull : it->second;
}

// ─── Playhead window ───────────────────────────────────────────────────────

void CachePolicy::setPlayheadWindow(uint64_t mediaId,
                                     int64_t playheadFrame,
                                     int aheadCount,
                                     int behindCount,
                                     ResolutionTier tier)
{
    Window& w = m_windows[mediaId];
    w.playheadFrame = playheadFrame;
    w.aheadCount    = std::max(0, aheadCount);
    w.behindCount   = std::max(0, behindCount);
    w.tier          = tier;
    w.lastUpdate    = std::chrono::steady_clock::now();

    // Forward to the underlying FrameCache so its eviction pass-1 honors
    // the wider window.  GpuTextureCache doesn't have a per-media playhead
    // concept yet — its pinning is via per-key pin counts (see
    // GpuUploadManager::recordPin).  When the full UnifiedCache rewrite
    // lands (Section G of RENDER_GRAPH_PLAN.txt), both caches share one
    // LRU and this forwarding goes away.
    if (m_frameCache) {
        m_frameCache->setPlayheadWindow(mediaId, playheadFrame,
                                        aheadCount, behindCount);
    }
}

bool CachePolicy::isInWindow(const Key& key) const noexcept
{
    auto it = m_windows.find(key.mediaId);
    if (it == m_windows.end()) return false;
    const Window& w = it->second;
    // Tier is informational only — a Half-tier frame within the window is
    // still pinned even if the window declared Full (lower-tier frames are
    // cheaper to re-decode if needed, but we still prefer to keep them).
    const int64_t lo = w.playheadFrame - w.behindCount;
    const int64_t hi = w.playheadFrame + w.aheadCount;
    return key.frameNumber >= lo && key.frameNumber <= hi;
}

// ─── Eviction coordination ─────────────────────────────────────────────────

void CachePolicy::runEvictionPass()
{
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (now - m_lastEvictionPass < kEvictionInterval)
        return;
    m_lastEvictionPass = now;

    // Drop stale windows (mediaId no longer active for >30s).
    constexpr auto kWindowTtl = seconds(30);
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if (now - it->second.lastUpdate > kWindowTtl) {
            it = m_windows.erase(it);
        } else {
            ++it;
        }
    }

    // Prune the lastAccess map if it has grown beyond the soft cap.
    // Keep only the kLastAccessSoftCap most-recent generations.
    if (m_lastAccess.size() > kLastAccessSoftCap) {
        // Compute a generation threshold: keep top half, drop bottom half.
        // Quick partial sort over generations.
        std::vector<uint64_t> gens;
        gens.reserve(m_lastAccess.size());
        for (auto& [k, g] : m_lastAccess) gens.push_back(g);
        const size_t mid = gens.size() / 2;
        std::nth_element(gens.begin(), gens.begin() + mid, gens.end());
        const uint64_t cutoff = gens[mid];
        for (auto it = m_lastAccess.begin(); it != m_lastAccess.end();) {
            if (it->second < cutoff) {
                it = m_lastAccess.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ─── Budget rebalance (B5) ─────────────────────────────────────────────────

void CachePolicy::rebalanceBudgets()
{
    if (m_generation - m_lastRebalanceGen < kRebalanceEveryNFrames)
        return;
    m_lastRebalanceGen = m_generation;

    // Pure stat snapshot for now — actual budget shifting is left to the
    // ROLE-1 side of this class (it owns CPU + VRAM hard budgets).  This
    // method logs the imbalance so an operator can confirm the heuristic
    // is sensible before we wire it to setCapacity/setBudget.
    if (!m_frameCache || !m_gpuTexCache) return;

    const auto cpuStats = m_frameCache->stats();
    const double cpuHit = cpuStats.hitRate();
    const size_t gpuHits = m_gpuTexCache->hits();
    const size_t gpuMisses = m_gpuTexCache->misses();
    const double gpuHit = (gpuHits + gpuMisses > 0)
        ? static_cast<double>(gpuHits) / static_cast<double>(gpuHits + gpuMisses)
        : 0.0;

    static int s_log = 0;
    if (++s_log % 4 == 0) {  // every ~120 frames at 30 frames-per-rebalance
        spdlog::info("[UNIFIED] gen={} cpuHit={:.1f}% gpuHit={:.1f}% "
                     "lastAccessEntries={} windows={}",
                     m_generation, cpuHit * 100.0, gpuHit * 100.0,
                     m_lastAccess.size(), m_windows.size());
    }
}

CachePolicy::Stats CachePolicy::stats() const noexcept
{
    Stats s;
    s.generation = m_generation;
    s.activeKeys = m_lastAccess.size();

    size_t pinned = 0;
    for (const auto& [mediaId, w] : m_windows) {
        pinned += static_cast<size_t>(w.aheadCount + w.behindCount + 1);
    }
    s.pinnedWindowCount = pinned;

    if (m_frameCache) {
        const auto fs = m_frameCache->stats();
        s.cpuHitRate = fs.hitRate();
    }
    if (m_gpuTexCache) {
        const size_t h = m_gpuTexCache->hits();
        const size_t m = m_gpuTexCache->misses();
        s.gpuHitRate = (h + m > 0)
            ? static_cast<double>(h) / static_cast<double>(h + m) : 0.0;
    }
    return s;
}


} // namespace rt
