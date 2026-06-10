/*
 * GpuUploadManager — Texture upload orchestration for GPU compositing.
 *
 * Extracted from CompositeServiceGpuOrchestration.cpp (Phase 4.1 of
 * permanent fix plan).  Encapsulates all CPU→GPU texture transfer logic:
 *   - Per-layer texture upload (cacheable + pool paths)
 *   - Mask texture upload
 *   - GpuTextureCache pinning / lookup
 *   - StagingRing interaction
 *   - Staging buffer cleanup tracking
 *
 * Thread safety: NOT thread-safe — must be called from the composite thread.
 */

#pragma once

#include "vulkan/Texture.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward declarations
namespace rt {
class GpuContext;
class GpuTextureCache;
struct LayerInfo;
class StagingRing;

/// Result of a single layer texture upload.
struct GpuUploadResult {
    VkDescriptorImageInfo descriptor{};  ///< GPU descriptor for the uploaded texture
    bool                  success{false}; ///< true if upload succeeded
    bool                  usedRing{false}; ///< true if staging ring was used (no VMA alloc)
    bool                  cacheHit{false}; ///< true if texture was found in GPU cache (no upload)
    bool                  isPacked{false}; ///< packed-alpha flag from cache
    bool                  isPMA{false};    ///< premultiplied-alpha flag from cache
    uint32_t              srcW{0};        ///< source width (after packed-alpha adjustment)
    uint32_t              srcH{0};        ///< source height (after packed-alpha adjustment)
};

class GpuUploadManager {
public:
    /// @param ctx  Application-wide Vulkan context.
    /// @param ring  Persistent staging ring (owned by CompositeService).
    GpuUploadManager(GpuContext& ctx, StagingRing& ring);
    ~GpuUploadManager();

    // Non-copyable, non-movable
    GpuUploadManager(const GpuUploadManager&) = delete;
    GpuUploadManager& operator=(const GpuUploadManager&) = delete;

    // ── Frame lifecycle ────────────────────────────────────────────────

    /// Begin a new composite frame.  Resets the staging ring, unpins only
    /// the textures that were pinned during this slot's previous submission
    /// (safe because the triple-buffered slot's fence was just waited on),
    /// and sets the command buffer that all upload commands will be
    /// recorded into.
    /// @param cmd            The command buffer for this frame's GPU work.
    /// @param submissionSlot The current GpuWorkSubmission ring slot index
    ///                       (0-2).  Used to track pinned textures per-slot
    ///                       so that unpinning only affects textures from
    ///                       this slot's previous submission — textures
    ///                       pinned by other in-flight slots are preserved.
    void beginFrame(VkCommandBuffer cmd, int submissionSlot = 0);

    /// End the current frame — destroys any staging buffers that were
    /// allocated as fallback (ring-buffer allocations are automatically
    /// reclaimed on the next beginFrame).
    void endFrame();

    // ── Layer texture upload ───────────────────────────────────────────

    /// Upload (or cache-lookup) a single layer's texture.
    ///
    /// Handles all three upload paths:
    ///   1. GPU-cache hit (fastest — no upload, just pin + lookup)
    ///   2. Cacheable miss — creates a new cache-owned texture
    ///   3. Pool texture path — reuses a pool texture with dirty tracking
    ///
    /// @param layer       The layer to upload.
    /// @param poolTex     Pool texture to use for non-cacheable layers.
    /// @param poolMediaId [in/out] Dirty-tracking media ID for pool texture.
    /// @param poolFrameNo [in/out] Dirty-tracking frame number for pool texture.
    /// @param scrubMode   True if scrubbing (influences cacheability).
    /// @return Upload result with descriptor and metadata.
    GpuUploadResult uploadLayer(const LayerInfo& layer,
                                Texture& poolTex,
                                uint64_t& poolMediaId,
                                int64_t& poolFrameNo,
                                const void*& poolFramePtr,
                                bool scrubMode);

    // ── Mask texture upload ────────────────────────────────────────────

    /// Upload a mask texture for a layer.
    /// @param maskPixels  The pre-rasterized mask pixel data (BGRA).
    /// @param maskTex     The texture to upload into (reused across frames).
    /// @param outW        Output width (mask dimensions).
    /// @param outH        Output height.
    /// @param outMaskDesc  [out] Descriptor for the uploaded mask texture.
    /// @return false if upload fails.
    bool uploadMask(const std::vector<uint8_t>& maskPixels,
                    Texture& maskTex,
                    uint32_t outW, uint32_t outH,
                    VkDescriptorImageInfo& outMaskDesc);

    // ── Cache management ───────────────────────────────────────────────

    /// Set the GPU texture cache.  May be null (no caching).
    /// Unhooks the recycle callback from any previously-bound cache (it
    /// captures `this`) and arms re-installation on the new cache — without
    /// this, rebinding after a shutdown()/re-init cycle would leave the new
    /// cache without a recycle hook (A4 texture recycling silently off) and
    /// the old cache with a callback into a manager that may outlive it.
    void setTextureCache(GpuTextureCache* cache);

