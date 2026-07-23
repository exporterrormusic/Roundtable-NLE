/*
 * GpuRenderGraph.cpp — DAG dispatcher implementation.
 *
 * Implements:
 *   - Resource table management (declare, allocate, free)
 *   - Kahn's algorithm topological sort with cycle detection
 *   - Automatic pipeline barrier computation from resource access patterns
 *   - DAG execution loop with fault isolation and GPU timestamp queries
 */

#include "GpuRenderGraph.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace rt::render_graph {

// ============================================================================
// Construction / Destruction
// ============================================================================

GpuRenderGraph::GpuRenderGraph() = default;

GpuRenderGraph::~GpuRenderGraph()
{
    if (m_device != VK_NULL_HANDLE)
        destroy(m_device);
}

// ============================================================================
// Graph Construction
// ============================================================================

ResourceId GpuRenderGraph::declareResource(const RenderResource& resource)
{
    ResourceId id = static_cast<ResourceId>(m_resources.size());
    RenderResource res = resource;
    res.id = id;
    m_resources.push_back(std::move(res));
    return id;
}

uint32_t GpuRenderGraph::addPass(RenderPass pass)
{
    uint32_t idx = static_cast<uint32_t>(m_passes.size());
    pass.passIndex = idx;
    m_passes.push_back(std::move(pass));
    return idx;
}

uint32_t GpuRenderGraph::addUploadPass(
    const std::string& name,
    ResourceId dstTextureId,
    VkBuffer srcBuffer,
    VkDeviceSize srcOffset,
    const std::vector<VkBufferImageCopy>& regions,
    bool optional)
{
    RenderPass pass;
    pass.name = name;
    pass.type = PassType::Upload;
    pass.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; // not really used; dispatch is vkCmdCopy
    pass.outputs = {dstTextureId};
    pass.copyBuffer = srcBuffer;
    pass.copyBufferOffset = srcOffset;
    pass.copyRegions = regions;
    pass.optional = optional;
    pass.fatal = false;
    return addPass(std::move(pass));
}

uint32_t GpuRenderGraph::addReadbackPass(
    const std::string& name,
    ResourceId srcTextureId,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    const std::vector<VkBufferImageCopy>& regions,
    bool optional)
{
    RenderPass pass;
    pass.name = name;
    pass.type = PassType::Readback;
    pass.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; // not really used
    pass.inputs = {srcTextureId};
    pass.copyBuffer = dstBuffer;
    pass.copyBufferOffset = dstOffset;
    pass.copyRegions = regions;
    pass.optional = optional;
    pass.fatal = false;
    return addPass(std::move(pass));
}

uint32_t GpuRenderGraph::addComputePass(
    const std::string& name,
    PassType type,
    std::vector<ResourceId> inputs,
    std::vector<ResourceId> outputs,
    VkPipeline pipeline,
    VkPipelineLayout pipelineLayout,
    std::vector<VkDescriptorSet> descriptorSets,
    std::vector<uint8_t> pushConstants,
    uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ,
    bool optional, bool fatal)
{
    RenderPass pass;
    pass.name = std::move(name);
    pass.type = type;
    pass.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    pass.inputs = std::move(inputs);
    pass.outputs = std::move(outputs);
    pass.pipeline = pipeline;
    pass.pipelineLayout = pipelineLayout;
    pass.descriptorSets = std::move(descriptorSets);
    pass.pushConstants = std::move(pushConstants);
    pass.groupCountX = groupCountX;
    pass.groupCountY = groupCountY;
    pass.groupCountZ = groupCountZ;
    pass.optional = optional;
    pass.fatal = fatal;
    return addPass(std::move(pass));
}

// ============================================================================
// Compilation
// ============================================================================

