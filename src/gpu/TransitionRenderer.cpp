/*
 * TransitionRenderer.cpp — GPU compute-shader transitions.
 * Step 10: GPU Compositor
 */

#include <volk.h>
#include "TransitionRenderer.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

// ── Constructor / Destructor ────────────────────────────────────────────────

TransitionRenderer::TransitionRenderer() = default;

TransitionRenderer::~TransitionRenderer()
{
    shutdown();
}

// ── init ────────────────────────────────────────────────────────────────────

bool TransitionRenderer::init(Device& device,
                               Allocator& allocator,
                               CommandPool& cmdPool,
                               VkQueue computeQueue,
                               const TransitionConfig& config)
{
    if (m_initialized)
    {
        spdlog::warn("TransitionRenderer already initialized");
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
        spdlog::error("TransitionRenderer: Failed to create pipeline manager");
        return false;
    }

    // Get timestamp period
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    // Create resources. Each concurrently-recorded transition owns an
    // independent output image, descriptor set, and transform buffer. The
    // default (0,0) target preserves the single-transition public API.
    if (!createDescriptorResources())  { shutdown(); return false; }
    if (!createPipelines())            { shutdown(); return false; }
    if (!ensureRenderTarget(0, 0))      { shutdown(); return false; }

    // Create placeholder textures
    {
        uint32_t black = 0xFF000000; // opaque black
        TextureConfig placeholderConfig;
        placeholderConfig.width  = 1;
        placeholderConfig.height = 1;
        placeholderConfig.format = VK_FORMAT_R8G8B8A8_UNORM;
        placeholderConfig.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_placeholderTexture.createFromData(
                m_allocator->handle(), device.handle(),
                placeholderConfig,
                &black, 4,
                cmdPool, computeQueue))
        {
            spdlog::error("TransitionRenderer: Failed to create placeholder texture");
            shutdown();
            return false;
        }

        uint32_t white = 0xFFFFFFFF; // opaque white
        if (!m_whitePlaceholderTexture.createFromData(
                m_allocator->handle(), device.handle(),
                placeholderConfig,
                &white, 4,
                cmdPool, computeQueue))
        {
            spdlog::error("TransitionRenderer: Failed to create white placeholder texture");
            shutdown();
            return false;
        }

        uint32_t transparent = 0x00000000; // fully transparent
        if (!m_transparentPlaceholderTexture.createFromData(
                m_allocator->handle(), device.handle(),
                placeholderConfig,
                &transparent, 4,
                cmdPool, computeQueue))
        {
            spdlog::error("TransitionRenderer: Failed to create transparent placeholder texture");
            shutdown();
            return false;
        }
    }

    // Create timestamp query pool
    {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;

        if (vkCreateQueryPool(device.handle(), &queryInfo, nullptr, &m_queryPool) != VK_SUCCESS)
        {
            m_queryPool = VK_NULL_HANDLE; // Non-fatal
        }
    }

    m_initialized = true;
    spdlog::info("TransitionRenderer initialized ({}x{}, 3 transition shaders)",
                 config.outputWidth, config.outputHeight);
    return true;
}

// ── shutdown ────────────────────────────────────────────────────────────────

