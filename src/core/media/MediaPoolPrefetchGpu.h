/*
 * MediaPoolPrefetchGpu.h — Vulkan-aware helpers for the GPU-resident
 * prefetch decode path (UPGRADE_PLAN Phase 4).
 *
 * Kept in its own header so the broader prefetch translation units
 * (MediaPoolPrefetchSchedule.cpp, MediaPoolPrefetchDecode.cpp, etc.)
 * are not forced to pull in Vulkan headers — only the two TUs that
 * actually touch GPU state (MediaPoolPrefetchSchedule.cpp for per-worker
 * lifecycle, MediaPoolPrefetchConvertGpu.cpp for the convert + copy
 * pipeline) include this.
 */

#pragma once

#include "vulkan/CommandPool.h"
#include "vulkan/Texture.h"   // Texture::StagingCleanup

#include <vulkan/vulkan_core.h>
#include <deque>
#include <memory>
#include <vector>

namespace rt {

class Nv12Converter;
class CudaVulkanInterop;
struct SharedAllocation;

// Forward decls so the helper signature compiles without including
// MediaPool.h here.
class  MediaPool;
struct PrefetchDecoderState;
struct PrefetchTask;
struct DecodedFrame;
struct CachedFrame;

/// Per-prefetch-worker Vulkan resources.  Allocated on the worker stack
/// in MediaPool::prefetchWorker; destroyed when the worker exits.
///
/// cmdPool: spec-correct per-worker VkCommandPool, addresses the May
///   2026 crash documented in UPGRADE_PLAN section C.
/// signalSem: reserved for UPGRADE_PLAN Phase 6 — the compositor-bound
///   inter-queue signal.  Created here so the lifecycle is wired up,
///   not used until PR-5 lands and the flag flips.
/// device: stored solely so the destructor can call vkDestroySemaphore;
///   the cmdPool already remembers its own VkDevice.
struct WorkerGpuState {
    rt::CommandPool cmdPool;
    VkSemaphore     signalSem{VK_NULL_HANDLE};
    VkDevice        device{VK_NULL_HANDLE};

    /// Per-worker Nv12Converter (UPGRADE_PLAN item 3 — per-worker
    /// descriptor sets refactor, 2026-05-22).  Each prefetch worker
    /// gets its OWN converter instance so the input planar textures,
    /// output texture, and descriptor sets are not shared across
    /// workers — removing the apiMutex bottleneck and the inline
    /// vkWaitForFences that was serialising all workers through a
    /// single shared converter.  Lazy-created the first time the
    /// worker takes the GPU-resident decode path (so a worker that
    /// only ever sees CPU-fallback frames never allocates one).
    std::unique_ptr<Nv12Converter> nv12Converter;
    uint32_t                       nv12ConverterW{0};
    uint32_t                       nv12ConverterH{0};

    // ── Chroma-key pass (2026-05-27) ─────────────────────────────────
    // Inline compute dispatch for GREEN-suffixed chroma-key media.
    // Runs after Nv12Converter produces BGRA; samples the converter
    // output texture, writes keyed BGRA directly into the pooled
    // destination texture.  Uses the existing chroma_key.comp shader.
    VkPipeline              chromaKeyPipeline{VK_NULL_HANDLE};
    VkPipelineLayout        chromaKeyLayout{VK_NULL_HANDLE};
    VkShaderModule          chromaKeyShader{VK_NULL_HANDLE};
    VkDescriptorSetLayout   chromaKeyDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool        chromaKeyDescPool{VK_NULL_HANDLE};
    VkDescriptorSet         chromaKeyDescSet{VK_NULL_HANDLE};

    // Stored VkDevice+Allocator for lazy-init (borrowed from GpuContext).
    VkDevice                ckDevice{VK_NULL_HANDLE};
    // Initialised flag
    bool                    chromaKeyReady{false};