bool GpuRenderGraph::compile(VkDevice device,
                             const GpuRenderGraphCompileOptions& options)
{
    m_device = device;

    // 1. Topological sort
    if (!topologicalSort()) {
        spdlog::error("[RENDER_GRAPH] Compilation failed: cycle detected ({} passes)", m_passes.size());
        dumpGraph("FAILED (cycle)");
        return false;
    }

    // 2. Compute automatic barriers
    computeBarriers();

    // 3. Set up timestamp query pool if requested
    if (options.enableTimestampQueries && options.maxTimestampQueries > 0) {
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = options.maxTimestampQueries * 2; // start + end per query

        if (vkCreateQueryPool(device, &qpci, nullptr, &m_timestampQueryPool) == VK_SUCCESS) {
            m_timestampQueriesEnabled = true;
            m_nextTimestampIndex = 0;

            // Assign timestamp indices to passes that want them
            for (auto& pass : m_passes) {
                if (pass.timestampQueryIndex == UINT32_MAX && m_nextTimestampIndex < options.maxTimestampQueries) {
                    pass.timestampQueryIndex = m_nextTimestampIndex++;
                }
            }
        } else {
            spdlog::warn("[RENDER_GRAPH] Failed to create timestamp query pool");
        }
    }

    // 4. Allocate transient resources
    if (!allocateTransientResources(device)) {
        spdlog::error("[RENDER_GRAPH] Failed to allocate transient resources");
        return false;
    }

    m_compiled = true;

    spdlog::debug("[RENDER_GRAPH] Compiled: {} passes, {} resources, {} barriers",
                  m_passes.size(), m_resources.size(),
                  [this]() {
                      size_t count = 0;
                      for (const auto& p : m_passes) {
                          count += p.preBarriers.size() + p.postBarriers.size();
                      }
                      return count;
                  }());

    return true;
}

// ── Kahn's algorithm topological sort ────────────────────────────────────

bool GpuRenderGraph::topologicalSort()
{
    const size_t n = m_passes.size();
    if (n == 0) {
        m_topologicalOrder.clear();
        return true;
    }

    // Build in-degree and adjacency lists.
    // Pass A depends on Pass B if any output resource of B is an input of A.
    // i.e., edge B→A means B must execute before A.

    // For each resource, find the pass that last writes it.
    // Then any pass that reads that resource depends on that writer.

    // Step 1: For each resource, record which pass WRITES it last.
    // (If multiple passes write same resource, only the last writer matters
    //  for dependency purposes — but that's an error we should flag.)
    std::unordered_map<ResourceId, uint32_t> lastWriter; // resource → pass index
    for (uint32_t pi = 0; pi < n; ++pi) {
        for (auto resId : m_passes[pi].outputs) {
            auto it = lastWriter.find(resId);
            if (it != lastWriter.end()) {
                // Multiple writers — this is normal for transient intermediates
                // (e.g., effect chain: effect_i writes, effect_i+1 writes again).
                // The dependency edge is from the PREVIOUS writer to this pass.
            }
            lastWriter[resId] = pi;
        }
    }

    // Step 2: Build adjacency: edges[writer] → list of readers that depend on writer.
    // Also compute in-degree for each pass.
    std::vector<std::vector<uint32_t>> edges(n);
    std::vector<uint32_t> inDegree(n, 0);

    for (uint32_t pi = 0; pi < n; ++pi) {
        for (auto resId : m_passes[pi].inputs) {
            auto it = lastWriter.find(resId);
            if (it != lastWriter.end()) {
                uint32_t writer = it->second;
                if (writer != pi) { // self-dependency is fine (output reused)
                    // Ensure we don't add duplicate edges
                    if (std::find(edges[writer].begin(), edges[writer].end(), pi) == edges[writer].end()) {
                        edges[writer].push_back(pi);
                        ++inDegree[pi];
                    }
                }
            }
        }
    }

    // Step 3: Kahn's algorithm
    std::queue<uint32_t> queue;
    for (uint32_t i = 0; i < n; ++i) {
        if (inDegree[i] == 0) {
            queue.push(i);
        }
    }

    m_topologicalOrder.clear();
    m_topologicalOrder.reserve(n);

    while (!queue.empty()) {
        uint32_t passIdx = queue.front();
        queue.pop();
        m_topologicalOrder.push_back(passIdx);

        for (uint32_t dependent : edges[passIdx]) {
            --inDegree[dependent];
            if (inDegree[dependent] == 0) {
                queue.push(dependent);
            }
        }
    }

    // If not all passes were processed, there's a cycle
    if (m_topologicalOrder.size() != n) {
        spdlog::error("[RENDER_GRAPH] Cycle detected: {}/{} passes topo-sorted",
                      m_topologicalOrder.size(), n);
        return false;
    }

    return true;
}

