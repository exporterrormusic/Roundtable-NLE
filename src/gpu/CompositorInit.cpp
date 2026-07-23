/*
 * CompositorInit.cpp -- init, shutdown, create* methods.
 *
 * Split from Compositor.cpp for maintainability.
 */

#include <volk.h>
#include "Compositor.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cmath>


namespace fs = std::filesystem;

namespace rt {

// ── Constructor / Destructor ────────────────────────────────────────────────

Compositor::Compositor() = default;

Compositor::~Compositor()
{
    shutdown();
}

// ── init ────────────────────────────────────────────────────────────────────

bool Compositor::init(Device& device,
                      Allocator& allocator,
                      CommandPool& cmdPool,
                      VkQueue computeQueue,
                      const CompositorConfig& config)
{
    if (m_initialized)
    {
        spdlog::warn("Compositor already initialized");
        return true;
    }

    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    // Initialize pipeline manager
    if (!m_pipelineManager.create(device))
    {
        spdlog::error("Compositor: Failed to create pipeline manager");
        return false;
    }

    // Get timestamp period for GPU timing
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    // Create resources
    if (!createOutputTexture())      { shutdown(); return false; }
    if (!createDescriptorResources()){ shutdown(); return false; }
    if (!createComputePipeline())    { shutdown(); return false; }
    if (!createTimestampQueries())   { shutdown(); return false; }

    // Create one host-visible SSBO per output slot. Descriptor sets using an
    // older slot may still be pending when the next frame is recorded.
    for (uint32_t i = 0; i < kOutputRing; ++i) {
        if (!m_layerParamsBuffers[i].create(m_allocator->handle(),
                                            sizeof(LayerParamsGPU),
                                            BufferUsage::Uniform)) {
            spdlog::error("Compositor: Failed to create layer params SSBO {}", i);
            shutdown();
            return false;
        }
    }

    // Create 1x1 transparent placeholder texture for unused layer slots
    {
        uint32_t transparent = 0x00000000;
        TextureConfig placeholderConfig;
        placeholderConfig.width  = 1;
        placeholderConfig.height = 1;
        placeholderConfig.format = VK_FORMAT_R8G8B8A8_UNORM;
        placeholderConfig.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_placeholderTexture.createFromData(
                m_allocator->handle(), device.handle(),
                placeholderConfig,
                &transparent, 4,
                cmdPool, computeQueue))
        {
            spdlog::error("Compositor: Failed to create placeholder texture");
            shutdown();
            return false;
        }
    }

    // Initialize descriptor set with placeholder textures
    m_layersDirty = true;
    clearLayers();
    updateSSBO();
    updateDescriptorSet();

    m_initialized = true;
    spdlog::info("Compositor initialized ({}x{}, max {} layers)",
                 config.outputWidth, config.outputHeight, kMaxCompositorLayers);
    return true;
}

// ── shutdown ────────────────────────────────────────────────────────────────

void Compositor::shutdown()
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();

    // Wait for GPU idle before destroying
    if (dev != VK_NULL_HANDLE)
        vkDeviceWaitIdle(dev);

    // Destroy timestamp query pool
    for (auto& queryPool : m_queryPools) {
        if (queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(dev, queryPool, nullptr);
            queryPool = VK_NULL_HANDLE;
        }
    }

    // Destroy descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSets.fill(VK_NULL_HANDLE);
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Destroy pipeline
    m_pipelineManager.destroy();
    m_pipeline       = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;

    // Destroy buffers and textures
    for (auto& staging : m_readbackStaging) staging.destroy();
    for (auto& params : m_layerParamsBuffers) params.destroy();
    m_placeholderTexture.destroy();
    m_outputTexture.reset();
    for (auto& t : m_outputRing) t.reset();

    m_layers.clear();
    m_layerCount  = 0;
    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
}

// ── createOutputTexture ─────────────────────────────────────────────────────

