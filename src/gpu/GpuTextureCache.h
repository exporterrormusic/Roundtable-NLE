/*
 * GpuTextureCache — LRU cache of Vulkan textures in VRAM.
 *
 * Stores decoded video frames as GPU textures so that repeated lookups
 * (scrubbing, looping, static layers) skip the CPU→GPU PCIe upload.
 *
 * Keyed by (mediaId, frameNumber, tier).  The cache owns the GPU memory
 * and evicts the least-recently-used textures when VRAM budget is exceeded.
 *
 * Thread safety: all public methods acquire an internal mutex.  Pin/unpin
 * and lookup are O(1) hash lookups; contention is negligible.  Required by
 * the UPGRADE_PLAN GPU-resident decode pipeline, where prefetch workers
 * call putShared() concurrently with the compositor thread calling get().
 */

#pragma once

#include "vulkan/Texture.h"
#include "vulkan/Allocator.h"

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rt {

class GpuTextureCache
{
public:
    /// @param vramBudgetBytes  Maximum VRAM for cached textures.
    ///        Default 4 GB. Half-tier packed-alpha character textures
    ///        are ~4-5 MB each; 4 GB allows ~800-1000 frames before
    ///        LRU eviction begins.
    explicit GpuTextureCache(size_t vramBudgetBytes = 4ULL * 1024 * 1024 * 1024);
    ~GpuTextureCache();

    // Non-copyable
    GpuTextureCache(const GpuTextureCache&) = delete;
    GpuTextureCache& operator=(const GpuTextureCache&) = delete;

    // ── Lookup / Insert ─────────────────────────────────────────────────

    /// Look up a texture by (mediaId, frameNumber, tier).
    /// Returns the descriptor info if cached, or a zeroed struct if not.
    /// Promotes the entry in the LRU on hit.
    struct LookupResult {
        VkDescriptorImageInfo descriptor{};
        bool                  found{false};
        uint32_t              width{0};
        uint32_t              height{0};
        bool                  isPacked{false}; ///< True if texture is packed-alpha (2× height)
        bool                  isPMA{false};    ///< True if texture is premultiplied-alpha
    };

    [[nodiscard]] LookupResult get(uint64_t mediaId, int64_t frameNumber, uint8_t tier);

    /// Insert a texture — takes ownership of `tex`.
    /// Evicts old entries if VRAM budget is exceeded.
    void put(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
             std::unique_ptr<Texture> tex, size_t textureBytes,
             bool isPacked = false, bool isPMA = false,
             bool isLoopFrame = false);

    /// Insert a texture with shared ownership — used for CUDA zero-copy
    /// frames where the CachedFrame (FrameCache) and GpuTexCache co-own
    /// the GPU texture.  This allows dirty-tracking cache hits to skip
    /// the entire decode + FrameCache lookup path.
    void putShared(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
                   std::shared_ptr<void> sharedOwner,
                   VkDescriptorImageInfo descriptor,
                   uint32_t width, uint32_t height,
                   size_t textureBytes,
                   bool isPacked = false, bool isPMA = false,
                   bool isLoopFrame = false);

    // ── Pinning ─────────────────────────────────────────────────────────

    /// Pin a cached texture to prevent eviction.
    /// Call during command buffer recording to protect textures that are
    /// still referenced by in-flight GPU work.  After the fence signals,
    /// unpin() or unpinAll() to release the pin.
    void pin(uint64_t mediaId, int64_t frameNumber, uint8_t tier);

    /// Unpin a previously pinned texture.
    void unpin(uint64_t mediaId, int64_t frameNumber, uint8_t tier);

    /// Unpin ALL entries (safe to call after fence signals since the GPU
    /// is done with all previously-submitted work).
    void unpinAll();

    // ── Maintenance ─────────────────────────────────────────────────────

    /// Evict all entries (skips pinned entries).
    void clear();

    /// Evict all entries for a specific media.
    void evictMedia(uint64_t mediaId);

    /// Set the VRAM budget.  May trigger evictions.
    void setBudget(size_t bytes);

    /// Set the hard ceiling on entry count (UPGRADE_PLAN 2026-05-22).
    ///
    /// Premiere-style bounded working set.  Previously the cache was
    /// byte-budget-only (default 40-60% of VRAM, GBs on a decent GPU),
    /// which meant it accumulated EVERY frame the compositor ever
    /// consumed in a session — a 60-second linear playback would
    /// hoard ~1800 textures (10+ GB) before bytes-pressure fired.
    /// Combined with FrameCache orphan-textures and the per-shape
    /// PrefetchTexturePool reserve, total VMA-tracked VRAM crossed
    /// the OS budget around the 50 s mark and the driver started
    /// paging textures to system RAM — submit latencies jumped
    /// 50-250 ms (perf_log 21:41:53 onward, gpuTexN climbed from 2
    /// to 1469 over 52 s).
    ///
    /// The architectural fix is to treat the GPU cache as a small
    /// bounded recent-frames pool, NOT a "best-effort hoard until
    /// VRAM hits the budget" pool.  DiskFrameCache (already wired)
    /// covers longer-term residency at a cost of ~5 ms re-decode on
    /// scrub-back; the GPU cache only needs to cover the immediate
    /// scrub-back window plus a few frames of pipeline lookahead.
    ///
    /// 0 = uncapped (legacy behaviour).
    void setMaxEntries(size_t maxEntries);

    [[nodiscard]] size_t maxEntries() const;

    // ── Statistics ──────────────────────────────────────────────────────
    //
    // noexcept dropped because std::lock_guard's constructor may throw
    // (std::system_error on a mutex failure).  In practice these calls
    // do not throw; the change is for spec correctness.

    [[nodiscard]] size_t entryCount() const;
    [[nodiscard]] size_t memoryUsed() const;
    [[nodiscard]] size_t budget()     const;
    [[nodiscard]] size_t hits()       const;
    [[nodiscard]] size_t misses()     const;

    /// Returns true if VRAM usage exceeds 90% of budget (pressure state).
    [[nodiscard]] bool isUnderPressure() const;

    /// Returns VRAM usage as a percentage 0-100.
    [[nodiscard]] int usagePercent() const;

    // ── Recycling hook (A4) ────────────────────────────────────────────
    /// When the LRU evicts an entry, invoke this callback with the
    /// unique_ptr<Texture>.  The callback may steal the texture (move-from)
    /// to return it to a recycled-texture pool, avoiding the
    /// vmaCreateImage/vmaDestroyImage churn that fresh allocations cause
    /// during heavy scrubbing.  Whatever the callback leaves in the
    /// unique_ptr is destroyed normally.  Default: null (no recycling).
    ///
    /// The callback runs while m_mtx is held (it fires from evictUntilFits).
    /// Implementations must not re-enter GpuTextureCache.  GpuUploadManager
    /// only pushes to its own pool vector, so this is safe.
    using RecycleFn = std::function<void(std::unique_ptr<Texture>&)>;
    void setRecycleFn(RecycleFn fn);

private:
    struct CacheKey {
        uint64_t mediaId;
        int64_t  frameNumber;
        uint8_t  tier{0};

        bool operator==(const CacheKey& o) const noexcept {
            return mediaId == o.mediaId && frameNumber == o.frameNumber && tier == o.tier;
        }
    };

    struct CacheKeyHash {
        size_t operator()(const CacheKey& k) const noexcept {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.tier) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    using LruList = std::list<CacheKey>;
    using LruIter = LruList::iterator;

    struct Entry {
        std::unique_ptr<Texture> texture;       // Exclusive ownership (CPU-uploaded)
        std::shared_ptr<void>    sharedOwner;   // Shared ownership (CUDA zero-copy)
        VkDescriptorImageInfo    sharedDescriptor{}; // Descriptor for shared textures
        size_t                   bytes{0};
        uint32_t                 width{0};
        uint32_t                 height{0};
        bool                     isPacked{false}; // Packed-alpha (2× height, UV split)
        bool                     isPMA{false};    // Premultiplied-alpha (native alpha video)
        bool                     isLoopFrame{false}; // Belongs to a short looping clip
        int                      pinCount{0};    // >0 prevents eviction
        LruIter                  lruIt;
    };

    /// Requires m_mtx held.  Walks the LRU back-to-front and evicts
    /// entries until VRAM usage + `needed` ≤ budget.  Skips pinned
    /// entries and (on the first pass) loop frames.
    void evictUntilFits(size_t needed);

    /// Serialises every public method.  Marked mutable so the const
    /// statistics accessors can lock.  Recursive locking is forbidden:
    /// internal helpers (evictUntilFits) are documented as
    /// "requires m_mtx held" and must not re-enter via a public method.
    mutable std::mutex m_mtx;

    std::unordered_map<CacheKey, Entry, CacheKeyHash> m_map;
    LruList m_lru;

    size_t m_budget;
    size_t m_used{0};
    // Entry-count ceiling for the Premiere-style bounded working set.
    // Default 120 — at 8 MB / 1080p frame that's ~1 GB max VRAM hostage,
    // safe on any GPU that's running the GPU-resident decode path at
    // all.  Tuned per-system by CachePolicy.  0 = uncapped.
    size_t m_maxEntries{120};
    size_t m_hits{0};
    size_t m_misses{0};

    RecycleFn m_recycleFn;
};

} // namespace rt
