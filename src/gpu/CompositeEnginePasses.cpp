/*
 * CompositeEnginePasses.cpp -- records the render-graph passes of one
 * composite in topological order: source/temporal uploads, effects (with
 * temporal interpolation first), transitions, the final composite (which
 * folds adjustment layers) and the readback. See CompositeEngineFrame.h.
 */

// define VK_NO_PROTOTYPES first.
#include <volk.h>

#include "CompositeEngine.h"
#include "CompositeEngineFrame.h"
#include "ExportStageTimes.h"
#include "render_graph/GpuRenderGraph.h"
#include "cache/CachePolicy.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "effects/CpuComposite.h"       // rasterizeMasks
#include "Compositor.h"
#include "diag/FrameSignatureLog.h"
#include "GpuContext.h"
#include "GpuTextureCache.h"
#include "GpuWorkSubmission.h"
#include "GpuUploadManager.h"
#include "vulkan/Texture.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"
#include "cache/FrameCache.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "effects/LUT.h"
#include "timeline/Transition.h"
#include "timeline/Clip.h"

#include <spdlog/spdlog.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using namespace rt;
using namespace rt::render_graph;
using rt::composite_detail::setLayerTransforms;

static GpuTransitionType toGpuTransitionType(TransitionType tt) noexcept
{
    switch (tt) {
    case TransitionType::CrossDissolve: return GpuTransitionType::Dissolve;
    case TransitionType::WipeLeft:  return GpuTransitionType::WipeLeft;
    case TransitionType::WipeRight: return GpuTransitionType::WipeRight;
    case TransitionType::WipeUp:    return GpuTransitionType::WipeUp;
    case TransitionType::WipeDown:  return GpuTransitionType::WipeDown;
    case TransitionType::PushLeft:  return GpuTransitionType::PushLeft;
    case TransitionType::PushRight: return GpuTransitionType::PushRight;
    case TransitionType::PushUp:    return GpuTransitionType::PushUp;
    case TransitionType::PushDown:  return GpuTransitionType::PushDown;
    case TransitionType::DipToBlack:
    case TransitionType::DipToWhite:       return GpuTransitionType::DipColor;
    case TransitionType::FilmDissolve:     return GpuTransitionType::FilmDissolve;
    case TransitionType::AdditiveDissolve: return GpuTransitionType::AdditiveDissolve;
    case TransitionType::BarnDoor:         return GpuTransitionType::BarnDoor;
    case TransitionType::ClockWipe:        return GpuTransitionType::ClockWipe;
    case TransitionType::RadialWipe:       return GpuTransitionType::RadialWipe;
    case TransitionType::IrisRound:
    case TransitionType::IrisDiamond:
    case TransitionType::IrisCross:        return GpuTransitionType::Iris;
    case TransitionType::DiagonalWipe:     return GpuTransitionType::DiagonalWipe;
    case TransitionType::CheckerWipe:      return GpuTransitionType::CheckerWipe;
    case TransitionType::VenetianBlinds:   return GpuTransitionType::VenetianBlinds;
    case TransitionType::Inset:            return GpuTransitionType::Inset;
    case TransitionType::SlideLeft:
    case TransitionType::SlideRight:
    case TransitionType::SlideUp:
    case TransitionType::SlideDown:        return GpuTransitionType::Slide;
    case TransitionType::Split:
    case TransitionType::CenterSplit:      return GpuTransitionType::SplitWipe;
    case TransitionType::Swap:             return GpuTransitionType::Swap;
    case TransitionType::Zoom:
    case TransitionType::CrossZoom:        return GpuTransitionType::ZoomTransition;
    case TransitionType::WhipPan:          return GpuTransitionType::WhipPan;
    case TransitionType::RandomBlocks:     return GpuTransitionType::RandomBlocks;
    case TransitionType::MorphCut:         return GpuTransitionType::MorphCut;
    case TransitionType::GradientWipe:     return GpuTransitionType::GradientWipe;
    case TransitionType::FadeToBlack:
    case TransitionType::FadeFromBlack:
    case TransitionType::FadeToWhite:
    case TransitionType::FadeFromWhite:
    default: return GpuTransitionType::Dissolve;
    }
}

static int32_t transitionDirectionOverride(TransitionType tt) noexcept
{
    switch (tt) {
    case TransitionType::DipToWhite:   return 1;
    case TransitionType::IrisDiamond:  return 1;
    case TransitionType::IrisCross:    return 2;
    case TransitionType::SlideLeft:    return 0;
    case TransitionType::SlideRight:   return 1;
    case TransitionType::SlideUp:      return 2;
    case TransitionType::SlideDown:    return 3;
    case TransitionType::CenterSplit:  return 1;
    case TransitionType::CrossZoom:    return 1;
    default: return -1;
    }
}