// ── Automatic barrier computation ────────────────────────────────────────

void GpuRenderGraph::computeBarriers()
{
    // State tracker: for each resource, what's its current access/layout?
    // Initialized from the resource declarations.
    struct ResourceState {
        VkImageLayout       layout{VK_IMAGE_LAYOUT_UNDEFINED};
        ResourceAccess      access{ResourceAccess::Undefined};
        VkPipelineStageFlags stage{0};
    };

    std::vector<ResourceState> resState(m_resources.size());
    for (size_t i = 0; i < m_resources.size(); ++i) {
        resState[i].layout = m_resources[i].currentLayout;
        resState[i].access = m_resources[i].currentAccess;
        resState[i].stage = m_resources[i].currentStage;
    }

    // Walk passes in topological order
    for (uint32_t passIdx : m_topologicalOrder) {
        auto& pass = m_passes[passIdx];

        // Determine the access pattern for this pass based on its type
        ResourceAccess inputAccess = ResourceAccess::ShaderRead;
        ResourceAccess outputAccess = ResourceAccess::ShaderWrite;
        VkPipelineStageFlags passStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

        switch (pass.type) {
        case PassType::Upload:
            inputAccess = ResourceAccess::TransferRead;   // reading from buffer
            outputAccess = ResourceAccess::TransferWrite;  // writing to image
            passStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case PassType::Readback:
            inputAccess = ResourceAccess::TransferRead;    // reading from image
            outputAccess = ResourceAccess::TransferWrite;  // writing to buffer
            passStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            break;
        case PassType::Effect:
        case PassType::Transition:
        case PassType::Composite:
            inputAccess = ResourceAccess::ShaderRead;
            outputAccess = ResourceAccess::ShaderWrite;
            passStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        case PassType::Present:
            inputAccess = ResourceAccess::ShaderRead;
            outputAccess = ResourceAccess::Present;
            passStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        case PassType::External:
        case PassType::Custom:
        default:
            inputAccess = ResourceAccess::ShaderRead;
            outputAccess = ResourceAccess::ShaderWrite;
            passStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            break;
        }

        // Compute pre-barriers for inputs
        for (ResourceId resId : pass.inputs) {
            if (resId >= m_resources.size()) continue;

            auto& state = resState[resId];
            VkImageLayout neededLayout = toVkImageLayout(inputAccess);
            VkAccessFlags neededAccess = toVkAccessFlags(inputAccess);

            // Only barrier if the resource is transitioning
            if (state.layout != neededLayout || state.access != inputAccess) {
                ImageBarrier barrier;
                barrier.resourceId = resId;
                barrier.oldLayout = state.layout;
                barrier.newLayout = neededLayout;
                barrier.srcAccessMask = toVkAccessFlags(state.access);
                barrier.dstAccessMask = neededAccess;
                barrier.srcStageMask = state.stage;
                barrier.dstStageMask = passStage;
                pass.preBarriers.push_back(barrier);

                // Update state
                state.layout = neededLayout;
                state.access = inputAccess;
                state.stage = passStage;
            }

            ++m_resources[resId].refCount;
        }

        // Compute pre-barriers for outputs (transition from whatever previous
        // state to the output access mode)
        for (ResourceId resId : pass.outputs) {
            if (resId >= m_resources.size()) continue;

            auto& state = resState[resId];
            VkImageLayout neededLayout = toVkImageLayout(outputAccess);
            VkAccessFlags neededAccess = toVkAccessFlags(outputAccess);

            if (state.layout != neededLayout || state.access != outputAccess) {
                ImageBarrier barrier;
                barrier.resourceId = resId;
                barrier.oldLayout = state.layout;
                barrier.newLayout = neededLayout;
                barrier.srcAccessMask = toVkAccessFlags(state.access);
                barrier.dstAccessMask = neededAccess;
                barrier.srcStageMask = state.stage;
                barrier.dstStageMask = passStage;
                pass.preBarriers.push_back(barrier);

                state.layout = neededLayout;
                state.access = outputAccess;
                state.stage = passStage;
            }
        }

        // For the last pass that uses a resource, add a post-barrier to
        // transition to SHADER_READ_ONLY or leave as-is.  We don't insert
        // post-barriers eagerly — the NEXT pass's pre-barriers handle this.
        // Post-barriers are only needed for Present (→ PRESENT_SRC_KHR).
        if (pass.type == PassType::Present) {
            for (ResourceId resId : pass.inputs) {
                if (resId >= m_resources.size()) continue;
                auto& state = resState[resId];
                // No post-barrier needed — Present takes care of itself
                // via the semaphore.
            }
        }
    }
}

