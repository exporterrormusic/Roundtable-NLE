/*
 * Nv12ConverterPipeline.cpp — Pipeline creation for Nv12Converter.
 *
 * Extracted from Nv12Converter.cpp to reduce translation unit size.
 * Contains: findShader(), createDescriptorResources(), createPipeline(),
 *           createYuv420pPipeline()
 */

#include <volk.h>
#include "PathUtils.h"
#include "Nv12Converter.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

//══════════════════════════════════════════════════════════════════════════
//  Helper: locate SPIR-V shader file
//══════════════════════════════════════════════════════════════════════════

static fs::path findShader(const char* name)
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

//══════════════════════════════════════════════════════════════════════════
//  Internal — create descriptor resources
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Layout: binding 0 = Y sampler, binding 1 = UV sampler, binding 2 = output storage
    VkDescriptorSetLayoutBinding bindings[3]{};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create descriptor set layout");
        return false;
    }

    // Pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create descriptor pool");
        return false;
    }

    // Allocate set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate descriptor set");
        return false;
    }

    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Internal — create compute pipeline
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createPipeline()
{
    VkDevice dev = m_device->handle();

    // Pipeline layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo layoutCI{};
    layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCI.setLayoutCount         = 1;
    layoutCI.pSetLayouts            = &m_descriptorSetLayout;
    layoutCI.pushConstantRangeCount = 1;
    layoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutCI, nullptr,
                                &m_pipelineLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create pipeline layout");
        return false;
    }

    // Load shader
    fs::path spvPath = findShader("nv12_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: nv12_to_bgra.comp.spv not found — "
                     "shader not compiled yet, conversion will be unavailable");
        return false;
    }

    // Read SPIR-V binary
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", pathToUtf8(spvPath));
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_shaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create shader module");
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo pci{};
    pci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_shaderModule;
    pci.stage.pName  = "main";
    pci.layout = m_pipelineLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_pipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: compute pipeline created");
    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Internal — create YUV420P pipeline (lazy, called on first use)
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createYuv420pPipeline()
{
    VkDevice dev = m_device->handle();

    // Descriptor set layout: 3 samplers (Y, U, V) + 1 storage image (output)
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding         = 3;
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 4;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_yuv420pDescSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P descriptor set layout");
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_yuv420pDescPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P descriptor pool");
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_yuv420pDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_yuv420pDescSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_yuv420pDescSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate YUV420P descriptor set");
        return false;
    }

    // Pipeline layout
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_yuv420pDescSetLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr,
                                &m_yuv420pPipeLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P pipeline layout");
        return false;
    }

    // Load shader
    fs::path spvPath = findShader("yuv420p_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: yuv420p_to_bgra.comp.spv not found — "
                     "YUV420P GPU conversion unavailable");
        return false;
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", pathToUtf8(spvPath));
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_yuv420pShaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create YUV420P shader module");
        return false;
    }

    // Compute pipeline
    VkComputePipelineCreateInfo pci{};
    pci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_yuv420pShaderModule;
    pci.stage.pName  = "main";
    pci.layout = m_yuv420pPipeLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_yuv420pPipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUV420P compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: YUV420P compute pipeline created");
    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Internal — create P010 (10-bit NV12) pipeline.  Lazy: first P010 frame.
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createP010Pipeline()
{
    VkDevice dev = m_device->handle();

    // Layout matches NV12: 2 samplers (Y, UV) + 1 storage image (output).
    // The descriptor type doesn't carry the underlying texture format —
    // R8 and R16 sampled images use the same descriptor type — so the
    // layout is identical to NV12, but we keep a separate set so the
    // bound image views can be R16 while the NV12 set stays R8.
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 3;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_p010DescSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create P010 descriptor set layout");
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_p010DescPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create P010 descriptor pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_p010DescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_p010DescSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_p010DescSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate P010 descriptor set");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_p010DescSetLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr,
                                &m_p010PipeLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create P010 pipeline layout");
        return false;
    }

    fs::path spvPath = findShader("p010_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: p010_to_bgra.comp.spv not found — "
                     "P010 GPU conversion unavailable (HEVC 10-bit / AV1 10-bit "
                     "will fall back to CPU sws_scale)");
        return false;
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", pathToUtf8(spvPath));
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_p010ShaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create P010 shader module");
        return false;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_p010ShaderModule;
    pci.stage.pName  = "main";
    pci.layout       = m_p010PipeLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_p010Pipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create P010 compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: P010 compute pipeline created");
    return true;
}

bool Nv12Converter::ensureP010Pipeline()
{
    if (m_p010Pipeline != VK_NULL_HANDLE) return true;
    return createP010Pipeline();
}

