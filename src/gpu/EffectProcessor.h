/*
 * EffectProcessor â€” GPU compute-shader effects pipeline.
 *
 * Step 22: Processes effects on clip images using Vulkan compute shaders.
 *
 * Each effect type maps to a compute shader:
 *   - color_correct.comp: Brightness, contrast, saturation, etc.
 *   - blur.comp: Separable Gaussian blur
 *   (Additional shaders dispatched for sharpen, glow, chroma key, transform2D)
 *
 * Usage:
 *   1. Call init() with Vulkan resources
 *   2. Call process() with source image + EffectStack snapshot
 *   3. Result is in outputImage()
 *
 * The processor applies effects in stack order, ping-ponging between
 * two internal storage images (avoiding extra allocations).
 */

#pragma once

#include "vulkan/Allocator.h"
#include "vulkan/Buffer.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Device.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Texture.h"

#include "effects/Effect.h"
#include "effects/EffectStack.h"

#include <cstdint>
#include <vector>

namespace rt {

// â”€â”€ Configuration â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct EffectProcessorConfig
{
    uint32_t width{1920};
    uint32_t height{1080};
    VkFormat format{VK_FORMAT_R8G8B8A8_UNORM};
};

// â”€â”€ Push constants for effect shaders â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct EffectPushConstants
{
    int32_t width;
    int32_t height;
    int32_t paramCount;
    int32_t _pad;
    float   params[28];  // Up to 28 floats per effect (128 bytes total)
};
static_assert(sizeof(EffectPushConstants) == 128);

// â”€â”€ Statistics â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

struct EffectProcessorStats
{
    int   effectsApplied{0};
    float gpuTimeMs{0.0f};
};

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

class EffectProcessor
{
public:
    EffectProcessor();
    ~EffectProcessor();

    EffectProcessor(const EffectProcessor&) = delete;
    EffectProcessor& operator=(const EffectProcessor&) = delete;

    // â”€â”€ Lifecycle â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool init(Device& device,
              Allocator& allocator,
              CommandPool& cmdPool,
              VkQueue computeQueue,
              const EffectProcessorConfig& config = {});

    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // â”€â”€ Processing â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    /// Apply effects from a snapshot (evaluated EffectStack) to a source image.
    /// Records commands into the provided command buffer.
    ///
    /// `effectMasks` (optional) is parallel to `effects`: when entry i has a
    /// non-null imageView, effect i is masked — after its dispatch a mix pass
    /// blends original/effected by the mask's alpha (Premiere Pro behaviour:
    /// masks on an effect limit where the effect applies).
    bool process(VkCommandBuffer cmd,
                 const VkDescriptorImageInfo& sourceImage,
                 const std::vector<EffectStack::EffectSnapshot>& effects,
                 const std::vector<VkDescriptorImageInfo>* effectMasks = nullptr);

    /// Synchronous version â€” creates its own command buffer.
    bool processSync(const VkDescriptorImageInfo& sourceImage,
                     const std::vector<EffectStack::EffectSnapshot>& effects);
    /// Copy current output to caller-owned Texture. Synchronous.
    bool snapshotOutputSync(Texture& dst);


    // â”€â”€ Resize â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    bool resize(uint32_t width, uint32_t height);

    // â”€â”€ Output access â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    [[nodiscard]] VkImage       outputImage()     const noexcept;
    [[nodiscard]] VkImageView   outputImageView() const noexcept;
    [[nodiscard]] uint32_t      outputWidth()     const noexcept { return m_config.width; }
    [[nodiscard]] uint32_t      outputHeight()    const noexcept { return m_config.height; }

    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const;

    /// Read back output pixels (for testing).
    bool readbackOutput(std::vector<uint8_t>& outPixels);

    // â”€â”€ Statistics â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    [[nodiscard]] const EffectProcessorStats& stats() const noexcept { return m_stats; }

public:
    bool uploadLUT3D(const std::vector<float>& lutData, int lutSize);
    void clearLUT3D();

private:
    bool createStorageTextures();
    bool createPipelines();
    bool createDescriptorResources();

