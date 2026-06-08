/*
 * EffectProcessor.cpp â€” GPU compute-shader effects pipeline.
 *
 * Step 22: Effects System
 *
 * Applies effects using Vulkan compute shaders with ping-pong storage images.
 * Each effect is dispatched as a separate compute shader invocation.
 * The processor chains effects by alternating read/write between two images.
 *
 * Shader bindings (must match color_correct.comp, blur.comp, etc.):
 *   binding 0: writeonly image2D  (output storage image)
 *   binding 1: sampler2D          (input combined image sampler)
 *
 * Push constants: EffectPushConstants (width, height, paramCount, params[16])
 */

#include <volk.h>
#include "EffectProcessor.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#include <cstdlib>
#include <cstring>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace rt {

// =============================================================================
//  Construction / Destruction
// =============================================================================

EffectProcessor::EffectProcessor() = default;

EffectProcessor::~EffectProcessor()
{
    shutdown();
}

// =============================================================================
//  Helper: locate SPIR-V shader file
// =============================================================================

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

// =============================================================================
//  Lifecycle
// =============================================================================

bool EffectProcessor::init(Device& device,
                           Allocator& allocator,
                           CommandPool& cmdPool,
                           VkQueue computeQueue,
                           const EffectProcessorConfig& config)
{
    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    // Query timestamp period for GPU timing
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    if (!createStorageTextures()) {
        spdlog::error("EffectProcessor: failed to create storage textures");
        return false;
    }

    if (!createDescriptorResources()) {
        spdlog::error("EffectProcessor: failed to create descriptor resources");
        return false;
    }

    if (!createPipelines()) {
        spdlog::error("EffectProcessor: failed to create pipelines");
        return false;
    }

    // Create timestamp query pool (2 queries: begin + end)
    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType   = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount  = 2;
    if (vkCreateQueryPool(device.handle(), &qpci, nullptr, &m_queryPool) != VK_SUCCESS)
        m_queryPool = VK_NULL_HANDLE; // non-fatal

    // Create a reusable fence for processSync (avoids per-call create/destroy)
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device.handle(), &fci, nullptr, &m_syncFence) != VK_SUCCESS)
        m_syncFence = VK_NULL_HANDLE; // non-fatal, will fall back to endSingleTime

    m_initialized = true;
    spdlog::info("EffectProcessor initialized ({}x{})", m_config.width, m_config.height);
    return true;
}

void EffectProcessor::shutdown()
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();
    if (dev) vkDeviceWaitIdle(dev);

    // Query pool
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev, m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }

    // Reusable fence
    if (m_syncFence != VK_NULL_HANDLE) {
        vkDestroyFence(dev, m_syncFence, nullptr);
        m_syncFence = VK_NULL_HANDLE;
    }

    // LUT 3D texture
    m_lutTexture3D.destroy();

    // Descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool      = VK_NULL_HANDLE;
        m_descriptorSets[0]   = VK_NULL_HANDLE;
        m_descriptorSets[1]   = VK_NULL_HANDLE;
        m_sourceDescriptorSets.clear();
        m_lutDescriptorSets.clear();
        m_descriptorRingCursor = 0;
        m_curSourceSet        = VK_NULL_HANDLE;
        m_curLutSet           = VK_NULL_HANDLE;
    }
    if (m_lutDescriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_lutDescriptorSetLayout, nullptr);
        m_lutDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Pipelines
    m_pipelineManager.destroy();
    m_colorCorrectPipeline = VK_NULL_HANDLE;
    m_blurPipeline         = VK_NULL_HANDLE;
    m_sharpenPipeline      = VK_NULL_HANDLE;
    m_glowPipeline         = VK_NULL_HANDLE;
    m_chromaKeyPipeline    = VK_NULL_HANDLE;
    m_ultraKeyMattePipeline     = VK_NULL_HANDLE;
    m_ultraKeyCleanupPipeline   = VK_NULL_HANDLE;
    m_ultraKeyFinalizePipeline  = VK_NULL_HANDLE;
    m_ultraKeyCombinedPipeline  = VK_NULL_HANDLE;
    m_ultraKeyLightPipeline     = VK_NULL_HANDLE;
    m_transform2dPipeline  = VK_NULL_HANDLE;
    m_vignettePipeline      = VK_NULL_HANDLE;
    m_lutPipeline            = VK_NULL_HANDLE;
    m_letterboxPipeline      = VK_NULL_HANDLE;
    m_colorGradingPipeline   = VK_NULL_HANDLE;
    m_otsPipeline            = VK_NULL_HANDLE;
    m_flipPipeline           = VK_NULL_HANDLE;
    m_pipelineLayout       = VK_NULL_HANDLE;

    // Storage textures + placeholder
    m_storageTextures[0].destroy();
    m_storageTextures[1].destroy();
    m_placeholderTexture.destroy();

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
    spdlog::info("EffectProcessor shut down");
}