static void setStaticTransform(CompositorLayer& compositorLayer,
                               const glm::mat4& transform)
{
    compositorLayer.transform = transform;
    compositorLayer.motionTransformStart = transform;
    compositorLayer.motionTransformEnd = transform;
    compositorLayer.motionSampleCount = 1;
}

static CompositorLayer fullFrameLayer(
    const VkDescriptorImageInfo& textureInfo)
{
    CompositorLayer layer;
    layer.enabled = true;
    layer.textureInfo = textureInfo;
    layer.opacity = 1.0f;
    layer.blendMode = BlendMode::Normal;
    layer.isPacked = false;
    layer.isPMA = false;
    layer.needsSwapRB = false;
    setStaticTransform(layer, Compositor::identityTransform());
    return layer;
}

static void makeComputeOutputSampleable(VkCommandBuffer cmd, VkImage image)
{
    if (image == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// EffectProcessor owns only two mutable ping-pong images. Copy an adjustment
// result to a submission-slot texture before another stack can overwrite it.
static bool snapshotEffectOutput(VkCommandBuffer cmd,
                                 EffectProcessor& processor,
                                 Texture& snapshot,
                                 GpuContext& ctx,
                                 uint32_t width, uint32_t height)
{
    if (snapshot.image() == VK_NULL_HANDLE ||
        snapshot.width() != width || snapshot.height() != height) {
        snapshot.destroy();
        TextureConfig cfg{};
        cfg.width = width;
        cfg.height = height;
        cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
        cfg.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!snapshot.create(ctx.allocator().handle(), ctx.vkDevice(), cfg))
            return false;
    }

    const VkImage effectImage = processor.outputImage();
    if (effectImage == VK_NULL_HANDLE || snapshot.image() == VK_NULL_HANDLE)
        return false;

    VkImageMemoryBarrier toSource{};
    toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSource.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSource.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSource.image = effectImage;
    toSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSource);

    snapshot.transitionLayout(cmd, snapshot.layout(),
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vkCmdCopyImage(cmd, effectImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   snapshot.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region);

    VkImageMemoryBarrier restoreSource{};
    restoreSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restoreSource.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restoreSource.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    restoreSource.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restoreSource.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    restoreSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSource.image = effectImage;
    restoreSource.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &restoreSource);

    snapshot.transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// ============================================================================
// Pass walk
// ============================================================================

void CompositeEngine::recordPasses(FrameRecording& frame)
{
    using SplitClock = FrameRecording::SplitClock;
    VkCommandBuffer cmd = frame.cmd;
    const int timingSlot = frame.timingSlot;
    bool& uploadsSeen = frame.uploadsSeen;
    bool& uploadTimestampWritten = frame.uploadTimestampWritten;
    bool& effectTimestampWritten = frame.effectTimestampWritten;
    double& passUploadMs = frame.passUploadMs;
    double& passEffectMs = frame.passEffectMs;
    double& passCompositeMs = frame.passCompositeMs;
    double& passOtherMs = frame.passOtherMs;

    // Walk passes in topological order.
    // NOTE: Graph-computed barriers are NOT used yet — the existing
    // renderers manage their own internal barriers.  We insert only
    // the global transfer→compute barrier that the old monolithic path
    // had between upload and compute stages.
    //
    // GPU-TIMING stage markers (written into m_timingPools[timingSlot]):
    //   0: frame start (already written before the loop)
    //   1: after all uploads, before first effect/transition
    //   2: after all effects+transitions, before composite
    //   3: after composite+readback (written after the loop)
    frame.timingActive = m_timingInitialized &&
                         m_timingPools[timingSlot] != VK_NULL_HANDLE;
    const bool timingActive = frame.timingActive;
    for (uint32_t passIdx : frame.graph.topologicalOrder()) {
        const auto& pass = frame.graph.pass(passIdx);

        // Phase D (CLAUDE_IMPROVEMENT_PLAN): session-disabled passes.
        // Optional passes (Effect, Transition) that failed earlier in
        // this session are skipped entirely.  gpuLayers[] keeps its
        // pre-pass state — effectively a passthrough.  Prevents the
        // engine from re-dispatching a known-broken shader 60 fps.
        if (pass.optional && !m_disabledPasses.empty() &&
            m_disabledPasses.count(pass.name) > 0)
        {
            continue;
        }

        // Insert global transfer→compute barrier before first compute pass
        // when uploads have been recorded (mirrors old monolithic path).
        if (uploadsSeen && pass.type != PassType::Upload && pass.type != PassType::External) {
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
            uploadsSeen = false; // only insert once
        }

        // Stage-boundary timestamps.  Written *before* the first pass of
        // each stage so the delta from the previous marker measures the
        // previous stage's GPU work.
        if (timingActive && !uploadTimestampWritten &&
            pass.type != PassType::Upload && pass.type != PassType::External)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 1);
            uploadTimestampWritten = true;
        }
        if (timingActive && !effectTimestampWritten &&
            (pass.type == PassType::Composite || pass.type == PassType::Readback))
        {
            // First non-upload marker may not have fired (no effects this
            // frame).  Ensure marker 1 is written before marker 2.
            if (!uploadTimestampWritten) {
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    m_timingPools[timingSlot], 1);
                uploadTimestampWritten = true;
            }
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 2);
            effectTimestampWritten = true;
        }

        // ── Execute the pass ─────────────────────────────────────
        struct PassLap {
            double& bucket;
            SplitClock::time_point t0{SplitClock::now()};
            ~PassLap() {
                bucket += std::chrono::duration<double, std::milli>(
                    SplitClock::now() - t0).count();
            }
        } passLap{pass.type == PassType::Upload    ? passUploadMs
                : pass.type == PassType::Effect    ? passEffectMs
                : pass.type == PassType::Composite ? passCompositeMs
                                                   : passOtherMs};
        switch (pass.type) {
        case PassType::Upload:
            recordUploadPass(frame, passIdx);
            break;
        case PassType::Effect:
            recordEffectPass(frame, passIdx);
            break;
        case PassType::Transition:
            recordTransitionPass(frame, passIdx);
            break;
        case PassType::Composite:
            recordCompositePass(frame);
            break;
        case PassType::Readback:
            recordReadbackPass(frame);
            break;
        default:
            break;
        }
    }
}