    /// Deferred-cleanup ring. Each entry holds the per-submission
    /// resources that can only be freed once the GPU has finished
    /// executing the submission. pollAndCleanup() walks the deque in
    /// submit-order; the destructor drains anything still in flight.
    ///
    /// Path C (2026-05-22) note: under the previous single-queue model
    /// (both compositor and prefetch on GpuQueueKind::Compute), Vulkan's
    /// per-queue FIFO ordering meant the convert+copy submission was
    /// guaranteed to be visible to any later compositor submission that
    /// sampled the destination texture without an explicit wait.  After
    /// Path C the compositor submits on GpuQueueKind::Graphics, so that
    /// guarantee no longer holds.  convertDecodedToCacheGpu now blocks
    /// on this fence inline before returning the CachedFrame — see the
    /// "Cross-queue visibility wait" comment in
    /// MediaPoolPrefetchConvertGpu.cpp.  By the time pollAndCleanup
    /// runs over an entry in this deque, its fence is already signalled
    /// and the cleanup is purely a resource-reclaim step.
    ///
    /// sharedAlloc (when set) is returned to its interop pool on
    /// cleanup — the zero-copy CUDA buffer can't be released until the
    /// GPU has finished reading from it.
    struct PendingSubmit {
        VkFence                              fence{VK_NULL_HANDLE};
        VkCommandBuffer                      cmdBuf{VK_NULL_HANDLE};
        std::vector<Texture::StagingCleanup> staging;
        std::unique_ptr<SharedAllocation>    sharedAlloc;
        CudaVulkanInterop*                   interop{nullptr};
        // Holds a ref to the per-frame dst Texture so it isn't returned
        // to PrefetchTexturePool by the CachedFrame's gpuTextureOwner
        // deleter before the GPU has finished writing to it.  Without
        // this, fast FrameCache eviction (within the few ms before the
        // fence signals) could drop the texture's refcount to 0 and
        // free its VkImage while the convert+copy is still pending.
        std::shared_ptr<Texture>             dstHold;
    };
    std::deque<PendingSubmit> pending;

    // Constructor + destructor defined out-of-line in
    // MediaPoolPrefetchConvertGpu.cpp so the complete type of
    // Nv12Converter (held by unique_ptr above) is in scope when the
    // implicit member destructors are generated.
    WorkerGpuState();
    ~WorkerGpuState();

    WorkerGpuState(const WorkerGpuState&) = delete;
    WorkerGpuState& operator=(const WorkerGpuState&) = delete;

    /// True when both cmdPool and signalSem are valid — i.e. the worker
    /// can take the GPU-resident decode path.
    [[nodiscard]] bool ready() const noexcept {
        return cmdPool.handle() != VK_NULL_HANDLE
            && signalSem != VK_NULL_HANDLE;
    }

    /// Non-blocking sweep: free resources for any pending submission
    /// whose fence has signalled. Called at the start of every
    /// convertDecodedToCacheGpu so the deque drains at the natural
    /// rate of subsequent prefetch work. O(N) in completed entries
    /// only; stops at the first not-yet-signalled fence (submit
    /// order = signal order).
    void pollAndCleanup();

    /// Lazy-create (or reuse) this worker's Nv12Converter, sized so
    /// that the convert shader writes (w, h) BGRA into the output
    /// texture.  Returns nullptr on init failure (GpuContext not up,
    /// queue family unavailable, etc.).  Internal calls into
    /// ensureOutputSize within the converter will resize the output
    /// texture if the dst dimensions change.
    Nv12Converter* ensureNv12Converter(uint32_t w, uint32_t h);

    /// Lazy-create (or reuse) the chroma-key compute pass.  Called once
    /// per worker the first time a GREEN file is encountered.  Borrows
    /// VkDevice from GpuContext — must be called after GpuContext::init.
    /// Returns false if the shader isn't compiled or GPU init failed.
    bool ensureChromaKeyPass();
};

/// Dispatch helper: wraps the eligibility checks (PR-4 feature flag,
/// wgs validity) around MediaPool::convertDecodedToCacheGpu so callers in
/// translation units that include this header do not need to depend on
/// CompositeService.h or on the WorkerGpuState methods directly.
///
/// Returns nullptr on any ineligibility OR on GPU-path failure;
/// callers must fall back to the CPU convertDecodedToCache path.
std::shared_ptr<CachedFrame> tryConvertDecodedToCacheGpu(
    MediaPool&            pool,
    PrefetchDecoderState& state,
    const PrefetchTask&   task,
    DecodedFrame&         decoded,
    int64_t               frameNumber,
    WorkerGpuState*       wgs);

} // namespace rt
