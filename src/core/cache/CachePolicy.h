/*
 * CachePolicy — the ONE cache-policy object for Roundtable's three-tier
 * cache stack (CPU FrameCache, GPU GpuTextureCache, DiskFrameCache).
 *
 * Merged from the former CacheCoordinator + UnifiedCache (fable_cleanup.txt
 * §3.3) — two coordinators with overlapping mandates that each owned half
 * the policy.  This class has TWO documented sub-roles:
 *
 * ROLE 1 — BUDGETS & CROSS-CACHE PRESSURE (formerly CacheCoordinator):
 *   System-adaptive budgets, computed at runtime:
 *     FrameCache (CPU RAM):    bounded working set (see the .cpp for the
 *                              architectural history), Boost-profile aware
 *     GpuTextureCache (VRAM):  60% of device-local VRAM (calculated here,
 *                              applied by CompositeEngine)
 *     DiskFrameCache (disk):   5% of free space on cache drive, 4-32 GB
 *   VRAM pressure: the GPU layer (CompositeEngine) registers a callback via
 *   setVramPressureFn(); after each composited frame onFrameCompleted()
 *   polls it and tells FrameCache to release GPU-co-owned frames when the
 *   GPU texture cache is under pressure.  The reverse path
 *   (setGpuBudgetFn) temporarily shrinks the GPU budget when the CPU
 *   FrameCache is under sustained pressure.
 *
 * ROLE 2 — ACCESS TRACKING, PLAYHEAD WINDOWS & EVICTION POLICY (formerly
 * UnifiedCache):
 *   A single global generation counter, a playhead window per media handle
 *   (frames inside the window are pinned against LRU eviction), per-key
 *   last-access generations, a throttled eviction pass, and a hit-rate
 *   budget-rebalance probe.  This side is a COORDINATOR, not a frame
 *   store: eviction decisions are delegated to the existing FrameCache +
 *   GpuTextureCache via their public APIs (evictMedia, setBudget,
 *   pin/unpin) — see the original design note in the .cpp for why
 *   (Vulkan lifecycle hooks make a wholesale replacement high-risk).
 *
 * Thread safety:
 *   - Setters/registration are called once during init (single-threaded).
 *   - onFrameCompleted() / onFrameStart() / markAccess() /
 *     setPlayheadWindow() / runEvictionPass() / rebalanceBudgets() are
 *     called from the composite (FrameProducer) thread.
 *   - Budget queries and stats() are read-only snapshots.
 */

#pragma once

