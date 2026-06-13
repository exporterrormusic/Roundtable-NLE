/*
 * CompositeEngine — GPU compositing engine extracted from CompositeService.
 *
 * Owns all GPU compositing resources (command submission, staging ring,
 * upload manager, texture cache, layer texture pool) and encapsulates
 * the single-submit GPU pipeline: upload -> effects -> transitions ->
 * composite -> readback.
 *
 * CompositeService holds an instance and delegates to it, keeping only
 * the higher-level orchestration (layer building, prewarm, spine, safe mode).
 *
 * NOTE: This class is in the global namespace (not rt::) to avoid a
 * vexing C2888 "symbol cannot be defined within namespace 'rt'" compiler
 * error that occurs when the header is processed after volk.h redefines
 * Vulkan handle types within the precompiled header.  All rt:: types are
 * qualified explicitly.
 */

#pragma once

#include "cache/FrameCache.h"            // rt::CachedFrame
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "Compositor.h"                  // rt::Compositor, rt::BlendMode, rt::ABPair
#include "TransitionRenderer.h"          // rt::GpuTransitionType, rt::TransitionSourceInfo
#include "EffectProcessor.h"             // rt::EffectProcessor, rt::EffectType
#include "GpuTextureCache.h"             // rt::GpuTextureCache
#include "GpuWorkSubmission.h"           // rt::GpuWorkSubmission
#include "GpuUploadManager.h"            // rt::GpuUploadManager
#include "StagingRing.h"                 // rt::StagingRing
#include "vulkan/Texture.h"              // rt::Texture

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace rt { class CachePolicy; }

// ── Composite result LRU entry ──────────────────────────────────────────────

struct CompositeCacheEntry
{
    int64_t  tick{-1};
    uint32_t w{0}, h{0};
    std::shared_ptr<rt::CachedFrame> frame;
};

// ═════════════════════════════════════════════════════════════════════════════

class CompositeEngine
{
public:
    CompositeEngine();
    ~CompositeEngine();

    CompositeEngine(const CompositeEngine&) = delete;
    CompositeEngine& operator=(const CompositeEngine&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────
    void init(VkDevice device);
    void shutdown();

    // ── LRU cache ───────────────────────────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> checkLru(
        int64_t tick, uint32_t w, uint32_t h) const;
    void insertLru(int64_t tick, uint32_t w, uint32_t h,
                   std::shared_ptr<rt::CachedFrame> frame);
    void flushLruOnResize(uint32_t w, uint32_t h);
    void clearLru();
    /// A3: drop only composite-LRU entries whose tick falls inside the
    /// inclusive range [fromTick, toTick].  Edits that affect a known
    /// time slice (trim, split, ripple of a single clip) should call
    /// this instead of clearLru() to preserve cached frames outside the
    /// affected range.
    void invalidateLruRange(int64_t fromTick, int64_t toTick);

