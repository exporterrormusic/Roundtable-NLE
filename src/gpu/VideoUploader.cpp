/*
 * VideoUploader.cpp — CPU staging buffer → Vulkan texture upload
 *
 * For now this implements the CPU staging path. The zero-copy CUDA path
 * will be added when CudaVulkanInterop is fully wired up.
 */

#include "VideoUploader.h"

#include <cache/FrameCache.h>

#include <spdlog/spdlog.h>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <cstring>

#ifdef ROUNDTABLE_HAS_FFMPEG

// We need Vulkan types for the actual implementation
#include <volk.h>
#include <vk_mem_alloc.h>

#include "GpuContext.h"
#include "GpuScheduler.h"

namespace rt {

namespace {

// Resolve VkQueue back to a GpuQueueKind for routing through the
// scheduler.  See identical helper in GpuWorkSubmission.cpp /
// CommandPool.cpp; could centralize once enough callers exist.
GpuQueueKind kindForQueue(VkQueue queue) noexcept
{
    if (queue == VK_NULL_HANDLE) return GpuQueueKind::Graphics;
    auto& gpu = GpuContext::get();
    if (!gpu.scheduler().isInitialized()) return GpuQueueKind::Graphics;
    if (queue == gpu.computeQueue())           return GpuQueueKind::Compute;
    if (queue == gpu.device().transferQueue()) return GpuQueueKind::Transfer;
    return GpuQueueKind::Graphics;
}

} // namespace

struct VideoUploader::Impl
{
    // Texture pool: reusable VkImage + VkImageView for frame display
    struct TextureSlot
    {
        VkImage        image{VK_NULL_HANDLE};
        VkImageView    imageView{VK_NULL_HANDLE};
        VkSampler      sampler{VK_NULL_HANDLE};
        VmaAllocation  allocation{VK_NULL_HANDLE};
        uint32_t       width{0};
        uint32_t       height{0};
        bool           inUse{false};
    };

    // Staging buffer pool
    struct StagingBuffer
    {
        VkBuffer       buffer{VK_NULL_HANDLE};
        VmaAllocation  allocation{VK_NULL_HANDLE};
        void*          mapped{nullptr};
        size_t         size{0};
        bool           inUse{false};
    };

    VmaAllocator allocator{VK_NULL_HANDLE};
    VkDevice     device{VK_NULL_HANDLE};
    VkQueue      queue{VK_NULL_HANDLE};
    VkCommandPool commandPool{VK_NULL_HANDLE};

    std::vector<TextureSlot>  textureSlots;
    std::vector<StagingBuffer> stagingBuffers;

    // Currently uploaded frames: (mediaId, frameNumber) → GpuFrame + slot index
    struct FrameKey
    {
        uint64_t mediaId;
        int64_t  frameNumber;
        bool operator==(const FrameKey& o) const { return mediaId == o.mediaId && frameNumber == o.frameNumber; }
    };
    struct FrameKeyHash
    {
        size_t operator()(const FrameKey& k) const
        {
            size_t h = std::hash<uint64_t>{}(k.mediaId);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct UploadedEntry {
        std::shared_ptr<GpuFrame> gpuFrame;
        size_t slotIndex{0};
    };
    std::unordered_map<FrameKey, UploadedEntry, FrameKeyHash> uploaded;
    mutable std::mutex mutex;

    // ── Helpers ─────────────────────────────────────────────────────────

    size_t acquireTextureSlot(uint32_t w, uint32_t h)
    {
        // Try to find a free slot with matching dimensions
        for (size_t i = 0; i < textureSlots.size(); ++i) {
            auto& s = textureSlots[i];
            if (!s.inUse && s.width == w && s.height == h && s.image != VK_NULL_HANDLE) {
                s.inUse = true;
                return i;
            }
        }
        // Try to find any free slot (will need resizing)
        for (size_t i = 0; i < textureSlots.size(); ++i) {
            auto& s = textureSlots[i];
            if (!s.inUse) {
                destroySlot(s);
                if (createSlot(s, w, h)) {
                    s.inUse = true;
                    return i;
                }
            }
        }
        // Grow pool
        textureSlots.push_back({});
        auto& s = textureSlots.back();
        if (createSlot(s, w, h)) {
            s.inUse = true;
            return textureSlots.size() - 1;
        }
        return SIZE_MAX;
    }

    bool createSlot(TextureSlot& slot, uint32_t w, uint32_t h)
    {
        slot.width  = w;
        slot.height = h;

        // Create VkImage  (B8G8R8A8 because CachedFrame pixels are BGRA)
        VkImageCreateInfo imgInfo{};
        imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType     = VK_IMAGE_TYPE_2D;
        imgInfo.format        = VK_FORMAT_B8G8R8A8_UNORM;
        imgInfo.extent        = { w, h, 1 };
        imgInfo.mipLevels     = 1;
        imgInfo.arrayLayers   = 1;
        imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        if (vmaCreateImage(allocator, &imgInfo, &allocInfo,
                           &slot.image, &slot.allocation, nullptr) != VK_SUCCESS)
        {
            spdlog::error("VideoUploader: failed to create {}x{} VkImage", w, h);
            return false;
        }

        // ImageView
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = slot.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &slot.imageView) != VK_SUCCESS) {
            spdlog::error("VideoUploader: failed to create image view");
            destroySlot(slot);
            return false;
        }

