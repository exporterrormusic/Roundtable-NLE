/*
 * MediaPool — manages open VideoDecoder instances and the shared FrameCache.
 *
 * Multiple timeline clips can reference the same source file. MediaPool
 * ensures only one decoder is open per file, and routes all frame requests
 * through the shared LRU cache.
 *
 * Thread-safe: clips on the decode thread and the UI thread can both
 * request frames concurrently.
 *
 * Prefetch: A background worker thread decodes ahead of the playhead during
 * sequential playback.  When getFrame(handle, N) detects forward sequential
 * access, it schedules frames N+1..N+K for background decode.  Subsequent
 * getFrame calls for those frames become instant cache hits.
 */

#pragma once

#include "FrameCache.h"
#include "DiskFrameCache.h"
#include "FrameScheduler.h"
#include "VideoDecoder.h"
#include "PerformanceProfile.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

// Forward declaration: PrefetchTexturePool drags in Vulkan headers, and
// MediaPool.h is included widely. The unique_ptr<PrefetchTexturePool>
// member below only needs the complete type at the point its destructor
// is instantiated (MediaPool.cpp).
class PrefetchTexturePool;

// UPGRADE_PLAN Phase 4: convertDecodedToCacheGpu takes a reference to
// per-worker Vulkan state.  Definition lives in MediaPoolPrefetchGpu.h
// (Vulkan-tainted); forward-declared here to keep MediaPool.h clean.
struct WorkerGpuState;

/// Handle returned by MediaPool when media is opened.
using MediaHandle = uint64_t;
constexpr MediaHandle InvalidMedia = 0;

/// Info about a media entry in the pool
struct MediaEntry
{
    MediaHandle                    handle{0};
    std::filesystem::path          path;
    std::unique_ptr<VideoDecoder>  decoder;
    VideoStreamInfo                info;
    int                            refCount{0};

    // Sequential-decode tracking (avoids expensive seek for sequential playback)
    int64_t                        lastDecodedFrame{-1};

    // Cached sws context for YUV→BGRA conversion (avoids re-creating per frame).
    // Stored as void* to avoid FFmpeg header dependency here.
    void*                          swsCtx{nullptr};
    int                            swsSrcW{0};
    int                            swsSrcH{0};
    int                            swsSrcFmt{-1};
    int                            swsDstW{0};
    int                            swsDstH{0};

    /// When true, the decoded video has packed-alpha layout (top half = RGB,
    /// bottom half = alpha as greyscale).  decodeFrame / decodePrefetchFrame
    /// will unpack inline so cached frames are ready-to-use BGRA at half height.
    bool                           packedAlpha{false};

    /// Diagnostic: tracks which decode path has been logged (0 = none).
    /// Prevents spamming logs — each path logs only once per handle.
    int                            decodePathLogged{0};

    /// Loop pre-decode: when a short video (≤LOOP_PREDECODE_MAX_FRAMES) is
    /// first accessed, a background thread decodes ALL frames sequentially
    /// and populates the FrameCache.  Random-access playback then becomes
    /// 100% cache hits.  This is the #1 performance fix for looping idles.
    bool                           loopPreDecodeStarted{false};
};

/// Pool of reusable pixel buffers to avoid per-frame 14.7 MB heap allocations.
/// Prefetch workers decode continuously at 6 workers × ~30 fps = ~180 frames/sec;
/// recycling buffers from evicted cache entries eliminates ~2.6 GB/sec heap churn.
class PixelBufferPool
{
public:
    std::vector<uint8_t> acquire(size_t size)
    {
        std::lock_guard lock(m_mutex);
        if (!m_pool.empty()) {
            auto buf = std::move(m_pool.back());
            m_pool.pop_back();
            buf.resize(size);
            return buf;
        }
        return std::vector<uint8_t>(size);
    }

    void recycle(std::vector<uint8_t>&& buf)
    {
        if (buf.empty()) return;
        std::lock_guard lock(m_mutex);
        if (m_pool.size() < kMaxPooled)
            m_pool.push_back(std::move(buf));
    }

private:
    static constexpr size_t kMaxPooled = 32;
    std::mutex m_mutex;
    std::vector<std::vector<uint8_t>> m_pool;
};

