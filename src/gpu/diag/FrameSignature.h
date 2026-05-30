/*
 * FrameSignature — GPU-side 256-bit signature of a composite output texture,
 * for the A/B render-path verification harness (#18).
 *
 * Runs frame_signature.comp on an rgba8 texture and reads back ONLY 32 bytes
 * (not the whole frame), so it does not pay the full GPU→CPU readback cost.
 * Two frames are bit-identical iff their signatures match. Run the SAME
 * FrameSignature on the preview path's output texture and the export path's
 * output texture, then compare the signatures.
 *
 * One-shot: each compute() records its own command buffer and submits+waits on
 * the graphics queue (under GpuContext's queue mutex). Cheap (32-byte readback)
 * but synchronous — intended for offline verification, not the playback loop.
 *
 * STATUS: compiles; NOT yet wired into the render paths and NOT runtime-verified.
 */

#pragma once

#include <volk.h>

#include <array>
#include <cstdint>

namespace rt {

class GpuContext;

class FrameSignature
{
public:
    FrameSignature() = default;
    ~FrameSignature();

    FrameSignature(const FrameSignature&) = delete;
    FrameSignature& operator=(const FrameSignature&) = delete;

    /// Create pipeline + resources. Returns false on failure (logs the reason).
    bool init(GpuContext& ctx);
    void destroy();

    [[nodiscard]] bool isReady() const noexcept { return m_ready; }

    /// Compute the signature of an rgba8 texture. `view`/`sampler` are the
    /// composite output; `layout` is its current layout (GENERAL or
    /// SHADER_READ_ONLY_OPTIMAL — both are samplable). The texture's writes
    /// must already be complete (caller composited synchronously first).
    /// Returns false on failure; on success `outSig` holds the 256-bit value.
    bool compute(VkImageView view, VkSampler sampler,
                 uint32_t width, uint32_t height,
                 std::array<uint32_t, 8>& outSig,
                 VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL);

private:
    GpuContext* m_ctx{nullptr};
    VkDevice    m_device{VK_NULL_HANDLE};

    VkDescriptorSetLayout m_dsl{VK_NULL_HANDLE};
    VkDescriptorPool      m_pool{VK_NULL_HANDLE};
    VkDescriptorSet       m_ds{VK_NULL_HANDLE};
    VkPipelineLayout      m_layout{VK_NULL_HANDLE};
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkShaderModule        m_shader{VK_NULL_HANDLE};

    // 32-byte host-visible storage buffer (the SSBO sig[8]); persistently mapped.
    VkBuffer      m_sigBuf{VK_NULL_HANDLE};
    void*         m_sigAlloc{nullptr};   // VmaAllocation (opaque; avoids vma in header)
    uint32_t*     m_sigMapped{nullptr};

    VkCommandPool m_cmdPool{VK_NULL_HANDLE};
    VkCommandBuffer m_cmd{VK_NULL_HANDLE};
    VkFence       m_fence{VK_NULL_HANDLE};

    bool m_ready{false};
};

} // namespace rt
