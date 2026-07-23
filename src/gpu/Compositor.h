/*
 * Compositor — GPU compute-shader multi-layer compositing.
 *
 * Step 10: Composites up to 32 layers (Spine characters, video frames,
 * backgrounds) into a single output image using a Vulkan compute shader.
 *
 * Each layer has:
 *   - Source texture (from SpineRenderer framebuffer, video decoder, etc.)
 *   - Transform matrix (position, scale, rotation in UV space)
 *   - Opacity [0,1]
 *   - Blend mode (Normal, Multiply, Screen, Add)
 *   - Enabled flag
 *
 * The compositor dispatches composite.comp which iterates layers bottom-to-top,
 * transforming UVs through each layer's matrix, sampling textures, and blending.
 *
 * Output is a storage image (VK_IMAGE_USAGE_STORAGE_BIT) that can be read back
 * for export or displayed via a fullscreen quad.
 */

#pragma once

#include "ICompositor.h"
#include "vulkan/Allocator.h"
#include "vulkan/Buffer.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Device.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Texture.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// ── Constants ───────────────────────────────────────────────────────────────

inline constexpr uint32_t kMaxCompositorLayers = 32;
inline constexpr uint32_t kCompositeWorkgroupSize = 16;

// ── Blend modes (must match composite.comp) ─────────────────────────────────

enum class BlendMode : int32_t
{
    Normal       = 0,
    Multiply     = 1,
    Screen       = 2,
    Add          = 3,
    Overlay      = 4,
    SoftLight    = 5,
    HardLight    = 6,
    Difference   = 7,
    ColorDodge   = 8,
    ColorBurn    = 9,
    Exclusion    = 10,
    Darken       = 11,
    Lighten      = 12,
    LinearDodge  = 13,  // same as Add in Photoshop, but with alpha
    LinearBurn   = 14,
    VividLight   = 15,
    PinLight     = 16,
    HardMix      = 17,
    Hue          = 18,
    Saturation   = 19,
    Color        = 20,
    Luminosity   = 21
};

// ── Configuration ───────────────────────────────────────────────────────────

struct CompositorConfig
{
    uint32_t outputWidth{1920};
    uint32_t outputHeight{1080};
    VkFormat outputFormat{VK_FORMAT_R8G8B8A8_UNORM};
};

/// Push constants for the composite compute shader (must match composite.comp).
struct CompositePushConstants
{
    int32_t width;
    int32_t height;
    int32_t hqSample;       // 1 = Catmull-Rom bicubic sampling (export), 0 = bilinear (preview)
    int32_t preserveAlpha;  // 1 = keep straight RGBA (alpha export), 0 = flatten over black
    int32_t outputSwizzleRB; // 1 = BGRA bytes for Qt/readback, 0 = native RGBA intermediate
    int32_t _pad[3];
};
static_assert(sizeof(CompositePushConstants) == 32);

// ── Layer descriptor ────────────────────────────────────────────────────────

/// Describes a single compositing layer.
struct CompositorLayer
{
    VkDescriptorImageInfo textureInfo{};  ///< Sampler + imageView + layout
    glm::mat4  transform{1.0f};          ///< UV-space transform matrix
    glm::mat4  motionTransformStart{1.0f}; ///< UV transform at exposure start
    glm::mat4  motionTransformEnd{1.0f};   ///< UV transform at exposure end
    int32_t    motionSampleCount{1};       ///< 1=off/current only, max 8
    float      opacity{1.0f};            ///< Layer opacity [0,1]
    BlendMode  blendMode{BlendMode::Normal};
    bool       enabled{true};

    /// When true, the texture contains packed-alpha layout (top half = RGB,
    /// bottom half = alpha as greyscale).  The compositor shader splits the
    /// UV sampling to extract proper RGBA without CPU pixel manipulation.
    bool       isPacked{false};

    /// When true, the layer contains premultiplied-alpha data (e.g. from
    /// SpineRenderer's offscreen FBO).  The compositor un-premultiplies
    /// before blending so that blendNormal (straight-alpha) works correctly.
    bool       isPMA{false};

    /// When true, swap R↔B after sampling.  Needed for nested sequence
    /// composite textures whose bytes are stored in BGRA order (due to the
    /// composite shader's output .bgra swizzle for Qt readback).
    bool       needsSwapRB{false};

    /// Crop rect as fractions 0–1 of the layer (left, right, top, bottom).
    /// e.g. cropLeft=0.1 means remove 10% from the left edge.
    float cropLeft{0.0f};
    float cropRight{0.0f};
    float cropTop{0.0f};
    float cropBottom{0.0f};
    float contentLeft{0.0f};
    float contentTop{0.0f};
    float contentRight{1.0f};
    float contentBottom{1.0f};