        // Sampler (linear, clamp-to-edge)
        VkSamplerCreateInfo sampInfo{};
        sampInfo.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampInfo.magFilter     = VK_FILTER_LINEAR;
        sampInfo.minFilter     = VK_FILTER_LINEAR;
        sampInfo.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampInfo.maxAnisotropy = 1.0f;
        sampInfo.borderColor   = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        sampInfo.unnormalizedCoordinates = VK_FALSE;
        sampInfo.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampInfo.maxLod        = 0.0f;

        if (vkCreateSampler(device, &sampInfo, nullptr, &slot.sampler) != VK_SUCCESS) {
            spdlog::error("VideoUploader: failed to create sampler");
            destroySlot(slot);
            return false;
        }

        return true;
    }

    void destroySlot(TextureSlot& slot)
    {
        if (slot.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, slot.sampler, nullptr);
            slot.sampler = VK_NULL_HANDLE;
        }
        if (slot.imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, slot.imageView, nullptr);
            slot.imageView = VK_NULL_HANDLE;
        }
        if (slot.image != VK_NULL_HANDLE && allocator) {
            vmaDestroyImage(allocator, slot.image, slot.allocation);
            slot.image      = VK_NULL_HANDLE;
            slot.allocation = VK_NULL_HANDLE;
        }
        slot.width = slot.height = 0;
        slot.inUse = false;
    }

    VkCommandBuffer beginSingleTime()
    {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd;
        vkAllocateCommandBuffers(device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        return cmd;
    }

    void endSingleTime(VkCommandBuffer cmd)
    {
        vkEndCommandBuffer(cmd);

        // Use a per-submit fence instead of vkQueueWaitIdle so we only
        // block until THIS command buffer completes, not the entire queue.
        // This allows overlapping GPU work from other submissions.
        VkFenceCreateInfo fenceCI{};
        fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(device, &fenceCI, nullptr, &fence);

        // P1.3: route through GpuScheduler.
        GpuSubmission sub{};
        sub.cmd             = cmd;
        sub.queue           = kindForQueue(queue);
        sub.completionFence = fence;
        sub.tag             = "VideoUploader::endSingleTime";
        GpuContext::get().scheduler().submit(sub);

        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &cmd);
    }

    bool uploadToSlot(TextureSlot& slot, const void* pixelData, VkDeviceSize dataSize)
    {
        // Create staging buffer
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size  = dataSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo stagingAllocInfo{};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                 VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBuffer stagingBuffer;
        VmaAllocation stagingAlloc;
        VmaAllocationInfo stagingResult{};

        if (vmaCreateBuffer(allocator, &bufInfo, &stagingAllocInfo,
                            &stagingBuffer, &stagingAlloc, &stagingResult) != VK_SUCCESS)
            return false;

        std::memcpy(stagingResult.pMappedData, pixelData, static_cast<size_t>(dataSize));

        // Record commands
        VkCommandBuffer cmd = beginSingleTime();

        // Transition UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = slot.image;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy buffer → image
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent      = { slot.width, slot.height, 1 };

        vkCmdCopyBufferToImage(cmd, stagingBuffer, slot.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition TRANSFER_DST → SHADER_READ_ONLY
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        endSingleTime(cmd);

        vmaDestroyBuffer(allocator, stagingBuffer, stagingAlloc);

        return true;
    }

    void destroyAll()
    {
        for (auto& slot : textureSlots) destroySlot(slot);
        textureSlots.clear();

        for (auto& sb : stagingBuffers) {
            if (sb.buffer != VK_NULL_HANDLE && allocator)
                vmaDestroyBuffer(allocator, sb.buffer, sb.allocation);
        }
        stagingBuffers.clear();
    }
};