void TransitionRenderer::shutdown()
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();
    if (dev != VK_NULL_HANDLE)
        vkDeviceWaitIdle(dev);

    if (m_queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(dev, m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    if (m_descriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_pipelineManager.destroy();
    m_dissolvePipeline         = VK_NULL_HANDLE;
    m_fadePipeline             = VK_NULL_HANDLE;
    m_wipePipeline             = VK_NULL_HANDLE;
    m_pushPipeline             = VK_NULL_HANDLE;
    m_dipColorPipeline         = VK_NULL_HANDLE;
    m_filmDissolvePipeline     = VK_NULL_HANDLE;
    m_additiveDissolvePipeline = VK_NULL_HANDLE;
    m_barnDoorPipeline         = VK_NULL_HANDLE;
    m_clockWipePipeline        = VK_NULL_HANDLE;
    m_radialWipePipeline       = VK_NULL_HANDLE;
    m_irisPipeline             = VK_NULL_HANDLE;
    m_diagonalWipePipeline     = VK_NULL_HANDLE;
    m_checkerWipePipeline      = VK_NULL_HANDLE;
    m_venetianBlindsPipeline   = VK_NULL_HANDLE;
    m_insetPipeline            = VK_NULL_HANDLE;
    m_slidePipeline            = VK_NULL_HANDLE;
    m_splitPipeline            = VK_NULL_HANDLE;
    m_swapPipeline             = VK_NULL_HANDLE;
    m_zoomPipeline             = VK_NULL_HANDLE;
    m_whipPanPipeline          = VK_NULL_HANDLE;
    m_randomBlocksPipeline     = VK_NULL_HANDLE;
    m_morphCutPipeline         = VK_NULL_HANDLE;
    m_gradientWipePipeline     = VK_NULL_HANDLE;
    m_pipelineLayout           = VK_NULL_HANDLE;

    m_placeholderTexture.destroy();
    m_whitePlaceholderTexture.destroy();
    m_transparentPlaceholderTexture.destroy();
    m_renderTargets.clear();

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
}

// ── createDescriptorResources ───────────────────────────────────────────────

bool TransitionRenderer::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Descriptor layout:
    // binding 0: storage image (output)
    // binding 1: combined image sampler (source A)
    // binding 2: combined image sampler (source B)
    // binding 3: storage buffer (per-source transforms SSBO)

    VkDescriptorSetLayoutBinding bindings[4] = {};

    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
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
    bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 4;
    layoutInfo.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("TransitionRenderer: Failed to create descriptor set layout");
        return false;
    }

    // Pool
    VkDescriptorPoolSize poolSizes[3] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = kMaxRenderTargets;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 2 * kMaxRenderTargets;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = kMaxRenderTargets;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = kMaxRenderTargets;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("TransitionRenderer: Failed to create descriptor pool");
        return false;
    }

    return true;
}

// ── createPipelines ─────────────────────────────────────────────────────────

