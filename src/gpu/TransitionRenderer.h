/*
 * TransitionRenderer — GPU compute-shader transitions between shots.
 *
 * Step 10: Renders GPU-accelerated transitions (dissolve, fade, wipe)
 * between two source images using compute shaders.
 *
 * Each transition type has its own .comp shader:
 *   - transition_dissolve.comp: Cross-dissolve with alpha blend
 *   - transition_fade.comp:    Fade to/from black
 *   - transition_wipe.comp:    Directional wipe (left, right, up, down)
 *
 * Usage:
 *   1. Call init() with Vulkan resources
 *   2. Call render() with source A, source B, progress [0,1], and type
 *   3. Read result from outputImage()
 */

#pragma once

#include "vulkan/Allocator.h"
#include "vulkan/Buffer.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Device.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Texture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace rt {

// ── Transition types ────────────────────────────────────────────────────────

enum class GpuTransitionType : int32_t
{
    Dissolve  = 0,
    FadeBlack = 1,
    WipeLeft  = 2,
    WipeRight = 3,
    WipeUp    = 4,
    WipeDown  = 5,
    PushLeft  = 6,
    PushRight = 7,
    PushUp    = 8,
    PushDown  = 9,

    // ── New transitions ──
    DipColor         = 10,
    FilmDissolve     = 11,
    AdditiveDissolve = 12,
    BarnDoor         = 13,
    ClockWipe        = 14,
    RadialWipe       = 15,
    Iris             = 16,
    DiagonalWipe     = 17,
    CheckerWipe      = 18,
    VenetianBlinds   = 19,
    Inset            = 20,
    Slide            = 21,
    SplitWipe        = 22,
    Swap             = 23,
    ZoomTransition   = 24,
    WhipPan          = 25,
    RandomBlocks     = 26,
    MorphCut         = 27,
    GradientWipe     = 28,
};

// ── Configuration ───────────────────────────────────────────────────────────

struct TransitionConfig
{
    uint32_t outputWidth{1920};
    uint32_t outputHeight{1080};
    VkFormat outputFormat{VK_FORMAT_R8G8B8A8_UNORM};
    float    wipeSoftness{0.02f};  ///< Soft edge width for wipe transitions [0,1]
};

// ── Push constants (must match transition shaders) ──────────────────────────

struct TransitionPushConstants
{
    int32_t width;
    int32_t height;
    float   progress;       ///< Transition progress [0,1]
    int32_t direction;      ///< Wipe direction (0=left, 1=right, 2=up, 3=down)
    float   softness;       ///< Wipe edge softness
    float   param2;         ///< Extra param (block count, blind count, etc.)
    float   param3;         ///< Extra param (blur intensity, etc.)
    float   param4;         ///< Reserved
};
static_assert(sizeof(TransitionPushConstants) == 32);

// ── Per-source transform data (uploaded as SSBO, binding 3) ────────────────

struct TransitionSourceParams
{
    glm::mat4 transformA{1.0f};  ///< Viewport UV → source A texture UV
    glm::mat4 transformB{1.0f};  ///< Viewport UV → source B texture UV
    glm::vec4 cropA{0.0f};       ///< Source A crop (L, R, T, B) as [0,1]
    glm::vec4 cropB{0.0f};       ///< Source B crop (L, R, T, B) as [0,1]
    int32_t   isPackedA{0};      ///< 1 if source A is packed-alpha
    int32_t   isPackedB{0};      ///< 1 if source B is packed-alpha
    int32_t   _pad[2]{};         ///< Alignment padding (std430)
};
static_assert(sizeof(TransitionSourceParams) == 176);

/// Convenience struct for passing source info to render().
struct TransitionSourceInfo
{
    VkDescriptorImageInfo textureInfo{};
    glm::mat4 transform{1.0f};
    glm::vec4 crop{0.0f};
    bool      isPacked{false};
};

// ── Statistics ──────────────────────────────────────────────────────────────

struct TransitionStats
{
    GpuTransitionType type{GpuTransitionType::Dissolve};
    float progress{0.0f};
    float gpuTimeMs{0.0f};
};

// ═════════════════════════════════════════════════════════════════════════════

class TransitionRenderer
{
public:
    TransitionRenderer();
    ~TransitionRenderer();