VideoUploader::VideoUploader()
    : m_impl(std::make_unique<Impl>())
{
}

VideoUploader::~VideoUploader()
{
    shutdown();
}

bool VideoUploader::init(void* vkDevice, void* vkPhysicalDevice,
                          void* vkQueue, void* vkCommandPool,
                          const VideoUploaderConfig& config)
{
    m_vkDevice         = vkDevice;
    m_vkPhysicalDevice = vkPhysicalDevice;
    m_vkQueue          = vkQueue;
    m_vkCommandPool    = vkCommandPool;
    m_config           = config;

    m_impl->device      = static_cast<VkDevice>(vkDevice);
    m_impl->queue       = static_cast<VkQueue>(vkQueue);
    m_impl->commandPool = static_cast<VkCommandPool>(vkCommandPool);

    // Create a VMA allocator for staging buffers and texture images
    VmaVulkanFunctions vulkanFuncs{};
    vulkanFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFuncs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocCI{};
    allocCI.vulkanApiVersion = VK_API_VERSION_1_2;
    allocCI.physicalDevice   = static_cast<VkPhysicalDevice>(vkPhysicalDevice);
    allocCI.device           = static_cast<VkDevice>(vkDevice);
    allocCI.instance         = VK_NULL_HANDLE; // Not needed with VMA 3.x when functions provided
    allocCI.pVulkanFunctions = &vulkanFuncs;

    if (vmaCreateAllocator(&allocCI, &m_impl->allocator) != VK_SUCCESS) {
        spdlog::error("VideoUploader: Failed to create VMA allocator");
        return false;
    }

    // Pre-allocate texture slots
    m_impl->textureSlots.resize(config.maxTextureSlots);

    m_initialized = true;
    spdlog::info("VideoUploader: initialized (staging={}, textures={})",
                 config.maxStagingBuffers, config.maxTextureSlots);
    return true;
}