/// Task for the background prefetch thread.
struct PrefetchTask
{
    MediaHandle            handle{0};
    std::filesystem::path  filePath;
    int64_t                frameNumber{0};
    ResolutionTier         tier{ResolutionTier::Full};
    double                 fps{30.0};
    VideoStreamInfo        info{};
    bool                   packedAlpha{false};
    bool                   urgent{false};
    /// True when this handle is a looping clip (loop pre-decode active).
    /// Propagated into CachedFrame::isLoopFrame so the LRU protects it
    /// against "behind playhead" eviction that would otherwise discard
    /// the loop every iteration.
    bool                   isLoop{false};
    /// True for export (forceExact) decodes: skip the Full-tier 1920px
    /// downscale cap so the frame is decoded at native source resolution.
    /// The 1920 cap is a real-time PREVIEW memory optimisation; export wants
    /// maximum quality (and supersampling headroom for the composite scale).
    bool                   exportFullRes{false};
};

/// Per-handle state for the prefetch worker (separate decoder, sws ctx).
struct PrefetchDecoderState
{
    std::unique_ptr<VideoDecoder>  decoder;
    int64_t                        lastDecodedFrame{-1};
    /// Wall-clock time of last use — used for LRU eviction so freshly
    /// opened decoders (with low frame numbers) aren't prematurely evicted.
    std::chrono::steady_clock::time_point lastUsed{};
    void*                          swsCtx{nullptr};
    int                            swsSrcW{0};
    int                            swsSrcH{0};
    int                            swsSrcFmt{-1};
    int                            swsDstW{0};
    int                            swsDstH{0};
    /// Number of consecutive slow hardware decodes.  A single slow frame
    /// from GPU contention (e.g., another decoder opening at a shot
    /// boundary) is NOT enough to permanently switch to software — that
    /// causes catastrophic SW decode of large packed-alpha frames.  We
    /// only fall back after multiple consecutive slow frames.
    int                            consecutiveSlowHwFrames{0};
};

class MediaPool
{
public:
    /// Create a pool with an optional shared frame cache.
    /// If no cache is provided, one is created with default capacity.
    explicit MediaPool(std::shared_ptr<FrameCache> cache = nullptr);
    ~MediaPool();

    // Non-copyable
    MediaPool(const MediaPool&) = delete;
    MediaPool& operator=(const MediaPool&) = delete;

    // ── Open / Close ────────────────────────────────────────────────────

    /// Open a media file and return a handle. If the file is already open,
    /// increments the reference count and returns the existing handle.
    /// Returns InvalidMedia on failure.
    [[nodiscard]] MediaHandle open(const std::filesystem::path& filePath);

    /// Asynchronously open a media file on a background worker thread.
    /// Returns immediately. The handle (when ready) can be retrieved later
    /// via isPathOpen()/findHandleByPath() or the standard open() call,
    /// which becomes a fast no-op once the async open has completed.
    /// Used by the compositor at shot boundaries to avoid stalling the
    /// playback thread on the ~500ms NVDEC initialization cost.
    void openAsync(const std::filesystem::path& filePath);

    /// Non-blocking query: is this path currently open in the pool?
    [[nodiscard]] bool isPathOpen(const std::filesystem::path& filePath) const;

    /// Release a handle. Decrements refcount. When refcount reaches 0,
    /// the decoder is closed and all cached frames for that media are evicted.
    void release(MediaHandle handle);

    /// Close all media. Clears the frame cache.
    void closeAll();

    /// Release the file handle for a single path — closes the decoder
    /// and the shared-mode HANDLE without reopening.  Safe to call on
    /// paths that aren't open (no-op).  Used before live-reload to
    /// guarantee Explorer can recreate the file after deleting it.
    void closePath(const std::filesystem::path& filePath);