// ============================================================================
// One function per pass type
// ============================================================================

void CompositeEngine::recordUploadPass(FrameRecording& frame, uint32_t passIdx)
{
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const bool scrubMode = frame.scrubMode;
    auto& slotResources = *frame.resources;
    auto& gpuLayerTextures = frame.resources->layerTextures;
    auto& gpuLayerTexKeys = frame.resources->layerTexKeys;
    auto& gpuLayerTexturesAlt = frame.resources->layerTexturesAlt;
    auto& layerInfo = frame.layerPasses;
    auto& gpuLayers = frame.gpuLayers;
    auto& temporalPrimarySourceInfos = frame.temporalPrimarySourceInfos;
    auto& temporalSourceInfos = frame.temporalSourceInfos;
    bool& uploadOk = frame.uploadOk;
    bool& uploadsSeen = frame.uploadsSeen;

    uploadsSeen = true;
    // Find which layer this upload belongs to
    for (size_t li = 0; li < layers.size(); ++li) {
        if (layerInfo[li].uploadPassIdx == passIdx) {
            const auto& layer = layers[li];

            // Upload passes exist only for layers without a ready GPU
            // texture (buildCompositeGraph); those were set up in
            // prepareCompositorLayers.
            // ── Packed-alpha double-buffer (#93) ──────────
            // Swap upload/composite targets so the compositor
            // always reads a texture the upload isn't writing
            // to.  Zero-copy: just exchange unique_ptrs.
            if (layer.isPacked &&
                gpuLayerTexturesAlt[li] &&
                gpuLayerTexturesAlt[li]->image() != VK_NULL_HANDLE) {
                gpuLayerTextures[li].swap(gpuLayerTexturesAlt[li]);
            }

            // Upload via existing GpuUploadManager
            auto uploadResult = m_uploadManager->uploadLayer(
                layer, *gpuLayerTextures[li],
                gpuLayerTexKeys[li].mediaId,
                gpuLayerTexKeys[li].frameNumber,
                gpuLayerTexKeys[li].framePtr,
                scrubMode);

            if (uploadResult.success) {
                // Keep the exact freshly-resolved endpoint for
                // temporal synthesis. Packed-alpha's normal
                // composite path may intentionally display its
                // stable alternate buffer, which can be one frame
                // older and must not be used for interpolation.
                temporalPrimarySourceInfos[li] =
                    uploadResult.descriptor;
                // Point the compositor at the stable (non-uploaded)
                // double-buffer texture — but ONLY when it matches the
                // just-uploaded texture's dimensions. The preceding
                // scrub frames can be GPU-resident (they take the
                // gpuTextureReady branch and never touch this double-
                // buffer), or the tier can change on a scrub→settle, so
                // the Alt buffer may hold STALE / wrong-size content.
                // Displaying it then shows garbage transformed by the
                // new frame's geometry — the Wells "settle" red-seam
                // glitch. When Alt is stale/mismatched, show the freshly
                // uploaded texture instead (correct size + content). In
                // steady state the dimensions match every frame, so the
                // #93 write-race protection is preserved unchanged.
                if (layer.isPacked &&
                    gpuLayerTexturesAlt[li] &&
                    gpuLayerTexturesAlt[li]->image() != VK_NULL_HANDLE &&
                    gpuLayerTextures[li] &&
                    gpuLayerTexturesAlt[li]->width()  == gpuLayerTextures[li]->width() &&
                    gpuLayerTexturesAlt[li]->height() == gpuLayerTextures[li]->height()) {
                    gpuLayers[li].textureInfo =
                        gpuLayerTexturesAlt[li]->descriptorInfo();
                } else {
                    gpuLayers[li].textureInfo = uploadResult.descriptor;
                }
                setLayerTransforms(gpuLayers[li], layer,
                                   uploadResult.srcW, uploadResult.srcH,
                                   outW, outH);
            } else {
                spdlog::warn("[RENDER_GRAPH] layer {} upload failed", li);
                gpuLayers[li].enabled = false;
                uploadOk = false;
            }
            break;
        }
        if (layerInfo[li].temporalUploadPassIdx == passIdx) {
            const auto& layer = layers[li];
            if (layer.temporalFrame) {
                LayerInfo temporalLayer = layer;
                temporalLayer.frame = layer.temporalFrame;
                temporalLayer.gpuTextureReady = false;
                temporalLayer.temporalFrame.reset();
                auto& key = slotResources.temporalSourceTexKeys[li];
                auto result = m_uploadManager->uploadLayer(
                    temporalLayer,
                    *slotResources.temporalSourceTextures[li],
                    key.mediaId, key.frameNumber, key.framePtr,
                    scrubMode);
                if (result.success) {
                    temporalSourceInfos[li] = result.descriptor;
                } else {
                    spdlog::debug("[RENDER_GRAPH] temporal source "
                                  "upload failed for layer {}", li);
                }
            }
            break;
        }
    }
}

