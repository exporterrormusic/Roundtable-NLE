/*
 * Texture — VkImage + VkImageView + VkSampler RAII wrapper.
 *
 * Step 2: Manages GPU textures for Spine atlases, backgrounds,
 * video frames, and render targets.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

// Forward declare VMA types
struct VmaAllocator_T;
struct VmaAllocation_T;
struct VmaAllocationInfo;
using VmaAllocator  = VmaAllocator_T*;
using VmaAllocation = VmaAllocation_T*;

namespace rt {

class Device;
class Allocator;
class CommandPool;

/// Texture configuration.
struct TextureConfig
{
    uint32_t     width{1};
    uint32_t     height{1};
    uint32_t     depth{1};   ///< 1 for 2D, >1 for 3D textures (e.g. LUT)
    VkFormat     format{VK_FORMAT_R8G8B8A8_SRGB};
    VkImageUsageFlags usage{VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    uint32_t     mipLevels{1};
    VkFilter     filter{VK_FILTER_LINEAR};
    VkSamplerAddressMode addressMode{VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    bool         generateMipmaps{false};

    /// Queue family indices for VK_SHARING_MODE_CONCURRENT.
    /// When non-empty, the image is created with CONCURRENT sharing
    /// so it can be accessed from multiple queue families without
    /// explicit ownership transfers (e.g. compute + graphics).
    std::vector<uint32_t> concurrentQueueFamilies;
};

/// RAII wrapper for VkImage + VkImageView + VkSampler.
class Texture
{
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;

    /// Create an empty texture (for render targets or later upload).
    bool create(VmaAllocator allocator, VkDevice device, const TextureConfig& config);

    /// Create and upload texture data from CPU memory.
    bool createFromData(VmaAllocator allocator, VkDevice device,
                        const TextureConfig& config,
                        const void* pixelData, VkDeviceSize dataSize,
                        CommandPool& cmdPool, VkQueue transferQueue);

    /// Upload new pixel data to an existing texture (same dimensions required).
    /// More efficient than destroy+createFromData when only pixels change:
    /// reuses VkImage/VkImageView/VkSampler, only creates a staging buffer.
    bool updateData(const void* pixelData, VkDeviceSize dataSize,
                    CommandPool& cmdPool, VkQueue transferQueue);

    // ── Batched upload (no vkQueueWaitIdle) ─────────────────────────────

    /// Staging buffer handle for deferred cleanup after command buffer completes.
    struct StagingCleanup {
        VkBuffer      buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{nullptr};
        VmaAllocator  allocator{nullptr};
        void destroy();
    };

    /// Record texture creation + upload commands into an EXTERNAL command buffer.
    /// Does NOT submit or wait — caller batches multiple uploads and submits once.
    /// Returns staging buffer info in `outStaging` for deferred cleanup.
    bool createFromDataBatched(VmaAllocator allocator, VkDevice device,
                               const TextureConfig& config,
                               const void* pixelData, VkDeviceSize dataSize,
                               VkCommandBuffer cmd,
                               StagingCleanup& outStaging);

    /// Record re-upload commands into an EXTERNAL command buffer.
    /// Does NOT submit or wait — caller batches multiple uploads and submits once.
    /// Returns staging buffer info in `outStaging` for deferred cleanup.
    bool updateDataBatched(const void* pixelData, VkDeviceSize dataSize,
                           VkCommandBuffer cmd,
                           StagingCleanup& outStaging);

    // ── Ring-buffer upload (zero per-frame VMA alloc) ───────────────────

    /// Like createFromDataBatched but sub-allocates from a StagingRing
    /// instead of creating a per-frame VMA staging buffer.
    /// No StagingCleanup needed — ring resets after fence wait.
    bool createFromDataRing(VmaAllocator allocator, VkDevice device,
                            const TextureConfig& config,
                            const void* pixelData, VkDeviceSize dataSize,
                            VkCommandBuffer cmd, class StagingRing& ring);

    /// Like updateDataBatched but sub-allocates from a StagingRing.
    bool updateDataRing(const void* pixelData, VkDeviceSize dataSize,
                        VkCommandBuffer cmd, class StagingRing& ring);

    /// Record an upload from caller-owned staging memory whose rows are
    /// `rowLengthTexels` texels apart (0 = tightly packed) — e.g. a decoder's
    /// padded planes copied verbatim, with no repack. Same transitions as
    /// updateDataBatched; the caller keeps the buffer alive until the GPU is
    /// done with it.
    void recordUploadFromBuffer(VkCommandBuffer cmd, VkBuffer buffer,
                                VkDeviceSize offset, uint32_t rowLengthTexels);

    /// Destroy all resources.
    void destroy();

    /// Transition image layout (e.g. UNDEFINED → SHADER_READ_ONLY).
    void transitionLayout(VkCommandBuffer cmd,
                          VkImageLayout oldLayout,
                          VkImageLayout newLayout);

    // ── Accessors ───────────────────────────────────────────────────────

    [[nodiscard]] VkImage       image()     const noexcept { return m_image; }
    [[nodiscard]] VkImageView   imageView() const noexcept { return m_imageView; }
    [[nodiscard]] VkSampler     sampler()   const noexcept { return m_sampler; }
    [[nodiscard]] uint32_t      width()     const noexcept { return m_width; }
    [[nodiscard]] uint32_t      height()    const noexcept { return m_height; }
    [[nodiscard]] uint32_t      depth()     const noexcept { return m_depth; }
    [[nodiscard]] VkFormat      format()    const noexcept { return m_format; }
    [[nodiscard]] VkImageLayout layout()    const noexcept { return m_currentLayout; }
    [[nodiscard]] uint32_t      mipLevels() const noexcept { return m_mipLevels; }

    /// Get descriptor image info for binding to shaders.
    [[nodiscard]] VkDescriptorImageInfo descriptorInfo() const;

private:
    /// Record the buffer-to-image copy + layout transitions into a command buffer.
    /// Transitions: prevLayout → TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL.
    void recordUpload(VkCommandBuffer cmd, VkBuffer stagingBuffer,
                      VkDeviceSize stagingOffset, VkImageLayout prevLayout,
                      uint32_t rowLengthTexels = 0);

    VkImage       m_image{VK_NULL_HANDLE};
    VkImageView   m_imageView{VK_NULL_HANDLE};
    VkSampler     m_sampler{VK_NULL_HANDLE};
    VmaAllocation m_allocation{nullptr};
    VmaAllocator  m_allocator{nullptr};
    VkDevice      m_device{VK_NULL_HANDLE};

    uint32_t      m_width{0};
    uint32_t      m_height{0};
    uint32_t      m_depth{1};
    VkFormat      m_format{VK_FORMAT_UNDEFINED};
    VkImageLayout m_currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t      m_mipLevels{1};
};

} // namespace rt