// =============================================================================
//  Processing
// =============================================================================

bool EffectProcessor::process(VkCommandBuffer cmd,
                              const VkDescriptorImageInfo& sourceImage,
                              const std::vector<EffectStack::EffectSnapshot>& effects)
{
    if (!m_initialized) return false;
    if (effects.empty()) return true;

    m_stats.effectsApplied = static_cast<int>(effects.size());
    m_currentOutput = 0;

    // Claim this call's ring slot for the source + LUT descriptor sets.  A
    // single shared set cannot be reused across the multiple process() calls
    // the render graph records into one command buffer (one per effected
    // layer), nor across the kRingSize frames that overlap in flight: the
    // binding-1 update is immediate on the CPU, so every dispatch would
    // sample whichever source was bound LAST.  Round-robin over the ring so
    // each call has its own set; a slot is only reused after the owning
    // frame has completed.
    m_curSourceSet = m_sourceDescriptorSets[m_descriptorRingCursor];
    m_curLutSet    = m_lutDescriptorSets[m_descriptorRingCursor];
    m_descriptorRingCursor =
        (m_descriptorRingCursor + 1) % kDescriptorRing;

    // Point this slot's binding 1 (input sampler) at the caller's source.
    {
        VkDescriptorImageInfo srcInfo = sourceImage;
        // Ensure layout is set for shader read
        if (srcInfo.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_curSourceSet;
        writes[0].dstBinding      = 1; // input sampler at binding 1
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &srcInfo;

        // Also point the LUT slot binding 1 at the same source, so a first
        // LUT effect (if any) can read from the external source.
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_curLutSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &srcInfo;

        vkUpdateDescriptorSets(m_device->handle(), 2, writes, 0, nullptr);
    }

    // Timestamp begin
    if (m_queryPool)
        vkCmdResetQueryPool(cmd, m_queryPool, 0, 2);
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0);

    // For each effect, dispatch compute shader
    int sourceIdx = -1;  // -1 = use external source via the current ring slot
    int targetIdx = 0;

    for (size_t i = 0; i < effects.size(); ++i) {
        const auto& snap = effects[i];

        // Ultra Key — prefer light (no cleanup), then combined, fall back to 3-pass
        if (snap.type == EffectType::ChromaKey &&
            (m_ultraKeyLightPipeline    != VK_NULL_HANDLE ||
             m_ultraKeyCombinedPipeline != VK_NULL_HANDLE ||
             m_ultraKeyMattePipeline    != VK_NULL_HANDLE))
        {
            // OutputMode=3 (Original) — bypass the key entirely
            bool isOriginal = (snap.params.size() > 3 &&
                               static_cast<int>(snap.params[3] + 0.5f) == 3);
            if (isOriginal) {
                // Pass through: copy source to target unmodified
                copyImage(cmd, sourceIdx, targetIdx);
                sourceIdx = targetIdx;
                targetIdx = 1 - targetIdx;
            } else {
                int finalTarget = dispatchUltraKey(cmd, snap.params, sourceIdx, targetIdx);
                sourceIdx = finalTarget;
                targetIdx = 1 - finalTarget;
            }
        }
        else if (!dispatchEffect(cmd, snap.type, snap.params, sourceIdx, targetIdx)) {
            spdlog::warn("EffectProcessor: failed to dispatch effect type {}",
                         static_cast<int>(snap.type));
        } else {
            // Ping-pong
            sourceIdx = targetIdx;
            targetIdx = 1 - targetIdx;
        }
    }

    // Timestamp end
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);

    m_currentOutput = sourceIdx;
    return true;
}