    /// Force a single media file to be re-read from disk after its
    /// contents changed (live file replacement / edited Color Matte).
    /// Evicts the CPU + disk frame cache for the file, cancels pending
    /// prefetch.  When reopenDecoder is true (default), reopens the
    /// decoder IN PLACE.  When false, the decoder is closed and left
    /// closed — the HANDLE is fully released so Explorer can recreate
    /// the file.  The next getFrame() transparently reopens on demand.
    /// The handle and the path→handle mapping are intentionally preserved:
    /// every consumer that cached the handle (compositor, Source Monitor,
    /// clips) keeps it and transparently receives freshly decoded pixels
    /// on the next getFrame() — no path re-resolution required.
    /// Returns the (preserved) handle so the caller can also evict GPU-side
    /// textures for it: GpuTextureCache is keyed by mediaId+frame+tier, and
    /// because the handle is preserved that key is unchanged — it would
    /// otherwise keep serving the stale uploaded texture. Returns
    /// InvalidMedia if the path was not open.
    [[nodiscard]] MediaHandle invalidatePath(const std::filesystem::path& filePath,
                                              bool reopenDecoder = true);

    /// Snapshot of every source file currently open in the pool (the
    /// canonical resolved paths actually decoded — covers timeline clips,
    /// bin/source-monitor previews, prewarm/lookahead opens, regardless of
    /// clip subtype). Used by the live file-swap watcher so it watches what
    /// the app actually touches rather than re-deriving it from clip types.
    [[nodiscard]] std::vector<std::filesystem::path> openMediaPaths() const;

    /// Set a callback invoked (on the calling thread, outside the pool
    /// mutex) right after a NEW media file is opened. The live file-swap
    /// watcher uses this to arm a watch on the just-opened path. Pass {}
    /// to clear. The callback must be cheap and thread-safe (open() is
    /// called from prefetch workers, the compositor, and the UI thread).
    void setOnMediaOpened(std::function<void(std::filesystem::path)> cb);

    // ── Frame access ────────────────────────────────────────────────────

    /// Get a decoded frame from cache, or decode on demand.
    /// Returns nullptr on failure.
    ///
    /// forceExact: used by export.  Skips the ±N nearby-frame "frame drop"
    /// fallback and forces a blocking, frame-accurate inline decode of the
    /// EXACT frame.  Without it, a cache miss during export returns the
    /// previous frame as a stale neighbor → frame duplication / stutter.
    [[nodiscard]] std::shared_ptr<CachedFrame> getFrame(
        MediaHandle handle, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full,
        bool scrubMode = false,
        bool forceExact = false);

    /// Non-blocking frame access for playback.
    /// Returns a cached frame (exact or nearby) WITHOUT ever decoding inline.
    /// On miss, schedules urgent prefetch and returns nullptr so the
    /// compositor can skip or reuse the previous frame — matching Premiere
    /// Pro's "never stall the render thread" playback design.
    [[nodiscard]] std::shared_ptr<CachedFrame> tryGetFrame(
        MediaHandle handle, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full);

    /// Schedule background decode-ahead for sequential playback.
    /// Called automatically by getFrame on sequential access.
    /// When urgent=true, the first frame is pushed to the front of the
    /// prefetch queue so workers pick it up immediately.
    void schedulePrefetch(MediaHandle handle, int64_t afterFrame,
                          int count = perfProfile().prefetchAheadFrames,
                          bool urgent = false,
                          ResolutionTier tier = ResolutionTier::Full);

    /// Set project frame rate so prefetch can skip unnecessary source frames.
    void setProjectFps(double fps) { m_projectFps.store(fps > 0.0 ? fps : 30.0, std::memory_order_relaxed); }

    // ── Queries ─────────────────────────────────────────────────────────

    /// Get stream info for an opened media.
    [[nodiscard]] const VideoStreamInfo* getInfo(MediaHandle handle) const;

    /// Get the file path for a handle.
    [[nodiscard]] std::filesystem::path getPath(MediaHandle handle) const;

    /// Check if a handle is valid.
    [[nodiscard]] bool isValid(MediaHandle handle) const;

    /// Mark a media handle as containing packed-alpha data (top half = RGB,
    /// bottom half = alpha).  When set, decode functions will automatically
    /// unpack the frame to half-height BGRA before caching, eliminating
    /// per-frame unpacking work on the compositor thread.
    void setPackedAlpha(MediaHandle handle, bool packed);

    /// Number of currently open media files.
    [[nodiscard]] size_t openCount() const;

