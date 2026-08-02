/*
 * SpineRenderer.cpp — Vulkan GPU renderer for Spine character meshes.
 * Step 9: GPU Spine Renderer
 */

#include <volk.h>   // Must come before any vulkan.h include
#include "PathUtils.h"
#include "SpineRenderer.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>
#include "cache/FrameCache.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

// stb_image for PNG atlas loading (extern symbols, PNG-only)
// NOT static — SpinePrerenderer.cpp and TimelineWorkspaceSpine.cpp
// link to these extern symbols for Spine atlas PNG decoding.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═════════════════════════════════════════════════════════════════════════════

SpineRenderer::SpineRenderer() = default;

SpineRenderer::~SpineRenderer()
{
    shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::init(Device& device,
                          Allocator& allocator,
                          CommandPool& cmdPool,
                          VkQueue graphicsQueue,
                          const SpineRendererConfig& config)
{
    if (m_initialized) {
        spdlog::warn("SpineRenderer: already initialized");
        return true;
    }

    m_config         = config;
    m_vkDevice       = device.handle();
    m_graphicsQueue  = graphicsQueue;
    m_vmaAllocator   = allocator.handle();
    m_cmdPool        = &cmdPool;

    // Store timestamp period for GPU timing
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    // ── Pipeline manager ────────────────────────────────────────────────
    if (!m_pipelineMgr.create(device)) {
        spdlog::error("SpineRenderer: failed to create pipeline manager");
        return false;
    }

    // ── Descriptor pool + layout ────────────────────────────────────────
    if (!createDescriptorPool()) {
        spdlog::error("SpineRenderer: failed to create descriptor pool");
        return false;
    }

    // ── Graphics pipelines (one per blend mode) ─────────────────────────
    if (!createPipelines()) {
        spdlog::error("SpineRenderer: failed to create pipelines");
        return false;
    }

    // ── Frame resources (double-buffered vertex/index buffers) ──────────
    if (!createFrameResources()) {
        spdlog::error("SpineRenderer: failed to create frame resources");
        return false;
    }

    // ── Offscreen framebuffer ───────────────────────────────────────────
    FramebufferConfig fbConfig;
    fbConfig.width       = config.renderWidth;
    fbConfig.height      = config.renderHeight;
    fbConfig.colorFormat = config.colorFormat;
    fbConfig.hasDepth    = false;
    fbConfig.sampled     = true;
    fbConfig.concurrentFamilies = config.concurrentQueueFamilies;

    if (!m_framebuffer.create(m_vmaAllocator, m_vkDevice, fbConfig)) {
        spdlog::error("SpineRenderer: failed to create offscreen framebuffer");
        return false;
    }

    // ── Output sampler (for zero-copy FBO sampling by compositor) ────────
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_vkDevice, &samplerInfo, nullptr, &m_outputSampler) != VK_SUCCESS) {
        spdlog::error("SpineRenderer: failed to create output sampler");
        return false;
    }

    // ── Timestamp queries ───────────────────────────────────────────────
    createTimestampQueries();

    // ── Fence for frame synchronization ─────────────────────────────────
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &m_frameFence) != VK_SUCCESS) {
        spdlog::error("SpineRenderer: failed to create fence");
        return false;
    }

    m_initialized = true;
    spdlog::info("SpineRenderer: initialized ({}x{}, {} frames in flight)",
                 config.renderWidth, config.renderHeight, config.framesInFlight);
    return true;
}