    /// When true, a mask texture is provided for this layer.
    bool       hasMask{false};
    VkDescriptorImageInfo maskTextureInfo{};  ///< Sampler + imageView + layout for mask

    // ── A/B pair metadata ─────────────────────────────────────────────
    /// Index of the A/B pair this layer belongs to (for pair-aware blending).
    uint32_t   pairIndex{0};
    /// True if this layer is the background (Track A) of its pair.
    bool       isBackground{false};
};

// ── A/B Track Pair ──────────────────────────────────────────────────────────

/// Describes a transition between two tracks in an A/B pair.
/// Setting type to a sentinel value means "no transition" (cut).
struct PairTransitionInfo
{
    int32_t    type{0};          ///< GpuTransitionType or -1 for cut
    float      progress{0.0f};   ///< Transition progress [0,1]
    float      softness{0.02f};  ///< Wipe softness for applicable types
    int32_t    direction{-1};    ///< Direction override
    float      extraParam{0.0f}; ///< Extra parameter for multi-variant types
};

/// An A/B track pair — two layers with an optional transition between them.
/// The compositor blends foreground over background according to the
/// transition progress, then uses the result as input to the next pair.
struct ABPair
{
    CompositorLayer   background;    ///< Track A (lower)
    CompositorLayer   foreground;    ///< Track B (upper)
    PairTransitionInfo transition;   ///< Transition between A and B (type=-1 = cut)
};

// ── GPU SSBO layout (must match composite.comp LayerParams) ─────────────────

struct alignas(16) LayerParamsGPU
{
    glm::mat4 transform[kMaxCompositorLayers];   // 32 * 64 = 2048 bytes
    glm::mat4 motionTransformStart[kMaxCompositorLayers];
    glm::mat4 motionTransformEnd[kMaxCompositorLayers];
    int32_t   motionSampleCount[kMaxCompositorLayers];
    float     opacity[kMaxCompositorLayers];      // 32 * 4  = 128 bytes
    int32_t   blendMode[kMaxCompositorLayers];    // 32 * 4  = 128 bytes
    int32_t   enabled[kMaxCompositorLayers];      // 32 * 4  = 128 bytes
    glm::vec4 cropRect[kMaxCompositorLayers];     // 32 * 16 = 512 bytes  (L, R, T, B)
    glm::vec4 contentRect[kMaxCompositorLayers];  // 32 * 16 = 512 bytes  (L, T, R, B)
    int32_t   isPacked[kMaxCompositorLayers];     // 32 * 4  = 128 bytes  (packed-alpha flag)
    int32_t   isPMA[kMaxCompositorLayers];         // 32 * 4  = 128 bytes  (premultiplied-alpha flag)
    int32_t   needsSwapRB[kMaxCompositorLayers];  // 32 * 4  = 128 bytes  (R↔B swap for nested seq)
    int32_t   hasMask[kMaxCompositorLayers];      // 32 * 4  = 128 bytes  (opacity mask flag)
    int32_t   layerCount{0};                      // 4 bytes
    // Padding to 16-byte alignment (std430)
    int32_t   _pad[3]{};
};

// ── Compositing statistics ──────────────────────────────────────────────────

struct CompositorStats
{
    uint32_t layerCount{0};
    uint32_t enabledLayers{0};
    float    gpuTimeMs{0.0f};
    uint32_t outputWidth{0};
    uint32_t outputHeight{0};
};

// ═════════════════════════════════════════════════════════════════════════════

class Compositor : public ICompositor
{
public:
    Compositor();
    ~Compositor() override;

    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Initialize the compositor with Vulkan resources.
    bool init(Device& device,
              Allocator& allocator,
              CommandPool& cmdPool,
              VkQueue computeQueue,
              const CompositorConfig& config = {});