#include "FrameCache.h"            // ResolutionTier

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rt {

class FrameCache;
class DiskFrameCache;
class GpuTextureCache;

class CachePolicy
{
public:
    // ════════════════════════════════════════════════════════════════════
    // ROLE 1 — Budgets & cross-cache pressure
    // ════════════════════════════════════════════════════════════════════

    /// Signature for VRAM pressure check callback.
    /// Returns true when the GPU texture cache is under pressure (>90%).
    /// Returns budget bytes via the out-parameter.
    using VramPressureFn = std::function<bool(size_t* vramBudgetOut)>;

    /// Signature for temporarily reducing the GPU texture cache budget.
    /// Called when the CPU FrameCache is under sustained pressure so the
    /// GPU side gives up some VRAM (which frees the matching shared_ptr
    /// references the CPU cache may be holding), and again to restore the
    /// original budget when pressure subsides.
    using SetGpuBudgetFn = std::function<void(size_t newBudgetBytes)>;

    CachePolicy();
    ~CachePolicy();

    // Non-copyable
    CachePolicy(const CachePolicy&) = delete;
    CachePolicy& operator=(const CachePolicy&) = delete;

    // ── Register caches (called once during init) ──────────────────────────

    /// Register the CPU-side FrameCache.  Sets its capacity to the
    /// recommended budget immediately, and binds it for the access-policy
    /// side (playhead-window forwarding, hit-rate stats).
    void setFrameCache(FrameCache* cache);

    /// Register the disk-backed DiskFrameCache.
    void setDiskCache(DiskFrameCache* cache);

    /// Bind the GPU texture cache for the access-policy side (eviction +
    /// hit-rate stats).  May be null until GPU init; never owned.
    void setGpuTexCache(GpuTextureCache* c) noexcept { m_gpuTexCache = c; }

    /// Re-apply the CPU FrameCache + disk budgets from the current
    /// recommended* values.  Call after the active PerformanceProfile is
    /// installed when that happens AFTER setFrameCache/setDiskCache (the
    /// PerformanceProfile needs GPU VRAM to pick a tier, which is only known
    /// after GPU init — later than cache registration).  No-op for any cache
    /// not yet registered.  GPU texture cache budgets are applied lazily on
    /// first composite, so they always see the installed profile and need no
    /// re-apply here.
    void reapplyBudgets();

    /// Register a callback to check GPU VRAM pressure.
    /// Called from onFrameCompleted().  The callback lives in the GPU layer
    /// (CompositeEngine) which has access to GpuTextureCache.
    void setVramPressureFn(VramPressureFn fn) { m_vramPressureFn = std::move(fn); }

    /// Register a callback to set the GPU texture cache budget.  Used by
    /// the CPU-pressure response path to shrink VRAM allocation when RAM
    /// is full, and to restore the original budget when pressure subsides.
    void setGpuBudgetFn(SetGpuBudgetFn fn) { m_setGpuBudgetFn = std::move(fn); }

    // ── Budget queries (used by factories before caches exist) ─────────────

    /// Recommended CPU RAM budget for FrameCache (bytes).
    [[nodiscard]] size_t recommendedFrameCacheBudget() const noexcept;

    /// Recommended VRAM budget for GpuTextureCache (bytes).
    /// 60% of device-local VRAM.
    /// @param deviceVramBytes  Device-local VRAM reported by VMA.
    [[nodiscard]] size_t recommendedGpuTexCacheBudget(
        size_t deviceVramBytes) const noexcept;

    /// Recommended disk budget for DiskFrameCache (bytes).
    /// 5% of free space on the cache drive, clamped 4-32 GB.
    [[nodiscard]] size_t recommendedDiskCacheBudget() const noexcept;

    /// Recommended hard cap on FrameCache entry count.  Derived from VRAM
    /// budget: each GPU-resident CachedFrame may hold an 8 MB VkImage via
    /// CachedFrame::gpuTextureOwner, so the cap bounds VRAM exposure to
    /// roughly half the GpuTextureCache budget.  Falls back to a safe
    /// default when VRAM is unknown.
    [[nodiscard]] size_t recommendedFrameCacheMaxEntries(
        size_t deviceVramBytes) const noexcept;

    /// Recommended hard cap on GpuTextureCache entry count
    /// (UPGRADE_PLAN 2026-05-22 v3 — Premiere-style bounded working set).
    /// See implementation for the rationale; in short: small number,
    /// not a percentage of VRAM.
    [[nodiscard]] size_t recommendedGpuTexCacheMaxEntries(
        size_t deviceVramBytes) const noexcept;

    // ── Per-frame hook ─────────────────────────────────────────────────────

    /// Called after each composited frame (from the composite thread).
    /// Periodically invokes the VRAM pressure callback and triggers
    /// cross-cache eviction if the GPU texture cache is over 90% full.
    void onFrameCompleted();

    // ── Lifecycle events ───────────────────────────────────────────────────

    /// Called when GPU becomes available (after successful init).
    /// Stores VRAM size for budget queries.
    void onGpuAvailable(size_t deviceVramBytes);

    /// Called when GPU is lost — resets VRAM tracking.
    void onGpuLost();

    /// Log all current budgets and usage to spdlog.
    void logBudgets() const;

    // ════════════════════════════════════════════════════════════════════
    // ROLE 2 — Access tracking, playhead windows & eviction policy
    // ════════════════════════════════════════════════════════════════════

    /// Cache key shared across CPU + GPU tiers.  mediaId is the
    /// MediaPool::MediaHandle (uint64_t — same as the rest of the media
    /// stack; not type-aliased here to avoid a circular include).
    struct Key {
        uint64_t       mediaId{0};
        int64_t        frameNumber{0};
        ResolutionTier tier{ResolutionTier::Full};

        bool operator==(const Key& o) const noexcept {
            return mediaId == o.mediaId && frameNumber == o.frameNumber && tier == o.tier;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.tier))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // ── Generation tracking ────────────────────────────────────────────

    /// Increment the generation counter.  Called once at the start of
    /// each compositeFrame (before layer building).  Generation drives
    /// LRU ordering — most recent generation = most recent access.
    uint64_t onFrameStart() noexcept;

    /// Update the last-access generation for a key.  Called on every
    /// frame fetch (cache hit or miss) so the LRU reflects actual
    /// usage, not just insertion order.
    void markAccess(const Key& key) noexcept;

    /// Returns the most recent access generation, or 0 if never accessed.
    [[nodiscard]] uint64_t lastAccess(const Key& key) const noexcept;

    // ── Playhead window ────────────────────────────────────────────────

    /// Declare the active playback window for a media source.  Frames
    /// inside [playheadFrame - behindCount, playheadFrame + aheadCount]
    /// are pinned against LRU eviction.  Outside the window, normal LRU
    /// applies.  Call once per composite frame for each active media.
    ///
    /// behindCount: small (e.g. 5–10 frames) — recent history for
    ///              backward step / fast-reverse.
    /// aheadCount:  larger (e.g. 30–60 frames) — upcoming playback.
    void setPlayheadWindow(uint64_t mediaId,
                           int64_t playheadFrame,
                           int aheadCount,
                           int behindCount,
                           ResolutionTier tier);

    /// Returns true if the given key is inside any registered playback
    /// window — i.e. should NOT be evicted by background pressure.
    [[nodiscard]] bool isInWindow(const Key& key) const noexcept;

    // ── Eviction coordination ──────────────────────────────────────────

    /// Throttled (~3s) coordinated eviction pass: drops stale playback
    /// windows and prunes the last-access map past its soft cap.  Called
    /// per composite frame; the throttle keeps it off the hot path.
    void runEvictionPass();

    // ── Budget rebalancing (B5) ────────────────────────────────────────

    /// Hit-rate-driven budget rebalance probe.  Periodically (every 30
    /// frames) logs the CPU/GPU hit-rate imbalance; actual budget shifting
    /// stays with the ROLE-1 hard budgets above until the heuristic is
    /// proven out.
    void rebalanceBudgets();

    // ── Stats ──────────────────────────────────────────────────────────

    struct Stats {
        uint64_t generation{0};
        size_t   activeKeys{0};        // last-access map size
        size_t   pinnedWindowCount{0}; // frames currently in any window
        double   cpuHitRate{0.0};      // 0..1
        double   gpuHitRate{0.0};      // 0..1
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    /// Query installed physical RAM (bytes) via OS API.
    static size_t queryTotalPhysicalRam();

    /// Check VRAM pressure and evict co-owned GPU textures from FrameCache.
    void checkVramPressure();

    /// Check CPU FrameCache pressure and shrink GPU budget if over 90%.
    /// Restores the original budget once pressure subsides.
    void checkCpuPressure();

    // ── Shared cache bindings ───────────────────────────────────────────
    FrameCache*      m_frameCache{nullptr};
    DiskFrameCache*  m_diskCache{nullptr};
    GpuTextureCache* m_gpuTexCache{nullptr};

    // ── ROLE 1 state ────────────────────────────────────────────────────
    VramPressureFn   m_vramPressureFn;
    SetGpuBudgetFn   m_setGpuBudgetFn;

    size_t m_totalRam{0};
    size_t m_totalVram{0};
    size_t m_vramBudget{0};

    // CPU-pressure response state: when active, the GPU budget has been
    // temporarily shrunk to relieve cross-cache pressure.  Reverted when
    // CPU usage drops below the low-water mark.
    bool   m_cpuPressureActive{false};

    // Throttle pressure checks to once every N seconds
    std::chrono::steady_clock::time_point m_lastPressureCheck;
    static constexpr auto kPressureInterval = std::chrono::seconds(3);

    // ── ROLE 2 state ────────────────────────────────────────────────────
    // Monotonic generation counter — incremented per composite frame.
    uint64_t m_generation{0};

    // Last-access generation per key.  Sized to ~5000 entries in
    // practice (a few minutes of cached content).  When the map exceeds
    // a soft cap, oldest entries are pruned during runEvictionPass.
    std::unordered_map<Key, uint64_t, KeyHash> m_lastAccess;
    static constexpr size_t kLastAccessSoftCap = 8192;

    // Active playback windows.  One entry per media handle with the
    // current playhead frame and the window extent.  setPlayheadWindow
    // replaces the entry for that mediaId.
    struct Window {
        int64_t        playheadFrame{0};
        int            aheadCount{0};
        int            behindCount{0};
        ResolutionTier tier{ResolutionTier::Full};
        std::chrono::steady_clock::time_point lastUpdate{};
    };
    std::unordered_map<uint64_t, Window> m_windows;

    // Throttle the eviction pass — running it on every frame would
    // dominate the composite budget.  3s matches the ROLE-1
    // pressure-check cadence.
    std::chrono::steady_clock::time_point m_lastEvictionPass{};
    static constexpr auto kEvictionInterval = std::chrono::seconds(3);

    // Throttle the budget rebalance — once every 30 frames is plenty.
    uint64_t m_lastRebalanceGen{0};
    static constexpr uint64_t kRebalanceEveryNFrames = 30;
};

} // namespace rt