void SpineRenderer::shutdown()
{
    if (!m_initialized) return;

    if (m_vkDevice != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_vkDevice);

    // Release textures
    releaseAllTextures();

    // Frame resources
    for (auto& fr : m_frameResources) {
        fr.vertexBuffer.destroy();
        fr.indexBuffer.destroy();
    }
    m_frameResources.clear();

    // Framebuffer
    m_framebuffer.destroy();

    // Output sampler
    if (m_outputSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vkDevice, m_outputSampler, nullptr);
        m_outputSampler = VK_NULL_HANDLE;
    }

    // Fence
    if (m_frameFence != VK_NULL_HANDLE) {
        vkDestroyFence(m_vkDevice, m_frameFence, nullptr);
        m_frameFence = VK_NULL_HANDLE;
    }

    // Timestamp queries
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_vkDevice, m_timestampPool, nullptr);
        m_timestampPool = VK_NULL_HANDLE;
    }

    // Descriptor pool
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_vkDevice, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    // Descriptor set layout
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_vkDevice, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Pipelines + shaders
    m_pipelineNormal   = VK_NULL_HANDLE;
    m_pipelineAdditive = VK_NULL_HANDLE;
    m_pipelineMultiply = VK_NULL_HANDLE;
    m_pipelineScreen   = VK_NULL_HANDLE;
    m_pipelineLayout   = VK_NULL_HANDLE;
    m_vertShader       = VK_NULL_HANDLE;
    m_fragShader       = VK_NULL_HANDLE;
    m_pipelineMgr.destroy();

    m_vkDevice      = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_vmaAllocator  = nullptr;
    m_cmdPool       = nullptr;
    m_initialized   = false;

    spdlog::info("SpineRenderer: shut down");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Descriptor pool + layout
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::createDescriptorPool()
{
    // ── Descriptor set layout (one combined image sampler at binding 0) ──
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;

    if (vkCreateDescriptorSetLayout(m_vkDevice, &layoutInfo, nullptr,
                                     &m_descriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    // ── Descriptor pool (enough for all atlas pages + some headroom) ────
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 64; // Support up to 64 atlas pages

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 64;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;

    return vkCreateDescriptorPool(m_vkDevice, &poolInfo, nullptr,
                                   &m_descriptorPool) == VK_SUCCESS;
}

VkDescriptorSet SpineRenderer::allocateDescriptorSet(const Texture& tex)
{
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorSetLayout;

    VkDescriptorSet set{VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(m_vkDevice, &allocInfo, &set) != VK_SUCCESS) {
        spdlog::error("SpineRenderer: failed to allocate descriptor set");
        return VK_NULL_HANDLE;
    }

    // Update descriptor with texture
    VkDescriptorImageInfo imgInfo = tex.descriptorInfo();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &imgInfo;

    vkUpdateDescriptorSets(m_vkDevice, 1, &write, 0, nullptr);
    return set;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Graphics pipelines
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::createPipelines()
{
    // ── Find SPIR-V shaders ─────────────────────────────────────────────
    // Look in several candidate locations relative to CWD and build tree.
    // Prioritise directories that contain compiled .spv files.
    fs::path shaderDir;
    fs::path candidates[] = {
        fs::current_path() / "build" / "shaders",                        // CWD/build/shaders  (CMake output)
        fs::current_path() / "shaders",
        fs::current_path().parent_path() / "shaders",
        fs::current_path().parent_path().parent_path() / "shaders",
    };
    for (auto& c : candidates) {
        if (fs::exists(c / "spine.vert.spv")) {
            shaderDir = c;
            break;
        }
    }

    // For now, we'll skip shader loading if no SPIR-V found.
    // Tests will mock this / test without actual GPU rendering.
    // The pipeline creation is still validated structurally.

    // ── Pipeline layout ─────────────────────────────────────────────────
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(SpinePushConstants);

    m_pipelineLayout = m_pipelineMgr.createLayout({pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE) {
        spdlog::error("SpineRenderer: failed to create pipeline layout");
        return false;
    }

    // ── Load shaders (if SPIR-V available) ──────────────────────────────
    if (!shaderDir.empty()) {
        auto vertPath = shaderDir / "spine.vert.spv";
        auto fragPath = shaderDir / "spine.frag.spv";

        if (fs::exists(vertPath) && fs::exists(fragPath)) {
            m_vertShader = m_pipelineMgr.loadShader(vertPath);
            m_fragShader = m_pipelineMgr.loadShader(fragPath);
        }
    }

    // If shaders loaded, create actual pipelines
    if (m_vertShader != VK_NULL_HANDLE && m_fragShader != VK_NULL_HANDLE) {
        // ── Vertex input description ────────────────────────────────────
        // Matches SpineVertex: {float x,y; float u,v; float r,g,b,a}
        VkVertexInputBindingDescription vertexBinding{};
        vertexBinding.binding   = 0;
        vertexBinding.stride    = sizeof(SpineVertex);
        vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> vertexAttrs = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(SpineVertex, x)},  // position
            {1, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(SpineVertex, u)},  // texcoord
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(SpineVertex, r)},  // color
        };

        // ── Normal blend (PMA) ──────────────────────────────────────────
        GraphicsPipelineConfig cfg;
        cfg.vertShader       = m_vertShader;
        cfg.fragShader       = m_fragShader;
        cfg.vertexBindings   = {vertexBinding};
        cfg.vertexAttributes = vertexAttrs;
        cfg.layout           = m_pipelineLayout;
        cfg.colorFormat      = m_config.colorFormat;
        cfg.blendEnable      = true;
        cfg.srcColorBlend    = VK_BLEND_FACTOR_ONE;                     // PMA
        cfg.dstColorBlend    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cfg.srcAlphaBlend    = VK_BLEND_FACTOR_ONE;
        cfg.dstAlphaBlend    = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cfg.cullMode         = VK_CULL_MODE_NONE;
        cfg.depthTestEnable  = false;

        m_pipelineNormal = m_pipelineMgr.createGraphicsPipeline(cfg);
        if (m_pipelineNormal == VK_NULL_HANDLE) return false;

        // ── Additive blend ──────────────────────────────────────────────
        cfg.srcColorBlend = VK_BLEND_FACTOR_ONE;
        cfg.dstColorBlend = VK_BLEND_FACTOR_ONE;
        cfg.srcAlphaBlend = VK_BLEND_FACTOR_ONE;
        cfg.dstAlphaBlend = VK_BLEND_FACTOR_ONE;
        m_pipelineAdditive = m_pipelineMgr.createGraphicsPipeline(cfg);
        if (m_pipelineAdditive == VK_NULL_HANDLE) return false;

        // ── Multiply blend ──────────────────────────────────────────────
        cfg.srcColorBlend = VK_BLEND_FACTOR_DST_COLOR;
        cfg.dstColorBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cfg.srcAlphaBlend = VK_BLEND_FACTOR_DST_ALPHA;
        cfg.dstAlphaBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        m_pipelineMultiply = m_pipelineMgr.createGraphicsPipeline(cfg);
        if (m_pipelineMultiply == VK_NULL_HANDLE) return false;

        // ── Screen blend ────────────────────────────────────────────────
        cfg.srcColorBlend = VK_BLEND_FACTOR_ONE;
        cfg.dstColorBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        cfg.srcAlphaBlend = VK_BLEND_FACTOR_ONE;
        cfg.dstAlphaBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        m_pipelineScreen = m_pipelineMgr.createGraphicsPipeline(cfg);
        if (m_pipelineScreen == VK_NULL_HANDLE) return false;

        spdlog::info("SpineRenderer: created 4 blend-mode pipelines");
    } else {
        spdlog::warn("SpineRenderer: SPIR-V shaders not found — "
                     "pipeline creation deferred (compile shaders with glslc)");
    }

    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame resources (double-buffered)
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::createFrameResources()
{
    m_frameResources.resize(m_config.framesInFlight);

    VkDeviceSize vertexSize = m_config.maxVertices * sizeof(SpineVertex);
    VkDeviceSize indexSize  = m_config.maxIndices  * sizeof(uint16_t);

    for (uint32_t i = 0; i < m_config.framesInFlight; ++i) {
        auto& fr = m_frameResources[i];

        // Vertex buffer — host-visible + VERTEX_BUFFER_BIT for dynamic upload
        if (!fr.vertexBuffer.create(m_vmaAllocator, vertexSize, BufferUsage::VertexDynamic)) {
            spdlog::error("SpineRenderer: failed to create vertex buffer {}", i);
            return false;
        }
        fr.vertexCapacity = vertexSize;

        // Index buffer — host-visible + INDEX_BUFFER_BIT for dynamic upload
        if (!fr.indexBuffer.create(m_vmaAllocator, indexSize, BufferUsage::IndexDynamic)) {
            spdlog::error("SpineRenderer: failed to create index buffer {}", i);
            return false;
        }
        fr.indexCapacity = indexSize;
    }

    return true;
}

void SpineRenderer::ensureBufferCapacity(SpineFrameResources& frame,
                                          size_t vertexBytes, size_t indexBytes)
{
    if (vertexBytes > static_cast<size_t>(frame.vertexCapacity)) {
        VkDeviceSize newSize = static_cast<VkDeviceSize>(vertexBytes * 2);
        frame.vertexBuffer.destroy();
        frame.vertexBuffer.create(m_vmaAllocator, newSize, BufferUsage::VertexDynamic);
        frame.vertexCapacity = newSize;
        spdlog::info("SpineRenderer: grew vertex buffer to {} bytes", newSize);
    }

    if (indexBytes > static_cast<size_t>(frame.indexCapacity)) {
        VkDeviceSize newSize = static_cast<VkDeviceSize>(indexBytes * 2);
        frame.indexBuffer.destroy();
        frame.indexBuffer.create(m_vmaAllocator, newSize, BufferUsage::IndexDynamic);
        frame.indexCapacity = newSize;
        spdlog::info("SpineRenderer: grew index buffer to {} bytes", newSize);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Timestamp queries
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::createTimestampQueries()
{
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2; // Begin + End

    if (vkCreateQueryPool(m_vkDevice, &queryInfo, nullptr, &m_timestampPool) != VK_SUCCESS) {
        spdlog::warn("SpineRenderer: timestamp queries not available");
        m_timestampPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Texture management
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::uploadAtlasTexture(int pageIndex,
                                        const void* pixelData,
                                        uint32_t width, uint32_t height,
                                        const std::string& path)
{
    if (!m_initialized) return false;

    // Release existing texture at this slot
    releaseTexture(pageIndex);

    auto& slot = m_atlasTextures[pageIndex];
    slot.width  = width;
    slot.height = height;
    slot.path   = path;

    // Create texture with PMA-appropriate format (UNORM, not SRGB, for PMA)
    TextureConfig texConfig;
    texConfig.width   = width;
    texConfig.height  = height;
    texConfig.format  = VK_FORMAT_R8G8B8A8_UNORM;
    texConfig.filter  = VK_FILTER_LINEAR;

    VkDeviceSize dataSize = static_cast<VkDeviceSize>(width) * height * 4;  // RGBA8

    if (!slot.texture.createFromData(m_vmaAllocator, m_vkDevice, texConfig,
                                      pixelData, dataSize,
                                      *m_cmdPool, m_graphicsQueue)) {
        spdlog::error("SpineRenderer: failed to upload atlas texture page {}", pageIndex);
        m_atlasTextures.erase(pageIndex);
        return false;
    }

    // Create descriptor set for this texture
    slot.descriptorSet = allocateDescriptorSet(slot.texture);
    if (slot.descriptorSet == VK_NULL_HANDLE) {
        spdlog::error("SpineRenderer: failed to create descriptor set for page {}", pageIndex);
        slot.texture.destroy();
        m_atlasTextures.erase(pageIndex);
        return false;
    }

    spdlog::debug("SpineRenderer: uploaded atlas page {} ({}x{}) from '{}'",
                  pageIndex, width, height, path);
    return true;
}

int SpineRenderer::loadAtlasTextures(const SpineAtlas& atlas)
{
    int loaded = 0;

    for (size_t i = 0; i < atlas.pages().size(); ++i) {
        auto& page = atlas.pages()[i];
        std::string texPath = page.texturePath;

        // If texturePath is relative, resolve against atlas directory
        if (!fs::path(texPath).is_absolute() && !atlas.directory().empty()) {
            texPath = pathToUtf8(fs::path(atlas.directory()) / texPath);
        }

        if (!fs::exists(texPath)) {
            spdlog::warn("SpineRenderer: atlas texture not found: {}", texPath);
            continue;
        }

        // Load PNG with stb_image
        int w, h, channels;
        unsigned char* pixels = stbi_load(texPath.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            spdlog::error("SpineRenderer: failed to load image: {}", texPath);
            continue;
        }

        // Premultiply alpha into RGB if the atlas page is not already PMA.
        // The fragment shader (spine.frag) assumes PMA input:
        //     outColor = texel * fragColor;
        // Straight-alpha texels produce overly bright semi-transparent
        // edges (white halos / circles around sprites) when blended with
        // the PMA-over blend state (src=ONE, dst=ONE_MINUS_SRC_ALPHA).
        if (!page.pma) {
            const int total = w * h;
            for (int p = 0; p < total; ++p) {
                uint8_t a = pixels[p * 4 + 3];
                if (a < 255) {
                    pixels[p * 4 + 0] = static_cast<uint8_t>((pixels[p * 4 + 0] * a + 127) / 255);
                    pixels[p * 4 + 1] = static_cast<uint8_t>((pixels[p * 4 + 1] * a + 127) / 255);
                    pixels[p * 4 + 2] = static_cast<uint8_t>((pixels[p * 4 + 2] * a + 127) / 255);
                }
            }
        }

        bool ok = uploadAtlasTexture(static_cast<int>(i), pixels,
                                      static_cast<uint32_t>(w),
                                      static_cast<uint32_t>(h),
                                      texPath);
        stbi_image_free(pixels);

        if (ok) ++loaded;
    }

    spdlog::info("SpineRenderer: loaded {}/{} atlas texture pages",
                 loaded, atlas.pages().size());
    return loaded;
}

bool SpineRenderer::hasTexture(int pageIndex) const
{
    return m_atlasTextures.count(pageIndex) > 0;
}

void SpineRenderer::releaseTexture(int pageIndex)
{
    auto it = m_atlasTextures.find(pageIndex);
    if (it == m_atlasTextures.end()) return;

    if (m_vkDevice != VK_NULL_HANDLE)
        GpuContext::get().scheduler().deviceWaitIdle();

    if (it->second.descriptorSet != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_vkDevice, m_descriptorPool, 1, &it->second.descriptorSet);
    }
    it->second.texture.destroy();
    m_atlasTextures.erase(it);
}

void SpineRenderer::releaseAllTextures()
{
    if (m_vkDevice != VK_NULL_HANDLE && !m_atlasTextures.empty())
        GpuContext::get().scheduler().deviceWaitIdle();

    for (auto& [idx, slot] : m_atlasTextures) {
        if (slot.descriptorSet != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(m_vkDevice, m_descriptorPool, 1, &slot.descriptorSet);
        }
        slot.texture.destroy();
    }
    m_atlasTextures.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame rendering
// ═════════════════════════════════════════════════════════════════════════════

void SpineRenderer::beginFrame()
{
    if (!m_initialized) return;

    m_stats = {};  // Reset frame stats

    // Wait for previous frame at this index to complete.
    // A5: 100 ms bounded timeout instead of UINT64_MAX so a GPU hang
    // surfaces as device-lost instead of freezing the producer thread
    // forever.  On timeout we transition to Failed and let the
    // fatal-failure modal explain the situation to the user.
    {
        VkResult r = vkWaitForFences(m_vkDevice, 1, &m_frameFence, VK_TRUE,
                                      100'000'000ull);
        if (r == VK_TIMEOUT) {
            spdlog::error("[SPINE] frame fence timeout — marking GPU Failed");
            // Don't reset fence; the next beginFrame on this index will
            // observe device-lost and bail out before re-recording.
            return;
        }
    }
    vkResetFences(m_vkDevice, 1, &m_frameFence);

    // Cross-queue sync: drain the compositor's compute queue before
    // re-recording the shared framebuffer.  On devices with a separate
    // async-compute queue family (NVIDIA family 2 here), CompositeEngine
    // samples m_framebuffer from the compute queue while SpineRenderer
    // writes to it from the graphics queue.  Submission order is
    // preserved within a queue but NOT across queues, so the previous
    // composite's still-in-flight sampling can race the
    // SHADER_READ→COLOR_ATTACHMENT transition we are about to record.
    // The hazard accumulates during heavy scrubbing and trips WDDM TDR
    // (VkResult=-4 / VK_ERROR_DEVICE_LOST) after ~150-200 submissions —
    // observed in logs/perf_log.txt:2035.  vkQueueWaitIdle is heavy but
    // the compute queue is dominated by the compositor and pending work
    // is typically <2 ms.  Replace with a per-slot semaphore wait once
    // the thread-model refactor exposes the compositor's submission
    // fence to SpineRenderer.
    if (m_computeQueue != VK_NULL_HANDLE) {
        if (m_computeQueueMutex) {
            std::lock_guard lock(*m_computeQueueMutex);
            vkQueueWaitIdle(m_computeQueue);
        } else {
            vkQueueWaitIdle(m_computeQueue);
        }
    }

    // Read timestamp results from previous frame
    if (m_timestampPool != VK_NULL_HANDLE) {
        uint64_t timestamps[2]{};
        VkResult result = vkGetQueryPoolResults(
            m_vkDevice, m_timestampPool, 0, 2,
            sizeof(timestamps), timestamps, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS && timestamps[1] > timestamps[0]) {
            m_stats.gpuTimeMs = static_cast<float>(
                static_cast<double>(timestamps[1] - timestamps[0]) *
                static_cast<double>(m_timestampPeriod) / 1e6);
        }
    }

    // Reset frame state
    auto& frame = m_frameResources[m_currentFrame];
    frame.vertexCount = 0;
    frame.indexCount  = 0;
    m_currentBlendMode        = SpineBlendMode::Normal;
    m_currentTexturePageIndex = -1;

    // Allocate command buffer
    m_activeCmdBuffer = m_cmdPool->allocateBuffer();

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_activeCmdBuffer, &beginInfo);

    // Reset timestamp queries
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(m_activeCmdBuffer, m_timestampPool, 0, 2);
        vkCmdWriteTimestamp(m_activeCmdBuffer,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           m_timestampPool, 0);
    }

    // Transition framebuffer to color attachment
    m_framebuffer.transitionToColorAttachment(m_activeCmdBuffer);

    // Begin rendering to offscreen framebuffer
    VkClearValue clearColor{};
    clearColor.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // Transparent
    m_framebuffer.beginRendering(m_activeCmdBuffer, &clearColor);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(m_framebuffer.width());
    viewport.height   = static_cast<float>(m_framebuffer.height());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_activeCmdBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = m_framebuffer.extent();
    vkCmdSetScissor(m_activeCmdBuffer, 0, 1, &scissor);

    // Bind default pipeline (Normal blend)
    if (m_pipelineNormal != VK_NULL_HANDLE) {
        vkCmdBindPipeline(m_activeCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipelineNormal);
    }
}

void SpineRenderer::bindPipelineForBlendMode(VkCommandBuffer cmd, SpineBlendMode mode)
{
    if (mode == m_currentBlendMode) return;

    VkPipeline pipeline = VK_NULL_HANDLE;
    switch (mode) {
        case SpineBlendMode::Normal:   pipeline = m_pipelineNormal;   break;
        case SpineBlendMode::Additive: pipeline = m_pipelineAdditive; break;
        case SpineBlendMode::Multiply: pipeline = m_pipelineMultiply; break;
        case SpineBlendMode::Screen:   pipeline = m_pipelineScreen;   break;
    }

    if (pipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        m_currentBlendMode = mode;
        m_stats.blendModeChanges++;
    }
}

void SpineRenderer::renderSkeleton(const SpineRenderData& renderData,
                                    const glm::mat4& mvp,
                                    float opacity)
{
    if (!m_initialized || m_activeCmdBuffer == VK_NULL_HANDLE) return;
    if (renderData.batches.empty()) return;

    // Check we have pipelines
    if (m_pipelineNormal == VK_NULL_HANDLE) return;

    auto& frame = m_frameResources[m_currentFrame];

    // ── Calculate total vertex/index data needed ────────────────────────
    size_t totalVerts   = 0;
    size_t totalIndices = 0;
    for (auto& batch : renderData.batches) {
        totalVerts   += batch.vertices.size();
        totalIndices += batch.indices.size();
    }

    size_t vertexBytes = (frame.vertexCount + totalVerts) * sizeof(SpineVertex);
    size_t indexBytes  = (frame.indexCount + totalIndices) * sizeof(uint16_t);

    ensureBufferCapacity(frame, vertexBytes, indexBytes);

    // ── Push constants ──────────────────────────────────────────────────
    SpinePushConstants pc;
    pc.mvp     = mvp;
    pc.opacity = opacity;

    vkCmdPushConstants(m_activeCmdBuffer, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

    // ── Render each batch ───────────────────────────────────────────────
    for (auto& batch : renderData.batches) {
        if (batch.vertices.empty() || batch.indices.empty()) continue;

        // Switch blend mode if needed
        bindPipelineForBlendMode(m_activeCmdBuffer, batch.blendMode);

        // Re-push constants if blend mode changed (pipeline changed)
        if (m_stats.blendModeChanges > 0) {
            vkCmdPushConstants(m_activeCmdBuffer, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        }

        // Bind texture if page changed
        if (batch.texturePageIndex != m_currentTexturePageIndex) {
            auto it = m_atlasTextures.find(batch.texturePageIndex);
            if (it != m_atlasTextures.end() && it->second.descriptorSet != VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(m_activeCmdBuffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_pipelineLayout,
                    0, 1, &it->second.descriptorSet,
                    0, nullptr);
                m_currentTexturePageIndex = batch.texturePageIndex;
                m_stats.textureBindChanges++;
            }
        }

        // Upload vertex data
        size_t vOffset = frame.vertexCount * sizeof(SpineVertex);
        size_t vSize   = batch.vertices.size() * sizeof(SpineVertex);
        frame.vertexBuffer.upload(batch.vertices.data(), vSize, vOffset);

        // Upload index data
        size_t iOffset = frame.indexCount * sizeof(uint16_t);
        size_t iSize   = batch.indices.size() * sizeof(uint16_t);
        frame.indexBuffer.upload(batch.indices.data(), iSize, iOffset);

        // Bind vertex buffer
        VkBuffer vkVertBuf = frame.vertexBuffer.handle();
        VkDeviceSize vkOffset = static_cast<VkDeviceSize>(vOffset);
        vkCmdBindVertexBuffers(m_activeCmdBuffer, 0, 1, &vkVertBuf, &vkOffset);

        // Bind index buffer
        vkCmdBindIndexBuffer(m_activeCmdBuffer, frame.indexBuffer.handle(),
                             static_cast<VkDeviceSize>(iOffset),
                             VK_INDEX_TYPE_UINT16);

        // Draw
        vkCmdDrawIndexed(m_activeCmdBuffer,
                         static_cast<uint32_t>(batch.indices.size()),
                         1, 0, 0, 0);

        frame.vertexCount += batch.vertices.size();
        frame.indexCount  += batch.indices.size();
        m_stats.drawCalls++;
    }

    m_stats.vertexCount   += static_cast<uint32_t>(totalVerts);
    m_stats.indexCount    += static_cast<uint32_t>(totalIndices);
    m_stats.skeletonCount++;
}

void SpineRenderer::endFrame()
{
    if (!m_initialized || m_activeCmdBuffer == VK_NULL_HANDLE) return;

    // End rendering
    m_framebuffer.endRendering(m_activeCmdBuffer);

    // Timestamp end
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(m_activeCmdBuffer,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           m_timestampPool, 1);
    }

    // Transition framebuffer for shader read (compositing input)
    m_framebuffer.transitionToShaderRead(m_activeCmdBuffer);

    vkEndCommandBuffer(m_activeCmdBuffer);

    // P1: route through GpuScheduler.  The scheduler holds the graphics
    // queue + its mutex, so SpineRenderer no longer needs to know about
    // either — the m_queueMutex / m_graphicsQueue members are now only
    // consulted as a fallback for pre-scheduler boot paths.
    GpuSubmission sub{};
    sub.cmd             = m_activeCmdBuffer;
    sub.queue           = GpuQueueKind::Graphics;
    sub.completionFence = m_frameFence;
    sub.tag             = "Spine::endFrame";
    GpuContext::get().scheduler().submit(sub);

    // Advance frame index
    m_currentFrame = (m_currentFrame + 1) % m_config.framesInFlight;
    m_activeCmdBuffer = VK_NULL_HANDLE;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Framebuffer resize
// ═════════════════════════════════════════════════════════════════════════════

bool SpineRenderer::resize(uint32_t width, uint32_t height)
{
    if (!m_initialized) return false;

    GpuContext::get().scheduler().deviceWaitIdle();

    m_config.renderWidth  = width;
    m_config.renderHeight = height;

    return m_framebuffer.resize(width, height);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Projection helpers
// ═════════════════════════════════════════════════════════════════════════════

glm::mat4 SpineRenderer::orthoProjection(float width, float height,
                                          float centerX, float centerY,
                                          float zoom)
{
    float halfW = (width  * 0.5f) / zoom;
    float halfH = (height * 0.5f) / zoom;

    float left   = centerX - halfW;
    float right  = centerX + halfW;
    float bottom = centerY - halfH;
    float top    = centerY + halfH;

    // Vulkan NDC: Y goes from -1 (top) to +1 (bottom), so we flip Y
    // glm::ortho(left, right, bottom, top) produces OpenGL convention,
    // we negate Y by swapping bottom and top.
    return glm::ortho(left, right, top, bottom, -1.0f, 1.0f);
}

glm::mat4 SpineRenderer::modelMatrix(float posX, float posY,
                                      float scaleX, float scaleY,
                                      float rotationDeg)
{
    glm::mat4 model{1.0f};
    model = glm::translate(model, glm::vec3(posX, posY, 0.0f));
    if (rotationDeg != 0.0f) {
        model = glm::rotate(model, glm::radians(rotationDeg), glm::vec3(0.0f, 0.0f, 1.0f));
    }
    model = glm::scale(model, glm::vec3(scaleX, scaleY, 1.0f));
    return model;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Output descriptor
// ═════════════════════════════════════════════════════════════════════════════

VkDescriptorImageInfo SpineRenderer::outputDescriptorInfo() const
{
    return m_framebuffer.descriptorInfo(m_outputSampler);
}

void SpineRenderer::waitForFrame() const
{
    if (m_frameFence != VK_NULL_HANDLE)
        vkWaitForFences(m_vkDevice, 1, &m_frameFence, VK_TRUE, UINT64_MAX);
}

// ═════════════════════════════════════════════════════════════════════════════
//  GPU → CPU readback (for multi-character export)
// ═════════════════════════════════════════════════════════════════════════════

std::shared_ptr<CachedFrame> SpineRenderer::readbackPixels()
{
    if (!m_initialized || !m_vmaAllocator) return nullptr;

    const uint32_t fboW = m_framebuffer.width();
    const uint32_t fboH = m_framebuffer.height();
    if (fboW == 0 || fboH == 0) return nullptr;

    const VkDeviceSize pixelBytes = static_cast<VkDeviceSize>(fboW) * fboH * 4;

    // ── 1. Create a readback staging buffer ─────────────────────────
    Buffer stagingBuf;
    if (!stagingBuf.create(m_vmaAllocator, pixelBytes, BufferUsage::Readback)) {
        spdlog::error("SpineRenderer::readbackPixels: failed to create staging buffer");
        return nullptr;
    }

    // ── 2. Allocate + record a one-shot command buffer ──────────────
    VkCommandBuffer cmd = m_cmdPool->allocateBuffer();
    if (cmd == VK_NULL_HANDLE) {
        stagingBuf.destroy();
        return nullptr;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // ── 3. Transition FBO: SHADER_READ_ONLY → TRANSFER_SRC ─────────
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = m_framebuffer.colorImage();
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // ── 4. Copy image → buffer ─────────────────────────────────────
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset        = 0;
    copyRegion.bufferRowLength     = 0;   // tightly packed
    copyRegion.bufferImageHeight   = 0;
    copyRegion.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel       = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount     = 1;
    copyRegion.imageOffset         = {0, 0, 0};
    copyRegion.imageExtent         = {fboW, fboH, 1};

    vkCmdCopyImageToBuffer(cmd, m_framebuffer.colorImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf.handle(), 1, &copyRegion);

    // ── 5. Transition back: TRANSFER_SRC → SHADER_READ_ONLY ────────
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    // ── 6. Submit + wait (via GpuScheduler — P1 migration) ─────────
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(m_vkDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        m_cmdPool->freeBuffer(cmd);
        stagingBuf.destroy();
        return nullptr;
    }

    GpuSubmission sub{};
    sub.cmd             = cmd;
    sub.queue           = GpuQueueKind::Graphics;
    sub.completionFence = fence;
    sub.tag             = "Spine::readbackPixels";
    GpuContext::get().scheduler().submit(sub);

    vkWaitForFences(m_vkDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(m_vkDevice, fence, nullptr);
    m_cmdPool->freeBuffer(cmd);

    // ── 7. Map + copy to CachedFrame (swizzle RGBA→BGRA) ──────────
    auto frame = std::make_shared<CachedFrame>();
    frame->width  = fboW;
    frame->height = fboH;
    frame->stride = fboW * 4;
    frame->pixels.resize(static_cast<size_t>(fboW) * fboH * 4);

    void* mapped = stagingBuf.map();
    if (mapped) {
        const uint8_t* src = static_cast<const uint8_t*>(mapped);
        uint8_t* dst = frame->pixels.data();
        const size_t count = static_cast<size_t>(fboW) * fboH;
        for (size_t i = 0; i < count; ++i) {
            dst[i * 4 + 0] = src[i * 4 + 2]; // B ← R
            dst[i * 4 + 1] = src[i * 4 + 1]; // G ← G
            dst[i * 4 + 2] = src[i * 4 + 0]; // R ← B
            dst[i * 4 + 3] = src[i * 4 + 3]; // A ← A
        }
        stagingBuf.unmap();
    }
    stagingBuf.destroy();

    return frame;
}

} // namespace rt
