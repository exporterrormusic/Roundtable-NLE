/*
 * PrefetchTexturePool — recycled VkImage pool for the GPU-resident prefetch
 * decode path (UPGRADE_PLAN Phase 3).
 *
 * Why this exists: every frame produced by the new convertDecodedToCacheGpu
 * (Phase 4) needs its own destination VkImage so the compositor can sample
 * it later. vmaCreateImage costs ~0.3-1 ms each and fragments VRAM under
 * sustained scrubbing. This pool recycles destroyed-but-not-freed Textures
 * keyed by (w, h, format, usage), mirroring the pattern already in
 * GpuUploadManager::m_texPool.
 *
 * Thread safety: all public methods take an internal mutex. The pool is
 * accessed by prefetch worker threads (multiple) and by the compositor
 * thread (release via shared_ptr deleter, when GpuTextureCache evicts a
 * Phase-5 shared entry).
 *
 * Capacity: kMaxPerShape = 16 (vs GpuUploadManager's 8). Prefetch produces
 * frames sequentially at up to ~30 fps, and PREFETCH_AHEAD_COUNT is 60.
 * 16 per shape avoids churn during cold scroll; the actual VRAM cost is
 * dominated by GpuTextureCache entries, not pool reserve.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace rt {

class Texture;

class PrefetchTexturePool
{
public:
    PrefetchTexturePool() = default;
    ~PrefetchTexturePool();

    PrefetchTexturePool(const PrefetchTexturePool&) = delete;
    PrefetchTexturePool& operator=(const PrefetchTexturePool&) = delete;

    /// Acquire a recycled Texture of the requested shape, or nullptr if
    /// the pool for that shape is empty. The caller is responsible for
    /// creating a fresh Texture on nullptr return.
    [[nodiscard]] std::unique_ptr<Texture> acquire(
        uint32_t width, uint32_t height,
        VkFormat format, VkImageUsageFlags usage);

    /// Return a Texture to the pool for later reuse. If the per-shape
    /// FIFO is at capacity, `tex` is destroyed instead (its destructor
    /// releases VMA memory). Safe to pass nullptr.
    void release(std::unique_ptr<Texture> tex,
                 uint32_t width, uint32_t height,
                 VkFormat format, VkImageUsageFlags usage);

    /// Drop every pooled Texture. Caller must ensure no GPU work is in
    /// flight on these images (vkDeviceWaitIdle, or guaranteed by
    /// shutdown ordering).
    void clear();

    [[nodiscard]] size_t totalEntries() const;

private:
    struct PoolKey {
        uint32_t          width{0};
        uint32_t          height{0};
        VkFormat          format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage{0};
        bool operator==(const PoolKey& o) const noexcept {
            return width == o.width && height == o.height &&
                   format == o.format && usage == o.usage;
        }
    };

    struct PoolKeyHash {
        size_t operator()(const PoolKey& k) const noexcept {
            size_t h = std::hash<uint32_t>{}(k.width);
            h ^= std::hash<uint32_t>{}(k.height) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.format))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.usage)
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Capacity per (w, h, format, usage) bucket.
    //
    // History:
    //  - Originally 16 (mirroring GpuUploadManager's pre-existing
    //    recycled-texture cap).
    //  - Bumped to 64 on 2026-05-22 to absorb steady-state eviction rate
    //    of the (then unbounded) GPU-resident decode path, where ~100 fps
    //    decode + multi-GB caches meant the cap-16 pool overflowed every
    //    frame and destroyed ~30 textures/sec — ~150-300 ms/sec of
    //    vmaDestroyImage on the prefetch worker, sustained stutter at
    //    the 30-60s mark.
    //  - REDUCED to 24 on 2026-05-22 v3 (UPGRADE_PLAN: Premiere-style
    //    bounded working set).  With GpuTextureCache + FrameCache both
    //    capped at small absolute entry counts (~180 / 360 on a 24 GB
    //    card), the eviction rate is much lower in steady state and
    //    64-per-shape reserve was just held-but-unused VRAM (~512 MB
    //    per shape).  24 = PREFETCH_AHEAD_COUNT (12) × 2, enough to
    //    absorb scrub-burst overshoot without overflow.  ~192 MB per
    //    shape at 1080p BGRA — fits any GPU.
    static constexpr size_t kMaxPerShape = 24;

    // A texture released back to the pool whose last GPU use may still be in
    // flight.  Held in quarantine until kQuarantineFrames composite epochs
    // (GpuContext::compositeEpoch) have elapsed since release — by then any
    // submit that referenced it has completed (the compositor is triple-
    // buffered) — then promoted to m_buckets for reuse.  This is what prevents
    // a prefetch convert from overwriting a VkImage the compositor is still
    // sampling (the stale/wrong-frame race on the GPU-resident path — e.g.
    // another clip's frame showing at the top of a packed-alpha character).
    struct Quarantined {
        std::unique_ptr<Texture> tex;
        uint64_t                 releaseEpoch{0};
    };

    // In-flight depth is kRingSize (3) in GpuUploadManager; quarantine one
    // extra frame for margin.
    static constexpr uint64_t kQuarantineFrames = 4;

    /// Requires m_mtx held.  Promote quarantined textures for `key` whose
    /// quarantine has elapsed (relative to `now`) into the reusable bucket,
    /// capping the bucket at kMaxPerShape and safely destroying any excess
    /// (excess is past-quarantine, so destruction cannot race the GPU).
    void sweepLocked(const PoolKey& key, uint64_t now);

    mutable std::mutex m_mtx;
    std::unordered_map<PoolKey,
        std::vector<std::unique_ptr<Texture>>, PoolKeyHash> m_buckets;
    std::unordered_map<PoolKey,
        std::vector<Quarantined>, PoolKeyHash> m_quarantine;
};

// ── Helper: create-or-acquire a pooled Texture wrapped in a shared_ptr ──
//
// On call: tries pool.acquire(); on miss, creates a fresh Texture via
// Texture::create(). On success, returns a shared_ptr<Texture> whose
// custom deleter returns the Texture to `pool` instead of destroying it.
// Returns nullptr on Vulkan allocation failure.
//
// `concurrentQueueFamilies` is forwarded into TextureConfig — pass
// {computeFamily, graphicsFamily} so the resulting image uses
// VK_SHARING_MODE_CONCURRENT and can be written by the prefetch worker's
// compute submission and sampled by the compositor's graphics submission
// without an explicit queue-ownership-transfer barrier.
//
// Lifetime invariant: `pool` must outlive every shared_ptr<Texture>
// returned.  MediaPool owns the pool and ALL containers that hold the
// returned shared_ptrs (via CachedFrame::gpuTextureOwner):
//   • FrameCache       (m_cache)
//   • DiskFrameCache   (m_diskCache, including its write-behind queue
//                       drained on the writer thread during shutdown)
//   • GpuTextureCache  (held by CompositeEngine, not by MediaPool — but
//                       cleared in CompositeService::shutdown(), which
//                       App.cpp invokes before MediaPool is destroyed)
//
// MediaPool's member layout puts m_prefetchTexPool BEFORE m_cache and
// m_diskCache so reverse-order destruction destroys the pool LAST,
// after every shared_ptr<Texture> it ever issued has been released.
// See the DECLARATION-ORDER NOTE in MediaPool.h.
class GpuContext;

std::shared_ptr<Texture> makePooledTexture(
    PrefetchTexturePool&       pool,
    GpuContext&                ctx,
    uint32_t                   width,
    uint32_t                   height,
    VkFormat                   format,
    VkImageUsageFlags          usage,
    std::vector<uint32_t>      concurrentQueueFamilies = {});

} // namespace rt
