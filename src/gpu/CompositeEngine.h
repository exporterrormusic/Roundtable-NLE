/*
 * CompositeEngine — GPU compositing engine extracted from CompositeService.
 *
 * Owns all GPU compositing resources (command submission, staging ring,
 * upload manager, texture cache, layer texture pool) and encapsulates
 * the single-submit GPU pipeline: upload -> effects -> transitions ->
 * composite -> readback.
 *
 * CompositeService holds an instance and delegates to it, keeping only
 * the higher-level orchestration (layer building, prewarm, spine, recovery).
 *
 * One composite is a list of stages (compositeViaRenderGraph): their order,
 * the frame state they share and the file each lives in are listed in
 * CompositeEngineFrame.h. Per-layer GPU resources belong to the submission
 * slot that records them (SlotLayerResources).
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
#include "TemporalInterpolator.h"        // rt::TemporalInterpolator
#include "EffectProcessor.h"             // rt::EffectProcessor, rt::EffectType
#include "GpuTextureCache.h"             // rt::GpuTextureCache
#include "GpuTeardownMode.h"
#include "GpuWorkSubmission.h"           // rt::GpuWorkSubmission
#include "GpuUploadManager.h"            // rt::GpuUploadManager
#include "StagingRing.h"                 // rt::StagingRing
#include "vulkan/Texture.h"              // rt::Texture

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace rt { class CachePolicy; class CommandPool; }

// ── Composite result LRU entry ──────────────────────────────────────────────

/// Complete identity of a CPU composite cached by CompositeEngine. Graph and
/// policy identity are explicit so callers do not depend on clearing a global
/// tick-only cache whenever the active sequence or render mode changes.
struct CompositeCacheKey
{
    const void* graph{nullptr};
    uint64_t editVersion{0};
    int64_t  tick{-1};
    uint32_t w{0}, h{0};
    rt::ResolutionTier tier{rt::ResolutionTier::Full};
    bool preservesAlpha{false};

    [[nodiscard]] bool operator==(const CompositeCacheKey& other) const noexcept
    {
        return graph == other.graph && editVersion == other.editVersion &&
               tick == other.tick && w == other.w && h == other.h &&
               tier == other.tier && preservesAlpha == other.preservesAlpha;
    }
};

struct CompositeCacheEntry
{
    CompositeCacheKey key{};
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
    void init(VkDevice device,
              rt::CommandPool* renderCommandPool = nullptr,
              VkQueue renderQueue = VK_NULL_HANDLE);
    void shutdown(
        rt::GpuTeardownMode mode = rt::GpuTeardownMode::DeviceWide);

    /// Resolve source-sized effect processors without reaching through the
    /// process-wide GpuContext. Export sessions install a resolver backed by
    /// their private, resolution-keyed resource bundle.
    void setEffectProcessorResolver(
        std::function<std::shared_ptr<rt::EffectProcessor>(uint32_t, uint32_t)> resolver)
    {
        m_effectProcessorResolver = std::move(resolver);
    }

    /// Offline export advances monotonically and does not need the editor's
    /// scrub-back texture history. Set before the first composite call.
    void setSequentialRenderMode(bool enabled) noexcept
    {
        m_sequentialRenderMode = enabled;
    }
    [[nodiscard]] bool sequentialRenderMode() const noexcept
    {
        return m_sequentialRenderMode;
    }

    // ── LRU cache ───────────────────────────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> checkLru(
        const CompositeCacheKey& key) const;
    void insertLru(const CompositeCacheKey& key,
                   std::shared_ptr<rt::CachedFrame> frame);
    void clearLru();
    /// Drop only entries for `graph` whose tick falls inside the inclusive
    /// range [fromTick, toTick]. Edits that affect a known time slice should
    /// preserve cached frames from other ranges and sequences.
    void invalidateLruRange(const void* graph, int64_t fromTick, int64_t toTick);

    // ── Main GPU compositing entry point ────────────────────────────────
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> composite(
        const std::vector<rt::LayerInfo>& layers,
        uint32_t outW, uint32_t outH,
        int64_t tick, bool scrubMode,
        bool gpuDisplayMode,
        const std::shared_ptr<rt::Compositor>& compositor,
        const std::shared_ptr<rt::EffectProcessor>& effectProcessor,
        const std::shared_ptr<rt::TransitionRenderer>& transitionRenderer,
        bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point& perfTlayers,
        std::chrono::high_resolution_clock::time_point& perfTgpuUp,
        std::chrono::high_resolution_clock::time_point& perfTcomp,
        int& effectLayerCount, int& effectPassCount,
        int& transitionCount,
        const CompositeCacheKey& cacheKey,
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
    bool m_sequentialRenderMode{false};

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

    // Per-slot mask rasterization cache: when the evaluated mask state
    // (hash) and clip match the previous frame, the CPU rasterize + GPU
    // upload are skipped and the existing texture content is reused.
    struct MaskCacheEntry {
        uint64_t clipId{0};
        uint64_t stateHash{0};
        VkDescriptorImageInfo desc{};
        bool     valid{false};
    };

    // Everything a frame records per layer belongs to its submission slot.
    // The CPU may begin recording the next frame before an older slot has
    // finished sampling its sources, so one shared set would let a frame
    // destroy or re-upload an image that is still in flight. Every list is
    // indexed by layer.
    struct SlotLayerResources {
        // Source upload textures and the frame each one holds.
        std::vector<std::unique_ptr<rt::Texture>> layerTextures;
        std::vector<PoolTexKey> layerTexKeys;
        // Alternate upload textures for packed-alpha layers. The compositor
        // reads packed alpha twice per pixel (RGB + alpha halves); if the
        // next frame's upload overwrote the same texture between those reads
        // we would get a 1-frame spill. Alternating upload/composite targets
        // avoids the race with zero copy overhead.
        std::vector<std::unique_ptr<rt::Texture>> layerTexturesAlt;
        // Clip masks.
        std::vector<std::unique_ptr<rt::Texture>> maskTextures;
        std::vector<MaskCacheEntry> maskCache;
        // Effect masks: one per masked effect in the layer's chain,
        // rasterized in the clip's source-pixel grid.
        std::vector<std::vector<std::unique_ptr<rt::Texture>>> effectMaskTextures;
        std::vector<std::vector<MaskCacheEntry>> effectMaskCache;
        // Effect-output snapshots. The EffectProcessor has only two ping-pong
        // storage images shared by all layers, so when layer B's effect chain
        // runs after A's it overwrites what gpuLayers[A].textureInfo points
        // at (a dissolve between two effected clips would sample B on both
        // sides). Each layer's result is copied here right after its chain.
        std::vector<std::unique_ptr<rt::Texture>> effectOutputs;
        // Time interpolation: the second decoded frame's upload and the
        // synthesized output, immutable until the slot's fence is recycled.
        std::vector<std::unique_ptr<rt::Texture>> temporalSourceTextures;
        std::vector<PoolTexKey> temporalSourceTexKeys;
        std::vector<std::unique_ptr<rt::Texture>> temporalOutputs;

        /// Grow every list to hold at least `layerCount` layers.
        void ensureLayerCount(size_t layerCount);
        void clear();
    };
    std::array<SlotLayerResources, rt::kTemporalSubmissionSlots> m_slots;

    // Adjustment effects operate on a flattened full-frame composite. Keep
    // their ping-pong images separate from the clip EffectProcessor supplied
    // to composite(): resizing or reusing one processor while earlier clip
    // commands are still unsubmitted invalidates those recorded descriptors.
    std::unique_ptr<rt::EffectProcessor> m_adjustmentEffectProcessor;
    std::function<std::shared_ptr<rt::EffectProcessor>(uint32_t, uint32_t)>
        m_effectProcessorResolver;

    std::unique_ptr<rt::TemporalInterpolator> m_temporalInterpolator;

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
    VkCommandPool            m_renderCommandPool{VK_NULL_HANDLE};

    // Exponential backoff state removed in P2 of CLAUDE_IMPROVEMENT_PLAN.

    // ── Phase D: per-pass fault isolation ────────────────────────────
    // When an optional render-graph pass fails, its name is added here
    // and SKIPPED on every subsequent frame.  Prevents the executor
    // from re-trying a broken shader 60 times per second.  Cleared on
    // device reset only — a session is the failure scope.
    std::unordered_set<std::string> m_disabledPasses;

    // ── Render-graph composite ───────────────────────────────────────
    // compositeViaRenderGraph runs the stages below in order; the frame
    // state they share is FrameRecording (CompositeEngineFrame.h, which
    // also says which file holds each stage).
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> compositeViaRenderGraph(
        const std::vector<rt::LayerInfo>& layers,
        uint32_t outW, uint32_t outH,
        int64_t tick, bool scrubMode,
        bool gpuDisplayMode,
        const std::shared_ptr<rt::Compositor>& compositor,
        const std::shared_ptr<rt::EffectProcessor>& effectProcessor,
        const std::shared_ptr<rt::TransitionRenderer>& transitionRenderer,
        bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point& perfTlayers,
        std::chrono::high_resolution_clock::time_point& perfTgpuUp,
        std::chrono::high_resolution_clock::time_point& perfTcomp,
        int& effectLayerCount, int& effectPassCount,
        int& transitionCount,
        const CompositeCacheKey& cacheKey,
        bool allowLruInsert = true,
        bool forceSyncReadback = false);
    struct FrameRecording;
    [[nodiscard]] bool beginCompositeRecording(FrameRecording& frame);
    void ensureGpuTextureCache();
    void buildCompositeGraph(FrameRecording& frame);
    void prepareCompositorLayers(FrameRecording& frame);
    void uploadLayerMasks(FrameRecording& frame);
    void recordPasses(FrameRecording& frame);
    void recordUploadPass(FrameRecording& frame, uint32_t passIdx);
    void recordEffectPass(FrameRecording& frame, uint32_t passIdx);
    void recordTransitionPass(FrameRecording& frame, uint32_t passIdx);
    void recordCompositePass(FrameRecording& frame);
    void recordReadbackPass(FrameRecording& frame);
    void submitComposite(FrameRecording& frame);
    [[nodiscard]] std::shared_ptr<rt::CachedFrame> buildCompositeResult(
        FrameRecording& frame, const CompositeCacheKey& cacheKey,
        bool allowLruInsert);
    void logCompositeTimings(
        const FrameRecording& frame, bool perfLog,
        std::chrono::high_resolution_clock::time_point perfT0,
        std::chrono::high_resolution_clock::time_point perfTlayers,
        std::chrono::high_resolution_clock::time_point perfTgpuUp,
        std::chrono::high_resolution_clock::time_point perfTcomp) const;

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
