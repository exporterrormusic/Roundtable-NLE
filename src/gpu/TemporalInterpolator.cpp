/*
 * TemporalInterpolator - source-space frame blending / optical flow.
 */

#include <volk.h>

#include "TemporalInterpolator.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace rt {

TemporalInterpolator::~TemporalInterpolator()
{
    shutdown();
}

bool TemporalInterpolator::init(Device& device)
{
    if (m_initialized) return true;
    m_device = &device;

    if (!m_pipelineManager.create(device) ||
        !createDescriptorResources() ||
        !createPipeline()) {
        shutdown();
        return false;
    }

    m_initialized = true;
    spdlog::info("TemporalInterpolator initialized ({} descriptor slots)",
                 m_descriptorSets.size());
    return true;
}

void TemporalInterpolator::shutdown()
{
    if (!m_device) return;
    const VkDevice dev = m_device->handle();

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
    m_pipelineManager.destroy();
    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorSets.fill(VK_NULL_HANDLE);
    m_initialized = false;
    m_device = nullptr;
}

bool TemporalInterpolator::createDescriptorResources()
{
    const VkDevice dev = m_device->handle();
    VkDescriptorSetLayoutBinding bindings[3]{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    for (uint32_t i = 1; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 3;
    lci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr,
                                    &m_descriptorSetLayout) != VK_SUCCESS) {
        spdlog::error("TemporalInterpolator: descriptor layout creation failed");
        return false;
    }

    constexpr uint32_t setCount =
        kTemporalMaxLayers * kTemporalSubmissionSlots;
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = setCount;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = setCount * 2;

    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = setCount;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(dev, &pci, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
        spdlog::error("TemporalInterpolator: descriptor pool creation failed");
        return false;
    }

    std::array<VkDescriptorSetLayout, setCount> layouts{};
    layouts.fill(m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = setCount;
    ai.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(dev, &ai,
                                 m_descriptorSets.data()) != VK_SUCCESS) {
        spdlog::error("TemporalInterpolator: descriptor allocation failed");
        return false;
    }
    return true;
}

bool TemporalInterpolator::createPipeline()
{
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(TemporalInterpolationPushConstants);
    m_pipelineLayout = m_pipelineManager.createLayout(
        {range}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE) return false;

    const std::vector<fs::path> searchDirs = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() /
            "build" / "shaders",
        fs::current_path() / "shaders",
        fs::current_path().parent_path() / "shaders",
        fs::current_path().parent_path() / "build" / "shaders",
        fs::path("shaders")
    };
    fs::path shaderPath;
    for (const auto& dir : searchDirs) {
        const fs::path candidate = dir / "time_interpolate.comp.spv";
        if (fs::exists(candidate)) {
            shaderPath = candidate;
            break;
        }
    }
    if (shaderPath.empty()) {
        spdlog::error("TemporalInterpolator: time_interpolate.comp.spv not found");
        return false;
    }

    const VkShaderModule shader = m_pipelineManager.loadShader(shaderPath);
    if (shader == VK_NULL_HANDLE) return false;
    ComputePipelineConfig cfg{};
    cfg.compShader = shader;
    cfg.layout = m_pipelineLayout;
    m_pipeline = m_pipelineManager.createComputePipeline(cfg);
    return m_pipeline != VK_NULL_HANDLE;
}

bool TemporalInterpolator::render(
    VkCommandBuffer cmd,
    VkImageView outputView,
    const VkDescriptorImageInfo& sourceA,
    const VkDescriptorImageInfo& sourceB,
    uint32_t width, uint32_t height,
    float phase, int32_t mode,
    bool packedA, bool packedB,
    uint32_t submissionSlot,
    uint32_t layerIndex)
{
    if (!m_initialized || cmd == VK_NULL_HANDLE ||
        outputView == VK_NULL_HANDLE ||
        sourceA.imageView == VK_NULL_HANDLE ||
        sourceB.imageView == VK_NULL_HANDLE ||
        width == 0 || height == 0 ||
        submissionSlot >= kTemporalSubmissionSlots ||
        layerIndex >= kTemporalMaxLayers) {
        return false;
    }

    const uint32_t descriptorIndex =
        submissionSlot * kTemporalMaxLayers + layerIndex;
    const VkDescriptorSet set = m_descriptorSets[descriptorIndex];

    VkDescriptorImageInfo output{};
    output.imageView = outputView;
    output.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[3]{};
    const VkDescriptorImageInfo infos[3] = {output, sourceA, sourceB};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 0
            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);

    TemporalInterpolationPushConstants pc{};
    pc.width = static_cast<int32_t>(width);
    pc.height = static_cast<int32_t>(height);
    pc.phase = std::clamp(phase, 0.0f, 1.0f);
    pc.mode = mode;
    pc.packedA = packedA ? 1 : 0;
    pc.packedB = packedB ? 1 : 0;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    constexpr uint32_t wg = 16;
    vkCmdDispatch(cmd, (width + wg - 1) / wg, (height + wg - 1) / wg, 1);
    return true;
}

} // namespace rt
