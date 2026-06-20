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

/// Per-layer render state built by buildLayersForFrame().
/// Carries the decoded frame, transforms, opacity, crop, effects, wipe
/// info, and optional GPU-resident texture descriptor.
struct LayerInfo
{
    std::shared_ptr<CachedFrame> frame;
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
    std::vector<EffectStack::EffectSnapshot> effects; // evaluated clip effects
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

} // namespace rt
