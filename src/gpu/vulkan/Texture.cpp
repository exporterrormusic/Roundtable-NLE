/*
 * Texture.cpp — VkImage + VkImageView + VkSampler wrapper.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/Texture.h"
#include "vulkan/CommandPool.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include "StagingRing.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace rt {

// ── Static helpers ──────────────────────────────────────────────────────────

static VkImageView createImageView(VkDevice device, VkImage image, VkFormat format,
                                    VkImageAspectFlags aspect, uint32_t mipLevels,
                                    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image;
    viewInfo.viewType                        = viewType;
    viewInfo.format                          = format;
    viewInfo.subresourceRange.aspectMask     = aspect;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;

    VkImageView view;
    if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return view;
}

static VkSampler createSampler(VkDevice device, VkFilter filter,
                                VkSamplerAddressMode addressMode, uint32_t mipLevels)
{
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter               = filter;
    samplerInfo.minFilter               = filter;
    samplerInfo.addressModeU            = addressMode;
    samplerInfo.addressModeV            = addressMode;
    samplerInfo.addressModeW            = addressMode;
    samplerInfo.anisotropyEnable        = VK_TRUE;
    samplerInfo.maxAnisotropy           = 16.0f;
    samplerInfo.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable           = VK_FALSE;
    samplerInfo.mipmapMode              = (filter == VK_FILTER_LINEAR)
                                               ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod     = 0.0f;
    samplerInfo.maxLod     = static_cast<float>(mipLevels);

    VkSampler sampler;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return sampler;
}

/// Create a host-accessible staging buffer and copy pixel data into it.
/// Returns true on success; fills outStaging with the buffer handle for cleanup.
static bool createStagingBuffer(VmaAllocator allocator, const void* pixelData,
                                 VkDeviceSize dataSize,
                                 VkBuffer& outBuffer, VmaAllocation& outAllocation)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = dataSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo allocResult{};
    VkResult result = vmaCreateBuffer(allocator, &info, &allocInfo,
                                       &outBuffer, &outAllocation, &allocResult);
    if (result != VK_SUCCESS) return false;

    std::memcpy(allocResult.pMappedData, pixelData, static_cast<size_t>(dataSize));
    return true;
}

// ── Texture member helpers (defined here, declared in header) ───────────────

/// Record the buffer-to-image copy + layout transitions into a command buffer.
/// Transitions: prevLayout → TRANSFER_DST → SHADER_READ_ONLY.
void Texture::recordUpload(VkCommandBuffer cmd, VkBuffer stagingBuffer,
                            VkDeviceSize stagingOffset, VkImageLayout prevLayout)
{
    // Transition prevLayout → TRANSFER_DST_OPTIMAL
    transitionLayout(cmd, prevLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const bool is3D = (m_depth > 1);

    VkBufferImageCopy region{};
    region.bufferOffset      = stagingOffset;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {m_width, m_height, m_depth};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ── Destructor ──────────────────────────────────────────────────────────────

Texture::~Texture()
{
    destroy();
}

// ── Move constructor ────────────────────────────────────────────────────────

Texture::Texture(Texture&& other) noexcept
    : m_image(other.m_image)
    , m_imageView(other.m_imageView)
    , m_sampler(other.m_sampler)
    , m_allocation(other.m_allocation)
    , m_allocator(other.m_allocator)
    , m_device(other.m_device)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_depth(other.m_depth)
    , m_format(other.m_format)
    , m_currentLayout(other.m_currentLayout)
    , m_mipLevels(other.m_mipLevels)
{
    other.m_image         = VK_NULL_HANDLE;
    other.m_imageView     = VK_NULL_HANDLE;
    other.m_sampler       = VK_NULL_HANDLE;
    other.m_allocation    = nullptr;
    other.m_allocator     = nullptr;
    other.m_device        = VK_NULL_HANDLE;
    other.m_width         = 0;
    other.m_height        = 0;
    other.m_depth         = 1;
    other.m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ── Move assignment ─────────────────────────────────────────────────────────

Texture& Texture::operator=(Texture&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_image         = other.m_image;
        m_imageView     = other.m_imageView;
        m_sampler       = other.m_sampler;
        m_allocation    = other.m_allocation;
        m_allocator     = other.m_allocator;
        m_device        = other.m_device;
        m_width         = other.m_width;
        m_height        = other.m_height;
        m_depth         = other.m_depth;
        m_format        = other.m_format;
        m_currentLayout = other.m_currentLayout;
        m_mipLevels     = other.m_mipLevels;

        other.m_image         = VK_NULL_HANDLE;
        other.m_imageView     = VK_NULL_HANDLE;
        other.m_sampler       = VK_NULL_HANDLE;
        other.m_allocation    = nullptr;
        other.m_allocator     = nullptr;
        other.m_device        = VK_NULL_HANDLE;
        other.m_width         = 0;
        other.m_height        = 0;
        other.m_depth         = 1;
        other.m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return *this;
}

// ── create (empty texture) ──────────────────────────────────────────────────

bool Texture::create(VmaAllocator allocator, VkDevice device, const TextureConfig& config)
{
    m_allocator = allocator;
    m_device    = device;
    m_width     = config.width;
    m_height    = config.height;
    m_depth     = config.depth;
    m_format    = config.format;
    m_mipLevels = config.generateMipmaps
                      ? static_cast<uint32_t>(std::floor(std::log2(std::max({config.width, config.height, config.depth})))) + 1
                      : config.mipLevels;

    const bool is3D = (m_depth > 1);

    // ── Create VkImage via VMA ──────────────────────────────────────────
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.extent.width  = m_width;
    imageInfo.extent.height = m_height;
    imageInfo.extent.depth  = m_depth;
    imageInfo.mipLevels     = m_mipLevels;
    imageInfo.arrayLayers   = 1;
    imageInfo.format        = m_format;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage         = config.usage;
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    if (!config.concurrentQueueFamilies.empty()) {
        imageInfo.sharingMode            = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount  = static_cast<uint32_t>(config.concurrentQueueFamilies.size());
        imageInfo.pQueueFamilyIndices    = config.concurrentQueueFamilies.data();
    } else {
        imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    }

    // Add transfer dst if mipmaps requested (for blit chain)
    if (config.generateMipmaps)
    {
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult result = vmaCreateImage(m_allocator, &imageInfo, &allocCreateInfo,
                                      &m_image, &m_allocation, nullptr);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create texture image ({}x{}, VkResult: {})",
                      m_width, m_height, static_cast<int>(result));
        return false;
    }

    // ── Determine aspect flags ──────────────────────────────────────────
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (m_format == VK_FORMAT_D32_SFLOAT || m_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        m_format == VK_FORMAT_D16_UNORM)
    {
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // ── Create image view ───────────────────────────────────────────────
    {
        VkImageViewType viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        m_imageView = createImageView(m_device, m_image, m_format, aspect, m_mipLevels, viewType);
    }
    if (m_imageView == VK_NULL_HANDLE)
    {
        spdlog::error("Failed to create image view");
        destroy();
        return false;
    }

    // ── Create sampler ──────────────────────────────────────────────────
    m_sampler = createSampler(m_device, config.filter, config.addressMode, m_mipLevels);
    if (m_sampler == VK_NULL_HANDLE)
    {
        spdlog::error("Failed to create sampler");
        destroy();
        return false;
    }

    m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// ── createFromData ──────────────────────────────────────────────────────────

bool Texture::createFromData(VmaAllocator allocator, VkDevice device,
                              const TextureConfig& config,
                              const void* pixelData, VkDeviceSize dataSize,
                              CommandPool& cmdPool, VkQueue transferQueue)
{
    if (!create(allocator, device, config)) return false;

    VkBuffer      stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    if (!createStagingBuffer(allocator, pixelData, dataSize, stagingBuffer, stagingAllocation)) {
        spdlog::error("Failed to create staging buffer for texture upload");
        return false;
    }

    VkCommandBuffer cmd = cmdPool.beginSingleTime();
    recordUpload(cmd, stagingBuffer, 0, VK_IMAGE_LAYOUT_UNDEFINED);
    cmdPool.endSingleTime(cmd, transferQueue);

    vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);
    return true;
}

// ── updateData (re-upload to existing texture) ──────────────────────────────

bool Texture::updateData(const void* pixelData, VkDeviceSize dataSize,
                          CommandPool& cmdPool, VkQueue transferQueue)
{
    if (m_image == VK_NULL_HANDLE || m_allocator == nullptr) {
        spdlog::error("Texture::updateData: texture not created yet");
        return false;
    }

    VkBuffer      stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    if (!createStagingBuffer(m_allocator, pixelData, dataSize, stagingBuffer, stagingAllocation)) {
        spdlog::error("Texture::updateData: failed to create staging buffer");
        return false;
    }

    VkCommandBuffer cmd = cmdPool.beginSingleTime();
    recordUpload(cmd, stagingBuffer, 0, m_currentLayout);
    cmdPool.endSingleTime(cmd, transferQueue);

    vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAllocation);
    return true;
}

// ── createFromDataBatched (records into external cmd buffer, no submit) ─────

bool Texture::createFromDataBatched(VmaAllocator allocator, VkDevice device,
                                      const TextureConfig& config,
                                      const void* pixelData, VkDeviceSize dataSize,
                                      VkCommandBuffer cmd,
                                      StagingCleanup& outStaging)
{
    if (!create(allocator, device, config)) return false;

    VkBuffer      stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    if (!createStagingBuffer(allocator, pixelData, dataSize, stagingBuffer, stagingAllocation)) {
        spdlog::error("Texture::createFromDataBatched: failed to create staging buffer");
        return false;
    }

    recordUpload(cmd, stagingBuffer, 0, VK_IMAGE_LAYOUT_UNDEFINED);

    outStaging.buffer     = stagingBuffer;
    outStaging.allocation = stagingAllocation;
    outStaging.allocator  = allocator;
    return true;
}

// ── updateDataBatched (records into external cmd buffer, no submit) ──────────

bool Texture::updateDataBatched(const void* pixelData, VkDeviceSize dataSize,
                                  VkCommandBuffer cmd,
                                  StagingCleanup& outStaging)
{
    if (m_image == VK_NULL_HANDLE || m_allocator == nullptr) {
        spdlog::error("Texture::updateDataBatched: texture not created yet");
        return false;
    }

    VkBuffer      stagingBuffer = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;
    if (!createStagingBuffer(m_allocator, pixelData, dataSize, stagingBuffer, stagingAllocation)) {
        spdlog::error("Texture::updateDataBatched: failed to create staging buffer");
        return false;
    }

    recordUpload(cmd, stagingBuffer, 0, m_currentLayout);

    outStaging.buffer     = stagingBuffer;
    outStaging.allocation = stagingAllocation;
    outStaging.allocator  = m_allocator;
    return true;
}

// ── createFromDataRing (zero per-frame VMA alloc) ───────────────────────────

bool Texture::createFromDataRing(VmaAllocator allocator, VkDevice device,
                                  const TextureConfig& config,
                                  const void* pixelData, VkDeviceSize dataSize,
                                  VkCommandBuffer cmd, StagingRing& ring)
{
    auto sub = ring.alloc(pixelData, dataSize);
    if (!sub.valid) {
        spdlog::warn("Texture::createFromDataRing: ring full");
        return false;
    }

    // Allocate staging first.  If the ring is full, the caller falls back to
    // createFromDataBatched(); creating the image before this check would
    // leak/overwrite that first VkImage when the fallback creates it again.
    if (!create(allocator, device, config)) return false;

    recordUpload(cmd, sub.buffer, sub.offset, VK_IMAGE_LAYOUT_UNDEFINED);
    return true;
}

// ── updateDataRing (zero per-frame VMA alloc) ───────────────────────────────

bool Texture::updateDataRing(const void* pixelData, VkDeviceSize dataSize,
                              VkCommandBuffer cmd, StagingRing& ring)
{
    if (m_image == VK_NULL_HANDLE || m_allocator == nullptr) {
        spdlog::error("Texture::updateDataRing: texture not created yet");
        return false;
    }

    auto sub = ring.alloc(pixelData, dataSize);
    if (!sub.valid) {
        spdlog::warn("Texture::updateDataRing: ring full");
        return false;
    }

    recordUpload(cmd, sub.buffer, sub.offset, m_currentLayout);
    return true;
}

// ── StagingCleanup::destroy ─────────────────────────────────────────────────

void Texture::StagingCleanup::destroy()
{
    if (buffer != VK_NULL_HANDLE && allocator != nullptr) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        buffer     = VK_NULL_HANDLE;
        allocation = nullptr;
    }
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Texture::destroy()
{
    if (m_device != VK_NULL_HANDLE)
    {
        if (m_sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(m_device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }

        if (m_imageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, m_imageView, nullptr);
            m_imageView = VK_NULL_HANDLE;
        }
    }

    if (m_image != VK_NULL_HANDLE && m_allocator != nullptr)
    {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
        m_image      = VK_NULL_HANDLE;
        m_allocation = nullptr;
    }

    m_device        = VK_NULL_HANDLE;
    m_allocator     = nullptr;
    m_width         = 0;
    m_height        = 0;
    m_currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// ── transitionLayout ────────────────────────────────────────────────────────

void Texture::transitionLayout(VkCommandBuffer cmd,
                                VkImageLayout oldLayout,
                                VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = oldLayout;
    barrier.newLayout                       = newLayout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = m_image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = m_mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags srcStage;
    VkPipelineStageFlags dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        // Use ALL_COMMANDS so this transition works on both graphics queues
        // (fragment shader reads) and compute-only queues (compute shader reads).
        // FRAGMENT_SHADER_BIT is invalid on dedicated compute queue families.
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        // Re-upload path (updateData): texture was in SHADER_READ_ONLY,
        // transition back to TRANSFER_DST for a new staging copy.
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // ALL_COMMANDS for srcStage so this works on any queue family.
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
             newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
             newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = 0;
        srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    else
    {
        // General fallback — full pipeline stall
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    m_currentLayout = newLayout;
}

// ── descriptorInfo ──────────────────────────────────────────────────────────

VkDescriptorImageInfo Texture::descriptorInfo() const
{
    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = m_imageView;
    info.sampler     = m_sampler;
    return info;
}

} // namespace rt