    TransitionRenderer(const TransitionRenderer&) = delete;
    TransitionRenderer& operator=(const TransitionRenderer&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    bool init(Device& device,
              Allocator& allocator,
              CommandPool& cmdPool,
              VkQueue computeQueue,
              const TransitionConfig& config = {});

    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Rendering ───────────────────────────────────────────────────────

    /// Render a transition between two images.
    /// @param cmd        Command buffer to record into
    /// @param sourceA    First image (outgoing shot)
    /// @param sourceB    Second image (incoming shot)
    /// @param type       Transition type
    /// @param progress   Interpolation [0=all A, 1=all B]
    bool render(VkCommandBuffer cmd,
                const TransitionSourceInfo& sourceA,
                const TransitionSourceInfo& sourceB,
                GpuTransitionType type,
                float progress,
                int32_t directionOverride = -1,
                float extraParam = 0.0f,
                float softnessOverride = -1.0f,
                uint32_t submissionSlot = 0,
                uint32_t transitionSlot = 0);

    /// Synchronous render (creates own command buffer).
    bool renderSync(const TransitionSourceInfo& sourceA,
                    const TransitionSourceInfo& sourceB,
                    GpuTransitionType type,
                    float progress,
                    int32_t directionOverride = -1,
                    float extraParam = 0.0f,
                    float softnessOverride = -1.0f);

    // ── Resize ──────────────────────────────────────────────────────────

    bool resize(uint32_t width, uint32_t height);

    // ── Output access ───────────────────────────────────────────────────

    [[nodiscard]] VkImage       outputImage()     const noexcept;
    [[nodiscard]] VkImageView   outputImageView() const noexcept;
    [[nodiscard]] uint32_t      outputWidth()     const noexcept { return m_config.outputWidth; }
    [[nodiscard]] uint32_t      outputHeight()    const noexcept { return m_config.outputHeight; }

    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo(
        uint32_t submissionSlot = 0, uint32_t transitionSlot = 0) const;

    /// Black placeholder descriptor (for single-clip FadeToBlack/FadeFromBlack).
    [[nodiscard]] VkDescriptorImageInfo blackDescriptorInfo() const;
    /// White placeholder descriptor (for single-clip FadeToWhite/FadeFromWhite).
    [[nodiscard]] VkDescriptorImageInfo whiteDescriptorInfo() const;
    /// Transparent placeholder descriptor (for single-clip wipe/push/dissolve exits).
    [[nodiscard]] VkDescriptorImageInfo transparentDescriptorInfo() const;

    /// Read back output pixels (for testing).
    bool readbackOutput(std::vector<uint8_t>& outPixels,
                        uint32_t submissionSlot = 0,
                        uint32_t transitionSlot = 0);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] const TransitionStats& stats() const noexcept { return m_stats; }

private:
    struct RenderTarget {
        Texture outputTexture;
        Buffer sourceParamsBuffer;
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    };

    static uint64_t renderTargetKey(uint32_t submissionSlot,
                                    uint32_t transitionSlot) noexcept;
    RenderTarget* ensureRenderTarget(uint32_t submissionSlot,
                                     uint32_t transitionSlot);
    [[nodiscard]] const RenderTarget* findRenderTarget(
        uint32_t submissionSlot, uint32_t transitionSlot) const noexcept;
    bool createPipelines();
    bool createDescriptorResources();

    void updateSourceDescriptors(RenderTarget& target,
                                 const VkDescriptorImageInfo& sourceA,
                                 const VkDescriptorImageInfo& sourceB);

    VkPipeline getPipeline(GpuTransitionType type) const;

    // ── Vulkan handles ──────────────────────────────────────────────────

    Device*       m_device{nullptr};
    Allocator*    m_allocator{nullptr};
    CommandPool*  m_cmdPool{nullptr};
    VkQueue       m_queue{VK_NULL_HANDLE};

    TransitionConfig m_config;
    bool             m_initialized{false};

    // Pipelines (one per transition type)
    PipelineManager m_pipelineManager;
    VkPipeline       m_dissolvePipeline{VK_NULL_HANDLE};
    VkPipeline       m_fadePipeline{VK_NULL_HANDLE};
    VkPipeline       m_wipePipeline{VK_NULL_HANDLE};
    VkPipeline       m_pushPipeline{VK_NULL_HANDLE};
    VkPipeline       m_dipColorPipeline{VK_NULL_HANDLE};
    VkPipeline       m_filmDissolvePipeline{VK_NULL_HANDLE};
    VkPipeline       m_additiveDissolvePipeline{VK_NULL_HANDLE};
    VkPipeline       m_barnDoorPipeline{VK_NULL_HANDLE};
    VkPipeline       m_clockWipePipeline{VK_NULL_HANDLE};
    VkPipeline       m_radialWipePipeline{VK_NULL_HANDLE};
    VkPipeline       m_irisPipeline{VK_NULL_HANDLE};
    VkPipeline       m_diagonalWipePipeline{VK_NULL_HANDLE};
    VkPipeline       m_checkerWipePipeline{VK_NULL_HANDLE};
    VkPipeline       m_venetianBlindsPipeline{VK_NULL_HANDLE};
    VkPipeline       m_insetPipeline{VK_NULL_HANDLE};
    VkPipeline       m_slidePipeline{VK_NULL_HANDLE};
    VkPipeline       m_splitPipeline{VK_NULL_HANDLE};
    VkPipeline       m_swapPipeline{VK_NULL_HANDLE};
    VkPipeline       m_zoomPipeline{VK_NULL_HANDLE};
    VkPipeline       m_whipPanPipeline{VK_NULL_HANDLE};
    VkPipeline       m_randomBlocksPipeline{VK_NULL_HANDLE};
    VkPipeline       m_morphCutPipeline{VK_NULL_HANDLE};
    VkPipeline       m_gradientWipePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    static constexpr uint32_t kMaxRenderTargets = 1024;
    std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> m_renderTargets;

    // Placeholder textures (1x1 solid-color)
    Texture m_placeholderTexture;            // opaque black
    Texture m_whitePlaceholderTexture;       // opaque white
    Texture m_transparentPlaceholderTexture; // transparent (alpha=0)

    // GPU timing
    VkQueryPool m_queryPool{VK_NULL_HANDLE};
    float       m_timestampPeriod{0.0f};

    // Stats
    TransitionStats m_stats;
};

} // namespace rt
