/*
 * CompositeServiceLayerBuild.h - Layer collection / building declarations.
 * Extracted from CompositeServiceFrame.cpp so layer-gathering logic can
 * be reused without pulling in the entire compositeFrame() orchestrator.
 */

#pragma once

#include "cache/FrameCache.h"       // CachedFrame
#include "timeline/Transition.h"    // TransitionType
#include "effects/EffectStack.h"    // EffectStack::EffectSnapshot

#include <cstdint>
#include <memory>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4005) // macro redefinition (volk vs vulkan.h)
#endif
#include <vulkan/vulkan_core.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace rt {

// Forward declarations
class Clip;
class AdjustmentClip;

/// One evaluated clip transform in compositor/output coordinates. Motion
/// blur carries the exposure endpoints alongside the current transform.
struct LayerTransformSample
{
    float posX{0.0f};
    float posY{0.0f};
    float scX{1.0f};
    float scY{1.0f};
    float rot{0.0f};
    float anchorX{0.0f};
    float anchorY{0.0f};
};

/// Per-layer render state built by buildLayersForFrame().
/// Carries the decoded frame, transforms, opacity, crop, effects, wipe
/// info, and optional GPU-resident texture descriptor.
struct LayerInfo
{
    std::shared_ptr<CachedFrame> frame;

    // Adjustment clips are render-stream boundaries rather than source
    // images.  When this marker is encountered, the compositor flattens all
    // layers collected below it to a full-frame texture, applies `effects`
    // once to that texture, then continues compositing layers above it.
    bool isAdjustmentLayer{false};

    // Optional second source frame used by retiming interpolation.  The
    // interpolation pass consumes frame + temporalFrame in source space,
    // then sends the synthesized result through the normal clip effects.
    std::shared_ptr<CachedFrame> temporalFrame;
    float temporalPhase{0.0f};
    int32_t temporalMode{0}; // TimeInterpolation numeric value (0/1/2)
    // The exact requested source frame was not ready and this layer uses a
    // nearby/last-good substitute (including a temporal nearest-frame
    // fallback). Composite output for this tick must not enter the LRU or the
    // substitute can remain pinned after the exact decode finishes.
    bool sourceFallbackPending{false};
    /// The source pixels are invariant until explicit cache invalidation
    /// (single-frame media or a cached tier-list visual state).  When every
    /// layer has this property the final composite may be reused across ticks.
    bool contentStableForStateCache{false};
    bool temporalGpuTextureReady{false};
    VkDescriptorImageInfo temporalGpuDescriptor{};
    bool temporalGpuCacheBacked{false};
    uint64_t temporalGpuCacheMediaId{0};
    int64_t temporalGpuCacheFrameNumber{0};
    uint8_t temporalGpuCacheTier{0};
    // Ordinary layers use this as source opacity. Adjustment markers use it
    // as effect strength: 0 keeps the pre-effect composite, 1 uses the fully
    // processed result, and intermediate values dissolve between the two.
    float opacity{1.0f};
    float posX{0.0f};     // pixels offset
    float posY{0.0f};
    float scX{1.0f};      // scale multiplier
    float scY{1.0f};
    float rot{0.0f};      // degrees
    /// Anchor / rotation-scale pivot, in OUTPUT pixels relative to the
    /// composited frame's geometric centre. Already scaled from the
    /// clip's REF-1920 anchor track by buildLayersForFrame.
    float anchorX{0.0f};
    float anchorY{0.0f};
    /// Transform exposure endpoints. A count of 1 uses the current transform
    /// only; counts above 1 average evenly-spaced temporal transform samples.
    LayerTransformSample motionStart{};
    LayerTransformSample motionEnd{};
    int32_t motionSampleCount{1};
    float cropL{0.0f};    // crop percentages 0–100
    float cropR{0.0f};
    float cropT{0.0f};
    float cropB{0.0f};
    uint32_t frameWidth{0};   // source dimensions (used when gpuTextureReady)
    uint32_t frameHeight{0};
    int srcRotation{0};       // source display rotation, clockwise: 0/90/180/270.
                              // From VideoStreamInfo::rotation (portrait phone
                              // footage etc.).  buildViewportTransform swaps the
                              // fit aspect for 90/270 and re-orients the sampled
                              // UV.  0 = no rotation (legacy byte-identical path).
    bool containFit{false};   // true = contain-fit (for pre-rendered spine cache)
    bool isPacked{false};     // true = packed-alpha (GPU shader handles unpack)
    bool isPMA{false};        // true = premultiplied-alpha (Spine FBO output)
    bool contentBoundsValid{false};
    float contentLeft{0.0f};
    float contentTop{0.0f};
    float contentRight{1.0f};
    float contentBottom{1.0f};
    std::vector<EffectStack::EffectSnapshot> effects; // evaluated clip effects
    /// Evaluated clip opacity masks (snapshotted at layer-build time so the
    /// render thread never reads live OpacityMask objects).
    std::vector<MaskRenderState> masks;
    int32_t blendMode{0}; // compositor blend mode from clip

    // Wipe transition info (for GPU spatial blending)
    uint64_t clipId{0};                         // originating clip ID
    TransitionType wipeType{TransitionType::CrossDissolve}; // default = no wipe
    float wipeProgress{-1.0f};                  // < 0 means not in a wipe
    float wipeSoftness{-1.0f};                  // per-transition softness (<0 = use default)
    uint64_t wipePeerClipId{0};                 // the clip on the other side
    bool isWipeOutgoing{false};                 // true = this is the outgoing (left) clip

    // GPU-resident texture (e.g. from SpineRenderer's offscreen FBO).
    // When gpuTextureReady is true, the compositor can use gpuDescriptor
    // directly instead of uploading from frame->pixels.
    bool gpuTextureReady{false};
    VkDescriptorImageInfo gpuDescriptor{};
    // Cache identity for descriptors resolved before uploadLayer().  The
    // render phase uses it to pin the exact cache entry for the duration of
    // the in-flight submission.
    bool gpuCacheBacked{false};
    uint64_t gpuCacheMediaId{0};
    int64_t gpuCacheFrameNumber{0};
    uint8_t gpuCacheTier{0};

    // Nested sequence composite frames have BGRA bytes stored in an
    // R8G8B8A8 texture (because composite.comp writes result.bgra).
    // When sampled as a layer in the outer compositor, R and B appear
    // swapped.  This flag tells the shader to undo the swap.
    bool needsSwapRB{false};

    // Source clip pointer (non-owning) — used to access masks during GPU compositing.
    Clip* clipPtr{nullptr};

    // True when this layer's frame numbers cycle (character loops).
    // Enables GPU texture caching so repeated frame numbers are free.
    bool isLoopContent{false};
};

/// Append one evaluated adjustment boundary without modifying any ordinary
/// layer's effect stack. `localTick` is relative to the adjustment clip's
/// timelineIn().
void appendAdjustmentLayerBoundary(std::vector<LayerInfo>& layers,
                                   AdjustmentClip& adjustment,
                                   int64_t localTick,
                                   float effectStrength = 1.0f);

/// Evaluate an adjustment layer's effect strength, including its opacity
/// track and any Cross Dissolve attached to its head or tail.
[[nodiscard]] float adjustmentLayerStrengthAtTick(
    AdjustmentClip& adjustment,
    int64_t localTick,
    int64_t timelineTick,
    const std::vector<Transition>& transitions);

} // namespace rt