bool EffectProcessor::processSync(const VkDescriptorImageInfo& sourceImage,
                                  const std::vector<EffectStack::EffectSnapshot>& effects)
{
    if (!m_initialized || !m_cmdPool) return false;
    if (effects.empty()) return true;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    bool ok = process(cmd, sourceImage, effects);
    vkEndCommandBuffer(cmd);

    // Use persistent fence (reset + reuse) to avoid per-call create/destroy.
    // A5: 100ms bounded fence waits.  These calls are on the synchronous
    // single-shot EffectProcessor path (compositeSync, test code) — the
    // per-frame DAG path does not reach here.  Even so, an infinite wait
    // could deadlock the test runner or export pipeline if the GPU hangs.
    if (ok && m_syncFence != VK_NULL_HANDLE) {
        constexpr uint64_t kFenceTimeoutNs = 100'000'000ull;
        if (vkWaitForFences(m_device->handle(), 1, &m_syncFence,
                             VK_TRUE, kFenceTimeoutNs) == VK_TIMEOUT)
        {
            spdlog::error("[EFFECT] pre-submit fence timeout — bailing");
            m_cmdPool->freeBuffer(cmd);
            return false;
        }
        vkResetFences(m_device->handle(), 1, &m_syncFence);

        // P1.1: route through GpuScheduler.  EffectProcessor's m_queue
        // is the compute queue (set at init); scheduler dispatches to
        // Compute kind explicitly so it owns the locking discipline.
        rt::GpuSubmission sub{};
        sub.cmd             = cmd;
        sub.queue           = rt::GpuQueueKind::Compute;
        sub.completionFence = m_syncFence;
        sub.tag             = "EffectProcessor::sync";
        rt::GpuContext::get().scheduler().submit(sub);
        if (vkWaitForFences(m_device->handle(), 1, &m_syncFence,
                             VK_TRUE, kFenceTimeoutNs) == VK_TIMEOUT)
        {
            spdlog::error("[EFFECT] post-submit fence timeout — GPU hang");
            m_cmdPool->freeBuffer(cmd);
            return false;
        }

        m_cmdPool->freeBuffer(cmd);
    } else {
        // Fallback: use CommandPool's single-time path
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // Read back GPU timestamps
    if (ok && m_queryPool) {
        uint64_t ts[2]{};
        VkResult qr = vkGetQueryPoolResults(
            m_device->handle(), m_queryPool,
            0, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qr == VK_SUCCESS)
            m_stats.gpuTimeMs = static_cast<float>(ts[1] - ts[0])
                              * m_timestampPeriod / 1e6f;
    }

    return ok;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// =============================================================================
//  Snapshot output
// =============================================================================

bool EffectProcessor::snapshotOutputSync(Texture& dst)
{
    if (!m_initialized || !m_device || !m_cmdPool) return false;
    VkImage srcImg = outputImage();
    if (srcImg == VK_NULL_HANDLE) return false;
    if (dst.image() == VK_NULL_HANDLE ||
        dst.width() != m_config.width || dst.height() != m_config.height)
        return false;

    VkDevice dev = m_device->handle();
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = srcImg;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = dst.image();
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {m_config.width, m_config.height, 1};
    vkCmdCopyImage(cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier restoreSrc{};
    restoreSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restoreSrc.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restoreSrc.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    restoreSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restoreSrc.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    restoreSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSrc.image = srcImg;
    restoreSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstReadable{};
    dstReadable.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstReadable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstReadable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dstReadable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstReadable.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dstReadable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstReadable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstReadable.image = dst.image();
    dstReadable.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier post[2] = {restoreSrc, dstReadable};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, post);

    vkEndCommandBuffer(cmd);
    if (m_syncFence != VK_NULL_HANDLE) {
        vkResetFences(dev, 1, &m_syncFence);
        rt::GpuSubmission sub{};
        sub.cmd = cmd; sub.queue = rt::GpuQueueKind::Compute;
        sub.completionFence = m_syncFence; sub.tag = "EffectProcessor::snapshot";
        rt::GpuContext::get().scheduler().submit(sub);
        vkWaitForFences(dev, 1, &m_syncFence, VK_TRUE, 100'000'000ull);
        m_cmdPool->freeBuffer(cmd);
    } else {
        m_cmdPool->endSingleTime(cmd, m_queue);
    }
    return true;
}

//  Resize
// =============================================================================

bool EffectProcessor::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;
    m_config.width  = width;
    m_config.height = height;

    if (m_initialized) {
        // Wait only on our compute queue rather than draining the entire
        // device â€” avoids stalling the graphics queue when processing
        // mixed-resolution clips back-to-back.
        vkQueueWaitIdle(m_queue);
        m_storageTextures[0].destroy();
        m_storageTextures[1].destroy();
        if (!createStorageTextures()) return false;

        // Re-point descriptor sets at new storage textures
        for (int i = 0; i < 2; ++i) {
            VkDescriptorImageInfo outInfo{};
            outInfo.imageView   = m_storageTextures[i].imageView();
            outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet w{};
            w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet          = m_descriptorSets[i];
            w.dstBinding      = 0; // output storage image
            w.descriptorCount = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo      = &outInfo;
            vkUpdateDescriptorSets(m_device->handle(), 1, &w, 0, nullptr);
        }
        // Re-point the source/LUT ring sets at the new storage[0].
        initDescriptorRingBindings();
    }
    return true;
}

// =============================================================================
//  LUT 3D texture management
// =============================================================================

bool EffectProcessor::uploadLUT3D(const std::vector<float>& lutData, int lutSize)
{
    if (!m_initialized || !m_device) return false;
    if (lutData.empty() || lutSize < 2) return false;

    // Destroy previous LUT texture if any
    m_lutTexture3D.destroy();

    // LUT data is size^3 Ã— 3 floats (RGB). We need to convert to RGBA8.
    const size_t numVoxels = static_cast<size_t>(lutSize) * lutSize * lutSize;
    const size_t dataSize  = numVoxels * 4; // RGBA8 = 4 bytes per voxel
    std::vector<uint8_t> rgbaData(dataSize);

    for (size_t i = 0; i < numVoxels; ++i) {
        rgbaData[i * 4 + 0] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 0] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 1] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 1] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 2] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 2] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 3] = 255; // alpha = opaque
    }

    TextureConfig cfg;
    cfg.width       = static_cast<uint32_t>(lutSize);
    cfg.height      = static_cast<uint32_t>(lutSize);
    cfg.depth       = static_cast<uint32_t>(lutSize);
    cfg.format      = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cfg.filter      = VK_FILTER_LINEAR;
    cfg.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (!m_lutTexture3D.createFromData(
            m_allocator->handle(), m_device->handle(), cfg,
            rgbaData.data(), static_cast<VkDeviceSize>(dataSize),
            *m_cmdPool, m_queue))
    {
        spdlog::error("EffectProcessor: Failed to create LUT 3D texture ({}x{}x{})",
                      lutSize, lutSize, lutSize);
        return false;
    }

    // No descriptor write here: dispatchEffect() binds binding 2 (the 3D LUT
    // sampler) on the selected ring slot for every LUT dispatch, reading the
    // current m_lutTexture3D.  A single-set update would not reach the ring.
    spdlog::info("EffectProcessor: uploaded {}x{}x{} LUT 3D texture", lutSize, lutSize, lutSize);
    return true;
}