    /// Diagnostic snapshot of the prefetch queue + per-handle bookkeeping.
    /// Used by the perf reporter to detect runaway queue growth during
    /// scrubbing or playback degradation.
    struct PrefetchStats {
        size_t queueDepth{0};      ///< Pending PrefetchTasks in m_prefetchQueue
        size_t ownedHandles{0};    ///< Distinct handles claimed by prefetch workers
        size_t scrubDecoders{0};   ///< Live scrub-decoder instances (UI thread cache)
    };
    [[nodiscard]] PrefetchStats prefetchStats() const;

    // ── Playback Performance Metrics ──────────────────────────────────
    // Atomic counters tracking frame delivery during playback.
    // Logged every 2 seconds so we can measure the impact of each
    // pipeline change with hard numbers.
    struct PerfMetrics {
        std::atomic<uint64_t> cacheHits{0};         // exact frame in FrameCache
        std::atomic<uint64_t> nearbyHits{0};        // ±N frame returned (frame drop)
        std::atomic<uint64_t> staleReturns{0};      // last-good-frame returned
        std::atomic<uint64_t> inlineDecodes{0};     // blocking decode on caller thread
        std::atomic<uint64_t> prefetchDeliveries{0};// frames decoded by prefetch workers
        std::atomic<uint64_t> totalMisses{0};       // nullptr returned (compositor skips layer)
        std::atomic<uint64_t> totalRequests{0};     // total getFrame + tryGetFrame calls
        std::atomic<uint64_t> prefetchScheduled{0}; // prefetch tasks enqueued
        std::atomic<uint64_t> avgDecodeUs{0};       // rolling average decode time in microseconds
        // UPGRADE_PLAN Phase 9: per-window counts of decode dispatch.
        // gpuResidentDecoded covers convertDecodedToCacheGpu successes;
        // cpuConvertDecoded covers convertDecodedToCache. The ratio tells
        // an operator whether the GPU-resident path is actually firing
        // (high ratio = working), or being skipped (low ratio = eligibility
        // failure, e.g. packed-alpha, headless, or flag off).
        std::atomic<uint64_t> gpuResidentDecoded{0};
        std::atomic<uint64_t> cpuConvertDecoded{0};
        // UPGRADE_PLAN A: zero-copy CUDA→Vulkan NVDEC path.  Counted
        // SEPARATELY from gpuResidentDecoded (mutually exclusive) — both
        // produce GPU-resident CachedFrames, but only ZC skips the
        // transferHardwareFrame CPU bounce.  Sum (gpuResidentDecoded +
        // zeroCopyDecoded) = all GPU-resident decodes in the window.
        std::atomic<uint64_t> zeroCopyDecoded{0};

        void reset() {
            cacheHits.store(0, std::memory_order_relaxed);
            nearbyHits.store(0, std::memory_order_relaxed);
            staleReturns.store(0, std::memory_order_relaxed);
            inlineDecodes.store(0, std::memory_order_relaxed);
            prefetchDeliveries.store(0, std::memory_order_relaxed);
            totalMisses.store(0, std::memory_order_relaxed);
            totalRequests.store(0, std::memory_order_relaxed);
            prefetchScheduled.store(0, std::memory_order_relaxed);
            avgDecodeUs.store(0, std::memory_order_relaxed);
            gpuResidentDecoded.store(0, std::memory_order_relaxed);
            cpuConvertDecoded.store(0, std::memory_order_relaxed);
            zeroCopyDecoded.store(0, std::memory_order_relaxed);
        }
    };
    PerfMetrics m_perf;

    /// Log a performance report and reset counters. Called periodically.
    void logPerfReport();

    // ── Loop pre-decode (public API) ────────────────────────────────────
    /// Maximum frames for loop pre-decode eligibility.
    /// Covers ~20 seconds at 30fps so short character animations can be
    /// pre-decoded once at project-load and serve 100% cache hits thereafter.
    static constexpr int64_t LOOP_PREDECODE_MAX_FRAMES = 600;