int GpuRenderGraph::findLastWriter(ResourceId resId) const
{
    int lastWriter = -1;
    for (int pi = static_cast<int>(m_passes.size()) - 1; pi >= 0; --pi) {
        for (auto outId : m_passes[pi].outputs) {
            if (outId == resId) {
                return pi;
            }
        }
    }
    return lastWriter;
}

// ============================================================================
// Execution
// ============================================================================

bool GpuRenderGraph::execute(VkCommandBuffer cmd,
                             uint64_t frameIndex,
                             GpuRenderGraphFrameStats* outStats,
                             std::unordered_set<std::string>* disabledPassNames)
{
    if (!m_compiled) {
        spdlog::error("[RENDER_GRAPH] execute() called but graph not compiled");
        return false;
    }

    m_currentFrameIndex = frameIndex;

    // Reset timestamp query pool
    if (m_timestampQueryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_timestampQueryPool, 0,
                            m_nextTimestampIndex * 2);
    }

    GpuRenderGraphFrameStats stats;
    stats.frameIndex = frameIndex;

    // Walk passes in topological order
    for (uint32_t passIdx : m_topologicalOrder) {
        auto& pass = m_passes[passIdx];

        // Phase D: session-disabled passes are skipped entirely.  These
        // are optional passes that failed previously and have been
        // permanently muted to avoid re-trying a broken shader 60 fps.
        // Pre/post barriers are also skipped — downstream passes will
        // see this pass's output texture in whatever layout the previous
        // (successful) execution left it.
        if (disabledPassNames && !disabledPassNames->empty() &&
            disabledPassNames->count(pass.name) > 0)
        {
            ++stats.passesSkipped;
            continue;
        }

        // ── Timestamp start ──────────────────────────────────────
        if (m_timestampQueriesEnabled && pass.timestampQueryIndex != UINT32_MAX) {
            vkCmdWriteTimestamp(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                m_timestampQueryPool, pass.timestampQueryIndex * 2);
        }

        // ── Insert pre-barriers ──────────────────────────────────
        for (const auto& barrier : pass.preBarriers) {
            insertImageBarrier(cmd, barrier);
            ++stats.barriersInserted;
        }

        // ── Execute the pass ─────────────────────────────────────
        bool passOk = executePass(cmd, pass);

        if (!passOk) {
            if (pass.fatal) {
                spdlog::error("[RENDER_GRAPH] Fatal pass '{}' failed — frame aborted",
                              pass.name);
                stats.completed = false;
                if (outStats) *outStats = stats;
                return false;
            }
            if (pass.optional) {
                // Phase D: first-time failure of an optional pass.
                // Disable it for the rest of the session — re-recording
                // a known-broken shader 60 fps wastes CPU and clutters
                // the log.  Caller's set is mutated (when provided) so
                // the next frame's execute() short-circuits at the top.
                bool firstTime = true;
                if (disabledPassNames) {
                    auto [it, inserted] = disabledPassNames->insert(pass.name);
                    firstTime = inserted;
                }
                if (firstTime) {
                    spdlog::warn("[RENDER_GRAPH] Optional pass '{}' failed "
                                 "— disabled for the rest of this session",
                                 pass.name);
                } else {
                    spdlog::debug("[RENDER_GRAPH] Optional pass '{}' failed (already disabled)",
                                  pass.name);
                }
                ++stats.passesSkipped;
                // Still write timestamp for skipped passes
                goto write_timestamp;
            }
            // Non-fatal, non-optional: shouldn't normally happen.
            // Treat as fatal for safety.
            spdlog::error("[RENDER_GRAPH] Pass '{}' failed (non-optional, non-fatal)"
                          " — treating as fatal", pass.name);
            stats.completed = false;
            if (outStats) *outStats = stats;
            return false;
        }

        ++stats.passesExecuted;

        // ── Write post-barriers if any ───────────────────────────
        for (const auto& barrier : pass.postBarriers) {
            insertImageBarrier(cmd, barrier);
            ++stats.barriersInserted;
        }

    write_timestamp:
        // ── Timestamp end ────────────────────────────────────────
        if (m_timestampQueriesEnabled && pass.timestampQueryIndex != UINT32_MAX) {
            vkCmdWriteTimestamp(cmd,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                m_timestampQueryPool, pass.timestampQueryIndex * 2 + 1);
        }
    }

    stats.completed = true;

    // Resolve timestamp queries if we have a way to read them back
    // (caller must call this after fence signal)
    if (outStats) {
        *outStats = stats;
    }

    return true;
}