    /// (Re)point the static bindings of every source/LUT ring set:
    /// binding 0 (output) → storageTexture[0], binding 1 (input) → placeholder,
    /// and (LUT only) binding 2 → placeholder.  Per-call bindings are set in
    /// process()/dispatchEffect.  Called after allocation and after resize().
    void initDescriptorRingBindings();

    /// Dispatch a single effect â€” writes to pingPong[targetIdx].
    bool dispatchEffect(VkCommandBuffer cmd,
                        EffectType type,
                        const std::vector<float>& params,
                        int sourceIdx, int targetIdx);

    /// Ultra Key single-pass dispatch (combined matte + cleanup + finalize).
    /// Falls back to legacy 3-pass if the combined shader is unavailable.
    /// Returns the final targetIdx written to.
    int dispatchUltraKey(VkCommandBuffer cmd,
                         const std::vector<float>& params,
                         int sourceIdx, int targetIdx);

    /// Helper: dispatch a single compute pass with given pipeline.
    void dispatchPass(VkCommandBuffer cmd, VkPipeline pipeline,
                      VkDescriptorSet ds,
                      const std::vector<float>& params);

    /// Copy source storage image into target — used to bypass effects.
    void copyImage(VkCommandBuffer cmd, int sourceIdx, int targetIdx);

    /// Blend original/effected by an effect mask (Premiere effect masks).
    /// Reads+writes storage[targetIdx] in place; samples `origInfo` and
    /// `maskInfo`. Claims one slot of the mask-mix descriptor ring.
    void dispatchMaskMix(VkCommandBuffer cmd,
                         const VkDescriptorImageInfo& origInfo,
                         const VkDescriptorImageInfo& maskInfo,
                         int targetIdx);

    VkPipeline getPipeline(EffectType type) const;

    // â”€â”€ Vulkan handles â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    Device*       m_device{nullptr};
    Allocator*    m_allocator{nullptr};
    CommandPool*  m_cmdPool{nullptr};
    VkQueue       m_queue{VK_NULL_HANDLE};

    EffectProcessorConfig m_config;
    bool                  m_initialized{false};

    // Ping-pong storage images for multi-effect chains
    Texture m_storageTextures[2];
    int     m_currentOutput{0}; // which storage texture holds the final result

    // Pipelines (one per effect type)
    PipelineManager  m_pipelineManager;
    VkPipeline       m_colorCorrectPipeline{VK_NULL_HANDLE};
    VkPipeline       m_blurPipeline{VK_NULL_HANDLE};
    VkPipeline       m_sharpenPipeline{VK_NULL_HANDLE};
    VkPipeline       m_glowPipeline{VK_NULL_HANDLE};
    VkPipeline       m_chromaKeyPipeline{VK_NULL_HANDLE};       // legacy (unused)
    VkPipeline       m_ultraKeyMattePipeline{VK_NULL_HANDLE};    // Pass 1: matte generation (legacy)
    VkPipeline       m_ultraKeyCleanupPipeline{VK_NULL_HANDLE};  // Pass 2: matte cleanup (legacy)
    VkPipeline       m_ultraKeyFinalizePipeline{VK_NULL_HANDLE}; // Pass 3: spill + CC + output (legacy)
    VkPipeline       m_ultraKeyCombinedPipeline{VK_NULL_HANDLE}; // Combined single-pass (with cleanup)
    VkPipeline       m_ultraKeyLightPipeline{VK_NULL_HANDLE};    // Light single-pass (no cleanup, fast path)
    VkPipeline       m_transform2dPipeline{VK_NULL_HANDLE};
    VkPipeline       m_vignettePipeline{VK_NULL_HANDLE};
    VkPipeline       m_lutPipeline{VK_NULL_HANDLE};
    VkPipeline       m_letterboxPipeline{VK_NULL_HANDLE};
    VkPipeline       m_colorGradingPipeline{VK_NULL_HANDLE};
    VkPipeline       m_otsPipeline{VK_NULL_HANDLE};
    VkPipeline       m_flipPipeline{VK_NULL_HANDLE};
    // Glitch building blocks
    VkPipeline       m_scanlinesPipeline{VK_NULL_HANDLE};
    VkPipeline       m_blockGlitchPipeline{VK_NULL_HANDLE};
    VkPipeline       m_chromaticSplitPipeline{VK_NULL_HANDLE};
    VkPipeline       m_turbulentDisplacePipeline{VK_NULL_HANDLE};
    VkPipeline       m_posterizePipeline{VK_NULL_HANDLE};
    VkPipeline       m_grainPipeline{VK_NULL_HANDLE};
    VkPipeline       m_signalTearPipeline{VK_NULL_HANDLE};
    // Beat-reactive effects
    VkPipeline       m_beatZoomPipeline{VK_NULL_HANDLE};
    VkPipeline       m_beatFlashPipeline{VK_NULL_HANDLE};
    VkPipeline       m_beatShakePipeline{VK_NULL_HANDLE};
    VkPipeline       m_beatChromaPipeline{VK_NULL_HANDLE};
    VkPipeline       m_beatDropPipeline{VK_NULL_HANDLE};
    // Effect-mask mix (original/effected blend by rasterized mask alpha)
    VkPipeline       m_maskMixPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_maskMixPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_lutDescriptorSetLayout{VK_NULL_HANDLE}; // LUT-specific layout (has binding 2)
    VkDescriptorSet       m_descriptorSets[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};