    // ── Main GPU compositing entry point ────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> composite(
        const std::vector<rt::LayerInfo>& layers,
        uint32_t outW, uint32_t outH,
        int64_t tick, bool scrubMode,
        bool gpuDisplayMode,
        rt::Compositor* compositor,
        rt::EffectProcessor* effectProcessor,
        rt::TransitionRenderer* transitionRenderer,
        bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point& perfTlayers,
        std::chrono::high_resolution_clock::time_point& perfTgpuUp,
        std::chrono::high_resolution_clock::time_point& perfTcomp,
        int& effectLayerCount, int& effectPassCount,
        int& transitionCount,
        bool allowLruInsert = true,
        bool forceSyncReadback = false);

    // ── GPU state ───────────────────────────────────────────────────────
    [[nodiscard]] bool isGpuAvailable() const noexcept;
    void notifyDeviceLost() noexcept;
    // backoffAttempts() / resetBackoff() removed in P2.  GPU submit
    // failures now go straight to signalDeviceLost; no per-frame retry.
    [[nodiscard]] bool isGpuCompositeEnabled() const noexcept
        { return m_gpuCompositeState > 0; }

    // ── Cache coordinator ───────────────────────────────────────────────
    /// Set the CachePolicy for system-adaptive budgets and VRAM
    /// pressure monitoring.  Must be called before the first composite frame.
    void setCachePolicy(rt::CachePolicy* coordinator) noexcept
        { m_cachePolicy = coordinator; }

    // ── Texture cache ───────────────────────────────────────────────────
    [[nodiscard]] rt::GpuTextureCache* textureCache() const noexcept
        { return m_gpuTexCache.get(); }
    void clearTextureCache();
    [[nodiscard]] int vramUsagePercent() const noexcept;

    /// Reset per-layer-slot dirty-tracking keys for a given media handle.
    /// GpuUploadManager::uploadLayer() skips the CPU→GPU upload when the
    /// pool-slot's recorded (mediaId, frameNumber) matches the requested
    /// frame — used to avoid PCIe transfers on stable layers.  On live
    /// file replacement the (mediaId, frameNumber) are unchanged but the
    /// CachedFrame's pixels are different; without resetting these keys
    /// the dirty-tracking incorrectly skips the upload and the layer
    /// keeps drawing the OLD GPU texture.  Call this on invalidate so
    /// the next composite re-uploads the freshly decoded pixels.
    void invalidateMediaPoolSlots(uint64_t mediaId);

    // ── Semaphore pool (inter-queue compute->graphics sync) ──────────────
    /// Each composite frame acquires a dedicated binary semaphore to
    /// avoid the ring-buffer wrap race: the per-slot semaphore in
    /// GpuWorkSubmission can be re-signalled before the presenter has
    /// consumed it — undefined behaviour in Vulkan.  A pool of
    /// dedicated semaphores ensures each frame gets a unique handle
    /// that is never re-signalled while still pending on the graphics
    /// queue.  The pool starts small and grows as needed.
    [[nodiscard]] VkSemaphore acquireFrameSemaphore();
    void releaseFrameSemaphore(VkSemaphore sem);

    // ── GPU timing telemetry (per-stage VkQueryPool) ────────────────────
    /// Most recent resolved per-stage GPU timings, in milliseconds.  Updated
    /// once per frame, lagging by kRingSize submissions (we read the slot
    /// whose fence just signaled when we begin the next recording).
    struct GpuStageTimings {
        double frameMs{0.0};
        double uploadMs{0.0};
        double effectMs{0.0};
        double composeMs{0.0};
        bool   valid{false};
    };
    [[nodiscard]] GpuStageTimings lastGpuTimings() const noexcept {
        std::lock_guard l(m_timingMtx);
        return m_lastTimings;
    }

    // ── Resource cleanup helpers ────────────────────────────────────────
    void destroyCompositeSlot();
    void clearGpuTexCache();