bool Compositor::createOutputTexture()
{
    TextureConfig cfg;
    cfg.width  = m_config.outputWidth;
    cfg.height = m_config.outputHeight;
    cfg.format = m_config.outputFormat;
    // Need STORAGE for compute write, SAMPLED for reading, TRANSFER for readback
    cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Enable concurrent sharing between compute and graphics queue families
    // so the output can be sampled without ownership transfers.
    const auto& families = m_device->queueFamilies();
    uint32_t computeFamily  = families.compute.value_or(families.graphics.value());
    uint32_t graphicsFamily = families.graphics.value();
    if (computeFamily != graphicsFamily) {
        cfg.concurrentQueueFamilies = {computeFamily, graphicsFamily};
    }

    // Create every slot of the output ring and transition each to GENERAL
    // for compute-shader writes.  composite() rotates m_outputTexture
    // through these so consecutive (and nested inner/outer) composites
    // never share a texture the presenter is still sampling.
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    for (uint32_t i = 0; i < kOutputRing; ++i)
    {
        auto tex = std::make_shared<Texture>();
        if (!tex->create(m_allocator->handle(), m_device->handle(), cfg))
        {
            spdlog::error("Compositor: Failed to create output ring texture {}", i);
            m_cmdPool->endSingleTime(cmd, m_queue);
            for (auto& t : m_outputRing) t.reset();
            m_outputTexture.reset();
            return false;
        }
        tex->transitionLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        m_outputRing[i] = std::move(tex);
    }
    m_cmdPool->endSingleTime(cmd, m_queue);

    m_outputRingIdx = 0;
    m_outputTexture = m_outputRing[0];

    return true;
}

// ── createComputePipeline ───────────────────────────────────────────────────

bool Compositor::createComputePipeline()
{
    // Find shader
    fs::path shaderPaths[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / "composite.comp.spv",
        fs::current_path() / "shaders" / "composite.comp.spv",
        fs::current_path().parent_path() / "shaders" / "composite.comp.spv",
        fs::current_path().parent_path() / "build" / "shaders" / "composite.comp.spv",
    };

    fs::path shaderPath;
    for (auto& p : shaderPaths)
    {
        if (fs::exists(p)) { shaderPath = p; break; }
    }

    if (shaderPath.empty())
    {
        spdlog::error("Compositor: composite.comp.spv not found");
        return false;
    }

    VkShaderModule compShader = m_pipelineManager.loadShader(shaderPath);
    if (compShader == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to load composite.comp.spv");
        return false;
    }

    // Create pipeline layout with push constants + descriptor set layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(CompositePushConstants);

    m_pipelineLayout = m_pipelineManager.createLayout({pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to create pipeline layout");
        return false;
    }

    ComputePipelineConfig pipeConfig;
    pipeConfig.compShader = compShader;
    pipeConfig.layout     = m_pipelineLayout;

    m_pipeline = m_pipelineManager.createComputePipeline(pipeConfig);
    if (m_pipeline == VK_NULL_HANDLE)
    {
        spdlog::error("Compositor: Failed to create compute pipeline");
        return false;
    }

    spdlog::debug("Compositor: Compute pipeline created successfully");
    return true;
}

// ── createDescriptorResources ───────────────────────────────────────────────

bool Compositor::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // ── Descriptor set layout ───────────────────────────────────────────
    // binding 0: storage image (output)
    // binding 1: combined image sampler array[32] (layer textures)
    // binding 2: storage buffer (layer params SSBO)
    // binding 3: combined image sampler array[32] (mask textures)

    VkDescriptorSetLayoutBinding bindings[4] = {};

    // Binding 0: output storage image
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: layer textures (array of 32 samplers)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = kMaxCompositorLayers;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: layer params SSBO
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 3: mask textures (array of 32 samplers)
    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = kMaxCompositorLayers;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to create descriptor set layout");
        return false;
    }

    // ── Descriptor pool ─────────────────────────────────────────────────
    VkDescriptorPoolSize poolSizes[4] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = kOutputRing;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kMaxCompositorLayers * kOutputRing;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = kOutputRing;
    poolSizes[3].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[3].descriptorCount = kMaxCompositorLayers * kOutputRing;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = kOutputRing;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to create descriptor pool");
        return false;
    }

    // ── Allocate descriptor set ─────────────────────────────────────────
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    std::array<VkDescriptorSetLayout, kOutputRing> layouts{};
    layouts.fill(m_descriptorSetLayout);
    allocInfo.descriptorSetCount = kOutputRing;
    allocInfo.pSetLayouts        = layouts.data();

    if (vkAllocateDescriptorSets(dev, &allocInfo,
                                 m_descriptorSets.data()) != VK_SUCCESS)
    {
        spdlog::error("Compositor: Failed to allocate descriptor set");
        return false;
    }

    return true;
}

// ── createTimestampQueries ──────────────────────────────────────────────────

bool Compositor::createTimestampQueries()
{
    for (uint32_t i = 0; i < kOutputRing; ++i) {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;  // begin + end

        if (vkCreateQueryPool(m_device->handle(), &queryInfo, nullptr,
                              &m_queryPools[i]) != VK_SUCCESS) {
            spdlog::warn("Compositor: Failed to create timestamp query pool {} "
                         "(timing disabled for slot)", i);
            m_queryPools[i] = VK_NULL_HANDLE;
        }
    }

    return true;
}


} // namespace rt
