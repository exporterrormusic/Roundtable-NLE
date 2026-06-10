/*
 * CompositeService — Timeline video compositing pipeline orchestration.
 *
 * Extracted from TimelineWorkspace to separate the rendering/compositing
 * subsystem from UI orchestration.  Lives in gpu/ — no Qt dependency.
 *
 * Owns: composite frame orchestration, layer building, prewarm, spine
 *       rendering, safe mode, sticky frame fallback, and the
 *       CompositeEngine (GPU compositing pipeline).
 *
 * GPU resources (command buffers, staging ring, texture cache, upload
 * manager, layer pool) are owned by CompositeEngine, accessible via
 * engine().
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declared Vulkan types used in the GPU spine zero-copy path.
struct VkDescriptorImageInfo_T;
#include <vulkan/vulkan_core.h>

struct VkDescriptorImageInfo;

#include "CompositeServiceLayerBuild.h"
#include "media/FrameCache.h"
#include "media/MediaSourceService.h"  // ResolutionTier
#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/SpineEngine.h"
#endif

// CompositeEngine is in global scope (not rt::) to work around a C2888
// compiler issue with volk's Vulkan type redefinitions.
class CompositeEngine;

namespace rt {

// Forward declarations
class AnimationVideoCache;
class AudioEngine;
class CacheCoordinator;
class UnifiedCache;
class Clip;
class GraphicClip;
class MediaPool;
class ModelManager;
class SpineClip;
class VideoClip;
class Project;
class SequenceClip;
class ShotPresetManager;
class Timeline;
class TitleClip;
struct CachedFrame;

class CompositeService {
public:
    CompositeService();
    ~CompositeService();

    /// Suppress all GPU compositing while a modal dialog is active.
    /// Set by the UI layer before showing modal dialogs (QDialog::exec).
    /// During modal dialogs, paint events still fire for widgets behind
    /// the dialog, and invoking the Vulkan compositor from a paint event
    /// cascade can overflow the NVIDIA driver stack (stack overflow in
    /// nvoglv64.dll).  The compositor returns the last good cached frame
    /// when suppressed, avoiding crashes while keeping the display valid.
    static void setModalDialogActive(bool active) noexcept {
        s_modalDialogActive.store(active, std::memory_order_release);
    }
    [[nodiscard]] static bool isModalDialogActive() noexcept {
        return s_modalDialogActive.load(std::memory_order_acquire);
    }

    /// Thread-local re-entrancy counter for compositeFrame().
    /// Unlike the mutex try_lock, this detects recursive calls on the
    /// SAME thread (e.g. signal re-entrancy during GPU compositing).
    /// When non-zero on entry, compositeFrame returns the last good frame
    /// immediately, preventing deep recursive compositing that would
    /// exhaust the call stack and cause a STACK_OVERFLOW crash.
    static int& compositeDepth() noexcept {
        static thread_local int s_depth = 0;
        return s_depth;
    }

    // ── UPGRADE_PLAN: GPU-resident decode + CUDA↔Vulkan zero-copy ───────
    //
    // Master switch for the prefetch GPU-resident decode pipeline and
    // its zero-copy CUDA→Vulkan branch.  Default ON as of 2026-05-21
    // (the legacy CPU upload path is reachable as a kill-switch only,
    // via the ROUNDTABLE_GPU_RESIDENT_DECODE=0 env var).
    //
    // When ON:
    //   - Prefetch workers produce GPU-resident CachedFrames; the
    //     compositor's GpuUploadManager::uploadLayer GPU-resident
    //     branch lifts the descriptor straight from the CachedFrame
    //     instead of paying a CPU→GPU upload.
    //   - On NVDEC-decoded H.264, frames take the zero-copy path
    //     (CudaVulkanInterop shared buffer + convertFromVkBuffer),
    //     skipping the transferHardwareFrame CPU bounce entirely.
    //
    // When OFF (env-var kill switch only):
    //   - Prefetch produces CPU-pixel CachedFrames as it did before
    //     2026-05-21; compositor uploads as before.  Used to diagnose
    //     regressions or to compare cold-start latency against the
    //     new path.
    static void setGpuResidentDecode(bool on) noexcept {
        s_gpuResidentDecode.store(on, std::memory_order_release);
    }
    [[nodiscard]] static bool gpuResidentDecodeEnabled() noexcept {
        return s_gpuResidentDecode.load(std::memory_order_acquire);
    }

private:
    static std::atomic<bool> s_modalDialogActive;
    static std::atomic<bool> s_gpuResidentDecode;

public:
    // Non-copyable
    CompositeService(const CompositeService&) = delete;
    CompositeService& operator=(const CompositeService&) = delete;

    // ── Dependency injection ────────────────────────────────────────────
    void setTimeline(Timeline* t) { m_timeline = t; }
    void setMediaPool(MediaPool* p) { m_mediaPool = p; }
    void setMediaSourceService(MediaSourceService* s) { m_mediaSources = s; }
    void setModelManager(ModelManager* m) { m_modelManager = m; }
    void setProject(Project* p) { m_project = p; }
    void setShotPresetManager(ShotPresetManager* m) { m_shotPresetManager = m; }

    // ── Core compositing ────────────────────────────────────────────────
    /// Composite a frame for the given tick.
    ///
    /// @param isNestedRecursion  Internal flag: when true, the call is a
    ///        recursive descent into a nested sequence (SequenceClip ->
    ///        inner timeline). The inner composite skips the cache-side-
    ///        effects (clearLru, m_lastGoodComposite write, settle-window
    ///        bookkeeping) so the recursive frame does not pollute the
    ///        outer composite's cached state. Without this, the presenter
    ///        can read the inner frame from m_lastGoodComposite -- visible
    ///        as the nested sequence "glitching to its original frame
    ///        every other display tick" during playback / scrub.
    std::shared_ptr<CachedFrame> compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                 bool scrubMode,
                                                 bool isNestedRecursion = false);

    /// Enqueue a prewarm request on the background thread.
    /// Returns immediately — the work runs asynchronously on m_prewarmThread.
    void prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

    /// Scan the timeline window (tick, tick + lookaheadTicks] and pre-open
    /// media files + schedule first-frame prefetch for upcoming clips that
    /// are NOT yet active.  Throttled internally to run at most every ~100ms.
    /// This eliminates the 150-200ms "cold decoder open" stall at every shot
    /// boundary during playback — the decoder is already warm by the time
    /// the playhead reaches the clip.
    void prewarmUpcomingShots(int64_t tick);

    // ── Shutdown ─────────────────────────────────────────────────────────
    /// Gracefully shut down GPU work.  Waits for all in-flight submissions,
    /// destroys GPU resources, and prevents new work from starting.
    void shutdown();

    /// Set the shutdown flag WITHOUT destroying GPU resources yet.
    /// Call this EARLY in App::~App (Phase 1) so any compositeFrame
    /// invoked during the subsequent Qt widget-tree teardown (via
    /// signals fired by destroyed widgets) returns nullptr immediately
    /// instead of running the full composite path on a partially-
    /// torn-down timeline / project.  The full shutdown() still runs
    /// later at the normal point.
    ///
    /// Without this, the Qt deleteChildren cascade in
    /// MainWindow::~MainWindow can re-enter compositeFrame, which then
    /// AVs on stale container state (the May 2026 deterministic crash
    /// at roundtable.exe+0x3DE88B observed across multiple sessions).
    void requestShutdown() noexcept {
        m_shutdown.store(true, std::memory_order_release);
    }

    // ── Cache management ────────────────────────────────────────────────
    /// Invalidate composite result LRU (thread-safe, lock-free flag).
    /// Immediately clears m_lastGoodComposite so stale frames are not
    /// returned during invalidation.  Sets an atomic flag that
    /// compositeFrame() checks on its next acquisition of
    /// m_compositeMutex (the FrameProducer thread is the sole
    /// compositor, so the flag is always picked up promptly).
    void requestCacheInvalidation() {
        m_cacheInvalidateRequested.store(true, std::memory_order_release);
        {
            std::lock_guard lg(m_lastCompositeMtx);
            m_lastGoodComposite.reset();
            m_lastGoodCompositeTick = -1;
            m_lastGoodCompositeClipIds.clear();
            // Re-arm the settle clock so the next partial composite
            // after invalidation triggers the first-view hold instead
            // of inheriting a stale timestamp from before the reset.
            m_lastFullCompositeAt = {};
            m_lastFullLayerCount  = 0;
        }
    }

    /// A3: range-scoped invalidation.  Drops only LRU entries whose
    /// tick is in [fromTick, toTick].  Cheap enough to call
    /// directly without the atomic-flag dance (acquires
    /// m_compositeMutex with try_lock; if busy, falls back to full
    /// invalidation via requestCacheInvalidation()).  Edits that
    /// modify a known time range should prefer this.
    void requestCacheInvalidationRange(int64_t fromTick, int64_t toTick);

    /// Try to invalidate immediately (used when caller holds no lock).
    void invalidateCacheDirect();

    // ── GPU display mode ────────────────────────────────────────────────
    void setGpuDisplayMode(bool on) { m_gpuDisplayMode = on; }
    [[nodiscard]] bool gpuDisplayMode() const noexcept { return m_gpuDisplayMode; }

    // ── Playback resolution tier ────────────────────────────────────────
    // Controls the ResolutionTier compositeFrame() requests from MediaPool
    // for video layers.  Driven by the program-monitor playback-resolution
    // dropdown (Full/1/2/1/4/1/8).  Lower tiers decode fewer pixels, cache
    // smaller frames, and upload less data each frame — the single biggest
    // lever for playback smoothness at the cost of preview resolution.
    void setPlaybackTier(ResolutionTier t) {
        m_playbackTier.store(static_cast<uint8_t>(t), std::memory_order_relaxed);
    }
    [[nodiscard]] ResolutionTier playbackTier() const noexcept {
        return static_cast<ResolutionTier>(
            m_playbackTier.load(std::memory_order_relaxed));
    }

    // Force Full resolution — used by ExportPanel for preview and export
    // frames.  Overrides character video tier from Half to Full.
    void setForceFullResolution(bool force) noexcept {
        m_forceFullResolution.store(force, std::memory_order_relaxed);
    }
    [[nodiscard]] bool forceFullResolution() const noexcept {
        return m_forceFullResolution.load(std::memory_order_relaxed);
    }

    // ── Media handle management ─────────────────────────────────────────
    /// Look up a cached media handle by path. Returns 0 if not found.
    /// Thread-safe via m_openMediaHandlesMutex (accessed by prewarm thread,
    /// FrameProducer thread, and UI thread).
    [[nodiscard]] uint64_t findMediaHandle(const std::string& path) const {
        std::lock_guard lock(m_openMediaHandlesMutex);
        auto it = m_openMediaHandles.find(path);
        return it != m_openMediaHandles.end() ? it->second : 0;
    }
    /// Register (or update) a media handle for the given path.
    void registerMediaHandle(const std::string& path, uint64_t handle) {
        std::lock_guard lock(m_openMediaHandlesMutex);
        m_openMediaHandles[path] = handle;
    }
    void clearMediaHandles() {
        std::lock_guard lock(m_openMediaHandlesMutex);
        m_openMediaHandles.clear();
    }
    /// Drop the cached handle for a single path so the next composite
    /// re-resolves it via MediaPool::open() (picking up a file whose
    /// contents changed on disk, e.g. an edited Color Matte).
    void forgetMediaPath(const std::string& path) {
        std::lock_guard lock(m_openMediaHandlesMutex);
        m_openMediaHandles.erase(path);
    }

    /// Evict every cached GPU texture belonging to a media handle. Called
    /// after MediaPool::invalidatePath() on a live file replacement: the
    /// GpuTextureCache is keyed by (mediaId, frame, tier) and the handle
    /// is preserved across invalidation, so without this the compositor
    /// keeps drawing the stale uploaded texture even though MediaPool now
    /// decodes the new file. Defined in the .cpp (needs CompositeEngine).
    void invalidateMediaTextures(uint64_t mediaId);

    // ── Title/Graphic rendering ─────────────────────────────────────────
    std::shared_ptr<CachedFrame> renderTitleClip(TitleClip* clip, int64_t tick,
                                                  uint32_t outW, uint32_t outH);
    std::shared_ptr<CachedFrame> renderGraphicClip(GraphicClip* clip, int64_t tick,
                                                    uint32_t outW, uint32_t outH);

    // ── Cache coordinator ───────────────────────────────────────────────
    /// Set the CacheCoordinator for system-adaptive budgets and VRAM
    /// pressure monitoring.  Forwards to CompositeEngine.
    void setCacheCoordinator(rt::CacheCoordinator* coordinator);

    /// Phase B: install the UnifiedCache coordinator.  May be null.
    /// CompositeService stores the pointer and forwards per-frame
    /// generation ticks + playhead window updates.
    void setUnifiedCache(rt::UnifiedCache* uc) noexcept { m_unifiedCache = uc; }

    // ── Composite engine access ─────────────────────────────────────────
    [[nodiscard]] CompositeEngine* engine() const noexcept { return m_engine.get(); }

    /// VRAM usage percentage (0-100) from GPU texture cache, or 0 if none.
    [[nodiscard]] int vramUsagePercent() const;

    // ── Composite mutex ──────────────────────────────────────────────────
    // Protects compositeFrame() execution.  Recursive because the A2
    // change (WIP commit, RENDER_GRAPH_PLAN) allows nested SequenceClip
    // recursion: an outer compositeFrame call temporarily unlocks before
    // recursing, but if any inner path forgets to unlock the same thread
    // re-acquires this mutex and MSVC's std::mutex throws EDEADLK.
    // Using recursive_mutex makes the locking discipline forgiving while
    // we finish migrating to a properly hierarchical lock model.
    std::recursive_mutex& compositeMutex() { return m_compositeMutex; }

    // ── Last good composite ─────────────────────────────────────────────
    // Kept for safe-mode fallback and exception safety.  The normal
    // compositeFrame() path no longer returns stale frames.
    std::shared_ptr<CachedFrame> lastGoodComposite() const {
        std::lock_guard lg(m_lastCompositeMtx);
        return m_lastGoodComposite;
    }

#ifdef ROUNDTABLE_HAS_SPINE
    // ── Spine shared data ───────────────────────────────────────────────
    struct SpineSharedData {
        std::vector<std::vector<uint8_t>> pagePixels;
        std::vector<int> pageWidths;
        std::vector<int> pageHeights;
        std::vector<bool> pagePMA;
        bool pagePixelsUnpremultiplied{false};
        bool boundsCached{false};
        float stableBoundsX{0}, stableBoundsY{0};
        float stableBoundsW{0}, stableBoundsH{0};
        // PER-ANIMATION framing box, keyed by animation name.  Each entry is
        // the union of the setup pose and that animation's own sampled
        // envelope (max extent across its frames).  The live renderer frames a
        // clip to its CURRENT animation's box, so:
        //   • normal animations (which stay within the setup silhouette) keep
        //     their original setup-pose size, and
        //   • a taller "action" pose grows ONLY its own box — it's framed to
        //     fit (never clipped) without shrinking every other animation.
        // Falls back to setup-pose bounds when an animation has no entry.
        struct AnimBounds { float x{0}, y{0}, w{0}, h{0}; };
        std::unordered_map<std::string, AnimBounds> animBounds;
        bool gpuTexturesUploaded{false};
        std::string skelPath;
        std::string atlasPath;
        std::vector<uint8_t> skelBytes;
        std::string atlasText;
        std::string atlasDir;
    };

    struct SpineCPUState {
        SpineEngine engine;
        std::shared_ptr<SpineSharedData> shared;
        std::shared_ptr<CachedFrame> cachedFrame;
        int64_t cachedTick{-1};
        // Clip-driven settings the engine was last configured with.
        // Used to detect a stale engine after the clip is mutated
        // out-of-band (undo/redo, shot switch, scripting) and re-apply
        // them cheaply without a skeleton/atlas reload.
        std::string appliedCharKey;  // spineCharKey() at load time
        std::string appliedAnim;
        bool        appliedLooping{false};
        bool        appliedTalking{false};
        float       appliedSpeed{1.0f};
        bool        appliedValid{false};
    };

    static std::string spineCharKey(const SpineClip& clip);
    std::shared_ptr<SpineSharedData> getOrCreateSharedSpineData(
        const SpineClip& clip, const std::string& assetsDir);
    SpineCPUState* getOrCreateSpineState(SpineClip* clip);
    SpineCPUState* tryGetSpineState(SpineClip* clip);
    std::shared_ptr<CachedFrame> renderSpineClip(SpineClip* clip, int64_t tick,
                                                  uint32_t outW, uint32_t outH);
    void preloadSpineAssets();
    void warmNewSpineClips();

    /// Get shared spine data for overlay sizing (null if not loaded).
    [[nodiscard]] const SpineSharedData* getSpineSharedDataForOverlay(
        const std::string& charName, const std::string& outfit, int stance) const;

    // Callback for schedule-based spine loading (set by UI layer)
    using SpineLoadCallback = std::function<void(const std::string& charName,
                                                  const std::string& outfit,
                                                  int stance,
                                                  const std::string& assetsDir)>;
    void setSpineLoadScheduler(SpineLoadCallback cb) { m_spineLoadScheduler = std::move(cb); }

    // Non-blocking spine state integration (called from UI thread after
    // background load completes)
    void integrateSpineSharedData(const std::string& key,
                                  std::shared_ptr<SpineSharedData> shared);
    void removeSpinePendingKey(const std::string& key);
    bool isSpinePending(const std::string& key) const;
    bool addSpinePendingKey(const std::string& key);

    // ── Spine futures ──────────────────────────────────────────────────
    /// Remove completed futures to avoid unbounded growth, then push a new one.
    void drainCompletedSpineFutures();
    void addSpineFuture(std::future<void> f);

    // ── Spine shared-data cache (read-only + keyed insert/lookup) ────
    [[nodiscard]] const std::unordered_map<std::string, std::shared_ptr<SpineSharedData>>&
        spineSharedCache() const { return m_spineSharedCache; }
    [[nodiscard]] std::shared_ptr<SpineSharedData> findSpineSharedData(const std::string& key) const;
    void storeSpineSharedData(const std::string& key, std::shared_ptr<SpineSharedData> data);

    // ── Animation name cache (keyed find/store) ─────────────────────
    [[nodiscard]] const std::vector<std::string>* findAnimNames(const std::string& key) const;
    void storeAnimNames(const std::string& key, std::vector<std::string> names);

    // ── Per-clip spine CPU state cache ──────────────────────────────
    void evictSpineState(uint64_t clipId);
    void purgeDeadSpineStates(const std::unordered_set<uint64_t>& liveIds);
    /// Re-apply the clip's animation/looping/talking/speed to its cached
    /// spine engine if they have drifted (e.g. after undo/redo). Cheap
    /// no-op when nothing changed; never reloads the skeleton/atlas.
    void resyncSpineClip(SpineClip* clip);
    [[nodiscard]] const std::unordered_map<uint64_t, std::unique_ptr<SpineCPUState>>&
        spineCache() const { return m_spineCache; }

    /// Get the animation video cache (may be null).
    [[nodiscard]] AnimationVideoCache* animVideoCache() const { return m_animVideoCache.get(); }
    /// Create the animation video cache if it doesn't exist.
    void initAnimVideoCache(MediaPool* pool);
#endif

    // ── Reset (new timeline / project close) ────────────────────────────
    // P2 (CLAUDE_IMPROVEMENT_PLAN): CPU safe-mode fallback was removed.
    // Device-lost is now fatal — GpuContext::tryRecover() fires the
    // fatal-failure callback which the UI translates into a modal
    // restart dialog.  Every major NLE behaves the same way.
    void reset();

private:
    // Layer building (extracted to CompositeServiceLayerBuild.cpp)
    std::vector<LayerInfo> buildLayersForFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                bool scrubMode, bool playbackNonBlocking,
                                                int& clipsAtTick, bool perfLog,
                                                std::unique_lock<std::recursive_mutex>& lock,
                                                bool& gpuSpineUsedThisFrame);

    // Per-clip-type layer builders (extracted to reduce CompositeServiceLayerBuild.cpp)
    std::shared_ptr<CachedFrame> resolveMediaFrame(MediaHandle handle, int64_t frameNumber,
                                                    ResolutionTier tier, bool scrubMode) const;
    // Lazily opens / search-resolves a VideoClip's media handle. skipClip=true
    // means the caller should skip the clip; a 0 return with skipClip=false
    // means an async open is still pending (proceed to sticky-frame fallback).
    uint64_t resolveVideoClipHandle(VideoClip* videoClip, bool playbackNonBlocking,
                                    bool& skipClip);
    // Renders a nested SequenceClip to a clean CPU BGRA frame via a recursive
    // composite of its inner timeline. Returns null when the clip references no
    // valid inner sequence. Temporarily swaps m_timeline and releases `lock`
    // for the recursion, restoring both before returning.
    std::shared_ptr<CachedFrame> buildSequenceClipFrame(
        SequenceClip* seqClip, int64_t localTick,
        uint32_t outW, uint32_t outH, bool scrubMode,
        std::unique_lock<std::recursive_mutex>& lock);
#ifdef ROUNDTABLE_HAS_SPINE
    // Result of building a live SpineClip layer: either a CPU `frame` or a GPU
    // zero-copy descriptor (when gpuSpineZeroCopy is set).
    struct SpineLayerResult {
        std::shared_ptr<CachedFrame> frame;
        bool                  gpuSpineZeroCopy = false;
        VkDescriptorImageInfo gpuSpineDescriptor{};
        uint32_t              gpuSpineW = 0;
        uint32_t              gpuSpineH = 0;
        bool                  cpuSpineRendered = false;
    };
    // Live Spine evaluation (GPU-first, CPU fallback) for one SpineClip.
    // Renders into the returned frame / zero-copy descriptor, may read a prior
    // GPU-spine render back into layers[m_gpuSpineInsertedLayer] (multi-char),
    // and sets gpuSpineUsedThisFrame. `sx` feeds a diagnostic log only.
    SpineLayerResult buildSpineClipLayer(
        SpineClip* spineClip, int64_t tick, int64_t localTick,
        uint32_t outW, uint32_t outH, bool scrubMode, bool playbackNonBlocking,
        bool staticRecomposite, bool& gpuSpineUsedThisFrame,
        std::vector<LayerInfo>& layers, float sx);
#endif

    // GPU compositing path (delegates to CompositeEngine)
    std::shared_ptr<CachedFrame> tryCompositeOnGpu(const std::vector<LayerInfo>& layers,
                                                     uint32_t outW, uint32_t outH,
                                                     int64_t tick, bool scrubMode,
                                                     bool perfLog,
                                                     std::chrono::high_resolution_clock::time_point perfT0,
                                                     std::chrono::high_resolution_clock::time_point& perfTlayers,
                                                     int& effectLayerCount, int& effectPassCount,
                                                     int& transitionCount,
                                                     bool isNestedRecursion = false);

    // External dependencies (non-owning)
    Timeline*  m_timeline{nullptr};
    MediaPool* m_mediaPool{nullptr};
    MediaSourceService* m_mediaSources{nullptr};
    ModelManager* m_modelManager{nullptr};
    Project* m_project{nullptr};
    ShotPresetManager* m_shotPresetManager{nullptr};

    // Composite engine (owns GPU compositing pipeline)
    std::unique_ptr<CompositeEngine> m_engine;

    bool m_isCompositing{false};

    std::recursive_mutex m_compositeMutex;
    mutable std::mutex   m_lastCompositeMtx;
    std::shared_ptr<CachedFrame> m_lastGoodComposite;
    int64_t m_lastGoodCompositeTick{-1};
    /// Clip ids that produced m_lastGoodComposite. Used to detect shot
    /// boundaries: if NONE of the currently active clips overlap with
    /// this set, the previous composite represents a *different* shot and
    /// must not be reused as a stale fallback while the new shot's
    /// decoders warm up.  Otherwise the prior shot lingers for 1+ frame
    /// after its clips end (e.g. Wells visible past her clip end).
    std::vector<uint64_t> m_lastGoodCompositeClipIds;

    // A1: settle-window state.  When a composite is incomplete (fewer
    // resolved layers than active clips at the tick), we hold the prior
    // complete frame for up to kSettleWindowMs ms instead of showing a
    // partial composite that "builds up" layer-by-layer over several
    // frames.  Reset when a fully-resolved composite is produced.
    int  m_lastFullLayerCount{0};
    std::chrono::steady_clock::time_point m_lastFullCompositeAt{};
    // 250ms gives one cold NVDEC decoder open (~150–200ms per the
    // prewarmUpcomingShots header comment) + headroom for a couple of
    // peers to decode in parallel under the P5 NVDEC-concurrency cap.
    // Past this we stop holding and accept the partial composite so a
    // permanently-unresolvable clip (missing file, codec mismatch)
    // can't freeze playback indefinitely.
    static constexpr int kSettleWindowMs = 250;

    // Phase B: UnifiedCache coordinator (non-owning pointer).  Wired by
    // App::createMainWindow after CompositeService is constructed.  May
    // be null in tests and during early startup.
    UnifiedCache* m_unifiedCache{nullptr};

    std::atomic<bool> m_cacheInvalidateRequested{false};

    // Track active clip IDs for shot-boundary detection.
    // Mutated from BOTH the FrameProducer thread (compositeFrame /
    // prewarmUpcomingShots, under m_compositeMutex) AND the RenderQueue
    // prewarm worker (doPrewarmPlaybackResources, under m_openMediaHandlesMutex).
    // Those are DIFFERENT mutexes, so they don't exclude each other — a
    // concurrent move-assign/clear here corrupted the hash set's heap nodes
    // (0xC0000374). This dedicated mutex guards EVERY access; keep it the
    // innermost lock (never acquire another mutex while holding it).
    mutable std::mutex           m_lastActiveClipIdsMutex;
    std::unordered_set<uint64_t> m_lastActiveClipIds;

    // ── Timeline lookahead prewarm ────────────────────────────────────
    // prewarmUpcomingShots() is called from TWO threads under TWO
    // DIFFERENT mutexes: compositeFrame() (composite thread, holds
    // m_compositeMutex) and doPrewarmPlaybackResources() (prewarm thread,
    // holds m_openMediaHandlesMutex).  Every container below is mutated
    // inside that function, so without its own lock the two callers race
    // — the exact unordered_set heap-corruption pattern (0xC0000374) that
    // m_lastActiveClipIds had.  This mutex serializes the whole scan; the
    // function try-locks and returns when the other thread is already
    // scanning (the scan is throttled + idempotent, so skipping is
    // correct).  Lock order: {m_compositeMutex | m_openMediaHandlesMutex}
    // → m_lookaheadMutex → m_lastActiveClipIdsMutex (innermost).
    std::mutex m_lookaheadMutex;
    // clip IDs whose decoders have already been proactively prewarmed
    // (file opened + first frame prefetch scheduled) ahead of their
    // timeline-in.  Cleared at shot boundary / reset.
    std::unordered_set<uint64_t> m_prewarmedClipIds;
    // clip IDs whose first ~0.5s of frames have been re-scheduled once the
    // clip became IMMINENT (within kHeadWarmTicks of the playhead).  The
    // early m_prewarmedClipIds pass fires ~2s out, when those head frames sit
    // far from the playhead and get evicted from the bounded prefetch queue by
    // nearer (currently-playing) frames — so only the single urgent frame 0
    // survives and the rest of the clip head is cold when the playhead
    // arrives.  Re-scheduling when imminent lands the head frames near the
    // playhead, where they survive the queue and warm before the cut.  Cleared
    // at shot boundary / reset alongside m_prewarmedClipIds.
    std::unordered_set<uint64_t> m_headWarmedClipIds;
    // Throttle: only scan the lookahead window every ~100ms during playback.
    std::chrono::steady_clock::time_point m_lastLookaheadScan{};
    // Last tick value passed to prewarmUpcomingShots.  Used to detect
    // playhead jumps (seek, scrubber release, JKL direction change) so
    // the throttle can be bypassed when the tick has moved by more than
    // continuous playback would produce within the throttle window.
    // Without this, a seek that lands right before a cut loses lookahead
    // coverage and the empty-sentinel path fires at the cut.
    int64_t m_lastLookaheadTick{INT64_MIN};

    // ── Sticky per-clip last-good-frame fallback ──────────────────────
    // When a clip is on-screen but its newest frame hasn't been decoded
    // yet (e.g. Spine skeleton still loading, first loop wrap after
    // scrub), the compositor used to drop the layer entirely — causing
    // visible 100-300ms "character vanishes" / "background renders but
    // character comes in late" pop-in.  Instead we keep the previous
    // good frame displayed until a fresh one arrives.  Map is cleared
    // on reset() and pruned when clip IDs go out of the active set.
    std::unordered_map<uint64_t, std::shared_ptr<CachedFrame>> m_stickyLastClipFrame;
    // Character-level sticky fallback — keyed by a string formed from
    // (characterName|outfit|animation) for SpineClips or by mediaPath
    // for VideoClips.  Used when a BRAND NEW clip (never rendered yet,
    // so no per-clipId sticky exists) starts playing but its frames
    // aren't cached.  Because the video file backing this clip is the
    // same as the file that backed the previous shot of the same
    // character, reusing that frame eliminates the "Modernia vanishes
    // for 13 frames while Spine skeleton loads" and "scrub + play shows
    // black character for 200ms" artefacts.
    std::unordered_map<std::string, std::shared_ptr<CachedFrame>> m_stickyLastCharFrame;

    // Reentrancy depth of buildLayersForFrame (nested SequenceClips composite
    // recursively). The sticky-cache prune may only run at depth 1 (the
    // top-level frame). Active clip IDs / character keys accumulate across the
    // whole nested build (cleared at depth-1 entry) so the top-level prune
    // keeps entries for clips inside nested sequences too. Touched only on the
    // composite thread under m_compositeMutex.
    int m_buildLayersDepth{0};
    std::unordered_set<uint64_t>    m_frameActiveClipIds;
    std::unordered_set<std::string> m_frameActiveCharKeys;

    // Open media handles (shared with preOpenVideoMedia)
    // Protected by its own mutex — the prewarm thread accesses this
    // concurrently with compositeFrame() on the FrameProducer thread.
    std::unordered_map<std::string, uint64_t> m_openMediaHandles;
    mutable std::mutex m_openMediaHandlesMutex;

    // ── Prewarm thread (Phase 2.A) ─────────────────────────────────────
    struct PrewarmRequest {
        int64_t  tick{0};
        uint32_t outW{0};
        uint32_t outH{0};
    };

    /// Background thread entry point — runs prewarm work off the UI thread.
    void prewarmThreadLoop();

    /// Actual prewarm implementation (moved to background thread).
    void doPrewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

    std::thread m_prewarmThread;
    /// Bulk-open thread spawned by prewarm iteration (now tracked so
    /// shutdown can join it instead of leaving it detached).
    std::thread m_bulkOpenThread;
    std::mutex              m_prewarmMutex;
    std::condition_variable m_prewarmCv;
    PrewarmRequest          m_prewarmRequest{};
    bool                    m_prewarmPending{false};
    std::atomic<bool> m_compositorReady{false};
    std::atomic<bool> m_destroying{false};

    // P2 (CLAUDE_IMPROVEMENT_PLAN): the safe-mode CPU fallback state
    // (m_safeMode, m_lastSafeModeComposite, m_safeModeCallback,
    // m_lastRecoveryCheck) was deleted.  Device-lost is now treated
    // as fatal — see CompositeServiceFrame.cpp.

    bool m_gpuDisplayMode{false};
    std::atomic<bool> m_shutdown{false};

    // Distinct from m_shutdown: this guards CompositeService::shutdown()
    // itself against re-entry, so the prewarm-thread join + container
    // teardown don't run twice (which would be wrong even though every
    // individual step is idempotent — we don't want to log "shutdown
    // complete" twice, and using m_shutdown for the guard would let
    // requestShutdown() falsely satisfy it).
    std::atomic<bool> m_shutdownDone{false};

    // Playback resolution tier (set via setPlaybackTier, read in compositeFrame).
    // Default Half matches the dropdown default (1/2) and the source monitor.
    std::atomic<uint8_t> m_playbackTier{static_cast<uint8_t>(ResolutionTier::Half)};

    // Force Full resolution override — set by ExportPanel before compositing
    // preview or export frames so characters and video render at full quality
    // regardless of the playback tier dropdown.  Does NOT affect Program/Source
    // Monitor previews (they use the normal playbackTier path).
    std::atomic<bool> m_forceFullResolution{false};

    // Rolling per-second playback perf summary (auto-emitted to perf_log.txt)
    struct PlaybackPerfWindow {
        std::chrono::steady_clock::time_point windowStart{};
        int    frameCount{0};
        int    slowFrameCount{0};      // > 33ms
        int    veryslowFrameCount{0};  // > 50ms
        double totalMs{0.0};
        double maxMs{0.0};
        int    layerSum{0};
        int    gpuHitSum{0};
        int    uploadSum{0};
        int    effectPassSum{0};
        int    transitionSum{0};
    };
    PlaybackPerfWindow m_perfWindow;

#ifdef ROUNDTABLE_HAS_SPINE
    std::unordered_map<std::string, std::shared_ptr<SpineSharedData>> m_spineSharedCache;
    std::unordered_map<std::string, std::vector<std::string>> m_animNameCache;
    std::string m_gpuSpineActiveCharKey;
    /// Per-frame readbacks from GPU Spine rendering, one per character.
    std::vector<std::shared_ptr<struct CachedFrame>> m_gpuSpineReadbacks;
    /// Number of Spine clips rendered on GPU this frame.
    int m_gpuSpineCount{0};
    /// Index into layers[] of the most recently inserted Spine layer.
    int m_gpuSpineInsertedLayer{-1};
    /// Stored spine render state for the single-char zero-copy optimization.
    VkDescriptorImageInfo m_gpuSpineDesc{};
    uint32_t m_gpuSpineW{0};
    uint32_t m_gpuSpineH{0};
    /// Set to true when a Spine clip is GPU-rendered, reset after layer tracking.
    bool m_gpuSpineJustRendered{false};
    /// Layer index of the PREVIOUS Spine character (for updating its layer
    /// when the next character's readback captures its FBO content).
    int m_gpuSpinePrevLayer{-1};
    /// Tick of the previous top-level buildLayersForFrame. When the current
    /// build repeats this tick, the playhead is parked (paused) and we are
    /// re-compositing a STATIC frame — e.g. during a transform-handle drag.
    /// On such frames the GPU Spine layer is read back into a stable frame so
    /// it no longer aliases the transient shared FBO and populates the sticky
    /// last-good cache (fixes "character vanishes when resizing, returns on
    /// play"). During playback the tick advances every frame, so zero-copy is
    /// preserved.
    int64_t m_lastBuiltTick{-1};
    std::unordered_map<uint64_t, std::unique_ptr<SpineCPUState>> m_spineCache;
    // Sticky last-good pre-rendered frame per Spine clip.
    // Prevents source switching (pre-rendered video <-> live Spine) when
    // tryGetFrame misses during non-blocking playback.
    std::unordered_map<uint64_t, std::shared_ptr<CachedFrame>> m_lastPreRenderedSpineFrame;

    mutable std::mutex m_spinePendingMutex;
    std::unordered_set<std::string> m_spinePendingKeys;
    std::vector<std::future<void>> m_spineLoadFutures;

    std::unique_ptr<AnimationVideoCache> m_animVideoCache;

    SpineLoadCallback m_spineLoadScheduler;
#endif
};

} // namespace rt
