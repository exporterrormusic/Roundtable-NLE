/*
 * Pipeline — Vulkan graphics and compute pipeline management.
 *
 * Step 2: Handles SPIR-V shader loading, pipeline layout creation,
 * and pipeline cache. Supports both graphics (Spine, text, waveform)
 * and compute (compositor, effects) pipelines.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>

namespace rt {

class Device;

/// Configuration for creating a graphics pipeline.
struct GraphicsPipelineConfig
{
    // Shaders
    VkShaderModule          vertShader{VK_NULL_HANDLE};
    VkShaderModule          fragShader{VK_NULL_HANDLE};

    // Vertex input
    std::vector<VkVertexInputBindingDescription>   vertexBindings;
    std::vector<VkVertexInputAttributeDescription>  vertexAttributes;

    // Pipeline state
    VkPrimitiveTopology     topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPolygonMode           polygonMode{VK_POLYGON_MODE_FILL};
    VkCullModeFlags         cullMode{VK_CULL_MODE_NONE};
    VkFrontFace             frontFace{VK_FRONT_FACE_COUNTER_CLOCKWISE};
    bool                    depthTestEnable{false};
    bool                    depthWriteEnable{false};
    bool                    blendEnable{true};

    // Blend state (premultiplied alpha by default for Spine)
    VkBlendFactor           srcColorBlend{VK_BLEND_FACTOR_ONE};
    VkBlendFactor           dstColorBlend{VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    VkBlendFactor           srcAlphaBlend{VK_BLEND_FACTOR_ONE};
    VkBlendFactor           dstAlphaBlend{VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};

    // Dynamic rendering (Vulkan 1.3)
    VkFormat                colorFormat{VK_FORMAT_B8G8R8A8_SRGB};
    VkFormat                depthFormat{VK_FORMAT_UNDEFINED};

    // Pipeline layout
    VkPipelineLayout        layout{VK_NULL_HANDLE};

    // Push constants
    std::vector<VkPushConstantRange> pushConstants;

    // Descriptor set layouts
    std::vector<VkDescriptorSetLayout> descriptorLayouts;
};

/// Configuration for creating a compute pipeline.
struct ComputePipelineConfig
{
    VkShaderModule          compShader{VK_NULL_HANDLE};
    VkPipelineLayout        layout{VK_NULL_HANDLE};
    std::vector<VkPushConstantRange>    pushConstants;
    std::vector<VkDescriptorSetLayout>  descriptorLayouts;
};

/// Pipeline manager — creates, caches, and destroys pipelines.
class PipelineManager
{
public:
    PipelineManager() = default;
    ~PipelineManager();

    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    /// Initialize with device. Loads pipeline cache from disk if available.
    bool create(const Device& device, const std::filesystem::path& cacheDir = "");

    /// Destroy all pipelines and the cache.
    void destroy();

    // ── Shader loading ──────────────────────────────────────────────────

    /// Load a SPIR-V shader from file.
    VkShaderModule loadShader(const std::filesystem::path& spirvPath);

    /// Destroy a shader module.
    void destroyShader(VkShaderModule shader);

    // ── Pipeline layout ─────────────────────────────────────────────────

    /// Create a pipeline layout from push constants and descriptor set layouts.
    VkPipelineLayout createLayout(
        const std::vector<VkPushConstantRange>& pushConstants = {},
        const std::vector<VkDescriptorSetLayout>& descriptorLayouts = {});

    void destroyLayout(VkPipelineLayout layout);

    // ── Pipeline creation ───────────────────────────────────────────────

    /// Create a graphics pipeline using dynamic rendering (VK 1.3).
    VkPipeline createGraphicsPipeline(const GraphicsPipelineConfig& config);

    /// Create a compute pipeline.
    VkPipeline createComputePipeline(const ComputePipelineConfig& config);

    /// Destroy a pipeline.
    void destroyPipeline(VkPipeline pipeline);

    // ── Cache ───────────────────────────────────────────────────────────

    /// Save pipeline cache to disk for faster startup next time.
    bool savePipelineCache(const std::filesystem::path& path);

    [[nodiscard]] VkPipelineCache pipelineCache() const noexcept { return m_pipelineCache; }

private:
    VkDevice                        m_device{VK_NULL_HANDLE};
    VkPipelineCache                 m_pipelineCache{VK_NULL_HANDLE};
    std::vector<VkShaderModule>     m_shaders;
    std::vector<VkPipeline>         m_pipelines;
    std::vector<VkPipelineLayout>   m_layouts;
};

} // namespace rt
