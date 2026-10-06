/*
 * GpuRenderGraph.h — DAG-based render graph for the GPU compositing pipeline.
 *
 * A render graph is a Directed Acyclic Graph where each node is a render
 * pass with explicit declared inputs, outputs, and resource transitions.
 * The graph dispatcher walks the DAG in topological order, inserting
 * automatic pipeline barriers between passes and executing each one.
 *
 * This is the GPU-level graph. Its input is the compositor's per-frame layer
 * list (the timeline's evaluated layers), as planned below.
 *
 * Typical roundtrip:
 *   1. CompositeGraphExecutor::plan() produces CompositeExecutionPlan
 *   2. GpuRenderGraph is built from the plan + layers[]
 *   3. compile() allocates resources, computes barriers, compiles pipelines
 *   4. execute() walks the DAG and dispatches each pass into VkCommandBuffer
 *   5. reset() clears transient state for next frame
 *
 * Belongs to rt::render_graph namespace.
 */

#pragma once

#include "RenderPass.h"
#include "RenderResource.h"

#include <volk.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace rt::render_graph {

// ── Compile-time options ───────────────────────────────────────────────────

struct GpuRenderGraphCompileOptions
{
    VkDescriptorPool    descriptorPool{VK_NULL_HANDLE};
    VkCommandPool       commandPool{VK_NULL_HANDLE};
    bool                enableTimestampQueries{false};
    uint32_t            maxTimestampQueries{64};
};

// ── Per-frame execution statistics ─────────────────────────────────────────

struct GpuRenderGraphFrameStats
{
    uint64_t            frameIndex{0};
    uint32_t            passesExecuted{0};
    uint32_t            passesSkipped{0};
    uint32_t            barriersInserted{0};
    bool                completed{false};

    // Per-pass GPU timing (populated if timestamp queries enabled)
    struct PassTiming {
        std::string     passName;
        double          gpuTimeMs{0.0};
    };
    std::vector<PassTiming> passTimings;
};

// ═════════════════════════════════════════════════════════════════════════════

class GpuRenderGraph
{
public:
    GpuRenderGraph();
    ~GpuRenderGraph();

    GpuRenderGraph(const GpuRenderGraph&) = delete;
    GpuRenderGraph& operator=(const GpuRenderGraph&) = delete;

    // ── Graph construction ──────────────────────────────────────────

    /// Declare a resource in the table.  Returns its ResourceId.
    [[nodiscard]] ResourceId declareResource(const RenderResource& resource);

    /// Add a render pass node.  Returns its index (passIndex).
    [[nodiscard]] uint32_t addPass(RenderPass pass);

    /// Shortcut: add a pass that copies buffer → image (upload).
    [[nodiscard]] uint32_t addUploadPass(
        const std::string& name,
        ResourceId dstTextureId,
        VkBuffer srcBuffer,
        VkDeviceSize srcOffset,
        const std::vector<VkBufferImageCopy>& regions,
        bool optional = true);

    /// Shortcut: add a pass that copies image → buffer (readback).
    [[nodiscard]] uint32_t addReadbackPass(
        const std::string& name,
        ResourceId srcTextureId,
        VkBuffer dstBuffer,
        VkDeviceSize dstOffset,
        const std::vector<VkBufferImageCopy>& regions,
        bool optional = true);

    /// Shortcut: add a compute dispatch pass (effect, transition, composite).
    [[nodiscard]] uint32_t addComputePass(
        const std::string& name,
        PassType type,
        std::vector<ResourceId> inputs,
        std::vector<ResourceId> outputs,
        VkPipeline pipeline,
        VkPipelineLayout pipelineLayout,
        std::vector<VkDescriptorSet> descriptorSets,
        std::vector<uint8_t> pushConstants,
        uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ = 1,
        bool optional = false, bool fatal = true);

    // ── Compilation ────────────────────────────────────────────────

    /// Compile the graph: topo-sort, compute barriers, prepare pipelines.
    /// Must be called once after construction and before execute().
    /// Returns false on compilation failure (cycle detected, missing resources).
    [[nodiscard]] bool compile(VkDevice device,
                               const GpuRenderGraphCompileOptions& options = {});

