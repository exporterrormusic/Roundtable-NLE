/*
 * Pipeline.cpp — Vulkan pipeline management.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "PathUtils.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Device.h"

#include <spdlog/spdlog.h>

#include <fstream>

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

PipelineManager::~PipelineManager()
{
    destroy();
}

// ── create ──────────────────────────────────────────────────────────────────

bool PipelineManager::create(const Device& device, const std::filesystem::path& cacheDir)
{
    m_device = device.handle();

    // Create pipeline cache (accelerates subsequent pipeline creation)
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    // Try to load existing cache from disk
    std::vector<char> cacheData;
    if (!cacheDir.empty())
    {
        auto cachePath = cacheDir / "pipeline_cache.bin";
        std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            size_t fileSize = static_cast<size_t>(file.tellg());
            cacheData.resize(fileSize);
            file.seekg(0);
            file.read(cacheData.data(), static_cast<std::streamsize>(fileSize));
            cacheInfo.initialDataSize = fileSize;
            cacheInfo.pInitialData    = cacheData.data();
            spdlog::info("Loaded pipeline cache ({} bytes)", fileSize);
        }
    }

    VkResult result = vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache);
    if (result != VK_SUCCESS)
    {
        spdlog::warn("Failed to create pipeline cache, continuing without");
        m_pipelineCache = VK_NULL_HANDLE;
    }

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void PipelineManager::destroy()
{
    if (m_device == VK_NULL_HANDLE) return;

    for (auto pipeline : m_pipelines)
    {
        if (pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, pipeline, nullptr);
    }
    m_pipelines.clear();

    for (auto layout : m_layouts)
    {
        if (layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, layout, nullptr);
    }
    m_layouts.clear();

    for (auto shader : m_shaders)
    {
        if (shader != VK_NULL_HANDLE)
            vkDestroyShaderModule(m_device, shader, nullptr);
    }
    m_shaders.clear();

    if (m_pipelineCache != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        m_pipelineCache = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
}

// ── loadShader ──────────────────────────────────────────────────────────────

VkShaderModule PipelineManager::loadShader(const std::filesystem::path& spirvPath)
{
    std::ifstream file(spirvPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        spdlog::error("Failed to open shader file: {}", pathToUtf8(spirvPath));
        return VK_NULL_HANDLE;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<uint32_t> code(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(fileSize));

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = fileSize;
    createInfo.pCode    = code.data();

    VkShaderModule shaderModule;
    VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create shader module from {}", pathToUtf8(spirvPath));
        return VK_NULL_HANDLE;
    }

    m_shaders.push_back(shaderModule);
    spdlog::debug("Loaded shader: {}", pathToUtf8(spirvPath.filename()));
    return shaderModule;
}

// ── destroyShader ───────────────────────────────────────────────────────────

void PipelineManager::destroyShader(VkShaderModule shader)
{
    if (shader != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(m_device, shader, nullptr);
        auto it = std::find(m_shaders.begin(), m_shaders.end(), shader);
        if (it != m_shaders.end())
            m_shaders.erase(it);
    }
}

// ── createLayout ────────────────────────────────────────────────────────────

VkPipelineLayout PipelineManager::createLayout(
    const std::vector<VkPushConstantRange>& pushConstants,
    const std::vector<VkDescriptorSetLayout>& descriptorLayouts)
{
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<uint32_t>(descriptorLayouts.size());
    layoutInfo.pSetLayouts            = descriptorLayouts.empty() ? nullptr : descriptorLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    layoutInfo.pPushConstantRanges    = pushConstants.empty() ? nullptr : pushConstants.data();

    VkPipelineLayout layout;
    VkResult result = vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &layout);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create pipeline layout");
        return VK_NULL_HANDLE;
    }

    m_layouts.push_back(layout);
    return layout;
}

// ── destroyLayout ───────────────────────────────────────────────────────────

void PipelineManager::destroyLayout(VkPipelineLayout layout)
{
    if (layout != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, layout, nullptr);
        auto it = std::find(m_layouts.begin(), m_layouts.end(), layout);
        if (it != m_layouts.end())
            m_layouts.erase(it);
    }
}

// ── createGraphicsPipeline ──────────────────────────────────────────────────

VkPipeline PipelineManager::createGraphicsPipeline(const GraphicsPipelineConfig& config)
{
    // ── Shader stages ───────────────────────────────────────────────────
    std::vector<VkPipelineShaderStageCreateInfo> stages;

    if (config.vertShader != VK_NULL_HANDLE)
    {
        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = config.vertShader;
        vertStage.pName  = "main";
        stages.push_back(vertStage);
    }

    if (config.fragShader != VK_NULL_HANDLE)
    {
        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = config.fragShader;
        fragStage.pName  = "main";
        stages.push_back(fragStage);
    }

    // ── Vertex input ────────────────────────────────────────────────────
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = static_cast<uint32_t>(config.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions      = config.vertexBindings.empty() ? nullptr : config.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(config.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions    = config.vertexAttributes.empty() ? nullptr : config.vertexAttributes.data();

    // ── Input assembly ──────────────────────────────────────────────────
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = config.topology;

    // ── Dynamic viewport and scissor ────────────────────────────────────
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // ── Rasterizer ──────────────────────────────────────────────────────
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = config.polygonMode;
    rasterizer.cullMode    = config.cullMode;
    rasterizer.frontFace   = config.frontFace;
    rasterizer.lineWidth   = 1.0f;

    // ── Multisampling (none — we composite in compute) ──────────────────
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // ── Depth stencil ───────────────────────────────────────────────────
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = config.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // ── Color blending ──────────────────────────────────────────────────
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable         = config.blendEnable ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = config.srcColorBlend;
    blendAttachment.dstColorBlendFactor = config.dstColorBlend;
    blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = config.srcAlphaBlend;
    blendAttachment.dstAlphaBlendFactor = config.dstAlphaBlend;
    blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    // ── Dynamic rendering info (VK 1.3 — no render pass) ────────────────
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &config.colorFormat;
    renderingInfo.depthAttachmentFormat   = config.depthFormat;

    // ── Pipeline creation ───────────────────────────────────────────────
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = config.layout;
    pipelineInfo.renderPass          = VK_NULL_HANDLE; // Dynamic rendering

    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1,
                                                 &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create graphics pipeline (VkResult: {})",
                      static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    m_pipelines.push_back(pipeline);
    return pipeline;
}

// ── createComputePipeline ───────────────────────────────────────────────────

VkPipeline PipelineManager::createComputePipeline(const ComputePipelineConfig& config)
{
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = config.compShader;
    stage.pName  = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage  = stage;
    pipelineInfo.layout = config.layout;

    VkPipeline pipeline;
    VkResult result = vkCreateComputePipelines(m_device, m_pipelineCache, 1,
                                                &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create compute pipeline (VkResult: {})",
                      static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    m_pipelines.push_back(pipeline);
    return pipeline;
}

// ── destroyPipeline ─────────────────────────────────────────────────────────

void PipelineManager::destroyPipeline(VkPipeline pipeline)
{
    if (pipeline != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(m_device, pipeline, nullptr);
        auto it = std::find(m_pipelines.begin(), m_pipelines.end(), pipeline);
        if (it != m_pipelines.end())
            m_pipelines.erase(it);
    }
}

// ── savePipelineCache ───────────────────────────────────────────────────────

bool PipelineManager::savePipelineCache(const std::filesystem::path& path)
{
    if (m_pipelineCache == VK_NULL_HANDLE) return false;

    size_t dataSize = 0;
    vkGetPipelineCacheData(m_device, m_pipelineCache, &dataSize, nullptr);

    std::vector<char> data(dataSize);
    vkGetPipelineCacheData(m_device, m_pipelineCache, &dataSize, data.data());

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    file.write(data.data(), static_cast<std::streamsize>(dataSize));
    spdlog::info("Pipeline cache saved ({} bytes)", dataSize);
    return true;
}

} // namespace rt