void EffectProcessor::clearLUT3D()
{
    // dispatchEffect() falls back to the placeholder for binding 2 whenever
    // m_lutTexture3D is null, so just dropping the texture is sufficient.
    m_lutTexture3D.destroy();
}

// =============================================================================
//  Output access
// =============================================================================

VkImage EffectProcessor::outputImage() const noexcept
{
    return m_storageTextures[m_currentOutput].image();
}

VkImageView EffectProcessor::outputImageView() const noexcept
{
    return m_storageTextures[m_currentOutput].imageView();
}

VkDescriptorImageInfo EffectProcessor::outputDescriptorInfo() const
{
    VkDescriptorImageInfo info{};
    info.sampler     = m_storageTextures[m_currentOutput].sampler();
    info.imageView   = outputImageView();
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

bool EffectProcessor::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;

    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(m_config.width)
                               * m_config.height * 4;

    // Create staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf{VK_NULL_HANDLE};
    VmaAllocation stagingAlloc{nullptr};
    if (vmaCreateBuffer(m_allocator->handle(), &bci, &aci,
                        &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS)
        return false;

    // Copy image â†’ buffer
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_config.width, m_config.height, 1};
    vkCmdCopyImageToBuffer(cmd, m_storageTextures[m_currentOutput].image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    // Map and copy to CPU
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    outPixels.resize(static_cast<size_t>(bufSize));
    std::memcpy(outPixels.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);

    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);
    return true;
}

// =============================================================================
//  Internal — create ping-pong storage textures
// =============================================================================

