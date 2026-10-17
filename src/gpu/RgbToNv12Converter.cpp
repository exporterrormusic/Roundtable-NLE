/*
 * RgbToNv12Converter.cpp — see RgbToNv12Converter.h.
 */

#include <volk.h>
#include "RgbToNv12Converter.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace rt {

namespace {

fs::path findShader(const char* name)
{
    fs::path candidates[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / name,
        fs::current_path() / "shaders" / name,
        fs::current_path().parent_path() / "shaders" / name,
        fs::current_path().parent_path() / "build" / "shaders" / name,
    };
    for (auto& p : candidates)
        if (fs::exists(p)) return p;
    return {};
}

struct PushConstants
{
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t swizzledRB;
};

} // namespace

RgbToNv12Converter::~RgbToNv12Converter()
{
    shutdown();
}

bool RgbToNv12Converter::init(VkDevice device)
{
    if (isInitialized()) return true;
    m_device = device;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_setLayout) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to create descriptor set layout");
        shutdown();
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = kSetRing;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = kSetRing;
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = kSetRing;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_pool) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to create descriptor pool");
        shutdown();
        return false;
    }

    std::array<VkDescriptorSetLayout, kSetRing> layouts;
    layouts.fill(m_setLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_pool;
    allocInfo.descriptorSetCount = kSetRing;
    allocInfo.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_sets.data()) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to allocate descriptor sets");
        shutdown();
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size       = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_setLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(m_device, &pipeLayoutCI, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to create pipeline layout");
        shutdown();
        return false;
    }

    const fs::path spvPath = findShader("rgb_to_nv12.comp.spv");
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (spvPath.empty() || !file.is_open()) {
        spdlog::warn("RgbToNv12Converter: rgb_to_nv12.comp.spv not found");
        shutdown();
        return false;
    }
    std::vector<char> spirv(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(spirv.size()));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());
    if (vkCreateShaderModule(m_device, &smCI, nullptr, &m_shader) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to create shader module");
        shutdown();
        return false;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_shader;
    pci.stage.pName  = "main";
    pci.layout       = m_pipelineLayout;
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr,
                                 &m_pipeline) != VK_SUCCESS) {
        spdlog::error("RgbToNv12Converter: failed to create compute pipeline");
        m_pipeline = VK_NULL_HANDLE;
        shutdown();
        return false;
    }
    return true;
}

void RgbToNv12Converter::shutdown()
{
    if (!m_device) return;
    if (m_pipeline)       vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_shader)         vkDestroyShaderModule(m_device, m_shader, nullptr);
    if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_pool)           vkDestroyDescriptorPool(m_device, m_pool, nullptr);
    if (m_setLayout)      vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    m_shader = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_pool = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_sets.fill(VK_NULL_HANDLE);
    m_device = VK_NULL_HANDLE;
}

bool RgbToNv12Converter::record(VkCommandBuffer cmd, VkImageView src,
                                uint32_t width, uint32_t height, bool swizzledRB,
                                VkBuffer dst, VkDeviceSize dstOffset)
{
    if (!isInitialized() || !src || !dst || width == 0 || height == 0 ||
        (height & 1u) != 0)
        return false;

    const VkDescriptorSet set = m_sets[m_nextSet];
    m_nextSet = (m_nextSet + 1) % kSetRing;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView   = src;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = dst;
    bufferInfo.offset = dstOffset;
    bufferInfo.range  = byteSize(width, height);

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo      = &imageInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo     = &bufferInfo;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    // The image's earlier writes (the composite) → this shader's reads.
    VkMemoryBarrier before{};
    before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &before, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &set, 0, nullptr);
    const PushConstants pc{static_cast<int32_t>(width), static_cast<int32_t>(height),
                           static_cast<int32_t>(stride(width)), swizzledRB ? 1 : 0};
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    const uint32_t blocksX = stride(width) / 4u;
    const uint32_t blocksY = height / 2u;
    vkCmdDispatch(cmd, (blocksX + 15u) / 16u, (blocksY + 7u) / 8u, 1);

    // This shader's writes → host reads and transfers (CUDA copies run after
    // the fence wait, which makes device writes available too).
    VkMemoryBarrier after{};
    after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &after, 0, nullptr, 0, nullptr);
    return true;
}

} // namespace rt