//══════════════════════════════════════════════════════════════════════════
//  Internal — create YUVA444P12 (ProRes 4444) pipeline.  Lazy: first frame.
//  Layout: 4 samplers (Y, U, V, A) + 1 storage image (output).
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createYuva444p12Pipeline()
{
    VkDevice dev = m_device->handle();

    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[4].binding         = 4;
    bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 5;
    layoutCI.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_yuva444DescSetLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUVA444P12 descriptor set layout");
        return false;
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 4;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_yuva444DescPool) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUVA444P12 descriptor pool");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_yuva444DescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_yuva444DescSetLayout;

    if (vkAllocateDescriptorSets(dev, &allocInfo, &m_yuva444DescSet) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to allocate YUVA444P12 descriptor set");
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(int32_t) * 2; // width, height

    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_yuva444DescSetLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(dev, &pipeLayoutCI, nullptr,
                                &m_yuva444PipeLayout) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUVA444P12 pipeline layout");
        return false;
    }

    fs::path spvPath = findShader("yuva444p12_to_bgra.comp.spv");
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: yuva444p12_to_bgra.comp.spv not found — "
                     "ProRes 4444 GPU conversion unavailable (falls back to CPU sws_scale)");
        return false;
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", pathToUtf8(spvPath));
        return false;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    if (vkCreateShaderModule(dev, &smCI, nullptr, &m_yuva444ShaderModule) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create YUVA444P12 shader module");
        return false;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = m_yuva444ShaderModule;
    pci.stage.pName  = "main";
    pci.layout       = m_yuva444PipeLayout;

    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &m_yuva444Pipeline) != VK_SUCCESS)
    {
        spdlog::error("Nv12Converter: failed to create YUVA444P12 compute pipeline");
        return false;
    }

    spdlog::info("Nv12Converter: YUVA444P12 (ProRes 4444) compute pipeline created");
    return true;
}

bool Nv12Converter::ensureYuva444p12Pipeline()
{
    if (m_yuva444Pipeline != VK_NULL_HANDLE) return true;
    return createYuva444p12Pipeline();
}

//══════════════════════════════════════════════════════════════════════════
//  Phase 4.2 — RGBA16F output pipelines (export passthrough).
//  These reuse the matching 8-bit format's descriptor-set layout + pipeline
//  layout (a storage image's FORMAT is not part of the layout), so only a new
//  shader module + pipeline is created, bound to the R16G16B16A16_SFLOAT
//  output texture.
//══════════════════════════════════════════════════════════════════════════

static VkPipeline createComputePipelineFromSpv(VkDevice dev, const char* spvName,
                                               VkPipelineLayout layout,
                                               VkShaderModule& outModule)
{
    fs::path spvPath = findShader(spvName);
    if (spvPath.empty()) {
        spdlog::warn("Nv12Converter: {} not found — 16F convert unavailable "
                     "(export passthrough falls back to the 8-bit path)", spvName);
        return VK_NULL_HANDLE;
    }

    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::error("Nv12Converter: failed to open {}", pathToUtf8(spvPath));
        return VK_NULL_HANDLE;
    }
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> spirv(fileSize);
    file.seekg(0);
    file.read(spirv.data(), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo smCI{};
    smCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCI.codeSize = spirv.size();
    smCI.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smCI, nullptr, &module) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create shader module for {}", spvName);
        return VK_NULL_HANDLE;
    }

    VkComputePipelineCreateInfo pci{};
    pci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = module;
    pci.stage.pName  = "main";
    pci.layout       = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        spdlog::error("Nv12Converter: failed to create compute pipeline for {}", spvName);
        vkDestroyShaderModule(dev, module, nullptr);
        return VK_NULL_HANDLE;
    }
    outModule = module;
    return pipeline;
}

bool Nv12Converter::ensureP010Pipeline16F()
{
    if (m_p010Pipeline16F != VK_NULL_HANDLE) return true;
    // ensureP010Pipeline() builds the shared descriptor set + pipeline layout
    // (and the 8-bit pipeline, whose spv always ships).
    if (!ensureP010Pipeline()) return false;
    m_p010Pipeline16F = createComputePipelineFromSpv(
        m_device->handle(), "p010_to_rgba16f.comp.spv",
        m_p010PipeLayout, m_p010ShaderModule16F);
    if (m_p010Pipeline16F != VK_NULL_HANDLE)
        spdlog::info("Nv12Converter: P010 → RGBA16F compute pipeline created");
    return m_p010Pipeline16F != VK_NULL_HANDLE;
}

bool Nv12Converter::ensureYuva444p12Pipeline16F()
{
    if (m_yuva444Pipeline16F != VK_NULL_HANDLE) return true;
    if (!ensureYuva444p12Pipeline()) return false;
    m_yuva444Pipeline16F = createComputePipelineFromSpv(
        m_device->handle(), "yuva444p12_to_rgba16f.comp.spv",
        m_yuva444PipeLayout, m_yuva444ShaderModule16F);
    if (m_yuva444Pipeline16F != VK_NULL_HANDLE)
        spdlog::info("Nv12Converter: YUVA444P12 → RGBA16F compute pipeline created");
    return m_yuva444Pipeline16F != VK_NULL_HANDLE;
}

} // namespace rt