    /// Shut down and release all GPU resources.
    void shutdown() override;

    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized; }

    // ── Layer management ────────────────────────────────────────────────

    /// Set layers for the next composite dispatch (max 32).
    void setLayers(const std::vector<CompositorLayer>& layers) override;

    /// Set A/B track pairs for the next composite dispatch.
    /// Internally flattens pairs into layers, handling transition blending
    /// between background (Track A) and foreground (Track B) per pair.
    /// Max 16 pairs (32 layers).
    void setPairs(const std::vector<ABPair>& pairs) override;

    /// Clear all layers.
    void clearLayers() override;

    /// Get current layer count.
    [[nodiscard]] uint32_t layerCount() const noexcept override { return m_layerCount; }

    // ── Compositing ─────────────────────────────────────────────────────

    /// Dispatch composite compute shader. Returns false on error.
    /// After this call, the output image is in VK_IMAGE_LAYOUT_GENERAL.
    bool composite(VkCommandBuffer cmd) override;

    /// Composite using an internal one-shot command buffer (synchronous).
    bool compositeSync() override;

    /// Enable high-quality (Catmull-Rom bicubic) layer sampling.  Off by
    /// default (bilinear) for fast real-time preview; the export renderer
    /// turns it on so downscaled high-res sources (e.g. ProRes characters)
    /// stay sharp.  See composite.comp.
    void setHighQualitySampling(bool hq) noexcept { m_hqSampling = hq; }

    /// Alpha export (Phase 4.2): when true the composite output keeps its
    /// STRAIGHT-alpha result (transparent background) instead of flattening
    /// over black, so ProRes 4444 / PNG export gets a real alpha channel.
    /// Set per-composite by CompositeService from its export-alpha flag;
    /// defaults false so viewport/playback are unaffected.
    void setPreserveAlpha(bool keep) noexcept { m_preserveAlpha = keep; }
    [[nodiscard]] bool preserveAlpha() const noexcept { return m_preserveAlpha; }

    /// Final readback/presentation expects BGRA byte order. Internal
    /// adjustment-layer composites must stay RGBA because EffectProcessor
    /// samples them directly as ordinary Vulkan textures.
    void setOutputSwizzleRB(bool swizzle) noexcept { m_outputSwizzleRB = swizzle; }
    [[nodiscard]] bool outputSwizzleRB() const noexcept { return m_outputSwizzleRB; }

    // ── Resize ──────────────────────────────────────────────────────────

    /// Resize the output image.
    bool resize(uint32_t width, uint32_t height) override;

    // ── Output access ───────────────────────────────────────────────────

    [[nodiscard]] VkImage       outputImage()     const noexcept { return m_outputTexture ? m_outputTexture->image()    : VK_NULL_HANDLE; }
    [[nodiscard]] VkImageView   outputImageView() const noexcept override { return m_outputTexture ? m_outputTexture->imageView() : VK_NULL_HANDLE; }
    [[nodiscard]] VkSampler     outputSampler()   const noexcept override { return m_outputTexture ? m_outputTexture->sampler()   : VK_NULL_HANDLE; }

    /// Opaque shared ownership for CachedFrame::gpuTextureOwner.
    /// Keeps the output texture alive as long as any composited frame
    /// that sampled it is still in flight in the viewport.
    [[nodiscard]] std::shared_ptr<void> outputTextureOwner() const noexcept { return m_outputTexture; }
    [[nodiscard]] uint32_t      outputWidth()     const noexcept { return m_config.outputWidth; }
    [[nodiscard]] uint32_t      outputHeight()    const noexcept { return m_config.outputHeight; }

    /// Descriptor info for sampling the composite output in another shader.
    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const override;

    /// Read back output pixels to CPU (for testing / export). Synchronous.
    bool readbackOutput(std::vector<uint8_t>& outPixels) override;

    /// Record readback commands into an external command buffer (no submit).
    /// Inserts compute→transfer barrier, transitions output to TRANSFER_SRC,
    /// copies to persistent staging buffer, transitions back to GENERAL.
    /// After the command buffer is submitted and waited on,
    /// call mapAndCopyReadback() to retrieve the pixels.
    bool recordReadback(VkCommandBuffer cmd) override;

    /// Map the persistent readback staging buffer and copy pixels out.
    /// Must only be called AFTER a command buffer containing recordReadback()
    /// commands has been submitted and completed.
    bool mapAndCopyReadback(std::vector<uint8_t>& outPixels) override;

    /// Stable readback token for a recorded frame. Lazy CPU fallback may run
    /// after newer composites have advanced the output ring.
    [[nodiscard]] uint32_t outputSlot() const noexcept { return m_outputRingIdx; }
    bool mapAndCopyReadbackSlot(uint32_t slot,
                                std::vector<uint8_t>& outPixels);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] const CompositorStats& stats() const noexcept override { return m_stats; }

    // ── Static helpers ──────────────────────────────────────────────────

    /// Build a UV-space transform matrix from position, scale, rotation.
    /// Position is in normalized [0,1] coords, (0,0) = top-left.
    static glm::mat4 buildLayerTransform(float posX, float posY,
                                          float scaleX, float scaleY,
                                          float rotationDeg = 0.0f);

    /// Build a transform that maps output UV → layer UV using "cover" fit
    /// semantics (preserves source aspect ratio, may crop edges).  This
    /// matches the CPU blitLayerWithTransform behaviour.
    /// @param srcW, srcH      Source texture pixel dimensions
    /// @param outW, outH      Output/viewport pixel dimensions
    /// @param posXPx, posYPx  Pixel offset from centre (at output resolution)
    /// @param scaleX, scaleY  Scale multiplier (1.0 = normal)
    /// @param rotDeg          Rotation in degrees
    /// @param containFit      When true, use "contain" fit (min scale, no crop,
    ///                        may letterbox).  Default is "cover" (max scale).
    /// @param srcRotationDeg  Source DISPLAY rotation (clockwise, 0/90/180/270)
    ///                        from VideoStreamInfo::rotation — portrait phone
    ///                        footage etc.  90/270 swap the fit aspect; the
    ///                        sampled UV is re-oriented into the source texture.
    ///                        Default 0 = legacy byte-identical path.
    static glm::mat4 buildViewportTransform(uint32_t srcW, uint32_t srcH,
                                             uint32_t outW, uint32_t outH,
                                             float posXPx, float posYPx,
                                             float scaleX, float scaleY,
                                             float rotDeg = 0.0f,
                                             bool containFit = false,
                                             float anchorXPx = 0.0f,
                                             float anchorYPx = 0.0f,
                                             int srcRotationDeg = 0);

    /// Identity transform (layer fills entire output).
    static glm::mat4 identityTransform() { return glm::mat4(1.0f); }

