/*
 * CacheCoordinator — Unified cache management for Roundtable NLE.
 *
 * Coordinates up to three caches (CPU FrameCache, DiskFrameCache) with
 * system-adaptive budgets and a VRAM pressure hook.
 *
 * Budget philosophy (all computed at runtime — no static waste):
 *   FrameCache (CPU RAM):   50% of installed physical RAM
 *   GpuTextureCache (VRAM): 60% of device-local VRAM (calculated here,
 *                            applied by CompositeEngine)
 *   DiskFrameCache (disk):   5% of free space on cache drive, 4-32 GB
 *
 * VRAM pressure monitoring:
 *   The GPU layer (CompositeEngine) registers a callback via
 *   setVramPressureFn().  After each composited frame, onFrameCompleted()
 *   calls this callback.  If it returns true (under pressure), the
 *   coordinator tells FrameCache to evict GPU-co-owned frames, freeing
 *   VRAM indirectly via shared_ptr release.
 *
 * Thread safety:
 *   All setters are called once during init (single-threaded).
 *   onFrameCompleted() is called from the composite thread.
 *   Budget queries are read-only after init.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>

namespace rt {

class FrameCache;
class DiskFrameCache;

class CacheCoordinator
{
public:
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

    CacheCoordinator();
    ~CacheCoordinator();

    // Non-copyable
    CacheCoordinator(const CacheCoordinator&) = delete;
    CacheCoordinator& operator=(const CacheCoordinator&) = delete;

    // ── Register caches (called once during init) ──────────────────────────

    /// Register the CPU-side FrameCache.  Sets its capacity to the
    /// recommended budget immediately.
    void setFrameCache(FrameCache* cache);

    /// Register the disk-backed DiskFrameCache.
    void setDiskCache(DiskFrameCache* cache);

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
    /// 50% of installed physical RAM.
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

private:
    /// Query installed physical RAM (bytes) via OS API.
    static size_t queryTotalPhysicalRam();

    /// Check VRAM pressure and evict co-owned GPU textures from FrameCache.
    void checkVramPressure();

    /// Check CPU FrameCache pressure and shrink GPU budget if over 90%.
    /// Restores the original budget once pressure subsides.
    void checkCpuPressure();

    FrameCache*      m_frameCache{nullptr};
    DiskFrameCache*  m_diskCache{nullptr};

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
};

} // namespace rt