    // Per-call ring of "source" + LUT descriptor sets.
    //
    // The external-source binding (binding 1) is the ONLY descriptor that
    // varies per layer, and it is mutated with vkUpdateDescriptorSets, which
    // takes effect on the CPU immediately rather than in command order.  The
    // render graph records one process() call per effected layer into a
    // SINGLE command buffer, and up to GpuWorkSubmission::kRingSize frames
    // overlap on the GPU.  A single shared source set would therefore be
    // overwritten before earlier dispatches consumed it — every effected
    // layer would sample the LAST layer's source ("blur pulls in other
    // layers / flickers").  Each process() call instead claims the next ring
    // slot, so concurrently-recorded and in-flight dispatches never share a
    // mutable binding.
    //
    // Size = (max compositor layers per frame, 32) × (kRingSize, 3) rounded
    // up for headroom: covers every process() call that can be pending at
    // once.  A slot is reused only after the ring wraps, by which point the
    // frame that last used it has completed (GpuWorkSubmission::beginRecording
    // waited on its fence).
    static constexpr uint32_t kDescriptorRing = 128;
    std::vector<VkDescriptorSet> m_sourceDescriptorSets;  // ring (main layout)
    std::vector<VkDescriptorSet> m_lutDescriptorSets;     // ring (LUT layout)
    uint32_t        m_descriptorRingCursor{0};
    VkDescriptorSet m_curSourceSet{VK_NULL_HANDLE};       // slot chosen by current process()
    VkDescriptorSet m_curLutSet{VK_NULL_HANDLE};

    // Mask-mix descriptor ring (LUT layout: storage + 2 samplers). Claimed
    // PER MASKED-EFFECT DISPATCH (not per process() call) — a chain can
    // contain several masked effects and each dispatch needs its own
    // immutable bindings while recorded/in-flight, same reasoning as the
    // source ring above.
    std::vector<VkDescriptorSet> m_maskMixDescriptorSets;
    uint32_t        m_maskMixRingCursor{0};

    // Placeholder texture + LUT 3D texture
    Texture m_placeholderTexture;
    Texture m_lutTexture3D;           ///< 3D texture holding the LUT cube data

    // Reusable fence for processSync (avoids create/destroy per call)
    VkFence m_syncFence{VK_NULL_HANDLE};

    // GPU timing
    VkQueryPool m_queryPool{VK_NULL_HANDLE};
    float       m_timestampPeriod{0.0f};

    // Stats
    EffectProcessorStats m_stats;
};

} // namespace rt