private:
    // ── Owned GPU resources ───────────────────────────────────────────
    // Declaration order is load-bearing: members are destroyed in REVERSE
    // order, and m_uploadManager holds a raw GpuTextureCache* plus a
    // StagingRing&.  The texture cache and staging ring must therefore be
    // declared BEFORE the upload manager so they outlive it during
    // destruction (shutdown() also tears down in this order explicitly).
    std::unique_ptr<rt::GpuWorkSubmission> m_gpuSubmission;
    std::unique_ptr<rt::StagingRing> m_stagingRing;
    std::unique_ptr<rt::GpuTextureCache> m_gpuTexCache;
    std::unique_ptr<rt::GpuUploadManager> m_uploadManager;

    // Layer texture pool
    //
    // PoolTexKey identifies the most-recently-uploaded CPU frame for each
    // pool slot.  GpuUploadManager::uploadLayer() skips the CPU→GPU upload
    // when the request matches the slot's key.
    //
    // The CachedFrame pointer is part of the identity: live file replacement
    // produces a NEW CachedFrame (shared_ptr) carrying new pixels under the
    // same (mediaId, frameNumber) tuple.  Comparing (mediaId, frameNumber)
    // alone would incorrectly skip the upload (the bug "needs scrub to
    // refresh").  Comparing the frame pointer too forces a re-upload
    // whenever the decoded frame object changes.
    struct PoolTexKey {
        uint64_t mediaId{0};
        int64_t  frameNumber{-1};
        const void* framePtr{nullptr};  // CachedFrame identity for re-decode detection
    };
    std::vector<std::unique_ptr<rt::Texture>> m_gpuLayerTextures;
    std::vector<PoolTexKey> m_gpuLayerTexKeys;
    std::vector<std::unique_ptr<rt::Texture>> m_gpuMaskTextures;

    // Per-layer effect-output snapshot textures.  The EffectProcessor uses
    // only two ping-pong storage images shared across all layers, so when
    // layer B's effect chain runs after A's it overwrites the textures that
    // gpuLayers[A].textureInfo references — breaking cross-dissolves between
    // two effected clips (A appears to vanish, B is sampled for both sides
    // of the mix).  We snapshot the EffectProcessor output into one of these
    // per-layer textures after each layer's effect chain completes so the
    // downstream Transition pass reads stable, layer-specific data.
    std::vector<std::unique_ptr<rt::Texture>> m_layerEffectOutputs;

    // Double-buffered upload textures for packed-alpha layers.  The
    // compositor reads packed-alpha twice per pixel (RGB + alpha halves);
    // if the next frame's upload overwrites the same texture between those
    // reads we get a 1-frame spill.  Alternating upload/composite targets
    // avoids the race with zero copy overhead.
    std::vector<std::unique_ptr<rt::Texture>> m_gpuLayerTexturesAlt;

    // Cache coordinator (optional — for dynamic budgets + VRAM pressure)
    rt::CachePolicy* m_cachePolicy{nullptr};

    // GPU state machine: 0 = untested, 1 = enabled, -1 = permanently failed
    int m_gpuCompositeState{0};

    // Inter-queue (compute → graphics) semaphores are pulled from and
    // returned to GpuContext's shared binary-semaphore pool — see
    // GpuContext::acquireBinarySemaphore.  The pool lives there so the
    // VulkanViewport presenter (GUI thread) can return consumed semaphores
    // to the same pool the FrameProducer thread acquires from.
    VkDevice                 m_device{nullptr};

    // Exponential backoff state removed in P2 of CLAUDE_IMPROVEMENT_PLAN.

    // ── Phase D: per-pass fault isolation ────────────────────────────
    // When an optional render-graph pass fails, its name is added here
    // and SKIPPED on every subsequent frame.  Prevents the executor
    // from re-trying a broken shader 60 times per second.  Cleared on
    // device reset only — a session is the failure scope.
    std::unordered_set<std::string> m_disabledPasses;

    // ── Render graph alternative path ────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> compositeViaRenderGraph(
        const std::vector<rt::LayerInfo>& layers,
        uint32_t outW, uint32_t outH,
        int64_t tick, bool scrubMode,
        bool gpuDisplayMode,
        rt::Compositor* compositor,
        rt::EffectProcessor* effectProcessor,
        rt::TransitionRenderer* transitionRenderer,
        bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point& perfTlayers,
        std::chrono::high_resolution_clock::time_point& perfTgpuUp,
        std::chrono::high_resolution_clock::time_point& perfTcomp,
        int& effectLayerCount, int& effectPassCount,
        int& transitionCount,
        bool allowLruInsert = true,
        bool forceSyncReadback = false);

    // Composite result LRU
    static constexpr size_t kCacheSize = 8;
    std::vector<CompositeCacheEntry> m_compositeLru;
    size_t m_compositeLruIdx{0};

    // ── GPU timing query pools (one per ring slot) ───────────────────
    // Four timestamp markers per frame:
    //   [0] frame start (before any uploads/compute)
    //   [1] after all PassType::Upload passes
    //   [2] after all PassType::Effect + PassType::Transition passes
    //   [3] after PassType::Composite + Readback (frame end)
    // Deltas yield upload / effect / compose stage timings.
    static constexpr uint32_t kTimingMarkers = 4;
    static constexpr int      kTimingRingSize = 3;  // matches GpuWorkSubmission::kRingSize
    VkQueryPool m_timingPools[kTimingRingSize]{};
    bool        m_timingPoolUsed[kTimingRingSize]{};
    double      m_timestampPeriodNs{1.0};
    mutable std::mutex m_timingMtx;
    GpuStageTimings   m_lastTimings;
    bool              m_timingInitialized{false};

    void initTimingPools(VkDevice device);
    void destroyTimingPools();
    /// Read query results for the slot we are about to record into
    /// (data is from the previous use of that slot — fence is signaled by
    /// the time GpuWorkSubmission::beginRecording() returns).
    void resolveTimingsForSlot(int slotIdx);
};