void VideoUploader::shutdown()
{
    if (!m_initialized) return;

    releaseAll();

    if (m_impl->device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(m_impl->device);

    m_impl->destroyAll();

    if (m_impl->allocator) {
        vmaDestroyAllocator(m_impl->allocator);
        m_impl->allocator = VK_NULL_HANDLE;
    }

    m_initialized = false;
    spdlog::info("VideoUploader: shutdown");
}

std::shared_ptr<GpuFrame> VideoUploader::upload(const CachedFrame& frame)
{
    if (!m_initialized || !m_impl->allocator) return nullptr;

    std::lock_guard lock(m_impl->mutex);

    // Check if already uploaded
    Impl::FrameKey key{frame.mediaId, frame.frameNumber};
    auto it = m_impl->uploaded.find(key);
    if (it != m_impl->uploaded.end()) {
        return it->second.gpuFrame;
    }

    // Evict oldest entry if at capacity
    if (m_impl->uploaded.size() >= m_config.maxTextureSlots) {
        // Simple FIFO eviction: erase oldest (first in map)
        auto oldest = m_impl->uploaded.begin();
        if (oldest != m_impl->uploaded.end()) {
            size_t si = oldest->second.slotIndex;
            if (si < m_impl->textureSlots.size())
                m_impl->textureSlots[si].inUse = false;
            oldest->second.gpuFrame->valid = false;
            m_impl->uploaded.erase(oldest);
        }
    }

    // Acquire a texture slot
    size_t slotIdx = m_impl->acquireTextureSlot(frame.width, frame.height);
    if (slotIdx == SIZE_MAX) {
        spdlog::warn("VideoUploader: no texture slot available for {}x{}", frame.width, frame.height);
        return nullptr;
    }

    auto& slot = m_impl->textureSlots[slotIdx];

    // Upload pixel data to GPU
    VkDeviceSize dataSize = static_cast<VkDeviceSize>(frame.pixels.size());
    if (!m_impl->uploadToSlot(slot, frame.pixels.data(), dataSize)) {
        spdlog::error("VideoUploader: upload failed for frame {}:{}", frame.mediaId, frame.frameNumber);
        slot.inUse = false;
        return nullptr;
    }

    // Build GpuFrame result
    auto gpuFrame = std::make_shared<GpuFrame>();
    gpuFrame->vkImage         = slot.image;
    gpuFrame->vkImageView     = slot.imageView;
    gpuFrame->vkSampler       = slot.sampler;
    gpuFrame->width           = frame.width;
    gpuFrame->height          = frame.height;
    gpuFrame->mediaId         = frame.mediaId;
    gpuFrame->frameNumber     = frame.frameNumber;
    gpuFrame->valid           = true;
    gpuFrame->isZeroCopy      = false;

    m_impl->uploaded[key] = { gpuFrame, slotIdx };

    return gpuFrame;
}

std::shared_ptr<GpuFrame> VideoUploader::findUploaded(
    uint64_t mediaId, int64_t frameNumber) const
{
    std::lock_guard lock(m_impl->mutex);
    Impl::FrameKey key{mediaId, frameNumber};
    auto it = m_impl->uploaded.find(key);
    return it != m_impl->uploaded.end() ? it->second.gpuFrame : nullptr;
}

void VideoUploader::release(std::shared_ptr<GpuFrame> frame)
{
    if (!frame) return;

    std::lock_guard lock(m_impl->mutex);

    Impl::FrameKey key{frame->mediaId, frame->frameNumber};
    auto it = m_impl->uploaded.find(key);
    if (it != m_impl->uploaded.end()) {
        size_t si = it->second.slotIndex;
        if (si < m_impl->textureSlots.size())
            m_impl->textureSlots[si].inUse = false;
        m_impl->uploaded.erase(it);
    }

    frame->valid = false;
}

void VideoUploader::releaseAll()
{
    std::lock_guard lock(m_impl->mutex);

    for (auto& [key, entry] : m_impl->uploaded) {
        entry.gpuFrame->valid = false;
        if (entry.slotIndex < m_impl->textureSlots.size())
            m_impl->textureSlots[entry.slotIndex].inUse = false;
    }
    m_impl->uploaded.clear();
}

void VideoUploader::evictMedia(uint64_t mediaId)
{
    std::lock_guard lock(m_impl->mutex);

    size_t evicted = 0;
    for (auto it = m_impl->uploaded.begin(); it != m_impl->uploaded.end(); ) {
        if (it->first.mediaId == mediaId) {
            it->second.gpuFrame->valid = false;
            if (it->second.slotIndex < m_impl->textureSlots.size())
                m_impl->textureSlots[it->second.slotIndex].inUse = false;
            it = m_impl->uploaded.erase(it);
            ++evicted;
        } else {
            ++it;
        }
    }
    if (evicted > 0) {
        spdlog::warn("[LIVE-RELOAD] VideoUploader: evicted {} uploaded "
                     "GPU texture(s) for mediaId={} — next upload will "
                     "re-stage from CPU pixels", evicted, mediaId);
    }
}

size_t VideoUploader::uploadedCount() const
{
    std::lock_guard lock(m_impl->mutex);
    return m_impl->uploaded.size();
}

bool VideoUploader::createStagingBuffers()
{
    // Staging buffers are created on-demand per upload (simpler, and VMA
    // handles the host-visible allocation pooling internally).
    return true;
}

bool VideoUploader::createTextureSlots()
{
    // Slots are pre-allocated in init() and lazily created in acquireTextureSlot().
    return true;
}

} // namespace rt

#else // !ROUNDTABLE_HAS_FFMPEG

namespace rt {

struct VideoUploader::Impl {};

VideoUploader::VideoUploader() : m_impl(std::make_unique<Impl>()) {}
VideoUploader::~VideoUploader() = default;

bool VideoUploader::init(void*, void*, void*, void*, const VideoUploaderConfig&)
{
    spdlog::info("VideoUploader: FFmpeg not available — video upload disabled");
    return false;
}
void VideoUploader::shutdown() {}
std::shared_ptr<GpuFrame> VideoUploader::upload(const CachedFrame&) { return nullptr; }
std::shared_ptr<GpuFrame> VideoUploader::findUploaded(uint64_t, int64_t) const { return nullptr; }
void VideoUploader::release(std::shared_ptr<GpuFrame>) {}
void VideoUploader::releaseAll() {}
size_t VideoUploader::uploadedCount() const { return 0; }
bool VideoUploader::createStagingBuffers() { return false; }
bool VideoUploader::createTextureSlots() { return false; }

} // namespace rt

#endif // ROUNDTABLE_HAS_FFMPEG