private:
    bool m_hqSampling{false};  // bicubic layer sampling (export); see composite.comp
    bool m_preserveAlpha{false}; // keep straight RGBA for alpha export; see composite.comp
    bool m_outputSwizzleRB{true}; // final Qt/readback output is BGRA; intermediates are RGBA
    bool createOutputTexture();
    /// Rotate m_outputTexture to the next slot in the output ring.  Called
    /// at the start of every composite() so each composited frame writes
    /// into its OWN texture instead of a single shared one.  The previous
    /// slot stays alive as long as a CachedFrame holds it via
    /// gpuTextureOwner (the presenter), so the producer can keep
    /// compositing the next frame(s) without overwriting the texture the
    /// presenter is still sampling.  This is what makes the existing
    /// gpuTextureOwner frame-isolation design actually work, and it also
    /// isolates the inner-vs-outer nested-sequence composites (they land
    /// on different ring slots within the same frame).
    void advanceOutputRing();
    bool createComputePipeline();
    bool createDescriptorResources();
    bool createTimestampQueries();

    void updateSSBO();
    void updateDescriptorSet();

    // ── Vulkan handles ──────────────────────────────────────────────────

    Device*       m_device{nullptr};
    Allocator*    m_allocator{nullptr};
    CommandPool*  m_cmdPool{nullptr};
    VkQueue       m_queue{VK_NULL_HANDLE};

    CompositorConfig m_config;
    bool             m_initialized{false};

    // Output storage image RING.  Each composite() advances to the next
    // slot so a freshly composited frame never overwrites a texture the
    // presenter (or the LRU / m_lastGoodComposite / a nested inner
    // readback) may still be referencing via gpuTextureOwner.  Single
    // shared output was the root cause of the nested-sequence flicker:
    // the presenter zero-copy-displayed it while the next frame's inner
    // composite stomped it.  Size must exceed the max number of output
    // frames simultaneously in flight (presenter last+current, the
    // in-progress outer, and a nested inner readback) — 6 gives margin.
    static constexpr uint32_t kOutputRing = 6;
    std::array<std::shared_ptr<Texture>, kOutputRing> m_outputRing;
    uint32_t m_outputRingIdx{0};

    // Alias to m_outputRing[m_outputRingIdx]: the slot the most recent
    // composite() wrote.  All output accessors / readback read this, so
    // the ring is transparent to callers.  shared_ptr so CachedFrame
    // references keep a slot alive after the ring rotates past it.
    std::shared_ptr<Texture> m_outputTexture;

    // Compute pipeline
    PipelineManager m_pipelineManager;
    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, kOutputRing> m_descriptorSets{};

    // Per-output-slot SSBOs. A host upload must never modify a buffer still
    // referenced by an older pending command buffer.
    std::array<Buffer, kOutputRing> m_layerParamsBuffers;

    // Per-output-slot readback buffers avoid overlapping transfer writes.
    std::array<Buffer, kOutputRing> m_readbackStaging;

    // Placeholder texture for unused layer slots
    Texture m_placeholderTexture;

    // Timestamp queries for GPU timing
    std::array<VkQueryPool, kOutputRing> m_queryPools{};
    float       m_timestampPeriod{0.0f};

    // Layer state
    std::vector<CompositorLayer> m_layers;
    uint32_t                     m_layerCount{0};
    std::atomic<bool>            m_layersDirty{true};

    // Stats
    CompositorStats m_stats;
};

} // namespace rt