bool TransitionRenderer::createPipelines()
{
    // Find shaders
    fs::path searchDirs[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders",
        fs::current_path() / "shaders",
        fs::current_path().parent_path() / "shaders",
        fs::current_path().parent_path() / "build" / "shaders",
    };

    auto findShader = [&](const std::string& name) -> fs::path {
        for (auto& dir : searchDirs)
        {
            fs::path p = dir / name;
            if (fs::exists(p)) return p;
        }
        return {};
    };

    // Create pipeline layout (shared across all 3 transition types)
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(TransitionPushConstants);

    m_pipelineLayout = m_pipelineManager.createLayout({pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE)
    {
        spdlog::error("TransitionRenderer: Failed to create pipeline layout");
        return false;
    }

    // Load and create each pipeline
    struct ShaderInfo { std::string name; VkPipeline* target; };
    ShaderInfo shaders[] = {
        {"transition_dissolve.comp.spv",          &m_dissolvePipeline},
        {"transition_fade.comp.spv",              &m_fadePipeline},
        {"transition_wipe.comp.spv",              &m_wipePipeline},
        {"transition_push.comp.spv",              &m_pushPipeline},
        {"transition_dip_color.comp.spv",         &m_dipColorPipeline},
        {"transition_film_dissolve.comp.spv",     &m_filmDissolvePipeline},
        {"transition_additive_dissolve.comp.spv", &m_additiveDissolvePipeline},
        {"transition_barn_door.comp.spv",         &m_barnDoorPipeline},
        {"transition_clock_wipe.comp.spv",        &m_clockWipePipeline},
        {"transition_radial_wipe.comp.spv",       &m_radialWipePipeline},
        {"transition_iris.comp.spv",              &m_irisPipeline},
        {"transition_diagonal_wipe.comp.spv",     &m_diagonalWipePipeline},
        {"transition_checker_wipe.comp.spv",      &m_checkerWipePipeline},
        {"transition_venetian_blinds.comp.spv",   &m_venetianBlindsPipeline},
        {"transition_inset.comp.spv",             &m_insetPipeline},
        {"transition_slide.comp.spv",             &m_slidePipeline},
        {"transition_split.comp.spv",             &m_splitPipeline},
        {"transition_swap.comp.spv",              &m_swapPipeline},
        {"transition_zoom.comp.spv",              &m_zoomPipeline},
        {"transition_whip_pan.comp.spv",          &m_whipPanPipeline},
        {"transition_random_blocks.comp.spv",     &m_randomBlocksPipeline},
        {"transition_morph_cut.comp.spv",         &m_morphCutPipeline},
        {"transition_gradient_wipe.comp.spv",     &m_gradientWipePipeline},
    };

    for (auto& si : shaders)
    {
        fs::path path = findShader(si.name);
        if (path.empty())
        {
            spdlog::error("TransitionRenderer: {} not found", si.name);
            return false;
        }

        VkShaderModule shader = m_pipelineManager.loadShader(path);
        if (shader == VK_NULL_HANDLE)
        {
            spdlog::error("TransitionRenderer: Failed to load {}", si.name);
            return false;
        }

        ComputePipelineConfig cfg;
        cfg.compShader = shader;
        cfg.layout     = m_pipelineLayout;

        *si.target = m_pipelineManager.createComputePipeline(cfg);
        if (*si.target == VK_NULL_HANDLE)
        {
            spdlog::error("TransitionRenderer: Failed to create pipeline for {}", si.name);
            return false;
        }
    }

    spdlog::debug("TransitionRenderer: All {} pipelines created", sizeof(shaders)/sizeof(shaders[0]));
    return true;
}

// ── Per-dispatch render targets ─────────────────────────────────────────────

uint64_t TransitionRenderer::renderTargetKey(uint32_t submissionSlot,
                                             uint32_t transitionSlot) noexcept
{
    return (static_cast<uint64_t>(submissionSlot) << 32) | transitionSlot;
}

const TransitionRenderer::RenderTarget* TransitionRenderer::findRenderTarget(
    uint32_t submissionSlot, uint32_t transitionSlot) const noexcept
{
    const auto it = m_renderTargets.find(
        renderTargetKey(submissionSlot, transitionSlot));
    return it == m_renderTargets.end() ? nullptr : it->second.get();
}

TransitionRenderer::RenderTarget* TransitionRenderer::ensureRenderTarget(
    uint32_t submissionSlot, uint32_t transitionSlot)
{
    const uint64_t key = renderTargetKey(submissionSlot, transitionSlot);
    if (const auto it = m_renderTargets.find(key); it != m_renderTargets.end())
        return it->second.get();

    if (m_renderTargets.size() >= kMaxRenderTargets) {
        spdlog::error("TransitionRenderer: render-target limit ({}) exceeded",
                      kMaxRenderTargets);
        return nullptr;
    }

    auto target = std::make_unique<RenderTarget>();

    TextureConfig cfg;
    cfg.width  = m_config.outputWidth;
    cfg.height = m_config.outputHeight;
    cfg.format = m_config.outputFormat;
    cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT
               | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!target->outputTexture.create(
            m_allocator->handle(), m_device->handle(), cfg)) {
        spdlog::error("TransitionRenderer: Failed to create output texture");
        return nullptr;
    }

    if (!target->sourceParamsBuffer.create(
            m_allocator->handle(), sizeof(TransitionSourceParams),
            BufferUsage::Uniform)) {
        spdlog::error("TransitionRenderer: Failed to create source params SSBO");
        return nullptr;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkAllocateDescriptorSets(m_device->handle(), &allocInfo,
                                 &target->descriptorSet) != VK_SUCCESS) {
        spdlog::error("TransitionRenderer: Failed to allocate descriptor set");
        return nullptr;
    }

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    target->outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    RenderTarget* result = target.get();
    m_renderTargets.emplace(key, std::move(target));
    return result;
}

// ── updateSourceDescriptors ─────────────────────────────────────────────────

