/*
 * FrameCache — LRU cache for decoded video frames.
 *
 * Stores decoded frames in CPU memory with configurable capacity.
 * Supports multi-resolution (full + proxy) caching.
 * Thread-safe for concurrent producer/consumer access.
 *
 * Cache key: (mediaId, frameNumber, resolution).
 * Eviction: LRU with optional priority hints (keyframes kept longer).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rt {

/// Zero out the RGB channels of fully-transparent (A==0) pixels in-place.
/// FFmpeg sws_scale leaves non-zero RGB in transparent regions; GPU linear
/// texture filtering would bleed those colours into visible edges.
///
/// Uses branchless 32-bit masking: if alpha==0 the entire pixel becomes
/// 0x00000000; otherwise the pixel is kept unchanged.  This eliminates
/// the per-pixel branch that was causing ~4ms stalls on 1080p frames.
inline void clearTransparentPixelRGB(uint8_t* pixels, size_t pixelCount)
{
    auto* p = reinterpret_cast<uint32_t*>(pixels);
    for (size_t i = 0; i < pixelCount; ++i) {
        // BGRA layout: byte 3 is alpha.  Build a mask that is 0xFFFFFFFF
        // when alpha != 0 and 0x00000000 when alpha == 0.
        uint32_t alpha = (p[i] >> 24) & 0xFF;
        // branchless: -!!alpha produces 0xFFFFFFFF when alpha>0, 0 when 0
        uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(!!alpha));
        p[i] &= mask;
    }
}

/// Chroma-key green pixels to transparent in-place.
/// Key color is #18FF00 (R=0x18, G=0xFF, B=0x00).
///
/// Uses a green-dominance test rather than per-channel distance so it
/// handles H.264 4:2:0 chroma subsampling gracefully — chroma planes
/// are at 1/4 resolution, so a single chroma sample bleeds across a
/// 2×2 block and produces heavy variance at object edges.
///
/// Two-stage key:
///   1. Hard key:  G dominates both R and B → alpha=0 (fully transparent)
///   2. Spill suppression: G is elevated but not dominant → desaturate G,
///      push R/B toward their original values (reduces green fringe)
inline void chromaKeyInPlace(uint8_t* pixels, size_t pixelCount)
{

    auto* p = reinterpret_cast<uint32_t*>(pixels);
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint32_t px = p[i];
        // BGRA layout
        const int b = static_cast<int>( px        & 0xFF);
        const int g = static_cast<int>((px >>  8) & 0xFF);
        const int r = static_cast<int>((px >> 16) & 0xFF);
        const int a = static_cast<int>((px >> 24) & 0xFF);

        // ── Hard key: green is clearly dominant ──────────────────────
        // H.264 4:2:0 chroma subsampling means the green channel can
        // bleed into the red/blue of edge pixels.  Require G to be
        // substantially ahead of both R and B, with a minimum brightness
        // floor so dark/shadow areas don't false-positive.
        if (g > 80 && g > r + 40 && g > b + 40) {
            p[i] = 0x00000000;  // fully transparent black
            continue;
        }

        // ── Spill suppression: green cast but not dominant ───────────
        // Semi-transparent edge pixels (hair, motion blur) pick up a
        // green tint from the background.  Pull G down toward the
        // average of R and B, weighted by how much G exceeds them.
        if (g > 60 && g > r && g > b && a > 0) {
            const int maxRB = (r > b) ? r : b;
            const int excess = g - maxRB;
            if (excess > 10) {
                // Clamp G down: newG = maxRB + 25% of excess
                const int newG = maxRB + excess / 4;
                const int gClamped = (newG < 0) ? 0 : (newG > 255 ? 255 : newG);
                p[i] = (static_cast<uint32_t>(a) << 24)
                     | (static_cast<uint32_t>(r) << 16)
                     | (static_cast<uint32_t>(gClamped) << 8)
                     | static_cast<uint32_t>(b);
            }
        }
    }
}

/// Resolution tier for multi-resolution caching
enum class ResolutionTier : uint8_t
{
    Full,       // Original resolution
    Half,       // 50% resolution (proxy)
    Quarter,    // 25% resolution (thumbnail)
};

/// Which conversion path produced a CachedFrame's pixels/texture.
/// Used by the [FLICKER-DIAG] consumption-side check (Phase 0 of the
/// pipeline upgrade): if the displayed frame for a clip switches origin
/// frame-to-frame during steady playback, the GPU and CPU paths are NOT
/// byte-identical and the difference shows as brightness/edge flicker.
enum class ConverterOrigin : uint8_t
{
    Unknown = 0,
    GpuZeroCopy,   ///< NVDEC CUDA buffer → Vulkan compute shader (no CPU bounce)
    GpuShader,     ///< CPU-transferred frame → Vulkan compute shader convert
    CpuSws,        ///< sws_scale on a prefetch/scrub/on-demand worker
    CpuLoop,       ///< sws_scale in the loop pre-decode worker
};

inline const char* converterOriginName(ConverterOrigin o)
{
    switch (o) {
        case ConverterOrigin::GpuZeroCopy: return "GpuZeroCopy";
        case ConverterOrigin::GpuShader:   return "GpuShader";
        case ConverterOrigin::CpuSws:      return "CpuSws";
        case ConverterOrigin::CpuLoop:     return "CpuLoop";
        default:                           return "Unknown";
    }
}

/// A cached frame — owns its pixel data
struct CachedFrame
{
    uint64_t    mediaId{0};     // Which media file this came from
    int64_t     frameNumber{0}; // Frame index in the source
    uint32_t    width{0};
    uint32_t    height{0};
    uint32_t    stride{0};      // Bytes per row
    ResolutionTier tier{ResolutionTier::Full};
    bool        isKeyframe{false};
    double      timestamp{0.0}; // PTS in seconds

    /// Which conversion path produced this frame (Phase 0 diagnostics).
    ConverterOrigin origin{ConverterOrigin::Unknown};

    std::vector<uint8_t> pixels; // BGRA pixel data (pre-converted)

    /// True when this frame was decoded from a packed-alpha video and has
    /// already been unpacked (top-half RGB + bottom-half alpha → BGRA).
    /// Downstream code checks this to avoid double-unpacking.
    bool     unpackedAlpha{false};

    /// True when the BGRA pixels have been pre-multiplied by alpha.
    /// Native-alpha video (ProRes 4444, VP9+alpha) is stored as PMA so
    /// that GPU linear-filtering doesn't bleed opaque RGB into transparent
    /// edge pixels (the classic "white fringe" artefact).
    bool     premultipliedAlpha{false};

    /// True when this frame must NOT be evicted by the LRU policy.
    /// Used for static images (PNGs, single-frame media) where re-decoding
    /// triggers a sync prefetch round-trip and a visible "missing layer"
    /// frame in the compositor.  Pinned frames are skipped by both eviction
    /// passes; the only way to remove them is evictMedia() (handle close).
    bool     pinned{false};

    /// True when this frame belongs to a looping short clip (character
    /// animation, etc.).  Looping frames must NOT be evicted just because
    /// the media playhead has advanced past them — the loop will wrap
    /// right back around.  Pass-1 (behind-playhead) eviction skips these.
    /// Pass-2 (pure LRU) still applies when the cache is genuinely full.
    bool     isLoopFrame{false};

    // ── GPU-resident frame (zero-copy display) ──────────────────────────
    // When gpuReady is true, the compositor output can be displayed
    // directly via VulkanViewport without CPU readback.  Pixels may be empty.
    // These are Vulkan non-dispatchable handles stored as uint64_t to avoid
    // pulling vulkan.h into the core library.  Cast to VkImageView / VkSampler
    // in GPU code.
    uint64_t gpuImageView{0};   // VkImageView handle
    uint64_t gpuSampler{0};     // VkSampler handle
    /// VkSemaphore handle for inter-queue sync (compute→graphics).
    /// Atomic so the producer thread can clear it on a cached frame
    /// (m_lastGoodFrame re-publish) without racing with the presenter
    /// thread that reads it in ProgramMonitor::presentFrame.
    /// 0 = VK_NULL_HANDLE (no semaphore).
    mutable std::atomic<uint64_t> gpuSemaphore{0};
    bool     gpuReady{false};

    /// Producer-side timeline semaphore + value used for cross-queue
    /// memory visibility (UPGRADE_PLAN Path C optimisation, 2026-05-22).
    /// When this frame's GPU texture was written by a prefetch worker
    /// on the compute queue and is now going to be sampled by the
    /// compositor on the graphics queue, the compositor must wait on
    /// this (semaphore, value) pair before its sample reads.  The
    /// wait is added to the compositor's VkSubmitInfo via a
    /// VkTimelineSemaphoreSubmitInfo in pNext, so the synchronisation
    /// happens GPU-side without blocking the prefetch worker thread.
    ///
    /// Storage as uint64_t (instead of VkSemaphore) keeps Vulkan
    /// headers out of FrameCache.h; the GPU layer reinterprets the
    /// handle on use.  0 = no producer semaphore (CPU-decode path,
    /// composite output, or scaled / lazyReadback-only frames).
    uint64_t producerTimelineSem{0};
    uint64_t producerTimelineValue{0};

    /// Opaque shared ownership of GPU resources (e.g., shared_ptr<Texture>).
    /// When all CachedFrame copies referencing this owner are destroyed,
    /// the GPU texture is released.  Keeps gpuImageView/gpuSampler valid.
    std::shared_ptr<void> gpuTextureOwner;

    // ── Lazy CPU readback ───────────────────────────────────────────────
    // When set, CPU pixels are deferred until actually needed.  The
    // callback reads back BGRA from the GPU texture into `pixels`.
    // Thread-safe: only the first caller runs the readback.
    using ReadbackFn = std::function<bool(std::vector<uint8_t>&)>;
    ReadbackFn lazyReadback;

    /// Materialise CPU pixels if they are empty and a lazy readback
    /// callback is available.  Returns true when `pixels` is non-empty
    /// after the call.  Safe to call from any thread.
    ///
    /// The fast path tests an atomic flag, NOT pixels.empty(): reading the
    /// vector's internals unsynchronized while another caller is inside
    /// lazyReadback() resizing it is a data race (a reader could observe a
    /// transiently inconsistent begin/end pair, return true, and then read
    /// through a stale data() pointer).  The flag is set with release
    /// ordering only AFTER pixels is fully written, so an acquire load
    /// guarantees the vector contents are visible.  pixels is never cleared
    /// during a frame's lifetime (the pool deleter moves it out only at
    /// final release), so the flag can never go stale.
    bool ensurePixels()
    {
        if (m_pixelsReady.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(m_readbackMutex);
        if (!pixels.empty()) {           // filled at decode time, or by an
                                          // earlier caller — publish the flag
            m_pixelsReady.store(true, std::memory_order_release);
            return true;
        }
        if (lazyReadback) {
            lazyReadback(pixels);
            lazyReadback = nullptr;      // one-shot
        }
        const bool ok = !pixels.empty();
        if (ok)
            m_pixelsReady.store(true, std::memory_order_release);
        return ok;
    }

    /// CPU memory footprint of this frame: pixel data + struct overhead.
    ///
    /// IMPORTANT (2026-05-22 architectural fix): the GPU texture VRAM
    /// is intentionally NOT counted here.  Co-owning a GPU texture via
    /// gpuTextureOwner is transient — the compositor moves sole
    /// ownership to GpuTextureCache on first use (see
    /// CompositeServiceLayerBuild's putShared + reset sequence), so a
    /// FrameCache entry typically holds the texture for ≤ one composite
    /// cycle.  Counting W*H*4 against this cache's budget produced two
    /// bad outcomes:
    ///
    ///   1. A 32 GB FrameCache budget on a 64 GB system tried to keep
    ///      ~32 GB of VRAM alive on a 24 GB GPU → driver paged or
    ///      evicted textures from under us → vkQueueSubmit access
    ///      violation → VK_ERROR_DEVICE_LOST.
    ///   2. The dual ownership (FrameCache + GpuTextureCache) meant
    ///      VRAM could not be released until BOTH caches evicted the
    ///      same entry, defeating GpuTextureCache's LRU.
    ///
    /// VRAM is now managed solely by GpuTextureCache.  This function
    /// reports only what FrameCache actually controls: CPU bytes.
    [[nodiscard]] size_t memoryUsage() const noexcept
    {
        return pixels.size() + sizeof(*this);
    }

private:
    mutable std::mutex m_readbackMutex;
    /// Set (release) once `pixels` is fully populated; checked (acquire) as
    /// ensurePixels()'s lock-free fast path.  See ensurePixels() docs.
    std::atomic<bool> m_pixelsReady{false};
};

/// Cache statistics
struct CacheStats
{
    size_t   hitCount{0};
    size_t   missCount{0};
    size_t   evictionCount{0};
    size_t   frameCount{0};
    size_t   memoryUsed{0};     // Bytes
    size_t   memoryCapacity{0}; // Bytes

    [[nodiscard]] double hitRate() const noexcept
    {
        size_t total = hitCount + missCount;
        return total > 0 ? static_cast<double>(hitCount) / total : 0.0;
    }
};

class FrameCache
{
public:
    /// Create a cache with the given memory capacity in bytes.
    /// Default: 32 GB — with ~21 character loops × 2 variants × ~5 MB/frame
    /// × 160-320 frames per loop, plus Wells ProRes 1.8 GB/variant, the
    /// working set for a full podcast timeline is ~35-50 GB of decoded
    /// frames.  16 GB forced LRU to constantly evict loop frames of
    /// currently-playing clips, producing random stutters.  32 GB keeps
    /// most content resident; LRU only kicks in when the timeline grows
    /// past ~2 minutes of unique active layers.
    // Default budget: 8 GB CPU RAM.  App::init() normally overrides this
    // with a size derived from installed physical RAM (~25%, capped to a
    // safe absolute) — see App.cpp.  The raw default only applies when
    // FrameCache is constructed without that dynamic sizing (e.g. tests).
    explicit FrameCache(size_t capacityBytes = 8ULL * 1024 * 1024 * 1024);
    ~FrameCache();

    // Non-copyable
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    // ── Insert / Lookup ─────────────────────────────────────────────────

    /// Insert a frame into the cache. Takes ownership of the pixel data.
    /// Evicts LRU frames if capacity is exceeded.
    void put(std::shared_ptr<CachedFrame> frame);

    /// Look up a cached frame. Returns nullptr if not cached.
    /// Moves the frame to the front of the LRU list (most recently used).
    [[nodiscard]] std::shared_ptr<CachedFrame> get(
        uint64_t mediaId, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full);

    /// Look up a cached frame WITHOUT promoting it in LRU order.
    /// Use for fallback/nearby-frame searches where touching the LRU
    /// would pollute the ordering and cause premature eviction of
    /// prefetched future frames.
    [[nodiscard]] std::shared_ptr<CachedFrame> getNoPromote(
        uint64_t mediaId, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full) const;

    /// Check if a frame is cached without promoting it in LRU order.
    [[nodiscard]] bool contains(uint64_t mediaId, int64_t frameNumber,
                                 ResolutionTier tier = ResolutionTier::Full) const;

    /// Return any cached frame for this media/tier, preferring the
    /// closest frame number to `preferFrame` that is <= preferFrame
    /// (then falling back to the closest any-direction match).  Does
    /// NOT promote the entry in LRU order.  Returns nullptr if no
    /// frames are cached for this media at this tier.
    ///
    /// Used by the compositor as a sticky last-good-frame fallback when
    /// the exact requested frame has not yet been decoded — preferable
    /// to skipping the layer entirely (which causes a visible pop-out).
    [[nodiscard]] std::shared_ptr<CachedFrame> getNearestBefore(
        uint64_t mediaId, int64_t preferFrame,
        ResolutionTier tier = ResolutionTier::Full) const;

    // ── Playhead tracking (for smart eviction) ──────────────────────────

    /// Tell the cache where the playhead is for a given media handle.
    /// The eviction policy uses this to keep frames ahead of the playhead
    /// and preferentially evict frames behind it.
    void setPlayhead(uint64_t mediaId, int64_t frameNumber);

    /// Phase B: declare a protected playback window for a media source.
    /// Frames inside [playheadFrame - behindCount, playheadFrame + aheadCount]
    /// are excluded from pass-1 eviction (behind-playhead sweep).  This is
    /// the data-side complement to UnifiedCache::setPlayheadWindow — call
    /// either; UnifiedCache forwards here.  Defaults (5 behind, 0 ahead)
    /// preserve historical behavior.
    void setPlayheadWindow(uint64_t mediaId,
                           int64_t playheadFrame,
                           int aheadCount,
                           int behindCount);

    // ── Bulk operations ─────────────────────────────────────────────────

    /// Evict all frames for a specific media file.
    void evictMedia(uint64_t mediaId);

    /// Evict frames that co-own GPU textures (CachedFrame::gpuTextureOwner set).
    /// This releases shared_ptr references so the GPU memory can be freed when
    /// GpuTextureCache's shared entries are also released.
    /// @param minBytes  Try to free at least this many bytes of CPU memory.
    /// @return Actual bytes freed.
    size_t evictGpuCoOwned(size_t minBytes);

    /// Remove a stale playhead entry (e.g. when a media handle is closed).
    void removePlayhead(uint64_t mediaId);

    /// Clear the entire cache.
    void clear();

    /// Set the capacity in bytes. May trigger evictions.
    void setCapacity(size_t capacityBytes);

    /// Set an upper bound on entry count.  GPU-resident frames have
    /// near-zero memoryUsage() (empty CPU pixel vectors + struct
    /// overhead ≈ 200 bytes) but each still pins an 8 MB VkImage via
    /// CachedFrame::gpuTextureOwner if the compositor has not yet
    /// consumed the frame to transfer ownership to GpuTextureCache.
    /// Without an entry-count ceiling, byte-based eviction never fires
    /// for GPU-resident-only workloads — orphan textures pile up in
    /// FrameCache and exhaust VRAM after ~45 s of continuous playback,
    /// producing the "playback slows to a crawl" symptom documented
    /// in logs around the 13:40-13:41 perf window.
    ///
    /// When the live entry count exceeds this cap, evictUntilFits runs
    /// pass 1 (behind-playhead) and falls through to pass 2 (pure LRU)
    /// just as it does for byte-pressure.  May trigger evictions when
    /// called.  Pass 0 leaves the count uncapped (legacy behaviour).
    void setMaxEntries(size_t maxEntries);

    /// Current max-entries cap.  0 = uncapped.
    [[nodiscard]] size_t maxEntries() const noexcept;

    /// Number of entries whose CachedFrame still holds gpuTextureOwner —
    /// each pins an 8 MB VkImage (1080p BGRA) in VRAM.  Reported in the
    /// CACHE-DUMP perf line so an operator can spot orphan-texture
    /// accumulation directly.
    [[nodiscard]] size_t gpuCoOwnedCount() const;

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] CacheStats stats() const;
    [[nodiscard]] size_t frameCount() const;
    [[nodiscard]] size_t memoryUsed() const;

private:
    struct CacheKey
    {
        uint64_t       mediaId;
        int64_t        frameNumber;
        ResolutionTier tier;

        bool operator==(const CacheKey& o) const noexcept
        {
            return mediaId == o.mediaId && frameNumber == o.frameNumber && tier == o.tier;
        }
    };

    struct CacheKeyHash
    {
        size_t operator()(const CacheKey& k) const noexcept
        {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.tier)) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // LRU list: front = most recently used, back = least recently used
    using LruList = std::list<CacheKey>;
    using LruIterator = LruList::iterator;

    struct CacheEntry
    {
        std::shared_ptr<CachedFrame> frame;
        LruIterator                  lruIt;
        // Memory footprint recorded AT PUT TIME (immutable for the
        // lifetime of this entry).  CachedFrame::pixels can grow after
        // insertion when lazyReadback materialises GPU pixels into CPU
        // memory (e.g., DiskFrameCache's writer thread requesting CPU
        // pixels for a GPU-resident frame).  If the cache used a live
        // memoryUsage() at evict time, the eviction would subtract a
        // LARGER number than was added at put, underflowing m_used and
        // triggering catastrophic "cache thinks it has 17.6 TB"
        // eviction storms — observed in perf_log.txt at 2026-05-22
        // 13:17:56 (frameMB=17592186033566/1023 frameN=1).  Recording
        // the size once locks the accounting symmetric regardless of
        // later pixel materialisation.
        size_t                       sizeBytes{0};
    };

    void evictUntilFits(size_t neededBytes);

    mutable std::mutex                                         m_mutex;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>     m_map;
    LruList                                                    m_lru;
    size_t                                                     m_capacity;
    size_t                                                     m_used{0};
    // Hard ceiling on entry count.  See setMaxEntries() for rationale.
    // Default 2000 — at 30 fps single-track this covers ~67 s of
    // prefetch/follow-up state; multi-track projects share the same
    // budget.  CacheCoordinator tunes this per GPU VRAM.  0 = uncapped.
    size_t                                                     m_maxEntries{2000};

    // Playhead positions per media handle (for smart eviction)
    std::unordered_map<uint64_t, int64_t>                      m_playheads;

    // Phase B: protected window extent per media handle (frames behind /
    // ahead of the playhead that are excluded from pass-1 eviction).
    // Default = 5 behind / 0 ahead to preserve pre-Phase-B behavior.
    struct WindowExtent { int behind{5}; int ahead{0}; };
    std::unordered_map<uint64_t, WindowExtent>                 m_windows;

    // Stats
    mutable size_t m_hits{0};
    mutable size_t m_misses{0};
    size_t         m_evictions{0};
};

} // namespace rt