    /// Start background pre-decode of all frames for a short looping video.
    /// Safe to call multiple times — duplicate calls are no-ops.
    /// `priority` is the clip's earliest playback start tick (lower = sooner =
    /// scheduled first).  When the worker pool is at capacity, lower-priority
    /// tasks queue while higher-priority ones run, so clips needed sooner
    /// finish their pre-decode first.
    void startLoopPreDecode(MediaHandle handle, ResolutionTier tier = ResolutionTier::Full,
                            int64_t priority = 0);

    // ── Disk cache ──────────────────────────────────────────────────────
    /// Attach a persistent disk-backed second-level cache.
    /// Call after construction, before opening any media.
    void setDiskCache(std::shared_ptr<DiskFrameCache> dc) { m_diskCache = std::move(dc); }

    /// Get the disk cache (may be null).
    [[nodiscard]] DiskFrameCache* diskCache() const noexcept { return m_diskCache.get(); }

    // ── Frame Scheduler ──────────────────────────────────────────────
    /// Access the frame scheduler used to prioritise and bound decode work.
    /// The scheduler mediates between frame consumers and decode workers,
    /// enforcing lookahead limits and cancelling stale work on seek.
    [[nodiscard]] FrameScheduler& scheduler() noexcept { return m_scheduler; }
    [[nodiscard]] const FrameScheduler& scheduler() const noexcept { return m_scheduler; }

    /// Quick non-blocking probe: is this exact (handle, frame, tier) in the
    /// in-memory FrameCache right now? Used by play-start preroll to wait
    /// for upcoming frames to land before declaring playback ready.
    [[nodiscard]] bool isFrameCached(MediaHandle handle, int64_t frameNumber,
                                     ResolutionTier tier = ResolutionTier::Full) const;

    std::atomic<bool> m_destroying{false};

private:
    MediaEntry* findEntry(MediaHandle handle);
    const MediaEntry* findEntry(MediaHandle handle) const;

    std::shared_ptr<CachedFrame> decodeFrame(MediaEntry& entry, int64_t frameNumber,
                                              ResolutionTier tier, bool scrubMode = false);

    // ── Prefetch background worker ──────────────────────────────────────
    // PREFETCH_THREAD_COUNT total workers; workers [0 .. PREFETCH_NVDEC_
    // WORKERS-1] are eligible for NVDEC hardware decode and the rest do
    // software decode.  Per-worker eligibility/affinity rules live in
    // MediaPoolPrefetchSchedule.cpp::acceptable() — that is the source
    // of truth; this header just sizes the pools.
    //
    // P5 (CLAUDE_IMPROVEMENT_PLAN): PREFETCH_NVDEC_WORKERS was 4 prior
    // to 2026-05-14.  Combined with 4 concurrent loop pre-decode workers
    // and the on-demand playhead decoder, peak NVDEC pressure reached
    // ~9 sessions — well past consumer NVDEC's 2-3 concurrent-decode
    // budget.  The user's own comment in CompositeServiceFrame.cpp
    // documented "count=8 urgent prefetch flooded NVDEC + the graphics
    // queue with ~16-24 simultaneous urgent decodes and tripped TDR."
    // Reduced to 2 to cap peak NVDEC sessions at ~4 (2 prefetch + 1
    // loop + 1 playhead).
    //
    // PREFETCH_AHEAD_COUNT lowered 60 → 12 (2026-05-22 perf fix).
    // The previous count=60 meant every cache miss in tryGetFrame/getFrame
    // scheduled 60 frames worth of decode work.  Under aggressive scrub
    // (~50 missed positions/sec across multiple clips), the scheduler
    // queue would balloon to 3000+ tasks and the NVDEC + compute-queue
    // pipeline saturated at ~235 fps of decode — 8× the 30 fps playback
    // rate the user actually needs.  That saturation is what produced
    // the 120-180 ms compositor `submit=` stalls observed at 2026-05-22
    // 12:53 after a scrub burst: the compositor's submit on the shared
    // compute queue queued behind a wall of prefetch convert+copy work.
    //
    // 12 = ~400 ms of lookahead at 30 fps Full tier, which is enough to
    // mask the round-trip latency between request and decode without
    // generating 8× the necessary work.  After scrub stops the user
    // typically settles within ~200 ms and 12 ahead frames cover the
    // next ~400 ms of playback while the regular lookahead refills.
    //
    // The proper architectural fix is UPGRADE_PLAN §1.3 Path C
    // (separate graphics + async-compute queues for compositor vs
    // prefetch); this is a tighten-the-knob mitigation in the
    // meantime.  Once Path C lands, these knobs can return higher.
    //
    // These three values now live in rt::PerformanceProfile
    // (perfProfile().prefetchAheadFrames / prefetchThreadCount /
    // nvdecWorkers) so they can be scaled per-machine and by the Boost
    // toggle — see docs/BOOST_MODE_PLAN.md.  The defaults above remain
    // the behaviour-neutral baseline.  nvdecWorkers is always clamped by
    // the GPU's real NVENC/NVDEC session cap before it is raised.