void CompositeEngine::recordEffectPass(FrameRecording& frame, uint32_t passIdx)
{
    auto& ctx = GpuContext::get();
    auto& slot = *frame.slot;
    VkCommandBuffer cmd = frame.cmd;
    const int timingSlot = frame.timingSlot;
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const bool forceSyncReadback = frame.forceSyncReadback;
    const auto& effectProcessor = frame.effectProcessor;
    int& effectLayerCount = frame.effectLayerCount;
    int& effectPassCount = frame.effectPassCount;
    auto& slotResources = *frame.resources;
    auto& layerEffectOutputs = frame.resources->effectOutputs;
    auto& layerInfo = frame.layerPasses;
    auto& gpuLayers = frame.gpuLayers;
    auto& temporalPrimarySourceInfos = frame.temporalPrimarySourceInfos;
    auto& temporalSourceInfos = frame.temporalSourceInfos;
    auto& layerEffectMaskInfos = frame.effectMaskInfos;
    const auto& pass = frame.graph.pass(passIdx);

    for (size_t li = 0; li < layers.size(); ++li) {
        if (layerInfo[li].effectPassIdx != passIdx) continue;
        const auto& layer = layers[li];
        if (!gpuLayers[li].enabled) break;

        const bool temporalRequested =
            layer.temporalMode != 0 &&
            layer.temporalPhase > 0.000001f &&
            temporalSourceInfos[li].imageView != VK_NULL_HANDLE;
        if (temporalRequested && m_temporalInterpolator &&
            m_temporalInterpolator->isInitialized()) {
            uint32_t temporalW = layer.frameWidth
                ? layer.frameWidth : outW;
            uint32_t temporalH = layer.frameHeight
                ? layer.frameHeight : outH;
            if (layer.isPacked && temporalH > 1)
                temporalH /= 2;

            auto& temporalOut =
                *slotResources.temporalOutputs[li];
            if (temporalOut.image() == VK_NULL_HANDLE ||
                temporalOut.width() != temporalW ||
                temporalOut.height() != temporalH) {
                temporalOut.destroy();
                TextureConfig cfg{};
                cfg.width = temporalW;
                cfg.height = temporalH;
                cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
                cfg.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
                if (temporalOut.create(ctx.allocator().handle(),
                                       ctx.vkDevice(), cfg)) {
                    temporalOut.transitionLayout(
                        cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_GENERAL);
                }
            }

            const bool wasPacked = gpuLayers[li].isPacked;
            const bool wasPma = gpuLayers[li].isPMA;
            if (temporalOut.image() != VK_NULL_HANDLE &&
                m_temporalInterpolator->render(
                    cmd, temporalOut.imageView(),
                    temporalPrimarySourceInfos[li].imageView !=
                            VK_NULL_HANDLE
                        ? temporalPrimarySourceInfos[li]
                        : gpuLayers[li].textureInfo,
                    temporalSourceInfos[li],
                    temporalW, temporalH,
                    layer.temporalPhase, layer.temporalMode,
                    wasPacked, wasPacked,
                    static_cast<uint32_t>(timingSlot),
                    static_cast<uint32_t>(li))) {
                makeComputeOutputSampleable(cmd, temporalOut.image());

                gpuLayers[li].textureInfo =
                    temporalOut.descriptorInfo();
                gpuLayers[li].textureInfo.imageLayout =
                    VK_IMAGE_LAYOUT_GENERAL;
                gpuLayers[li].isPacked = false;
                // Packed-alpha inputs are unpacked to straight RGBA.
                // Native PMA video remains PMA after linear/warped mix.
                gpuLayers[li].isPMA = wasPacked ? false : wasPma;
                gpuLayers[li].needsSwapRB = false;
            }
        }

        // Temporal synthesis is deliberately before the effect chain.
        // A layer with no effects can proceed directly to composite.
        if (layer.effects.empty()) break;

        const bool ots = std::any_of(
            layer.effects.begin(), layer.effects.end(),
            [](const EffectStack::EffectSnapshot& snap) {
                return snap.type == EffectType::OtsLeft ||
                       snap.type == EffectType::OtsRight ||
                       snap.type == EffectType::OtsIntro;
            });

        ++effectLayerCount;
        effectPassCount += (int)layer.effects.size();

        // Select an EffectProcessor matching this layer's processing
        // resolution.  Never resize the shared processor here: this
        // function records every layer into one command buffer, so a
        // resize would destroy images still referenced by commands
        // recorded for an earlier layer and can cause DEVICE_LOST.
        //
        // Normal effects are clip-bounded. OTS deliberately builds a
        // full-frame composition, so it processes at output size.
        // Compute source dimensions from layer info, handling
        // packed-alpha where the texture height is 2× the logical
        // frame height.
        uint32_t srcW = layer.frameWidth;
        uint32_t srcH = layer.frameHeight;
        if (srcW == 0 || srcH == 0) {
            srcW = outW;
            srcH = outH;
        }
        if (layer.isPacked && srcH > 1) srcH /= 2;
        const uint32_t effectW = ots ? outW : srcW;
        const uint32_t effectH = ots ? outH : srcH;
        std::shared_ptr<EffectProcessor> activeEffectLease =
            (effectProcessor &&
             effectProcessor->outputWidth() == effectW &&
             effectProcessor->outputHeight() == effectH)
                ? effectProcessor
                : (m_effectProcessorResolver
                    ? m_effectProcessorResolver(effectW, effectH)
                    : ctx.effectProcessor(effectW, effectH));
        EffectProcessor* activeEffectProcessor = activeEffectLease.get();
        if (!activeEffectProcessor || !activeEffectProcessor->isInitialized())
            break;
        slot.retainForCurrentSlot(activeEffectLease);

        VkDescriptorImageInfo srcInfo = gpuLayers[li].textureInfo;
        const std::vector<VkDescriptorImageInfo>* fxMasks =
            (li < layerEffectMaskInfos.size() &&
             !layerEffectMaskInfos[li].empty())
                ? &layerEffectMaskInfos[li] : nullptr;
        const bool effectsOk = activeEffectProcessor->process(
            cmd, srcInfo, layer.effects, fxMasks);
        // The cached LUT texture this dispatch samples must outlive
        // the frame even if the cache drops it meanwhile.
        if (const auto& lut = activeEffectProcessor->activeLUT3D())
            slot.retainForCurrentSlot(lut);
        if (effectsOk) {
            gpuLayers[li].textureInfo = activeEffectProcessor->outputDescriptorInfo();
            gpuLayers[li].needsSwapRB = false;
            gpuLayers[li].isPacked = false;
            gpuLayers[li].isPMA = false;

            // Barrier: compute write → sampler read (#40 fix)
            makeComputeOutputSampleable(cmd, activeEffectProcessor->outputImage());

            // Snapshot EVERY effected layer's output into its own
            // texture.  EffectProcessor has only two ping-pong storage
            // images shared across all layers; render-graph passes do
            // NOT necessarily execute in layer order, so any layer
            // (even the highest-indexed one) can have its live output
            // clobbered by another layer's effect before the composite
            // samples it.  Copying each result out immediately after
            // its own dispatch — in command order — makes every
            // layer's textureInfo independent of the shared storage.
            if (li < layerEffectOutputs.size()) {
                auto& snap = *layerEffectOutputs[li];
                if (snapshotEffectOutput(cmd, *activeEffectProcessor, snap,
                                         ctx, effectW, effectH))
                    gpuLayers[li].textureInfo = snap.descriptorInfo();
            }

            {
                static std::atomic<int> s_diagCount{0};
                if (s_diagCount.fetch_add(1) < 60) {
                    spdlog::debug("[EFFECT-DIAG] out={}x{} sync={} li={} "
                                 "effects={} srcView={} outView={} snapImg={}",
                                 outW, outH, forceSyncReadback, li,
                                 layer.effects.size(),
                                 (uint64_t)srcInfo.imageView,
                                 (uint64_t)gpuLayers[li].textureInfo.imageView,
                                 (li < layerEffectOutputs.size() && layerEffectOutputs[li])
                                     ? (uint64_t)layerEffectOutputs[li]->image() : 0ull);
                }
            }

            // ── Transform ─────────────────────────────────────
            // Premiere Pro behaviour: effects are clip-bounded —
            // they operate at the clip's native resolution and the
            // compositor places the result with the same transform
            // as the un-effected source.  The upload pass already
            // computed the correct transform from the original
            // source dimensions; don't recompute it here, because
            // the effect output is now at the same resolution as
            // the source (we resized the EffectProcessor above).
            //
            // Exception: OTS (On-The-Shoulder) effects intentionally
            // work at full-frame size and override the transform.
            if (ots) {
                setStaticTransform(gpuLayers[li],
                    Compositor::buildViewportTransform(
                        outW, outH, outW, outH, 0,0,1,1,0,false));
            }
            // Non-OTS: keep the transform from the upload pass
            // (already set via buildViewportTransform with correct
            // srcW/srcH during the Upload pass handler).
        } else if (pass.optional) {
            if (m_disabledPasses.insert(pass.name).second)
                spdlog::warn("[RENDER_GRAPH] Effect pass '{}' failed — disabled for session", pass.name);
        }
        break;
    }
}