void TransitionRenderer::updateSourceDescriptors(
    RenderTarget& target,
    const VkDescriptorImageInfo& sourceA,
    const VkDescriptorImageInfo& sourceB)
{
    VkDevice dev = m_device->handle();

    // Binding 0: output storage image
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView   = target.outputTexture.imageView();
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = target.sourceParamsBuffer.handle();
    bufferInfo.offset = 0;
    bufferInfo.range  = sizeof(TransitionSourceParams);

    VkWriteDescriptorSet writes[4] = {};

    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = target.descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &outputInfo;

    // Binding 1: source A
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = target.descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &sourceA;

    // Binding 2: source B
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = target.descriptorSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo      = &sourceB;

    // Binding 3: source params SSBO
    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = target.descriptorSet;
    writes[3].dstBinding      = 3;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].descriptorCount = 1;
    writes[3].pBufferInfo     = &bufferInfo;

    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
}

// ── getPipeline ─────────────────────────────────────────────────────────────

VkPipeline TransitionRenderer::getPipeline(GpuTransitionType type) const
{
    switch (type)
    {
        case GpuTransitionType::Dissolve:         return m_dissolvePipeline;
        case GpuTransitionType::FadeBlack:         return m_fadePipeline;
        case GpuTransitionType::WipeLeft:
        case GpuTransitionType::WipeRight:
        case GpuTransitionType::WipeUp:
        case GpuTransitionType::WipeDown:          return m_wipePipeline;
        case GpuTransitionType::PushLeft:
        case GpuTransitionType::PushRight:
        case GpuTransitionType::PushUp:
        case GpuTransitionType::PushDown:          return m_pushPipeline;
        case GpuTransitionType::DipColor:          return m_dipColorPipeline;
        case GpuTransitionType::FilmDissolve:      return m_filmDissolvePipeline;
        case GpuTransitionType::AdditiveDissolve:  return m_additiveDissolvePipeline;
        case GpuTransitionType::BarnDoor:          return m_barnDoorPipeline;
        case GpuTransitionType::ClockWipe:         return m_clockWipePipeline;
        case GpuTransitionType::RadialWipe:        return m_radialWipePipeline;
        case GpuTransitionType::Iris:              return m_irisPipeline;
        case GpuTransitionType::DiagonalWipe:      return m_diagonalWipePipeline;
        case GpuTransitionType::CheckerWipe:       return m_checkerWipePipeline;
        case GpuTransitionType::VenetianBlinds:    return m_venetianBlindsPipeline;
        case GpuTransitionType::Inset:             return m_insetPipeline;
        case GpuTransitionType::Slide:             return m_slidePipeline;
        case GpuTransitionType::SplitWipe:         return m_splitPipeline;
        case GpuTransitionType::Swap:              return m_swapPipeline;
        case GpuTransitionType::ZoomTransition:    return m_zoomPipeline;
        case GpuTransitionType::WhipPan:           return m_whipPanPipeline;
        case GpuTransitionType::RandomBlocks:      return m_randomBlocksPipeline;
        case GpuTransitionType::MorphCut:          return m_morphCutPipeline;
        case GpuTransitionType::GradientWipe:      return m_gradientWipePipeline;
        default:                                   return m_dissolvePipeline;
    }
}

// ── render ──────────────────────────────────────────────────────────────────