    /// Get the current GPU texture cache (may be null).
    [[nodiscard]] GpuTextureCache* textureCache() const noexcept { return m_texCache; }

    // ── First-upload telemetry (UPGRADE_PLAN Phase 0 D.1) ───────────────
    //
    // Tracks the cost of the "cacheable miss" branch of uploadLayer — i.e.
    // a layer.frame that the GpuTextureCache did NOT have, and so had to be
    // uploaded from CPU pixels before the compositor could sample it. This
    // is exactly the per-cold-frame cost the GPU-resident decode pipeline
    // (Phase 4-5) is designed to eliminate. The counters are process-wide
    // (there is one GpuUploadManager today, but statics keep the perf-
    // surface readable from MediaPoolPerf without plumbing a pointer
    // across subsystems). Reset by MediaPoolPerf::logPerfReport on each
    // emission so the numbers are per-report-window, matching the other
    // counters in that report.
    [[nodiscard]] static uint64_t firstUploadTotalUs() noexcept;
    [[nodiscard]] static uint64_t firstUploadCount()   noexcept;
    static void resetFirstUploadStats() noexcept;

    // ── Shutdown ────────────────────────────────────────────────────────

    /// Release all resources.  Safe to call multiple times.
    void shutdown();

    // ── Accessors ───────────────────────────────────────────────────────

    /// The command buffer being recorded into for the current frame.
    [[nodiscard]] VkCommandBuffer commandBuffer() const noexcept { return m_cmd; }

private:
    /// Try to upload via staging ring first, fall back to batched VMA alloc.
    bool uploadViaRingOrBatched(Texture& tex,
                                const void* data, VkDeviceSize dataSize,
                                uint32_t width, uint32_t height,
                                VkFormat format,
                                VkImageUsageFlags usage,
                                bool isNewTexture);

    /// Try to update via staging ring first, fall back to batched VMA alloc.
    bool updateViaRingOrBatched(Texture& tex,
                                const void* data, VkDeviceSize dataSize);

    GpuContext&           m_ctx;
    StagingRing&          m_ring;
    GpuTextureCache*      m_texCache{nullptr};
    VkCommandBuffer       m_cmd{VK_NULL_HANDLE};

    /// Staging buffers allocated via the batched fallback path.
    /// Destroyed in endFrame().
    std::vector<Texture::StagingCleanup> m_stagingCleanups;

    // ── Per-slot pin tracking ──────────────────────────────────────────
    // GpuWorkSubmission has 3 ring slots.  Each slot tracks which texture
    // keys it pinned.  When beginFrame() is called for a slot, only that
    // slot's previous pins are released — textures pinned by other
    // in-flight slots remain protected from eviction.
    static constexpr int kRingSize = 3;
    // A9: cap on how many textures one ring slot can keep pinned at a
    // time.  Beyond this, the oldest pin in the slot is released to allow
    // the LRU to evict its texture if memory pressure is high.  Premiere
    // / Resolve enforce similar caps (~24-32 active media textures per
    // composite frame).  Pin lifetime is one ring traversal (~3 frames).
    static constexpr size_t kMaxPinsPerSlot = 32;
    struct PinKey {
        uint64_t mediaId;
        int64_t  frameNumber;
        uint8_t  tier;
    };
    std::array<std::vector<PinKey>, kRingSize> m_slotPins;

    int  m_currentSubmissionSlot{0};  // set by beginFrame()
    void recordPin(uint64_t mediaId, int64_t frameNumber, uint8_t tier);
    void releaseSlotPins(int slotIndex);

    // ── A4: Recycled-texture pool ──────────────────────────────────────
    // Each cacheable upload used to vmaCreateImage + vkCreateImageView a
    // fresh Texture (~3-10 allocations per composite frame).  Under heavy
    // scrubbing this churns dozens of VkImages per second.  The pool keeps
    // a small set of textures keyed by (w, h, format, usage) and recycles
    // them when the caller doesn't claim ownership (i.e. when the texture
    // was inserted into GpuTextureCache, not when the cache evicted one).
    //
    // The pool is consulted before vmaCreateImage; on cache eviction the
    // evicted Texture is returned to the pool instead of being destroyed.
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
    // Per-shape FIFO of recycled textures.  Capped at kMaxPoolPerShape
    // so the pool itself doesn't become a memory leak (excess textures
    // are destroyed when the cap is reached).
    static constexpr size_t kMaxPoolPerShape = 8;
    std::unordered_map<PoolKey,
        std::vector<std::unique_ptr<Texture>>, PoolKeyHash> m_texPool;
    bool m_recycleHookInstalled{false};

    /// Pop a recycled texture of the requested shape, or nullptr.
    [[nodiscard]] std::unique_ptr<Texture> acquireFromPool(
        uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);

    /// Return a texture to the pool for later reuse.  Excess textures are
    /// dropped (destructor frees VkImage / VkImageView via VMA).
    void releaseToPool(std::unique_ptr<Texture> tex,
                       uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage);
};

} // namespace rt
