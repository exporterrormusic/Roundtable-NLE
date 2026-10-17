/*
 * RgbToNv12Converter — converts an 8-bit RGB(A) storage image to NV12
 * (BT.709, limited range) in a buffer, on the GPU, for the encoder.
 *
 * Export used to read the composite back as BGRA (4 bytes/pixel) and convert
 * it with swscale on the CPU. NV12 is 1.5 bytes/pixel, and a CUDA-shared
 * buffer can feed NVENC without leaving the GPU.
 *
 * Layout written (see shaders/rgb_to_nv12.comp): a Y plane of height rows,
 * then a UV plane of height/2 rows, both `stride(width)` bytes per row.
 */
#pragma once

#include <array>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace rt {

class RgbToNv12Converter
{
public:
    RgbToNv12Converter() = default;
    ~RgbToNv12Converter();
    RgbToNv12Converter(const RgbToNv12Converter&) = delete;
    RgbToNv12Converter& operator=(const RgbToNv12Converter&) = delete;

    bool init(VkDevice device);
    void shutdown();
    [[nodiscard]] bool isInitialized() const noexcept { return m_pipeline != VK_NULL_HANDLE; }

    /// Bytes per row of each plane: the width rounded up to 4.
    [[nodiscard]] static constexpr uint32_t stride(uint32_t width) noexcept
    {
        return (width + 3u) & ~3u;
    }
    /// Bytes the NV12 output of a width x height image takes.
    [[nodiscard]] static constexpr VkDeviceSize byteSize(uint32_t width, uint32_t height) noexcept
    {
        return static_cast<VkDeviceSize>(stride(width)) * height * 3u / 2u;
    }

    /// Record the conversion. `src` is an RGBA8 storage image in
    /// VK_IMAGE_LAYOUT_GENERAL whose writes were recorded earlier in `cmd`;
    /// `swizzledRB` says its channels hold B,G,R,A (the compositor's BGRA
    /// output). `dst` must hold byteSize() bytes from `dstOffset` and allow
    /// storage writes. The output is made visible to host reads and to
    /// transfers. Height must be even. At most kSetRing conversions may be
    /// in flight on the GPU at once.
    bool record(VkCommandBuffer cmd, VkImageView src,
                uint32_t width, uint32_t height, bool swizzledRB,
                VkBuffer dst, VkDeviceSize dstOffset);

    static constexpr uint32_t kSetRing = 8;

private:
    VkDevice              m_device{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_setLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_pool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, kSetRing> m_sets{};
    uint32_t              m_nextSet{0};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkShaderModule        m_shader{VK_NULL_HANDLE};
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
};

} // namespace rt