void CompositeEngine::recordTransitionPass(FrameRecording& frame, uint32_t passIdx)
{
    VkCommandBuffer cmd = frame.cmd;
    const int timingSlot = frame.timingSlot;
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const auto& transitionRenderer = frame.transitionRenderer;
    int& transitionCount = frame.transitionCount;
    auto& layerInfo = frame.layerPasses;
    auto& gpuLayers = frame.gpuLayers;
    const auto& pass = frame.graph.pass(passIdx);

    for (size_t wi = 0; wi < layers.size(); ++wi) {
        if (layerInfo[wi].transitionPassIdx != passIdx)
            continue;
        if (!transitionRenderer || !transitionRenderer->isInitialized())
            break;
        // Fault isolation: skip transition if the A side never
        // uploaded (peer-side enabled is checked further down).
        if (!gpuLayers[wi].enabled)
            break;

        const auto& layer = layers[wi];
        TransitionSourceInfo srcA{};
        TransitionSourceInfo srcB{};
        srcA.textureInfo = gpuLayers[wi].textureInfo;
        srcA.transform   = gpuLayers[wi].transform;
        srcA.crop        = glm::vec4(gpuLayers[wi].cropLeft,
                                     gpuLayers[wi].cropRight,
                                     gpuLayers[wi].cropTop,
                                     gpuLayers[wi].cropBottom);
        srcA.isPacked    = gpuLayers[wi].isPacked;

        size_t pi = SIZE_MAX;
        if (layer.wipePeerClipId == 0) {
            // Singleton transition (fade to/from color)
            if (layer.wipeType == TransitionType::FadeToWhite) {
                srcB.textureInfo = transitionRenderer->whiteDescriptorInfo();
            } else if (layer.wipeType == TransitionType::FadeFromWhite) {
                srcB = srcA;
                srcA = TransitionSourceInfo{};
                srcA.textureInfo = transitionRenderer->whiteDescriptorInfo();
            } else if (layer.wipeType == TransitionType::FadeToBlack) {
                srcB.textureInfo = transitionRenderer->blackDescriptorInfo();
            } else if (layer.wipeType == TransitionType::FadeFromBlack) {
                srcB = srcA;
                srcA = TransitionSourceInfo{};
                srcA.textureInfo = transitionRenderer->blackDescriptorInfo();
            } else if (layer.wipeType == TransitionType::CrossDissolve) {
                // Single-clip dissolve. Honor direction the same way
                // FadeFrom*/FadeTo* do, so the boundary frame is
                // correct:
                //   incoming (fade-in at clip head, leftClipId==0):
                //     mix(transparent, clip, p) — p=0 → transparent,
                //     p=1 → clip. Without the swap, p=0 rendered
                //     mix(clip, transparent, 0)=clip, i.e. a 1-frame
                //     fully-opaque flash before the fade-in.
                //   outgoing (fade-out at clip tail): mix(clip,
                //     transparent, p) — p=0 → clip, p=1 → transparent.
                if (!layer.isWipeOutgoing) {
                    srcB = srcA;
                    srcA = TransitionSourceInfo{};
                    srcA.textureInfo =
                        transitionRenderer->transparentDescriptorInfo();
                } else {
                    srcB.textureInfo =
                        transitionRenderer->transparentDescriptorInfo();
                }
            } else {
                srcB.textureInfo = transitionRenderer->transparentDescriptorInfo();
            }
        } else {
            // Find peer layer
            for (size_t wj = 0; wj < layers.size(); ++wj) {
                if (wj != wi && layers[wj].clipId == layer.wipePeerClipId) {
                    pi = wj;
                    break;
                }
            }
            if (pi == SIZE_MAX || !gpuLayers[pi].enabled) break;
            srcB.textureInfo = gpuLayers[pi].textureInfo;
            srcB.transform   = gpuLayers[pi].transform;
            srcB.crop        = glm::vec4(gpuLayers[pi].cropLeft,
                                         gpuLayers[pi].cropRight,
                                         gpuLayers[pi].cropTop,
                                         gpuLayers[pi].cropBottom);
            srcB.isPacked    = gpuLayers[pi].isPacked;
        }

        GpuTransitionType gt = toGpuTransitionType(layer.wipeType);
        int32_t dirOvr = transitionDirectionOverride(layer.wipeType);
        const uint32_t transitionTargetSlot =
            layerInfo[wi].transitionTargetSlot;

        if (transitionRenderer->render(cmd, srcA, srcB,
            gt, layer.wipeProgress, dirOvr, 0.0f, layer.wipeSoftness,
            static_cast<uint32_t>(timingSlot),
            transitionTargetSlot))
        {
            ++transitionCount;
            gpuLayers[wi].textureInfo =
                transitionRenderer->outputDescriptorInfo(
                    static_cast<uint32_t>(timingSlot),
                    transitionTargetSlot);
            gpuLayers[wi].opacity    = 1.0f;
            gpuLayers[wi].isPacked   = false;
            gpuLayers[wi].isPMA      = false;
            gpuLayers[wi].cropLeft   = 0.0f;
            gpuLayers[wi].cropRight  = 0.0f;
            gpuLayers[wi].cropTop    = 0.0f;
            gpuLayers[wi].cropBottom = 0.0f;
            gpuLayers[wi].blendMode  = BlendMode::Normal;
            setStaticTransform(gpuLayers[wi],
                Compositor::buildViewportTransform(
                    outW, outH, outW, outH,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, false));
            if (pi != SIZE_MAX)
                gpuLayers[pi].enabled = false;
        } else if (pass.optional) {
            // Phase D: transition failed.  Disable for the rest
            // of the session (gpuLayers[wi] keeps pre-transition
            // state — clean visual passthrough).
            if (m_disabledPasses.insert(pass.name).second) {
                spdlog::warn("[RENDER_GRAPH] Transition pass '{}' failed "
                             "— disabled for the rest of this session",
                             pass.name);
            }
        }
        break;
    }
}

