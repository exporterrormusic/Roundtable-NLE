/*
 * Nv12Converter.cpp — GPU compute-shader NV12 → BGRA color conversion.
 *
 * Core lifecycle + texture management. Pipeline creation extracted to
 * Nv12ConverterPipeline.cpp, conversion functions to Nv12ConverterConvert.cpp
 * and Nv12ConverterYuv420p.cpp.
 */

#include <volk.h>
#include "Nv12Converter.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

namespace rt {

//══════════════════════════════════════════════════════════════════════════
//  Lifecycle
//══════════════════════════════════════════════════════════════════════════

Nv12Converter::Nv12Converter() = default;

Nv12Converter::~Nv12Converter()
{
    shutdown();
}

bool Nv12Converter::init(Device& device, Allocator& allocator,
                          CommandPool& cmdPool, VkQueue computeQueue,
                          const Nv12ConverterConfig& config)
{
    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    if (!createTextures()) {
        spdlog::error("Nv12Converter: failed to create textures");
        return false;
    }
    if (!createDescriptorResources()) {
        spdlog::error("Nv12Converter: failed to create descriptor resources");
        return false;
    }
    if (!createPipeline()) {
        spdlog::error("Nv12Converter: failed to create pipeline");
        return false;
    }

    m_initialized = true;
    spdlog::info("Nv12Converter initialized ({}x{})", m_config.width, m_config.height);
    return true;
}

void Nv12Converter::shutdown()
{
    if (!m_device) return;
    VkDevice dev = m_device->handle();
    if (dev) vkDeviceWaitIdle(dev);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_shaderModule, nullptr);
        m_shaderModule = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet  = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    m_yTexture.destroy();
    m_uvTexture.destroy();
    m_uTexture.destroy();
    m_vTexture.destroy();
    m_outputTexture.destroy();

    // YUV420P pipeline cleanup
    if (m_yuv420pPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_yuv420pPipeline, nullptr);
        m_yuv420pPipeline = VK_NULL_HANDLE;
    }
    if (m_yuv420pPipeLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_yuv420pPipeLayout, nullptr);
        m_yuv420pPipeLayout = VK_NULL_HANDLE;
    }
    if (m_yuv420pShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_yuv420pShaderModule, nullptr);
        m_yuv420pShaderModule = VK_NULL_HANDLE;
    }
    if (m_yuv420pDescPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_yuv420pDescPool, nullptr);
        m_yuv420pDescPool = VK_NULL_HANDLE;
        m_yuv420pDescSet  = VK_NULL_HANDLE;
    }
    if (m_yuv420pDescSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_yuv420pDescSetLayout, nullptr);
        m_yuv420pDescSetLayout = VK_NULL_HANDLE;
    }

    // P010 pipeline cleanup
    m_p010YTexture.destroy();
    m_p010UvTexture.destroy();
    if (m_p010Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_p010Pipeline, nullptr);
        m_p010Pipeline = VK_NULL_HANDLE;
    }
    if (m_p010PipeLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_p010PipeLayout, nullptr);
        m_p010PipeLayout = VK_NULL_HANDLE;
    }
    if (m_p010ShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_p010ShaderModule, nullptr);
        m_p010ShaderModule = VK_NULL_HANDLE;
    }
    if (m_p010DescPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_p010DescPool, nullptr);
        m_p010DescPool = VK_NULL_HANDLE;
        m_p010DescSet  = VK_NULL_HANDLE;
    }
    if (m_p010DescSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_p010DescSetLayout, nullptr);
        m_p010DescSetLayout = VK_NULL_HANDLE;
    }

    // YUVA444P12 (ProRes 4444) cleanup
    m_yuvaYTexture.destroy();
    m_yuvaUTexture.destroy();
    m_yuvaVTexture.destroy();
    m_yuvaATexture.destroy();
    if (m_yuva444Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_yuva444Pipeline, nullptr);
        m_yuva444Pipeline = VK_NULL_HANDLE;
    }
    if (m_yuva444PipeLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, m_yuva444PipeLayout, nullptr);
        m_yuva444PipeLayout = VK_NULL_HANDLE;
    }
    if (m_yuva444ShaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_yuva444ShaderModule, nullptr);
        m_yuva444ShaderModule = VK_NULL_HANDLE;
    }
    if (m_yuva444DescPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_yuva444DescPool, nullptr);
        m_yuva444DescPool = VK_NULL_HANDLE;
        m_yuva444DescSet  = VK_NULL_HANDLE;
    }
    if (m_yuva444DescSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, m_yuva444DescSetLayout, nullptr);
        m_yuva444DescSetLayout = VK_NULL_HANDLE;
    }

    // Phase 4.2 — RGBA16F output resources.  These reuse the P010 / yuva444
    // descriptor sets + pipeline layouts (freed above), so only the extra
    // pipelines, shader modules, and the 16F output texture are owned here.
    m_output16FTexture.destroy();
    m_output16W = 0;
    m_output16H = 0;
    if (m_p010Pipeline16F != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_p010Pipeline16F, nullptr);
        m_p010Pipeline16F = VK_NULL_HANDLE;
    }
    if (m_p010ShaderModule16F != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_p010ShaderModule16F, nullptr);
        m_p010ShaderModule16F = VK_NULL_HANDLE;
    }
    if (m_yuva444Pipeline16F != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, m_yuva444Pipeline16F, nullptr);
        m_yuva444Pipeline16F = VK_NULL_HANDLE;
    }
    if (m_yuva444ShaderModule16F != VK_NULL_HANDLE) {
        vkDestroyShaderModule(dev, m_yuva444ShaderModule16F, nullptr);
        m_yuva444ShaderModule16F = VK_NULL_HANDLE;
    }

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
}