    void startPrefetchThread();
    void stopPrefetchThread();
    void prefetchWorker(int workerId);
    /// Per-worker GPU state is passed in as a pointer so it can be null
    /// for the scrub-path caller (MediaPoolFrame.cpp) that has no worker.
    /// Defined nullable for forward-compatibility; the dispatch helper
    /// short-circuits to the CPU path on null + feature-flag-off.
    std::shared_ptr<CachedFrame> decodePrefetchFrame(
        PrefetchDecoderState& state, const PrefetchTask& task,
        WorkerGpuState* wgs = nullptr);
    std::shared_ptr<CachedFrame> convertDecodedToCache(
        PrefetchDecoderState& state, const PrefetchTask& task,
        DecodedFrame& decoded, int64_t frameNumber);

public:
    /// UPGRADE_PLAN Phase 4: GPU-resident sibling of convertDecodedToCache.
    /// Routes the NV12/YUV420P → BGRA conversion through Nv12Converter and
    /// writes the result into a per-frame pooled VkImage, so the
    /// compositor can sample without a CPU↔GPU round-trip.  Falls back to
    /// nullptr on any eligibility failure (packed-alpha, headless build,
    /// unsupported format, device-lost, etc.); callers must respond by
    /// calling the CPU convertDecodedToCache.  Definition in
    /// MediaPoolPrefetchConvertGpu.cpp.
    ///
    /// Public so the free dispatch helper tryConvertDecodedToCacheGpu
    /// (MediaPoolPrefetchGpu.h) can call it without `friend` noise.
    std::shared_ptr<CachedFrame> convertDecodedToCacheGpu(
        PrefetchDecoderState& state, const PrefetchTask& task,
        DecodedFrame& decoded, int64_t frameNumber,
        WorkerGpuState& wgs);

    /// UPGRADE_PLAN: App calls this after GpuContext::init() succeeds.
    /// Allocates m_prefetchTexPool, which the ctor could not create
    /// because GpuContext was not initialised at that point.  Idempotent;
    /// safe to call multiple times.  Without this call, the GPU-resident
    /// prefetch path stays disabled and every frame takes the CPU path.
    void onGpuContextReady();

    /// Shared producer-side timeline VkSemaphore used by every prefetch
    /// convert+copy submission to signal a monotonically increasing
    /// value (UPGRADE_PLAN Path C, 2026-05-22).  Each CachedFrame
    /// produced by convertDecodedToCacheGpu carries the value its
    /// submission signalled; the compositor waits on the per-frame
    /// max value before sampling — replacing the previous synchronous
    /// CPU fence wait that was the main throughput bottleneck under
    /// seek-recovery and fast playback rates.
    ///
    /// Returned as uint64_t (VkSemaphore handle) so callers outside
    /// the GPU layer don't need volk.h.  Returns 0 until
    /// onGpuContextReady() succeeds.
    [[nodiscard]] uint64_t prefetchTimelineSem() const noexcept;

    /// Atomic ++ and return the next value to signal.  Called once per
    /// convert+copy submission; the same value is also stored in the
    /// produced CachedFrame's producerTimelineValue field.
    [[nodiscard]] uint64_t nextPrefetchTimelineValue() noexcept;

private:

    // ── Loop pre-decode ─────────────────────────────────────────────────
    // For short looping videos (idle character animations), decode ALL
    // frames into the FrameCache on a background thread so that random-
    // access playback is 100% cache hits.
    // (Public startLoopPreDecode declared above.)
    void loopPreDecodeWorker(MediaHandle handle, std::filesystem::path path,
                             VideoStreamInfo info, bool packedAlpha,
                             ResolutionTier tier);

    mutable std::mutex                             m_mutex;
    std::unordered_map<MediaHandle, MediaEntry>     m_entries;
    std::unordered_map<std::string, MediaHandle>    m_pathToHandle; // canonical path → handle
    std::function<void(std::filesystem::path)>      m_onMediaOpened; // live-swap watcher hook
    std::unordered_set<std::string>                 m_failedPaths;  // paths that failed to open (no retry)

    // UPGRADE_PLAN Phase 3: recycled VkImage pool for the GPU-resident
    // prefetch decode path. Null when GpuContext is not initialised
    // (headless / no-Vulkan builds) — prefetch workers must tolerate
    // null and fall back to the CPU path. Phase 4 is the first consumer.
    //
    // DECLARATION-ORDER NOTE (2026-05-24): m_prefetchTexPool MUST be
    // declared BEFORE m_cache and m_diskCache so reverse-order
    // destruction destroys it LAST.  Both caches transitively own
    // shared_ptr<Texture>s whose deleter (makePooledTexture's lambda)
    // recycles into this pool — destroying the pool first would AV in
    // the DiskFrameCache writer thread on shutdown as it drained its
    // queue, hitting the pool's already-freed unordered_map.  See the
    // lifetime invariant in PrefetchTexturePool.h.
    std::unique_ptr<PrefetchTexturePool>            m_prefetchTexPool;

    std::shared_ptr<FrameCache>                     m_cache;
    std::shared_ptr<DiskFrameCache>                 m_diskCache;
    std::shared_ptr<PixelBufferPool>                m_pixelPool;
    FrameScheduler                                  m_scheduler;
    uint64_t                                        m_nextHandle{1};

    // UPGRADE_PLAN Path C: shared timeline semaphore for cross-queue
    // memory visibility between prefetch (compute queue) and
    // compositor (graphics queue).  Created in onGpuContextReady,
    // destroyed in the destructor.  Stored as uint64_t (VkSemaphore
    // handle) to keep volk.h out of MediaPool.h.  m_prefetchTimelineValue
    // is fetch_add'd once per convert+copy submission; the produced
    // CachedFrame stores the value it received.
    uint64_t                                        m_prefetchTimelineSem{0};
    std::atomic<uint64_t>                            m_prefetchTimelineValue{0};

    // Prefetch thread pool state
    std::vector<std::thread>                         m_prefetchThreads;
    mutable std::mutex                               m_prefetchMutex;
    std::condition_variable                           m_prefetchCv;
    std::atomic<bool>                                m_prefetchRunning{false};
    std::deque<PrefetchTask>                         m_prefetchQueue;
    std::atomic<int64_t>                              m_playheadFrame{0}; // for priority scheduling
    std::atomic<double>                                m_projectFps{30.0}; // for frame-stride optimization

    // Per-handle cooldown to avoid rebuilding the queue every frame tick.
    // Protected by m_prefetchMutex.
    std::unordered_map<MediaHandle, std::chrono::steady_clock::time_point> m_lastScheduleTime;
    // Per-handle prefetch ownership (handle -> worker id).
    // Ensures only ONE worker services a given handle at a time, which forces
    // sequential frame decoding on the fast path (~10ms/frame) and avoids
    // multiple workers each paying the cold-seek cost (40-110ms/frame).
    // Covers ALL handles (not just packed-alpha).  An owning worker releases
    // ownership when no more tasks for its handle remain in the queue.
    std::unordered_map<MediaHandle, int>           m_prefetchPackedOwner;

    // ── Async-open background worker ────────────────────────────────────
    // A single dedicated worker that serially opens media files queued by
    // openAsync().  Keeps the playback thread off the ~500ms NVDEC init
    // cost when shots transition to characters that aren't loaded yet.
    void openWorkerLoop();
    void startOpenWorker();
    void stopOpenWorker();
    std::thread                                      m_openWorker;
    std::mutex                                       m_openWorkerMutex;
    std::condition_variable                           m_openWorkerCv;
    std::deque<std::filesystem::path>                m_openWorkerQueue;
    std::unordered_set<std::string>                  m_openWorkerInFlight; // canonical paths currently queued/opening
    std::atomic<bool>                                m_openWorkerRunning{false};