bool TransitionRenderer::render(VkCommandBuffer cmd,
                                 const TransitionSourceInfo& sourceA,
                                 const TransitionSourceInfo& sourceB,
                                 GpuTransitionType type,
                                 float progress,
                                 int32_t directionOverride,
                                 float extraParam,
                                 float softnessOverride,
                                 uint32_t submissionSlot,
                                 uint32_t transitionSlot)
{
    if (!m_initialized) return false;

    RenderTarget* target = ensureRenderTarget(submissionSlot, transitionSlot);
    if (!target) return false;

    progress = std::clamp(progress, 0.0f, 1.0f);

    // Upload per-source transform data to SSBO
    TransitionSourceParams sp{};
    sp.transformA = sourceA.transform;
    sp.transformB = sourceB.transform;
    sp.cropA      = sourceA.crop;
    sp.cropB      = sourceB.crop;
    sp.isPackedA  = sourceA.isPacked ? 1 : 0;
    sp.isPackedB  = sourceB.isPacked ? 1 : 0;
    target->sourceParamsBuffer.upload(&sp, sizeof(sp));

    // Update descriptors
    updateSourceDescriptors(*target, sourceA.textureInfo, sourceB.textureInfo);

    // Timestamp begin
    const bool recordTiming = submissionSlot == 0 && transitionSlot == 0;
    if (recordTiming && m_queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmd, m_queryPool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0);
    }

    // Bind pipeline
    VkPipeline pipeline = getPipeline(type);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

    // Bind descriptors
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &target->descriptorSet,
                            0, nullptr);

    // Push constants
    TransitionPushConstants pc{};
    pc.width    = static_cast<int32_t>(m_config.outputWidth);
    pc.height   = static_cast<int32_t>(m_config.outputHeight);
    pc.progress = progress;
    pc.softness = (softnessOverride >= 0.0f) ? softnessOverride : m_config.wipeSoftness;

    // Set wipe direction
    switch (type)
    {
        case GpuTransitionType::WipeLeft:  pc.direction = 0; break;
        case GpuTransitionType::WipeRight: pc.direction = 1; break;
        case GpuTransitionType::WipeUp:    pc.direction = 2; break;
        case GpuTransitionType::WipeDown:  pc.direction = 3; break;
        case GpuTransitionType::PushLeft:  pc.direction = 0; break;
        case GpuTransitionType::PushRight: pc.direction = 1; break;
        case GpuTransitionType::PushUp:    pc.direction = 2; break;
        case GpuTransitionType::PushDown:  pc.direction = 3; break;
        // Dip to color: 0=black, 1=white
        case GpuTransitionType::DipColor:  pc.direction = 0; break;
        // Barn door: 0=horizontal, 1=vertical
        case GpuTransitionType::BarnDoor:  pc.direction = 0; break;
        // Venetian blinds: 0=horizontal, 1=vertical
        case GpuTransitionType::VenetianBlinds: pc.direction = 0; pc.param2 = 6.0f; break;
        // Iris: 0=round, 1=diamond, 2=cross
        case GpuTransitionType::Iris:      pc.direction = 0; break;
        // Slide: 0=left, 1=right, 2=up, 3=down
        case GpuTransitionType::Slide:     pc.direction = 0; break;
        // Split: 0=vertical, 1=center
        case GpuTransitionType::SplitWipe: pc.direction = 0; break;
        // Zoom: 0=simple, 1=cross zoom
        case GpuTransitionType::ZoomTransition: pc.direction = 0; break;
        // Checker/random blocks: set grid size
        case GpuTransitionType::CheckerWipe:    pc.param2 = 8.0f; break;
        case GpuTransitionType::RandomBlocks:   pc.param2 = 8.0f; break;
        // Gradient wipe: 0=diagonal, 1=radial, 2=noise
        case GpuTransitionType::GradientWipe:   pc.direction = 0; pc.param2 = 6.0f; break;
        default:                        pc.direction = 0; break;
    }

    // Allow caller to override direction and extra param for variant types
    if (directionOverride >= 0)
        pc.direction = directionOverride;
    if (extraParam != 0.0f)
        pc.param2 = extraParam;

    // Packed-alpha flags via push constants (guaranteed delivery)
    pc.param3 = sourceA.isPacked ? 1.0f : 0.0f;
    pc.param4 = sourceB.isPacked ? 1.0f : 0.0f;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // Dispatch
    constexpr uint32_t WG = 16;
    uint32_t groupsX = (m_config.outputWidth  + WG - 1) / WG;
    uint32_t groupsY = (m_config.outputHeight + WG - 1) / WG;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Timestamp end
    if (recordTiming && m_queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);
    }

    m_stats.type     = type;
    m_stats.progress = progress;

    return true;
}

// ── renderSync ──────────────────────────────────────────────────────────────