    /// Returns true if compile() has been called successfully.
    [[nodiscard]] bool isCompiled() const noexcept { return m_compiled; }

    // ── Execution ──────────────────────────────────────────────────

    /// Execute all passes in topological order into the given command buffer.
    /// Allocates transient resources and writes timestamp queries if enabled.
    /// Returns false if a fatal pass failed.
    ///
    /// Phase D (CLAUDE_IMPROVEMENT_PLAN): `disabledPassNames` is an
    /// owner-managed set of pass names that should be skipped entirely.
    /// When an optional pass fails, its name is added to this set so the
    /// executor stops re-trying it on every subsequent frame.  Pass
    /// nullptr to disable the safety net (testing / one-shot uses).
    [[nodiscard]] bool execute(VkCommandBuffer cmd,
                               uint64_t frameIndex,
                               GpuRenderGraphFrameStats* outStats = nullptr,
                               std::unordered_set<std::string>* disabledPassNames = nullptr);

    // ── Lifecycle ──────────────────────────────────────────────────

    /// Reset for the next frame.  Frees transient resources, clears
    /// per-frame state, but keeps the graph topology intact (passes +
    /// resources are not cleared — only pipeline/descriptor bindings are
    /// reset for re-recording).
    void reset();

    /// Full teardown.  Destroys all Vulkan resources owned by the graph.
    void destroy(VkDevice device);

    // ── Accessors ──────────────────────────────────────────────────

    [[nodiscard]] size_t passCount() const noexcept { return m_passes.size(); }
    [[nodiscard]] size_t resourceCount() const noexcept { return m_resources.size(); }
    [[nodiscard]] const RenderPass& pass(uint32_t index) const noexcept { return m_passes[index]; }
    [[nodiscard]] const RenderResource& resource(ResourceId id) const noexcept { return m_resources[id]; }

    /// Topological ordering of pass indices.  Only valid after compile().
    [[nodiscard]] const std::vector<uint32_t>& topologicalOrder() const noexcept
        { return m_topologicalOrder; }

    /// Mutable access to passes (for setting up pipeline state before compile).
    [[nodiscard]] std::vector<RenderPass>& mutablePasses() noexcept { return m_passes; }

    /// Debug dump of the graph topology to spdlog.
    void dumpGraph(const char* label = nullptr) const;

private:
    // ── Internal state ─────────────────────────────────────────────
    std::vector<RenderPass>     m_passes;
    std::vector<RenderResource> m_resources;
    std::vector<uint32_t>       m_topologicalOrder;   ///< Pass indices in exec order
    bool                        m_compiled{false};

    // Per-frame state (cleared in reset())
    uint64_t                    m_currentFrameIndex{0};
    VkQueryPool                 m_timestampQueryPool{VK_NULL_HANDLE};
    bool                        m_timestampQueriesEnabled{false};
    uint32_t                    m_nextTimestampIndex{0};

    // Device handle (cached from compile/destroy)
    VkDevice                    m_device{VK_NULL_HANDLE};

    // ── Compilation helpers ────────────────────────────────────────

    /// Kahn's algorithm topological sort.  Returns false if cycle detected.
    [[nodiscard]] bool topologicalSort();

    /// Compute automatic pipeline barriers between passes based on
    /// resource access transitions.
    void computeBarriers();

    /// Find the last pass that wrote to a given resource.
    [[nodiscard]] int findLastWriter(ResourceId resId) const;

    // ── Execution helpers ──────────────────────────────────────────

    /// Execute a single pass into the command buffer.
    bool executePass(VkCommandBuffer cmd, const RenderPass& pass);

    /// Insert a single image barrier into the command buffer.
    void insertImageBarrier(VkCommandBuffer cmd, const ImageBarrier& barrier);

    /// Resolve GPU timestamp queries after a fence signal.
    void resolveTimestampQueries(GpuRenderGraphFrameStats& stats);

    // ── Resource management ────────────────────────────────────────

    /// Allocate transient resources (textures, buffers) for this frame.
    bool allocateTransientResources(VkDevice device);

    /// Free transient resources.
    void freeTransientResources(VkDevice device);
};

} // namespace rt::render_graph