bool GpuRenderGraph::executePass(VkCommandBuffer cmd, const RenderPass& pass)
{
    switch (pass.type) {
    case PassType::Upload: {
        // Copy buffer → image
        for (const auto& region : pass.copyRegions) {
            vkCmdCopyBufferToImage(cmd, pass.copyBuffer,
                m_resources[pass.outputs[0]].image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &region);
        }
        return true;
    }

    case PassType::Readback: {
        // Copy image → buffer
        for (const auto& region : pass.copyRegions) {
            vkCmdCopyImageToBuffer(cmd,
                m_resources[pass.inputs[0]].image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                pass.copyBuffer, 1, &region);
        }
        return true;
    }

    case PassType::Effect:
    case PassType::Transition:
    case PassType::Composite:
    case PassType::Custom: {
        // Compute shader dispatch
        if (pass.pipeline == VK_NULL_HANDLE) {
            spdlog::error("[RENDER_GRAPH] Pass '{}': null pipeline", pass.name);
            return false;
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);

        if (!pass.descriptorSets.empty()) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                pass.pipelineLayout, 0,
                static_cast<uint32_t>(pass.descriptorSets.size()),
                pass.descriptorSets.data(), 0, nullptr);
        }

        if (!pass.pushConstants.empty()) {
            vkCmdPushConstants(cmd, pass.pipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT,
                0, static_cast<uint32_t>(pass.pushConstants.size()),
                pass.pushConstants.data());
        }

        vkCmdDispatch(cmd, pass.groupCountX, pass.groupCountY, pass.groupCountZ);
        return true;
    }

    case PassType::Present: {
        // For compute-only present, no work is needed in the command buffer.
        // The compositor's output image is already in GENERAL layout.
        // Presentation is handled by the swapchain via semaphore signaling,
        // which is done by the caller (CompositeEngine) after execute().
        return true;
    }

    case PassType::External: {
        // External resources are already present — nothing to do.
        return true;
    }

    default:
        spdlog::error("[RENDER_GRAPH] Pass '{}': unknown type {}", pass.name,
                      static_cast<int>(pass.type));
        return false;
    }
}