bool TransitionRenderer::renderSync(const TransitionSourceInfo& sourceA,
                                     const TransitionSourceInfo& sourceB,
                                     GpuTransitionType type,
                                     float progress,
                                     int32_t directionOverride,
                                     float extraParam,
                                     float softnessOverride)
{
    if (!m_initialized) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    bool result = render(cmd, sourceA, sourceB, type, progress, directionOverride, extraParam, softnessOverride);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Read GPU timing
    if (result && m_queryPool != VK_NULL_HANDLE)
    {
        uint64_t timestamps[2] = {};
        VkResult qr = vkGetQueryPoolResults(
            m_device->handle(), m_queryPool,
            0, 2, sizeof(timestamps), timestamps,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        if (qr == VK_SUCCESS)
        {
            m_stats.gpuTimeMs = static_cast<float>(timestamps[1] - timestamps[0])
                              * m_timestampPeriod / 1e6f;
        }
    }

    return result;
}

// ── resize ──────────────────────────────────────────────────────────────────

bool TransitionRenderer::resize(uint32_t width, uint32_t height)
{
    if (!m_initialized) return false;
    if (width == m_config.outputWidth && height == m_config.outputHeight) return true;

    GpuContext::get().scheduler().deviceWaitIdle();

    m_config.outputWidth  = width;
    m_config.outputHeight = height;

    // Descriptor sets are owned by the pool and can be reused only with the
    // images they reference. Rebuild targets lazily at the new resolution;
    // create the default one now to preserve outputImage() guarantees.
    m_renderTargets.clear();
    if (m_descriptorPool != VK_NULL_HANDLE)
        vkResetDescriptorPool(m_device->handle(), m_descriptorPool, 0);
    return ensureRenderTarget(0, 0) != nullptr;
}

// ── outputDescriptorInfo ────────────────────────────────────────────────────

VkImage TransitionRenderer::outputImage() const noexcept
{
    const auto* target = findRenderTarget(0, 0);
    return target ? target->outputTexture.image() : VK_NULL_HANDLE;
}

VkImageView TransitionRenderer::outputImageView() const noexcept
{
    const auto* target = findRenderTarget(0, 0);
    return target ? target->outputTexture.imageView() : VK_NULL_HANDLE;
}

VkDescriptorImageInfo TransitionRenderer::outputDescriptorInfo(
    uint32_t submissionSlot, uint32_t transitionSlot) const
{
    const auto* target = findRenderTarget(submissionSlot, transitionSlot);
    if (!target) return {};
    VkDescriptorImageInfo info = target->outputTexture.descriptorInfo();
    // Output texture lives in GENERAL layout (storage image).
    // Override the layout so the compositor samples it correctly.
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

// ── placeholder descriptor accessors ────────────────────────────────────────

VkDescriptorImageInfo TransitionRenderer::blackDescriptorInfo() const
{
    return m_placeholderTexture.descriptorInfo();
}

VkDescriptorImageInfo TransitionRenderer::whiteDescriptorInfo() const
{
    return m_whitePlaceholderTexture.descriptorInfo();
}

VkDescriptorImageInfo TransitionRenderer::transparentDescriptorInfo() const
{
    return m_transparentPlaceholderTexture.descriptorInfo();
}

// ── readbackOutput ──────────────────────────────────────────────────────────

bool TransitionRenderer::readbackOutput(std::vector<uint8_t>& outPixels,
                                        uint32_t submissionSlot,
                                        uint32_t transitionSlot)
{
    if (!m_initialized) return false;
    auto it = m_renderTargets.find(
        renderTargetKey(submissionSlot, transitionSlot));
    if (it == m_renderTargets.end()) return false;
    auto& outputTexture = it->second->outputTexture;

    const uint32_t w = m_config.outputWidth;
    const uint32_t h = m_config.outputHeight;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    Buffer staging;
    if (!staging.create(m_allocator->handle(), imageSize, BufferUsage::Readback))
        return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    outputTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount     = 1;
    region.imageExtent = {w, h, 1};

    vkCmdCopyImageToBuffer(cmd, outputTexture.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.handle(), 1, &region);

    outputTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    outPixels.resize(static_cast<size_t>(imageSize));
    void* mapped = staging.map();
    if (!mapped) { staging.destroy(); return false; }
    std::memcpy(outPixels.data(), mapped, static_cast<size_t>(imageSize));
    staging.unmap();
    staging.destroy();

    return true;
}

} // namespace rt