bool EffectProcessor::createStorageTextures()
{
    for (int i = 0; i < 2; ++i) {
        TextureConfig cfg;
        cfg.width  = m_config.width;
        cfg.height = m_config.height;
        cfg.format = m_config.format;
        cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (!m_storageTextures[i].create(m_allocator->handle(),
                                          m_device->handle(), cfg))
        {
            spdlog::error("EffectProcessor: Failed to create storage texture {}", i);
            return false;
        }

        // Transition to GENERAL for compute shader read/write
        VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
        m_storageTextures[i].transitionLayout(
            cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // Also create a 1Ã—1 placeholder (for unused sampler slots)
    TextureConfig phCfg;
    phCfg.width  = 1;
    phCfg.height = 1;
    phCfg.format = m_config.format;
    phCfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    uint32_t transparent = 0;
    if (!m_placeholderTexture.createFromData(
            m_allocator->handle(), m_device->handle(), phCfg,
            &transparent, 4, *m_cmdPool, m_queue))
    {
        spdlog::error("EffectProcessor: Failed to create placeholder texture");
        return false;
    }

    return true;
}

// =============================================================================
//  Internal — create descriptor set layout, pool, and sets
// =============================================================================

bool EffectProcessor::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Main descriptor set layout (bindings 0-1, used by most shaders):
    //   binding 0: storage image   (writeonly output)
    //   binding 1: combined sampler (readonly input)
    VkDescriptorSetLayoutBinding mainBindings[2]{};

    mainBindings[0].binding         = 0;
    mainBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    mainBindings[0].descriptorCount = 1;
    mainBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    mainBindings[1].binding         = 1;
    mainBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    mainBindings[1].descriptorCount = 1;
    mainBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings    = mainBindings;

    if (vkCreateDescriptorSetLayout(dev, &layoutCI, nullptr,
                                     &m_descriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create descriptor set layout");
        return false;
    }

    // LUT-specific descriptor set layout (bindings 0-2):
    //   binding 0: storage image   (writeonly output)
    //   binding 1: combined sampler (readonly input)
    //   binding 2: combined sampler (3D LUT texture)
    VkDescriptorSetLayoutBinding lutBindings[3]{};

    lutBindings[0].binding         = 0;
    lutBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    lutBindings[0].descriptorCount = 1;
    lutBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    lutBindings[1].binding         = 1;
    lutBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lutBindings[1].descriptorCount = 1;
    lutBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    lutBindings[2].binding         = 2;
    lutBindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lutBindings[2].descriptorCount = 1;
    lutBindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo lutLayoutCI{};
    lutLayoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lutLayoutCI.bindingCount = 3;
    lutLayoutCI.pBindings    = lutBindings;

    if (vkCreateDescriptorSetLayout(dev, &lutLayoutCI, nullptr,
                                     &m_lutDescriptorSetLayout) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create LUT descriptor set layout");
        return false;
    }

    // Pool sizing:
    //   ping-pong[0], ping-pong[1]                       = 2 main sets
    //   source ring (kDescriptorRing main-layout sets)
    //   LUT ring    (kDescriptorRing LUT-layout sets)
    //   main set = 1 storage + 1 sampler ; LUT set = 1 storage + 2 samplers.
    // See EffectProcessor.h (m_sourceDescriptorSets) for why the rings exist.
    const uint32_t kMainSets = 2u + kDescriptorRing;   // ping-pong + source ring
    const uint32_t kLutSets  = kDescriptorRing;
    const uint32_t kTotalSets = kMainSets + kLutSets;

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = kMainSets + kLutSets;          // 1 storage per set
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kMainSets + 2u * kLutSets;     // main:1, LUT:2

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = kTotalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create descriptor pool "
                      "(storage={}, samplers={}, sets={})",
                      poolSizes[0].descriptorCount,
                      poolSizes[1].descriptorCount,
                      poolCI.maxSets);
        return false;
    }

    // Allocate the main-layout sets: 2 ping-pong + kDescriptorRing source.
    std::vector<VkDescriptorSetLayout> mainLayouts(kMainSets, m_descriptorSetLayout);
    std::vector<VkDescriptorSet>       mainSets(kMainSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = kMainSets;
    allocInfo.pSetLayouts        = mainLayouts.data();

    if (vkAllocateDescriptorSets(dev, &allocInfo, mainSets.data()) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate {} main descriptor sets "
                      "(pool maxSets={})", kMainSets, poolCI.maxSets);
        return false;
    }
    m_descriptorSets[0] = mainSets[0];  // target = storageTexture[0]
    m_descriptorSets[1] = mainSets[1];  // target = storageTexture[1]
    m_sourceDescriptorSets.assign(mainSets.begin() + 2, mainSets.end());

    // Allocate the LUT ring from the LUT-specific layout.
    std::vector<VkDescriptorSetLayout> lutLayouts(kLutSets, m_lutDescriptorSetLayout);
    m_lutDescriptorSets.assign(kLutSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo lutAllocInfo{};
    lutAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    lutAllocInfo.descriptorPool     = m_descriptorPool;
    lutAllocInfo.descriptorSetCount = kLutSets;
    lutAllocInfo.pSetLayouts        = lutLayouts.data();

    if (vkAllocateDescriptorSets(dev, &lutAllocInfo,
                                 m_lutDescriptorSets.data()) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate {} LUT descriptor sets "
                      "(pool maxSets={})", kLutSets, poolCI.maxSets);
        return false;
    }
    m_descriptorRingCursor = 0;

    // â”€â”€ Initialize descriptor sets for the ping-pong pair â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Each set has:
    //   binding 0 â†’ storageTexture[i]  (output)
    //   binding 1 â†’ storageTexture[1-i] (input, from previous ping-pong)
    //
    // The source descriptor set (sets[2]) has:
    //   binding 0 â†’ storageTexture[0] (output for first effect)
    //   binding 1 â†’ external source (will be updated per-call)

    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[i].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo inInfo{};
        inInfo.sampler     = m_storageTextures[1 - i].sampler();
        inInfo.imageView   = m_storageTextures[1 - i].imageView();
        inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSets[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSets[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &inInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // Source + LUT rings: binding 0 â†’ storageTexture[0], binding 1 (input) â†’
    // placeholder, LUT binding 2 â†’ placeholder.  Per-call bindings (the
    // external source, and the live 3D LUT) are written in process() /
    // dispatchEffect on the slot that call claims.
    initDescriptorRingBindings();

    return true;
}

// =============================================================================
//  Internal — (re)point the static bindings of the source/LUT descriptor rings
// =============================================================================

void EffectProcessor::initDescriptorRingBindings()
{
    if (!m_device) return;
    VkDevice dev = m_device->handle();

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = m_storageTextures[0].imageView();
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const VkDescriptorImageInfo phInfo = m_placeholderTexture.descriptorInfo();

    // Source ring: binding 0 = output (storage[0]), binding 1 = placeholder.
    for (VkDescriptorSet set : m_sourceDescriptorSets) {
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &phInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // LUT ring: binding 0 = output, binding 1 = placeholder, binding 2 =
    // placeholder.  dispatchEffect rewrites all three per LUT dispatch.
    for (VkDescriptorSet set : m_lutDescriptorSets) {
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &phInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = set;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &phInfo;

        vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    }
}

// =============================================================================
//  Internal — create compute pipelines (one per effect type)
// =============================================================================

bool EffectProcessor::createPipelines()
{
    if (!m_pipelineManager.create(*m_device)) {
        spdlog::error("EffectProcessor: Failed to create pipeline manager");
        return false;
    }

    // Pipeline layout shared by all effect shaders
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(EffectPushConstants);

    m_pipelineLayout = m_pipelineManager.createLayout(
        {pushRange}, {m_descriptorSetLayout});
    if (m_pipelineLayout == VK_NULL_HANDLE) {
        spdlog::error("EffectProcessor: Failed to create pipeline layout");
        return false;
    }

    // Helper lambda â€” load shader + create pipeline.  Non-fatal if not found
    // (we just leave that pipeline null and skip it at dispatch time).
    auto loadPipeline = [&](const char* spvName) -> VkPipeline {
        fs::path path = findShader(spvName);
        if (path.empty()) {
            spdlog::warn("EffectProcessor: shader {} not found â€” skipping", spvName);
            return VK_NULL_HANDLE;
        }
        VkShaderModule mod = m_pipelineManager.loadShader(path);
        if (mod == VK_NULL_HANDLE) {
            spdlog::warn("EffectProcessor: failed to load {}", spvName);
            return VK_NULL_HANDLE;
        }
        ComputePipelineConfig pc;
        pc.compShader = mod;
        pc.layout     = m_pipelineLayout;
        VkPipeline p = m_pipelineManager.createComputePipeline(pc);
        if (p == VK_NULL_HANDLE)
            spdlog::warn("EffectProcessor: failed to create pipeline for {}", spvName);
        return p;
    };

    m_colorCorrectPipeline = loadPipeline("color_correct.comp.spv");
    m_blurPipeline         = loadPipeline("blur.comp.spv");
    m_sharpenPipeline      = loadPipeline("sharpen.comp.spv");
    m_glowPipeline         = loadPipeline("glow.comp.spv");
    m_chromaKeyPipeline    = loadPipeline("chroma_key.comp.spv");
    m_ultraKeyMattePipeline     = loadPipeline("ultra_key_matte.comp.spv");
    m_ultraKeyCleanupPipeline   = loadPipeline("ultra_key_cleanup.comp.spv");
    m_ultraKeyFinalizePipeline  = loadPipeline("ultra_key_finalize.comp.spv");
    m_ultraKeyCombinedPipeline  = loadPipeline("ultra_key_combined.comp.spv");
    m_ultraKeyLightPipeline     = loadPipeline("ultra_key_light.comp.spv");
    m_transform2dPipeline  = loadPipeline("transform2d.comp.spv");
    m_vignettePipeline    = loadPipeline("vignette.comp.spv");
    m_lutPipeline         = loadPipeline("lut.comp.spv");
    m_letterboxPipeline   = loadPipeline("letterbox.comp.spv");
    m_colorGradingPipeline = loadPipeline("lumetri_color.comp.spv");
    m_otsPipeline         = loadPipeline("ots.comp.spv");
    m_flipPipeline        = loadPipeline("flip.comp.spv");
    m_scanlinesPipeline         = loadPipeline("scanlines.comp.spv");
    m_blockGlitchPipeline       = loadPipeline("block_glitch.comp.spv");
    m_chromaticSplitPipeline    = loadPipeline("chromatic_split.comp.spv");
    m_turbulentDisplacePipeline = loadPipeline("turbulent_displace.comp.spv");
    m_posterizePipeline         = loadPipeline("posterize.comp.spv");
    m_grainPipeline             = loadPipeline("grain.comp.spv");
    m_signalTearPipeline        = loadPipeline("signal_tear.comp.spv");
    m_beatZoomPipeline          = loadPipeline("beat_zoom.comp.spv");
    m_beatFlashPipeline         = loadPipeline("beat_flash.comp.spv");
    m_beatShakePipeline         = loadPipeline("beat_shake.comp.spv");
    m_beatChromaPipeline        = loadPipeline("beat_chroma.comp.spv");
    m_beatDropPipeline          = loadPipeline("beat_drop.comp.spv");

    spdlog::info("EffectProcessor: pipelines created (colorCorrect={}, blur={}, lumetri={})",
                 m_colorCorrectPipeline != VK_NULL_HANDLE,
                 m_blurPipeline != VK_NULL_HANDLE,
                 m_colorGradingPipeline != VK_NULL_HANDLE);
    return true;
}

// =============================================================================
//  Internal — dispatch a single effect
// =============================================================================

bool EffectProcessor::dispatchEffect(VkCommandBuffer cmd,
                                     EffectType type,
                                     const std::vector<float>& params,
                                     int sourceIdx, int targetIdx)
{
    VkPipeline pipeline = getPipeline(type);
    if (pipeline == VK_NULL_HANDLE) return false;

    // Select descriptor set:
    //   sourceIdx == -1 â†’ use m_curSourceSet (external source image)
    //   sourceIdx ==  0 or 1 â†’ use m_descriptorSets[targetIdx]
    //     (descriptorSets[targetIdx] has binding0 = storage[targetIdx],
    //      binding1 = storage[1-targetIdx] which is the source)
    //
    // For LUT effects, use the LUT-specific descriptor set which also has
    // binding 2 pointing to the 3D LUT texture.
    VkDescriptorSet ds;
    if (type == EffectType::LUT) {
        // Use LUT descriptor set â€“ update bindings 0+1 for current ping-pong state
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[targetIdx].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo inInfo{};
        if (sourceIdx == -1) {
            // First effect: input from external source (already set in m_curLutSet binding 1)
            // For LUT, we need to reference the first storage texture as output and
            // the source image needs to be handled. Since we can't share the external
            // image between descriptor sets, we use a workaround: set source to storage[1]
            // which will be overwritten. Better: update LUT set binding 1 to source.
            inInfo = m_placeholderTexture.descriptorInfo(); // fallback
        } else {
            inInfo.sampler     = m_storageTextures[1 - targetIdx].sampler();
            inInfo.imageView   = m_storageTextures[1 - targetIdx].imageView();
            inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        VkDescriptorImageInfo lutInfo = m_lutTexture3D.image() != VK_NULL_HANDLE
            ? m_lutTexture3D.descriptorInfo()
            : m_placeholderTexture.descriptorInfo();

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_curLutSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_curLutSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &inInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_curLutSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &lutInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);

        ds = m_curLutSet;
    } else if (sourceIdx == -1) {
        // First effect (non-LUT): source is external image (the current ring
        // slot's binding 1 was set in process()).  binding 0 = storage[0].
        ds = m_curSourceSet;
    } else {
        // Subsequent effects: ping-pong.
        // m_descriptorSets[targetIdx] has binding0 = storage[targetIdx] (output),
        // binding1 = storage[1-targetIdx] = storage[sourceIdx] (input).
        ds = m_descriptorSets[targetIdx];
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &ds, 0, nullptr);

    // Push constants
    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    // Dispatch: 16Ã—16 workgroups
    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Pipeline barrier: compute write â†’ compute read for next effect
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    return true;
}

VkPipeline EffectProcessor::getPipeline(EffectType type) const
{
    switch (type) {
    case EffectType::ColorCorrect: return m_colorCorrectPipeline;
    case EffectType::Blur:         return m_blurPipeline;
    case EffectType::Sharpen:      return m_sharpenPipeline;
    case EffectType::Glow:         return m_glowPipeline;
    case EffectType::ChromaKey:    return m_chromaKeyPipeline;
    case EffectType::Transform2D:  return m_transform2dPipeline;
    case EffectType::Vignette:      return m_vignettePipeline;
    case EffectType::LUT:            return m_lutPipeline;
    case EffectType::Letterbox:      return m_letterboxPipeline;
    case EffectType::ColorGrading:   return m_colorGradingPipeline;
    case EffectType::LumetriColor:   return m_colorGradingPipeline;
    case EffectType::OtsLeft:        return m_otsPipeline;
    case EffectType::OtsRight:       return m_otsPipeline;
    case EffectType::FlipHorizontal: return m_flipPipeline;
    case EffectType::FlipVertical:   return m_flipPipeline;
    case EffectType::Scanlines:         return m_scanlinesPipeline;
    case EffectType::BlockGlitch:       return m_blockGlitchPipeline;
    case EffectType::ChromaticSplit:    return m_chromaticSplitPipeline;
    case EffectType::TurbulentDisplace: return m_turbulentDisplacePipeline;
    case EffectType::Posterize:         return m_posterizePipeline;
    case EffectType::Grain:             return m_grainPipeline;
    case EffectType::SignalTear:        return m_signalTearPipeline;
    case EffectType::BeatZoom:          return m_beatZoomPipeline;
    case EffectType::BeatFlash:         return m_beatFlashPipeline;
    case EffectType::BeatShake:         return m_beatShakePipeline;
    case EffectType::BeatChroma:        return m_beatChromaPipeline;
    case EffectType::BeatDrop:          return m_beatDropPipeline;
    default:                       return VK_NULL_HANDLE;
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Ultra Key â€” 3-pass dispatch
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void EffectProcessor::dispatchPass(VkCommandBuffer cmd, VkPipeline pipeline,
                                   VkDescriptorSet ds,
                                   const std::vector<float>& params)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &ds, 0, nullptr);

    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier between passes — use image barrier for correct
    // storage-image → sampled-image cache coherency on all GPUs.
    VkMemoryBarrier memBarrier{};
    memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

void EffectProcessor::copyImage(VkCommandBuffer cmd, int sourceIdx, int targetIdx)
{
    if (targetIdx < 0 || targetIdx > 1)
        return;

    if (sourceIdx < 0) {
        // Source is the external image (first effect in chain).
        // Use dispatchPass with a passthrough: the current ring slot's source
        // set reads from the external source (binding 1) and writes to
        // storage[targetIdx] (binding 0).
        // We use the color correct pipeline with identity params as a passthrough.
        std::vector<float> identity(28, 0.0f);
        identity[0] = 1.0f;  // brightness = 1 (neutral)
        identity[1] = 1.0f;  // contrast = 1 (neutral)
        identity[2] = 1.0f;  // saturation = 1 (neutral)
        dispatchPass(cmd, m_colorCorrectPipeline, m_curSourceSet, identity);
    } else if (sourceIdx == targetIdx) {
        // Same source and target, no copy needed.
        return;
    } else {
        // Storage-to-storage copy
        VkImage srcImage = m_storageTextures[sourceIdx].image();
        VkImage dstImage = m_storageTextures[targetIdx].image();

        // Transition source to TRANSFER_SRC
        VkImageMemoryBarrier srcBarrier{};
        srcBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image                           = srcImage;
        srcBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        srcBarrier.subresourceRange.levelCount     = 1;
        srcBarrier.subresourceRange.layerCount     = 1;
        srcBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;

        // Transition destination to TRANSFER_DST
        VkImageMemoryBarrier dstBarrier{};
        dstBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
        dstBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.image                           = dstImage;
        dstBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        dstBarrier.subresourceRange.levelCount     = 1;
        dstBarrier.subresourceRange.layerCount     = 1;
        dstBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        dstBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier barriers[2] = {srcBarrier, dstBarrier};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount     = 1;
        copyRegion.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount     = 1;
        copyRegion.extent.width                  = m_config.width;
        copyRegion.extent.height                 = m_config.height;
        copyRegion.extent.depth                  = 1;

        vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition both back to GENERAL
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        VkImageMemoryBarrier restore[2] = {srcBarrier, dstBarrier};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, restore);
    }
}

int EffectProcessor::dispatchUltraKey(VkCommandBuffer cmd,
                                      const std::vector<float>& params,
                                      int sourceIdx, int targetIdx)
{
    VkDescriptorSet ds = (sourceIdx == -1) ? m_curSourceSet
                                           : m_descriptorSets[targetIdx];

    // Determine whether matte cleanup (choke/soften/contrast) is needed.
    // Indices match ChromaKey::Param: Choke=10, Soften=11, Contrast=12.
    bool needsCleanup = false;
    if (params.size() > 12) {
        float chokeVal    = params[10];
        float softenVal   = params[11];
        float contrastVal = params[12];
        needsCleanup = (chokeVal > 0.01f || softenVal > 0.01f ||
                        contrastVal > 0.01f);
    }

    // ── Experimental single-pass path (opt-in via env var) ─────────────
    static bool singlePass = []() {
        const char* v = std::getenv("ROUNDTABLE_ULTRAKEY_SINGLEPASS");
        return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
    }();

    if (singlePass) {
        if (!needsCleanup && m_ultraKeyLightPipeline != VK_NULL_HANDLE) {
            dispatchPass(cmd, m_ultraKeyLightPipeline, ds, params);
            return targetIdx;
        }
        if (m_ultraKeyCombinedPipeline != VK_NULL_HANDLE) {
            dispatchPass(cmd, m_ultraKeyCombinedPipeline, ds, params);
            return targetIdx;
        }
    }

    // ── Proven 3-pass pipeline (matte → cleanup → finalize) ────────────
    // Always runs all 3 passes to maintain correct output-slot parity.
    // The cleanup pass is lightweight when params are at defaults
    // (choke=0, soften=0, contrast=0) — it just reads/clamps alpha.

    // Pass 1: Matte generation  (source → targetIdx)
    dispatchPass(cmd, m_ultraKeyMattePipeline, ds, params);

    int pass1Output = targetIdx;
    int pass2Target = 1 - pass1Output;

    // Pass 2: Matte cleanup  (pass1Output → pass2Target)
    {
        VkDescriptorSet ds2 = m_descriptorSets[pass2Target];
        dispatchPass(cmd, m_ultraKeyCleanupPipeline, ds2, params);
    }

    int pass2Output = pass2Target;
    int pass3Target = 1 - pass2Output;

    // Pass 3: Finalize (spill suppress + color correct + output mode)
    //         (pass2Output → targetIdx, maintaining slot parity)
    {
        VkDescriptorSet ds3 = m_descriptorSets[pass3Target];
        dispatchPass(cmd, m_ultraKeyFinalizePipeline, ds3, params);
    }

    return pass3Target;
}

} // namespace rt
