/*
 * UnifiedCache — Coordinator over FrameCache + GpuTextureCache + composite LRU.
 *
 * Per RENDER_GRAPH_PLAN.txt Section G ("Unified Resource Management").
 *
 * Design choice (vs the plan's wholesale-replacement variant): this is a
 * COORDINATOR, not a replacement.  It does not store frames itself.  It
 * tracks a single global generation counter, a playhead window per media
 * handle, and per-key last-access timestamps.  Eviction decisions are
 * delegated to the existing FrameCache + GpuTextureCache via their public
 * APIs (evictMedia, setBudget, pin/unpin).
 *
 * Why a coordinator: the existing caches each have non-trivial Vulkan
 * lifecycle hooks (pin counts, shared ownership with CachedFrame::
 * gpuTextureOwner, fence-driven recycling).  Migrating those into a new
 * data structure has a multi-day validation surface (pixel-equality
 * across 1000-frame timelines, low-VRAM stress).  The coordinator
 * approach delivers the user-visible wins (playhead-window pinning,
 * coordinated CPU↔GPU pressure, transient texture pool, adaptive
 * budgets) without that risk.  When future work warrants the full
 * rewrite, the coordinator's eviction-policy methods become its inner
 * implementation.
 *
 * Thread safety:
 *   - markAccess() / onFrameStart() / setPlayheadWindow() are called from
 *     the FrameProducer thread.
 *   - getStats() is safe from any thread (read-only snapshot).
 *   - Other methods are not thread-safe; call from the composite thread.
 */

#pragma once

#include "FrameCache.h"            // ResolutionTier

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rt {

class FrameCache;
class GpuTextureCache;

class UnifiedCache
{
public:
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

    UnifiedCache();
    ~UnifiedCache();

    UnifiedCache(const UnifiedCache&) = delete;
    UnifiedCache& operator=(const UnifiedCache&) = delete;

    // ── Registration ───────────────────────────────────────────────────

    /// Bind the underlying caches.  Both may be null (early startup); set
    /// once they're available.  This class never owns them.
    void setFrameCache(FrameCache* cache) noexcept   { m_frameCache = cache; }
    void setGpuTexCache(GpuTextureCache* c) noexcept { m_gpuTexCache = c; }

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

    /// Called periodically (every ~3s) by CacheCoordinator to perform
    /// coordinated eviction:
    ///   - Out-of-window cold entries are evicted from the GPU cache.
    ///   - CPU FrameCache entries that no longer have a GPU counterpart
    ///     AND are out of window can be released (their pixels are
    ///     re-decodable from disk via the prefetch workers).
    ///   - Pressure-aware: more aggressive when usage > 80% on either side.
    void runEvictionPass();

    // ── Budget rebalancing (B5) ────────────────────────────────────────

    /// Hit-rate-driven budget rebalance.  Periodically (every 30 frames)
    /// shifts a small fraction of the budget from the lower-hit-rate side
    /// to the higher one.  Bounded changes prevent oscillation.
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
    FrameCache*       m_frameCache{nullptr};
    GpuTextureCache*  m_gpuTexCache{nullptr};

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
    // dominate the composite budget.  3s matches CacheCoordinator's
    // existing pressure-check cadence.
    std::chrono::steady_clock::time_point m_lastEvictionPass{};
    static constexpr auto kEvictionInterval = std::chrono::seconds(3);

    // Throttle the budget rebalance — once every 30 frames is plenty.
    uint64_t m_lastRebalanceGen{0};
    static constexpr uint64_t kRebalanceEveryNFrames = 30;
};

} // namespace rt