void GpuRenderGraph::insertImageBarrier(VkCommandBuffer cmd, const ImageBarrier& barrier)
{
    if (barrier.resourceId >= m_resources.size())
        return;

    const auto& res = m_resources[barrier.resourceId];

    // Descriptor-only external resources are synchronized by their owning
    // subsystem and intentionally have no VkImage registered with the graph.
    // Emitting a barrier for VK_NULL_HANDLE is invalid Vulkan and can trigger
    // validation errors or a driver fault.
    if (res.image == VK_NULL_HANDLE)
        return;

    VkImageMemoryBarrier imgBarrier{};
    imgBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imgBarrier.oldLayout = barrier.oldLayout;
    imgBarrier.newLayout = barrier.newLayout;
    imgBarrier.srcAccessMask = barrier.srcAccessMask;
    imgBarrier.dstAccessMask = barrier.dstAccessMask;
    imgBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imgBarrier.image = res.image;
    imgBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imgBarrier.subresourceRange.baseMipLevel = 0;
    imgBarrier.subresourceRange.levelCount = 1;
    imgBarrier.subresourceRange.baseArrayLayer = 0;
    imgBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        barrier.srcStageMask, barrier.dstStageMask,
        0, 0, nullptr, 0, nullptr, 1, &imgBarrier);
}

// ============================================================================
// Timestamp resolution
// ============================================================================