void CompositeEngine::recordCompositePass(FrameRecording& frame)
{
    auto& ctx = GpuContext::get();
    auto& slot = *frame.slot;
    VkCommandBuffer cmd = frame.cmd;
    const int timingSlot = frame.timingSlot;
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const auto& compositor = frame.compositor;
    const auto& transitionRenderer = frame.transitionRenderer;
    int& effectLayerCount = frame.effectLayerCount;
    int& effectPassCount = frame.effectPassCount;
    int& transitionCount = frame.transitionCount;
    auto& layerEffectOutputs = frame.resources->effectOutputs;
    auto& layerInfo = frame.layerPasses;
    auto& gpuLayers = frame.gpuLayers;
    auto& layerEffectMaskInfos = frame.effectMaskInfos;
    bool& compOk = frame.compOk;

    // Transition outputs are storage-image writes that this pass
    // immediately samples. The graph's transition resources are
    // logical (the actual per-layer images live in
    // TransitionRenderer), so issue the matching global dependency
    // here instead of relying on an image barrier for a null graph
    // handle. This covers any number of simultaneous transitions.
    if (transitionCount > 0) {
        VkMemoryBarrier transitionBarrier{};
        transitionBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        transitionBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transitionBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &transitionBarrier, 0, nullptr, 0, nullptr);
    }

    // Fold the bottom-to-top render stream at every adjustment
    // marker. Ordinary clip effects have already run above. At a
    // marker we flatten only the layers below it, apply that stack
    // ONCE to the full frame, then continue with layers above.
    std::vector<CompositorLayer> workingLayers;
    workingLayers.reserve(gpuLayers.size());
    bool workingIsSingleFlattenedFrame = false;

    const bool finalPreserveAlpha = compositor->preserveAlpha();
    const bool finalOutputSwizzle = compositor->outputSwizzleRB();
    bool adjustmentReady = m_adjustmentEffectProcessor &&
        m_adjustmentEffectProcessor->isInitialized();
    const bool hasAdjustmentEffects = std::any_of(
        layers.begin(), layers.end(), [](const LayerInfo& layer) {
            return layer.isAdjustmentLayer && !layer.effects.empty();
        });
    if (adjustmentReady && hasAdjustmentEffects)
        adjustmentReady = m_adjustmentEffectProcessor->resize(outW, outH);

    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];
        if (!layer.isAdjustmentLayer) {
            if (gpuLayers[li].enabled) {
                workingLayers.push_back(gpuLayers[li]);
                workingIsSingleFlattenedFrame = false;
            }
            continue;
        }

        if (layer.effects.empty() || workingLayers.empty())
            continue;
        const float adjustmentStrength =
            std::clamp(layer.opacity, 0.0f, 1.0f);
        if (adjustmentStrength <= 0.000001f)
            continue;
        if (!adjustmentReady) {
            static std::atomic<bool> s_warned{false};
            if (!s_warned.exchange(true))
                spdlog::warn("Adjustment-layer effects unavailable; using passthrough");
            continue;
        }

        VkDescriptorImageInfo adjustmentSource{};
        if (workingIsSingleFlattenedFrame &&
            workingLayers.size() == 1) {
            // Consecutive adjustment layers can consume the previous
            // full-frame snapshot directly without another flatten.
            adjustmentSource = workingLayers.front().textureInfo;
        } else {
            compositor->setPreserveAlpha(true);
            compositor->setOutputSwizzleRB(false);
            compositor->setLayers(workingLayers);
            if (!compositor->composite(cmd)) {
                adjustmentReady = false;
                break;
            }
            makeComputeOutputSampleable(cmd, compositor->outputImage());
            adjustmentSource = compositor->outputDescriptorInfo();
        }

        ++effectLayerCount;
        effectPassCount += static_cast<int>(layer.effects.size());
        const std::vector<VkDescriptorImageInfo>* effectMasks =
            (li < layerEffectMaskInfos.size() &&
             !layerEffectMaskInfos[li].empty())
                ? &layerEffectMaskInfos[li] : nullptr;
        const bool adjustmentOk = m_adjustmentEffectProcessor->process(
            cmd, adjustmentSource, layer.effects, effectMasks);
        if (const auto& lut = m_adjustmentEffectProcessor->activeLUT3D())
            slot.retainForCurrentSlot(lut);
        if (!adjustmentOk) {
            spdlog::warn("Adjustment-layer effect stack failed for clip {}",
                         layer.clipId);
            continue;
        }

        auto& snapshot = *layerEffectOutputs[li];
        if (!snapshotEffectOutput(
                cmd, *m_adjustmentEffectProcessor, snapshot,
                ctx, outW, outH)) {
            spdlog::warn("Adjustment-layer output snapshot failed for clip {}",
                         layer.clipId);
            continue;
        }

        VkDescriptorImageInfo adjustmentResult = snapshot.descriptorInfo();
        if (adjustmentStrength < 0.999999f && transitionRenderer &&
            layerInfo[li].transitionTargetSlot != UINT32_MAX) {
            TransitionSourceInfo originalSource{};
            originalSource.textureInfo = adjustmentSource;
            TransitionSourceInfo effectedSource{};
            effectedSource.textureInfo = snapshot.descriptorInfo();
            const uint32_t targetSlot =
                layerInfo[li].transitionTargetSlot;

            if (transitionRenderer->render(
                    cmd, originalSource, effectedSource,
                    GpuTransitionType::Dissolve, adjustmentStrength,
                    -1, 0.0f, -1.0f,
                    static_cast<uint32_t>(timingSlot), targetSlot)) {
                // The next adjustment stack or the final compositor
                // samples this transition output in the same command
                // buffer. Make the compute write visible immediately.
                VkMemoryBarrier adjustmentBlendBarrier{};
                adjustmentBlendBarrier.sType =
                    VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                adjustmentBlendBarrier.srcAccessMask =
                    VK_ACCESS_SHADER_WRITE_BIT;
                adjustmentBlendBarrier.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(
                    cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &adjustmentBlendBarrier,
                    0, nullptr, 0, nullptr);
                adjustmentResult =
                    transitionRenderer->outputDescriptorInfo(
                        static_cast<uint32_t>(timingSlot), targetSlot);
                ++transitionCount;
            } else {
                spdlog::warn(
                    "Adjustment-layer dissolve failed for clip {}; "
                    "using the fully processed frame",
                    layer.clipId);
            }
        }

        workingLayers.clear();
        workingLayers.push_back(fullFrameLayer(adjustmentResult));
        workingIsSingleFlattenedFrame = true;
    }

    compositor->setPreserveAlpha(finalPreserveAlpha);
    compositor->setOutputSwizzleRB(finalOutputSwizzle);
    compositor->setLayers(workingLayers);
    compOk = compositor->composite(cmd);
}

void CompositeEngine::recordReadbackPass(FrameRecording& frame)
{
    VkCommandBuffer cmd = frame.cmd;
    const bool forceSyncReadback = frame.forceSyncReadback;
    const auto& compositor = frame.compositor;
    bool& compOk = frame.compOk;
    bool& readbackOk = frame.readbackOk;
    bool& nv12ReadbackOk = frame.nv12ReadbackOk;

    if (compOk) {
        // Export to an 8-bit 4:2:0 encoder reads back NV12 (1.5
        // bytes/pixel, converted on the GPU); anything that can't
        // falls back to BGRA.
        if (compositor->nv12Readback() && forceSyncReadback)
            nv12ReadbackOk = compositor->recordNv12Readback(cmd);
        readbackOk = nv12ReadbackOk || compositor->recordReadback(cmd);
    }
}
