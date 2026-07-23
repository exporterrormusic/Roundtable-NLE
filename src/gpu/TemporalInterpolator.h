/*
 * TemporalInterpolator - GPU synthesis of retimed video frames.
 *
 * Frame blending and optical-flow interpolation run in source-pixel space,
 * before the clip effect chain and before the clip's viewport transform.
 * Descriptor sets are partitioned by submission-ring slot so recording frame
 * N cannot rewrite descriptors still referenced by an in-flight frame.
 */

#pragma once

#include "vulkan/Device.h"
#include "vulkan/Pipeline.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace rt {

inline constexpr uint32_t kTemporalMaxLayers = 32;
inline constexpr uint32_t kTemporalSubmissionSlots = 3;

struct TemporalInterpolationPushConstants
{
    int32_t width{0};
    int32_t height{0};
    float   phase{0.0f};
    int32_t mode{0};       // 1 = frame blend, 2 = optical flow
    int32_t packedA{0};
    int32_t packedB{0};
    int32_t _pad[2]{};
};
static_assert(sizeof(TemporalInterpolationPushConstants) == 32);

class TemporalInterpolator
{
public:
    TemporalInterpolator() = default;
    ~TemporalInterpolator();

    TemporalInterpolator(const TemporalInterpolator&) = delete;
    TemporalInterpolator& operator=(const TemporalInterpolator&) = delete;

    bool init(Device& device);
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    /// Record one interpolation dispatch.  `mode` uses TimeInterpolation's
    /// persisted numeric values (FrameBlending=1, OpticalFlow=2).
    bool render(VkCommandBuffer cmd,
                VkImageView outputView,
                const VkDescriptorImageInfo& sourceA,
                const VkDescriptorImageInfo& sourceB,
                uint32_t width, uint32_t height,
                float phase, int32_t mode,
                bool packedA, bool packedB,
                uint32_t submissionSlot,
                uint32_t layerIndex);

private:
    bool createDescriptorResources();
    bool createPipeline();

    Device* m_device{nullptr};
    PipelineManager m_pipelineManager;
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    std::array<VkDescriptorSet,
               kTemporalMaxLayers * kTemporalSubmissionSlots> m_descriptorSets{};
    bool m_initialized{false};
};

} // namespace rt