void GpuRenderGraph::resolveTimestampQueries(GpuRenderGraphFrameStats& stats)
{
    if (!m_timestampQueriesEnabled || m_timestampQueryPool == VK_NULL_HANDLE)
        return;

    if (m_device == VK_NULL_HANDLE)
        return;

    // Get physical device properties for timestamp period
    VkPhysicalDeviceProperties physProps{};
    // We cache the timestamp period in a static since it doesn't change
    static double s_timestampPeriod = 0.0;
    if (s_timestampPeriod == 0.0) {
        // Caller should provide this — we approximate
        s_timestampPeriod = 1.0; // nanoseconds
    }

    std::vector<uint64_t> timestamps(m_nextTimestampIndex * 2);
    VkResult result = vkGetQueryPoolResults(m_device, m_timestampQueryPool,
        0, static_cast<uint32_t>(timestamps.size()),
        timestamps.size() * sizeof(uint64_t), timestamps.data(),
        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);

    if (result != VK_SUCCESS)
        return;

    for (uint32_t passIdx : m_topologicalOrder) {
        const auto& pass = m_passes[passIdx];
        if (pass.timestampQueryIndex >= m_nextTimestampIndex)
            continue;

        uint64_t start = timestamps[pass.timestampQueryIndex * 2];
        uint64_t end = timestamps[pass.timestampQueryIndex * 2 + 1];

        if (end > start) {
            double deltaNs = static_cast<double>(end - start) * s_timestampPeriod;
            double deltaMs = deltaNs / 1000000.0;

            GpuRenderGraphFrameStats::PassTiming timing;
            timing.passName = pass.name;
            timing.gpuTimeMs = deltaMs;
            stats.passTimings.push_back(timing);
        }
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void GpuRenderGraph::reset()
{
    // Free transient resources from the previous frame
    if (m_device != VK_NULL_HANDLE) {
        freeTransientResources(m_device);
    }

    // Reset per-frame state
    m_currentFrameIndex = 0;
    m_nextTimestampIndex = 0;

    // Reset resource ref counts and state tracking
    for (auto& res : m_resources) {
        res.refCount = 0;
        res.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        res.currentAccess = ResourceAccess::Undefined;
        res.currentStage = 0;
    }

    // Clear per-pass barriers (they will be recomputed on next compile)
    for (auto& pass : m_passes) {
        pass.preBarriers.clear();
        pass.postBarriers.clear();
    }

    m_compiled = false;
}

void GpuRenderGraph::destroy(VkDevice device)
{
    if (device == VK_NULL_HANDLE)
        return;

    freeTransientResources(device);

    if (m_timestampQueryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, m_timestampQueryPool, nullptr);
        m_timestampQueryPool = VK_NULL_HANDLE;
    }

    m_passes.clear();
    m_resources.clear();
    m_topologicalOrder.clear();
    m_compiled = false;
    m_device = VK_NULL_HANDLE;
}

// ============================================================================
// Resource management
// ============================================================================

bool GpuRenderGraph::allocateTransientResources(VkDevice device)
{
    // For now, transient resources are expected to be pre-allocated by
    // the caller (CompositeEngine) via declareResource with external=true.
    // Full VMA-based transient allocation will be added in a later phase.

    // Validate that all external resources have non-null handles
    for (const auto& res : m_resources) {
        if (res.external) {
            // External resources are validated by the caller
            continue;
        }
        if (res.transient) {
            // Transient resources must be allocated externally for now
            spdlog::warn("[RENDER_GRAPH] Transient resource '{}' (id={}) not yet "
                         "supported — caller must pre-allocate with external=true",
                         res.name, res.id);
        }
    }

    return true;
}

void GpuRenderGraph::freeTransientResources(VkDevice device)
{
    // No-op for now — transient allocation/deallocation is caller-managed.
    // Will be implemented when TransientResourcePool is added.
}

// ============================================================================
// Debug
// ============================================================================

void GpuRenderGraph::dumpGraph(const char* label) const
{
    if (label) {
        spdlog::info("[RENDER_GRAPH] === DAG: {} ===", label);
    } else {
        spdlog::info("[RENDER_GRAPH] === DAG ({} passes, {} resources) ===",
                     m_passes.size(), m_resources.size());
    }

    // Print passes in topological order if available, else declaration order
    const auto& order = m_topologicalOrder.empty()
        ? [this]() {
              std::vector<uint32_t> idx(m_passes.size());
              for (uint32_t i = 0; i < m_passes.size(); ++i) idx[i] = i;
              return idx;
          }()
        : m_topologicalOrder;

    for (uint32_t passIdx : order) {
        if (passIdx >= m_passes.size()) continue;
        const auto& pass = m_passes[passIdx];

        std::string deps;
        for (size_t i = 0; i < pass.inputs.size(); ++i) {
            if (i > 0) deps += ", ";
            ResourceId id = pass.inputs[i];
            if (id < m_resources.size())
                deps += m_resources[id].name;
            else
                deps += "?";
        }

        std::string outs;
        for (size_t i = 0; i < pass.outputs.size(); ++i) {
            if (i > 0) outs += ", ";
            ResourceId id = pass.outputs[i];
            if (id < m_resources.size())
                outs += m_resources[id].name;
            else
                outs += "?";
        }

        spdlog::info("[RENDER_GRAPH]   [{:2}] {} ({})", passIdx, pass.name, toString(pass.type));
        if (!deps.empty())
            spdlog::info("[RENDER_GRAPH]         reads:  {}", deps);
        if (!outs.empty())
            spdlog::info("[RENDER_GRAPH]         writes: {}", outs);
        if (!pass.preBarriers.empty())
            spdlog::info("[RENDER_GRAPH]         {} barriers", pass.preBarriers.size());
        if (!pass.preBarriers.empty())
            spdlog::info("[RENDER_GRAPH]         optional={}, fatal={}",
                         pass.optional, pass.fatal);
    }
}

} // namespace rt::render_graph