    // Loop pre-decode state (short looping videos decoded entirely into cache)
    // A small worker pool drains a priority queue ordered by playback start
    // tick — clips needed sooner finish their pre-decode first.  Replaces
    // the older "spawn a thread per call + race a counting_semaphore" model
    // which gave back-of-queue clips arbitrary FIFO latency.
    /// NVDEC on RTX 4090 supports 5+ concurrent sessions; 4 loop workers
    /// + 4 prefetch NVDEC workers = 8 potential sessions.  FFmpeg
    /// auto-falls back to software if the HW limit is hit — no crash risk.
    // P5 (CLAUDE_IMPROVEMENT_PLAN): reduced from 4 to 1 on 2026-05-14.
    // Loop pre-decode is background warm-up for looping clips (character
    // idles); serializing it does not affect real-time playback (the
    // first iteration through the loop still cold-decodes on demand
    // and warm-starts the cache for subsequent iterations).  Reducing
    // to 1 eliminates 3 concurrent NVDEC sessions that were contending
    // with the playhead decoder + prefetch workers, contributing to
    // shot-boundary TDR events (see perf_log.txt:3032 — 62.9 ms
    // composite during atlas re-upload + simultaneous prefetch).
    static constexpr int LOOP_PREDECODE_MAX_CONCURRENT = 1;
    struct LoopPreDecodeTask {
        int64_t                 priority;   // earliest playback start tick (lower = sooner)
        uint64_t                seq;        // FIFO tiebreaker
        MediaHandle             handle;
        std::filesystem::path   path;
        VideoStreamInfo         info;
        bool                    packedAlpha;
        ResolutionTier          tier;
        // Greater = lower scheduling priority.  std::priority_queue is a
        // max-heap, so we invert: smaller priority value pops first.
        bool operator<(const LoopPreDecodeTask& o) const {
            if (priority != o.priority) return priority > o.priority;
            return seq > o.seq;
        }
    };
    std::mutex                                       m_loopPreDecodeMutex;
    std::condition_variable                          m_loopPreDecodeCv;
    std::set<MediaHandle>                            m_loopPreDecodeActive;
    std::set<MediaHandle>                            m_loopPreDecodeDone;
    std::priority_queue<LoopPreDecodeTask>           m_loopPreDecodeQueue;
    std::vector<std::thread>                         m_loopPreDecodeThreads;
    std::atomic<bool>                                m_loopPreDecodeRunning{false};
    uint64_t                                         m_loopPreDecodeSeq{0};
    void startLoopPreDecodeWorkers();
    void stopLoopPreDecodeWorkers();
    void loopPreDecodeDispatcher();

    // Last-good-frame map: stores the most recent successfully decoded frame
    // per handle during playback.  On cache miss, getFrame returns this
    // instead of blocking on inline decode — matching Premiere Pro's
    // "never stall the render thread" design.  Prefetch workers fill the
    // real frames in the background while the stale frame is displayed.
    mutable std::mutex                               m_lastGoodMtx;
    std::unordered_map<MediaHandle, std::shared_ptr<CachedFrame>> m_lastGoodFrame;

    // ── Dedicated scrub decoder ─────────────────────────────────────────
    // Separate decoder instances for scrub mode, avoiding m_mutex contention.
    // Only used from the UI thread (single-threaded scrub path).
    std::unordered_map<MediaHandle, PrefetchDecoderState> m_scrubDecoders;
    PrefetchDecoderState& getScrubDecoder(MediaHandle handle,
                                          const std::filesystem::path& path,
                                          const VideoStreamInfo& info);

    // Real-time preview requests extend this deadline so opportunistic
    // background work can back off during cold playback startup.
    std::atomic<int64_t>                             m_interactivePlaybackUntilMs{0};

    // Prefetch decoders are thread-local inside each worker — no shared state.
};

} // namespace rt