//══════════════════════════════════════════════════════════════════════════
//  Internal — create textures
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::createTextures()
{
    // Output BGRA storage image
    TextureConfig outCfg;
    outCfg.width  = m_config.width;
    outCfg.height = m_config.height;
    outCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    outCfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!m_outputTexture.create(m_allocator->handle(), m_device->handle(), outCfg)) {
        spdlog::error("Nv12Converter: failed to create output texture");
        return false;
    }

    // Transition to GENERAL for compute shader write
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Y and UV textures are created on demand in convert() when we know the
    // actual frame dimensions (they may differ from config if resize hasn't
    // been called yet).

    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Resize
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;

    m_config.width  = width;
    m_config.height = height;

    if (m_initialized) {
        GpuContext::get().scheduler().deviceWaitIdle();
        m_yTexture.destroy();
        m_uvTexture.destroy();
        m_uTexture.destroy();
        m_vTexture.destroy();
        m_outputTexture.destroy();
        if (!createTextures()) return false;
    }
    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Output access
//══════════════════════════════════════════════════════════════════════════

VkDescriptorImageInfo Nv12Converter::outputDescriptorInfo() const
{
    return m_outputTexture.descriptorInfo();
}

bool Nv12Converter::convertAndReadbackNV12Scaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());
    if (!convertSyncScaled(yData, yLinesize, uvData, uvLinesize,
                           srcW, srcH, dstW, dstH))
        return false;
    return readbackOutput(outPixels);
}

bool Nv12Converter::convertAndReadbackYuv420pScaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());
    if (!convertYuv420pSyncScaled(yData, yLinesize, uData, uLinesize,
                                   vData, vLinesize, srcW, srcH, dstW, dstH))
        return false;
    return readbackOutput(outPixels);
}

bool Nv12Converter::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized || !m_cmdPool || !m_allocator) return false;

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

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

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, m_outputTexture.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    outPixels.resize(static_cast<size_t>(bufSize));
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    std::memcpy(outPixels.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);
    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);

    return true;
}

// Phase 4.2 — read back the RGBA16F output texture (8 bytes/px, raw IEEE-754
// halfs).  Mirrors readbackOutput but uses the 16F texture's OWN dimensions
// (m_output16W/H), never m_config.
bool Nv12Converter::readbackOutput16F(std::vector<uint8_t>& outHalfRgba)
{
    if (!m_cmdPool || !m_allocator) return false;
    if (m_output16FTexture.image() == VK_NULL_HANDLE) return false;

    const uint32_t w = m_output16W;
    const uint32_t h = m_output16H;
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 8; // RGBA16F

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

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_output16FTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, m_output16FTexture.image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_output16FTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    outHalfRgba.resize(static_cast<size_t>(bufSize));
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    std::memcpy(outHalfRgba.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);
    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);

    return true;
}

} // namespace rt
