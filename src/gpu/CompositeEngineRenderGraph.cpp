/*
 * CompositeEngineRenderGraph.cpp -- CompositeEngine::compositeViaRenderGraph(),
 * the DAG render-graph compositing path (the only composite path), plus its
 * TransitionType->GpuTransitionType static helpers.
 * Extracted from CompositeEngine.cpp (behavior-preserving).
 */

// define VK_NO_PROTOTYPES first.
#include <volk.h>

#include "CompositeEngine.h"
#include "render_graph/GpuRenderGraph.h"
#include "cache/CachePolicy.h"
#include "StagingRing.h"
#include "CompositeServiceLayerBuild.h"  // rt::LayerInfo
#include "CompositeServiceBlend.h"       // rasterizeMasks
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

// ============================================================================
// Static helpers: Timeline TransitionType -> GpuTransitionType mapping
// ============================================================================

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

struct LogicalLayerSize
{
    uint32_t width;
    uint32_t height;
};

// Dimensions of the layer image as seen by compositor layerUV. Packed-alpha
// sources store the matte below the RGB image, so their logical image height
// is half the backing texture height.
static LogicalLayerSize logicalLayerSize(const LayerInfo& layer,
                                         uint32_t fallbackW,
                                         uint32_t fallbackH) noexcept
{
    uint32_t width = layer.frameWidth;
    uint32_t height = layer.frameHeight;
    if (layer.frame) {
        if (width == 0) width = layer.frame->width;
        if (height == 0) height = layer.frame->height;
    }
    if (width == 0) width = fallbackW;
    if (height == 0) height = fallbackH;
    if (layer.isPacked && height > 1) height /= 2;
    return {width, height};
}

static glm::mat4 viewportTransformForSample(
    const LayerTransformSample& sample, const LayerInfo& layer,
    uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH)
{
    return Compositor::buildViewportTransform(
        srcW, srcH, outW, outH,
        sample.posX, sample.posY, sample.scX, sample.scY, sample.rot,
        layer.containFit, sample.anchorX, sample.anchorY, layer.srcRotation);
}

static void setLayerTransforms(CompositorLayer& compositorLayer,
                               const LayerInfo& layer,
                               uint32_t srcW, uint32_t srcH,
                               uint32_t outW, uint32_t outH)
{
    compositorLayer.transform = Compositor::buildViewportTransform(
        srcW, srcH, outW, outH,
        layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
        layer.containFit, layer.anchorX, layer.anchorY, layer.srcRotation);
    compositorLayer.motionSampleCount =
        std::clamp(layer.motionSampleCount, 1, 8);
    if (compositorLayer.motionSampleCount > 1) {
        compositorLayer.motionTransformStart = viewportTransformForSample(
            layer.motionStart, layer, srcW, srcH, outW, outH);
        compositorLayer.motionTransformEnd = viewportTransformForSample(
            layer.motionEnd, layer, srcW, srcH, outW, outH);
    } else {
        compositorLayer.motionTransformStart = compositorLayer.transform;
        compositorLayer.motionTransformEnd = compositorLayer.transform;
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

static MaskRasterTransform legacyMaskTransform(const LayerInfo& layer,
                                               uint32_t srcW, uint32_t srcH,
                                               uint32_t outW, uint32_t outH)
{
    const glm::mat4 outputToSource = Compositor::buildViewportTransform(
        srcW, srcH, outW, outH,
        layer.posX, layer.posY, layer.scX, layer.scY, layer.rot,
        layer.containFit, layer.anchorX, layer.anchorY, layer.srcRotation);
    auto map = [&](float u, float v) {
        glm::vec4 p = outputToSource * glm::vec4(u, v, 0.0f, 1.0f);
        if (std::abs(p.w) > 1.0e-8f) p /= p.w;
        return glm::vec2(p.x * static_cast<float>(srcW),
                         p.y * static_cast<float>(srcH));
    };
    const glm::vec2 p0 = map(0.0f, 0.0f);
    const glm::vec2 px = map(1.0f, 0.0f);
    const glm::vec2 py = map(0.0f, 1.0f);
    return {{px.x - p0.x, py.x - p0.x, p0.x,
             px.y - p0.y, py.y - p0.y, p0.y}};
}

static bool containsLegacyMasks(const std::vector<MaskRenderState>& masks)
{
    return std::any_of(masks.begin(), masks.end(), [](const auto& mask) {
        return mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame;
    });
}

static uint64_t layerMaskHash(const std::vector<MaskRenderState>& masks,
                              const LayerInfo& layer,
                              uint32_t srcW, uint32_t srcH,
                              uint32_t outW, uint32_t outH)
{
    uint64_t hash = hashMaskStates(masks, srcW, srcH);
    if (!containsLegacyMasks(masks)) return hash;
    const auto xf = legacyMaskTransform(layer, srcW, srcH, outW, outH);
    const auto* bytes = reinterpret_cast<const uint8_t*>(xf.m);
    for (size_t i = 0; i < sizeof(xf.m); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::vector<uint8_t> rasterizeLayerMasks(
    const std::vector<MaskRenderState>& masks, const LayerInfo& layer,
    uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH)
{
    std::vector<MaskRenderState> sourceMasks;
    std::vector<MaskRenderState> legacyMasks;
    sourceMasks.reserve(masks.size());
    legacyMasks.reserve(masks.size());
    for (const auto& mask : masks) {
        if (mask.coordinateSpace == MaskCoordinateSpace::LegacySequenceFrame)
            legacyMasks.push_back(mask);
        else
            sourceMasks.push_back(mask);
    }

    if (legacyMasks.empty())
        return rasterizeMasks(sourceMasks, srcW, srcH);

    const auto xf = legacyMaskTransform(layer, srcW, srcH, outW, outH);
    if (sourceMasks.empty())
        return rasterizeMasks(legacyMasks, srcW, srcH, &xf);

    auto pixels = rasterizeMasks(sourceMasks, srcW, srcH);
    const auto legacyPixels = rasterizeMasks(legacyMasks, srcW, srcH, &xf);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(std::min(
            255, static_cast<int>(pixels[i]) +
                 static_cast<int>(legacyPixels[i])));
    }
    return pixels;
}

// ============================================================================
// Render graph path (formerly the "alternative path"; now the only path)
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::compositeViaRenderGraph(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool gpuDisplayMode,
    Compositor* compositor,
    EffectProcessor* effectProcessor,
    TransitionRenderer* transitionRenderer,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    std::chrono::high_resolution_clock::time_point& perfTgpuUp,
    std::chrono::high_resolution_clock::time_point& perfTcomp,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount,
    bool allowLruInsert,
    bool forceSyncReadback)
{
    using namespace render_graph;

    auto& ctx = GpuContext::get();

    // ── Ensure texture pool is large enough ──────────────────────────
    for (uint32_t si = 0; si < kTemporalSubmissionSlots; ++si) {
        while (m_gpuLayerTextures[si].size() < layers.size())
            m_gpuLayerTextures[si].push_back(std::make_unique<Texture>());
        m_gpuLayerTexKeys[si].resize(m_gpuLayerTextures[si].size());
        while (m_layerEffectOutputs[si].size() < layers.size())
            m_layerEffectOutputs[si].push_back(std::make_unique<Texture>());
        while (m_gpuLayerTexturesAlt[si].size() < layers.size())
            m_gpuLayerTexturesAlt[si].push_back(std::make_unique<Texture>());
        while (m_gpuMaskTextures[si].size() < layers.size())
            m_gpuMaskTextures[si].push_back(std::make_unique<Texture>());
        if (m_maskCache[si].size() < m_gpuMaskTextures[si].size())
            m_maskCache[si].resize(m_gpuMaskTextures[si].size());
        if (m_effectMaskTextures[si].size() < layers.size())
            m_effectMaskTextures[si].resize(layers.size());
        if (m_effectMaskCache[si].size() < layers.size())
            m_effectMaskCache[si].resize(layers.size());
        while (m_gpuTemporalSourceTextures[si].size() < layers.size())
            m_gpuTemporalSourceTextures[si].push_back(
                std::make_unique<Texture>());
        m_gpuTemporalSourceTexKeys[si].resize(
            m_gpuTemporalSourceTextures[si].size());
        while (m_layerTemporalOutputs[si].size() < layers.size())
            m_layerTemporalOutputs[si].push_back(
                std::make_unique<Texture>());
    }

    // ── Set up command buffer (reuse existing triple-buffer slot) ────
    if (!m_gpuSubmission) {
        m_gpuSubmission = std::make_unique<GpuWorkSubmission>();
        // UPGRADE_PLAN Path C (2026-05-22): allocate the per-frame
        // composite cmd buffer from the graphics-family pool so the
        // submission below can target the graphics queue, off the
        // prefetch workers' compute-queue contention path.  Vulkan
        // forbids submitting a cmdbuf to a queue of a different family
        // than its pool — pool family change and submit queue change
        // are paired.  graphicsCmdPool() falls back to cmdPool() when
        // graphics and compute share a family, preserving the
        // single-queue path on simpler devices.
        m_gpuSubmission->init(ctx.vkDevice(), ctx.graphicsCmdPool().handle());
    }
    auto& slot = *m_gpuSubmission;
    if (!slot.beginRecording()) {
        // beginRecording() fails when the previous frame's fence cannot be
        // waited on — almost always VK_ERROR_DEVICE_LOST surfacing through
        // vkWaitForFences.  If we ignore this and keep recording, every
        // subsequent vkCmd* call writes into an unstarted command buffer
        // and the driver raises an SEH exception inside FrameProducer
        // ("Unknown exception in produceFrame").  Surface device-lost so
        // the next composite call stops the pipeline and triggers the fatal
        // Restart/Quit flow instead of repeating the failure every frame.
        GpuContext::get().signalDeviceLost();
        m_gpuCompositeState = -1;
        return nullptr;
    }
    VkCommandBuffer cmd = slot.cmdBuffer();
    const int timingSlot = slot.currentSlot();
    auto& gpuLayerTextures =
        m_gpuLayerTextures[static_cast<size_t>(timingSlot)];
    auto& gpuLayerTexKeys =
        m_gpuLayerTexKeys[static_cast<size_t>(timingSlot)];
    auto& gpuLayerTexturesAlt =
        m_gpuLayerTexturesAlt[static_cast<size_t>(timingSlot)];
    auto& layerEffectOutputs =
        m_layerEffectOutputs[static_cast<size_t>(timingSlot)];
    auto& gpuMaskTextures = m_gpuMaskTextures[static_cast<size_t>(timingSlot)];
    auto& maskCache = m_maskCache[static_cast<size_t>(timingSlot)];
    auto& effectMaskTextures =
        m_effectMaskTextures[static_cast<size_t>(timingSlot)];
    auto& effectMaskCache =
        m_effectMaskCache[static_cast<size_t>(timingSlot)];

    // Resolve last frame's timestamps from this ring slot.  beginRecording()
    // above waited for the fence so the queries are guaranteed ready.
    resolveTimingsForSlot(timingSlot);

    // Reset + write start timestamp for THIS frame.
    if (m_timingInitialized && m_timingPools[timingSlot] != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd, m_timingPools[timingSlot], 0, kTimingMarkers);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            m_timingPools[timingSlot], 0);
    }

    m_uploadManager->beginFrame(cmd, slot.currentSlot());
    m_uploadManager->setTextureCache(m_gpuTexCache.get());

    // ── Initialize GPU texture cache if needed (same logic as old path) ─
    if (!m_gpuTexCache) {
        const auto memStats = ctx.allocator().queryStats();
        const size_t gpuVram = memStats.deviceLocalBudgetBytes;
        size_t budget;
        if (m_cachePolicy) {
            budget = m_cachePolicy->recommendedGpuTexCacheBudget(gpuVram);
            m_cachePolicy->onGpuAvailable(gpuVram);
        } else {
            budget = std::clamp<size_t>(
                gpuVram / 4, 512ull * 1024 * 1024, 8ull * 1024 * 1024 * 1024);
        }
        m_gpuTexCache = std::make_unique<GpuTextureCache>(budget);
        // UPGRADE_PLAN 2026-05-22 v3 — Premiere-style bounded working
        // set.  Cap the entry count to a small absolute number, not a
        // percentage of VRAM.  CachePolicy computes the recommended
        // value based on installed VRAM but stays in the 40-180 range
        // even on 24 GB cards.  Without this cap the texture cache
        // grew unbounded (1469 entries / 11.5 GB observed at the 50s
        // mark in 21:51 perf logs), VMA-tracked VRAM crossed the OS
        // budget, and the driver started paging textures to system
        // RAM — submit stalls 50-250 ms / frame.
        if (m_cachePolicy) {
            const size_t maxEntries =
                m_cachePolicy->recommendedGpuTexCacheMaxEntries(gpuVram);
            m_gpuTexCache->setMaxEntries(maxEntries);
            spdlog::info("[PERF] GpuTexCache max entries: {} "
                         "(Premiere-style bounded working set)",
                         maxEntries);
        }
        m_uploadManager->setTextureCache(m_gpuTexCache.get());
        // Register for diagnostic visibility from MediaPool's perf-dump
        // (see GpuContext::registerGpuTextureCache rationale).  Cleared
        // in shutdown() / clearTextureCache().
        ctx.registerGpuTextureCache(m_gpuTexCache.get());
        if (m_cachePolicy) {
            m_cachePolicy->setVramPressureFn(
                [this](size_t* budgetOut) -> bool {
                    if (!m_gpuTexCache) return false;
                    *budgetOut = m_gpuTexCache->budget();

                    // Real pressure signal: query VMA's view of device-
                    // local VRAM use vs the OS budget.  GpuTextureCache's
                    // OWN pressure (>90% of self-budget) was the previous
                    // trigger but it fires far too late — the cache
                    // budget defaults to 60% of VRAM, while VMA total
                    // (cache + prefetch pool + compositor outputs +
                    // FrameCache orphan textures + swapchain + staging
                    // buffers + Spine/Effect working sets) easily
                    // exceeds the OS budget while the cache itself is
                    // only at 60% of its 14 GB cap.  At that point the
                    // driver starts paging textures to system RAM and
                    // submit latencies jump 50-250 ms (see CACHE-DUMP
                    // sequence at 21:41:53 → 21:42:07 in
                    // logs/perf_log.txt: gpuTexMB stable at 9-10 GB,
                    // vmaMB climbed past vmaBudget).
                    //
                    // Pressure trigger: VMA used > 85% of the OS-reported
                    // budget.  At that point we ask the coordinator to
                    // both (a) evict FrameCache GPU-co-owned entries and
                    // (b) shrink the texture-cache budget so it evicts
                    // some entries itself.
                    const auto memStats = GpuContext::get().allocator().queryStats();
                    if (memStats.deviceLocalBudgetBytes == 0) {
                        // VMA didn't report a budget (older drivers /
                        // headless).  Fall back to the original self-
                        // pressure check.
                        return m_gpuTexCache->isUnderPressure();
                    }
                    const double vmaUsage =
                        double(memStats.deviceLocalUsedBytes) /
                        double(memStats.deviceLocalBudgetBytes);
                    return vmaUsage > 0.85;
                });
            // Bidirectional pressure: when the CPU FrameCache is over its
            // high-water mark, CachePolicy calls this to shrink the
            // GPU budget (which evicts cold textures and releases their
            // shared_ptr ownerships).  GpuTextureCache::setBudget triggers
            // eviction down to the new size.  Called back to restore the
            // original budget when CPU pressure subsides.
            m_cachePolicy->setGpuBudgetFn(
                [this](size_t newBudget) {
                    if (m_gpuTexCache)
                        m_gpuTexCache->setBudget(newBudget);
                });
        }
        spdlog::info("[PERF] GpuTexCache budget: {:.0f} MB (GPU VRAM: {:.0f} MB)",
                     budget / 1048576.0, gpuVram / 1048576.0);
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 1: Build the Render Graph
    // ══════════════════════════════════════════════════════════════════

    GpuRenderGraph graph;

    // Masks are clip-local assets. Keep one authoritative logical size for
    // every layer so clip masks, effect masks, and the layer transform all use
    // the same normalized coordinate system.
    std::vector<LogicalLayerSize> logicalLayerSizes;
    logicalLayerSizes.reserve(layers.size());
    for (const auto& layer : layers)
        logicalLayerSizes.push_back(logicalLayerSize(layer, outW, outH));

    // ── Declare the final composite output resource ────────────────
    ResourceId outputTexId = graph.declareResource({
        .type = ResourceType::StorageImage,
        .name = "compositeOutput",
        .width = outW,
        .height = outH,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_SAMPLED_BIT,
        .transient = true,
        .external = true,    // owned by Compositor
    });

    // ── Declare per-layer textures and build pass lists ────────────
    struct LayerPassInfo {
        ResourceId layerTexId{kInvalidResource};
        ResourceId temporalTexId{kInvalidResource};
        ResourceId maskTexId{kInvalidResource};
        uint32_t   uploadPassIdx{UINT32_MAX};
        uint32_t   temporalUploadPassIdx{UINT32_MAX};
        uint32_t   effectPassIdx{UINT32_MAX};
        uint32_t   transitionPassIdx{UINT32_MAX};
        uint32_t   transitionTargetSlot{UINT32_MAX};
    };
    std::vector<LayerPassInfo> layerInfo(layers.size());

    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];

        // Adjustment markers have no source image or standalone graph pass.
        // They are evaluated at their stack boundary in the Composite pass.
        if (layer.isAdjustmentLayer)
            continue;

        // ── Declare this layer's texture resource ──────────────────
        ResourceId layerTexId = graph.declareResource({
            .type = ResourceType::Texture,
            .name = "layer" + std::to_string(li) + "_tex",
            .image = layer.gpuTextureReady
                ? VK_NULL_HANDLE
                : (gpuLayerTextures[li]
                    ? gpuLayerTextures[li]->image() : VK_NULL_HANDLE),
            .width = outW,
            .height = outH,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_STORAGE_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .currentLayout = layer.gpuTextureReady
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED,
            .currentAccess = layer.gpuTextureReady
                ? ResourceAccess::ShaderRead : ResourceAccess::Undefined,
            .transient = true,
            .external = true,    // owned by this submission slot
        });
        layerInfo[li].layerTexId = layerTexId;

        if (!layer.gpuTextureReady) {
            // ── Add Upload pass for this layer ─────────────────────
            std::vector<VkBufferImageCopy> regions(1);
            regions[0].bufferOffset = 0;
            regions[0].bufferRowLength = 0;
            regions[0].bufferImageHeight = 0;
            regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[0].imageSubresource.mipLevel = 0;
            regions[0].imageSubresource.baseArrayLayer = 0;
            regions[0].imageSubresource.layerCount = 1;
            regions[0].imageOffset = {0, 0, 0};
            regions[0].imageExtent = {outW, outH, 1};

            layerInfo[li].uploadPassIdx = graph.addUploadPass(
                "uploadLayer" + std::to_string(li),
                layerTexId,
                VK_NULL_HANDLE, // staging buffer assigned at execution time
                0, regions, true);
        }

        const bool hasTemporal =
            layer.temporalMode != 0 && layer.temporalPhase > 0.000001f &&
            (layer.temporalGpuTextureReady || layer.temporalFrame);
        if (hasTemporal) {
            ResourceId temporalTexId = graph.declareResource({
                .type = ResourceType::Texture,
                .name = "layer" + std::to_string(li) + "_temporalSource",
                .image = layer.temporalGpuTextureReady
                    ? VK_NULL_HANDLE
                    : (m_gpuTemporalSourceTextures[timingSlot][li]
                        ? m_gpuTemporalSourceTextures[timingSlot][li]->image()
                        : VK_NULL_HANDLE),
                .width = layer.frameWidth ? layer.frameWidth : outW,
                .height = layer.frameHeight ? layer.frameHeight : outH,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .currentLayout = layer.temporalGpuTextureReady
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_UNDEFINED,
                .currentAccess = layer.temporalGpuTextureReady
                    ? ResourceAccess::ShaderRead : ResourceAccess::Undefined,
                .transient = true,
                .external = true,
            });
            layerInfo[li].temporalTexId = temporalTexId;

            if (!layer.temporalGpuTextureReady) {
                std::vector<VkBufferImageCopy> temporalRegions(1);
                temporalRegions[0].imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                temporalRegions[0].imageSubresource.layerCount = 1;
                temporalRegions[0].imageExtent = {
                    layer.frameWidth ? layer.frameWidth : outW,
                    layer.frameHeight ? layer.frameHeight : outH, 1};
                layerInfo[li].temporalUploadPassIdx = graph.addUploadPass(
                    "uploadTemporalSource" + std::to_string(li),
                    temporalTexId, VK_NULL_HANDLE, 0, temporalRegions, true);
            }
        }

        // ── Declare mask texture if needed ─────────────────────────
        if (!layer.masks.empty()) {
            const auto maskSize = logicalLayerSizes[li];
            ResourceId maskTexId = graph.declareResource({
                .type = ResourceType::Texture,
                .name = "layer" + std::to_string(li) + "_mask",
                .image = gpuMaskTextures[li]
                    ? gpuMaskTextures[li]->image() : VK_NULL_HANDLE,
                .width = maskSize.width,
                .height = maskSize.height,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT
                            | VK_IMAGE_USAGE_STORAGE_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                .currentLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .currentAccess = ResourceAccess::Undefined,
                .transient = true,
                .external = true,
            });
            layerInfo[li].maskTexId = maskTexId;

            std::vector<VkBufferImageCopy> maskRegions(1);
            maskRegions[0].bufferOffset = 0;
            maskRegions[0].bufferRowLength = 0;
            maskRegions[0].bufferImageHeight = 0;
            maskRegions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            maskRegions[0].imageSubresource.mipLevel = 0;
            maskRegions[0].imageSubresource.baseArrayLayer = 0;
            maskRegions[0].imageSubresource.layerCount = 1;
            maskRegions[0].imageOffset = {0, 0, 0};
            maskRegions[0].imageExtent = {maskSize.width, maskSize.height, 1};

            (void)graph.addUploadPass(
                "uploadMask" + std::to_string(li),
                maskTexId, VK_NULL_HANDLE, 0, maskRegions, true);
        }

        // ── Add ONE Effect pass per layer ──────────────────────────
        // process() runs ALL effects in one call — one pass per layer
        // prevents O(N²) dispatches and topo-sort interleaving (#40).
        if (!layer.effects.empty() || hasTemporal) {
            ResourceId effectOutput = graph.declareResource({
                .type = ResourceType::StorageImage,
                .name = "layer" + std::to_string(li) + "_effectOut",
                .width = outW, .height = outH,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .transient = true, .external = true,
            });
            std::vector<ResourceId> effectInputs{layerTexId};
            if (hasTemporal)
                effectInputs.push_back(layerInfo[li].temporalTexId);
            layerInfo[li].effectPassIdx = graph.addComputePass(
                "effect_layer" + std::to_string(li), PassType::Effect,
                std::move(effectInputs), {effectOutput},
                VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1, true, false);
        }
    }

    // ── Add Transition passes ──────────────────────────────────────
    uint32_t transitionTargetCount = 0;
    for (size_t wi = 0; wi < layers.size(); ++wi) {
        if (layers[wi].wipeProgress < 0.0f)
            continue;
        if (!layers[wi].isWipeOutgoing && layers[wi].wipePeerClipId != 0)
            continue;

        if (wi < layerInfo.size() && layerInfo[wi].layerTexId != kInvalidResource) {
            ResourceId transitionOutput = graph.declareResource({
                .type = ResourceType::StorageImage,
                .name = "transition" + std::to_string(wi) + "_out",
                .width = outW, .height = outH,
                .format = VK_FORMAT_R8G8B8A8_UNORM,
                .usageFlags = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .transient = true,
                .external = true,    // managed by TransitionRenderer
            });

            // Two-clip transitions sample the peer's texture AND read its
            // CPU-side transform inside the Transition pass handler. Listing
            // the peer's layerTexId as an input forces the graph's topo sort
            // to run the peer's Upload pass FIRST — without this the
            // Transition handler races the Upload, reads a default
            // identity-matrix transform, and renders the peer at scale 100%
            // / center for the first few frames of a cross dissolve until
            // the upload finally lands and rebuilds the cached transform.
            // Depend on the post-effect resource when present. Temporal
            // interpolation is recorded in that same pass before effects, so
            // this guarantees both operations finish before a transition
            // reads the mutable gpuLayers descriptor.
            const ResourceId wipeInput =
                layerInfo[wi].effectPassIdx != UINT32_MAX
                    ? graph.pass(layerInfo[wi].effectPassIdx).outputs[0]
                    : layerInfo[wi].layerTexId;
            std::vector<ResourceId> transitionInputs{wipeInput};
            if (layers[wi].wipePeerClipId != 0) {
                for (size_t wj = 0; wj < layers.size(); ++wj) {
                    if (wj == wi) continue;
                    if (layers[wj].clipId != layers[wi].wipePeerClipId) continue;
                    if (wj < layerInfo.size() &&
                        layerInfo[wj].layerTexId != kInvalidResource)
                    {
                        const ResourceId peerInput =
                            layerInfo[wj].effectPassIdx != UINT32_MAX
                                ? graph.pass(layerInfo[wj].effectPassIdx).outputs[0]
                                : layerInfo[wj].layerTexId;
                        transitionInputs.push_back(peerInput);
                    }
                    break;
                }
            }

            uint32_t tpIdx = graph.addComputePass(
                "transition_" + std::to_string(wi),
                PassType::Transition,
                transitionInputs, {transitionOutput},
                VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1,
                true, false);
            layerInfo[wi].transitionPassIdx = tpIdx;
            // Dense per-frame numbering avoids retaining a full-resolution
            // output image for every layer index that has ever transitioned.
            layerInfo[wi].transitionTargetSlot = transitionTargetCount++;
        }
    }

    // Adjustment dissolves are evaluated inside the final Composite pass,
    // after the layers below each marker have been flattened and processed.
    // Reserve dense TransitionRenderer targets for partial-strength markers;
    // strength 0 and 1 are handled without an extra blend dispatch.
    for (size_t li = 0; li < layers.size(); ++li) {
        if (!layers[li].isAdjustmentLayer)
            continue;
        if (layers[li].opacity <= 0.000001f ||
            layers[li].opacity >= 0.999999f)
            continue;
        layerInfo[li].transitionTargetSlot = transitionTargetCount++;
    }

    // ── Collect final layer texture IDs for composite pass ─────────
    std::vector<ResourceId> compositeInputs;
    compositeInputs.reserve(layers.size());
    for (auto& li : layerInfo) {
        // If the layer had a transition, use the transition output;
        // if it had effects, use the last effect output;
        // otherwise use the layer texture.
        ResourceId finalTex = li.layerTexId;
        if (li.transitionPassIdx != UINT32_MAX)
            finalTex = graph.pass(li.transitionPassIdx).outputs[0];
        else if (li.effectPassIdx != UINT32_MAX)
            finalTex = graph.pass(li.effectPassIdx).outputs[0];

        if (finalTex != kInvalidResource)
            compositeInputs.push_back(finalTex);
    }

    // ── Add Composite pass ─────────────────────────────────────────
    (void)graph.addComputePass(
        "finalComposite", PassType::Composite,
        compositeInputs, {outputTexId},
        VK_NULL_HANDLE, VK_NULL_HANDLE, {}, {}, 1, 1, 1,
        false, true);  // not optional, fatal

    // ── Add Readback pass (if needed) ──────────────────────────────
    bool needsReadback = !gpuDisplayMode || scrubMode;
    if (needsReadback) {
        std::vector<VkBufferImageCopy> readbackRegions(1);
        readbackRegions[0].bufferOffset = 0;
        readbackRegions[0].bufferRowLength = 0;
        readbackRegions[0].bufferImageHeight = 0;
        readbackRegions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        readbackRegions[0].imageSubresource.mipLevel = 0;
        readbackRegions[0].imageSubresource.baseArrayLayer = 0;
        readbackRegions[0].imageSubresource.layerCount = 1;
        readbackRegions[0].imageOffset = {0, 0, 0};
        readbackRegions[0].imageExtent = {outW, outH, 1};

        (void)graph.addReadbackPass(
            "readback", outputTexId,
            VK_NULL_HANDLE, 0, readbackRegions, true);
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 2: Compile the graph (topo sort + barriers)
    // ══════════════════════════════════════════════════════════════════

    if (!graph.compile(ctx.vkDevice())) {
        spdlog::warn("[RENDER_GRAPH] Graph compilation failed — falling back");
        slot.endRecording();
        m_uploadManager->endFrame();
        return nullptr;
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 3: Execute passes in topological order
    // ══════════════════════════════════════════════════════════════════

    // Local state needed during execution
    std::vector<CompositorLayer> gpuLayers;
    gpuLayers.reserve(layers.size());
    std::vector<VkDescriptorImageInfo> temporalPrimarySourceInfos(layers.size());
    std::vector<VkDescriptorImageInfo> temporalSourceInfos(layers.size());
    bool uploadOk = true;
    bool compOk = false;
    bool readbackOk = false;
    bool uploadsSeen = false;

    // Pre-allocate gpuLayers from layer info.
    // For layers with gpuTextureReady=true (Spine renders, cached textures),
    // populate the CompositorLayer immediately — these won't have an
    // Upload pass in the graph.
    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];

        if (layer.isAdjustmentLayer) {
            CompositorLayer markerSlot;
            markerSlot.enabled = false;
            gpuLayers.push_back(markerSlot);
            continue;
        }

        if (layer.gpuCacheBacked) {
            m_uploadManager->pinCachedTexture(
                layer.gpuCacheMediaId, layer.gpuCacheFrameNumber,
                layer.gpuCacheTier);
        }
        if (layer.temporalGpuCacheBacked) {
            m_uploadManager->pinCachedTexture(
                layer.temporalGpuCacheMediaId,
                layer.temporalGpuCacheFrameNumber,
                layer.temporalGpuCacheTier);
        }

        CompositorLayer cl;
        cl.enabled = true;
        cl.opacity = layer.opacity;
        cl.blendMode = static_cast<BlendMode>(layer.blendMode);
        cl.isPacked = layer.isPacked;
        cl.isPMA = layer.isPMA;
        cl.needsSwapRB = layer.needsSwapRB;
        cl.cropLeft  = layer.cropL / 100.0f;
        cl.cropRight = layer.cropR / 100.0f;
        cl.cropTop   = layer.cropT / 100.0f;
        cl.cropBottom= layer.cropB / 100.0f;
        if (layer.contentBoundsValid) {
            cl.contentLeft = layer.contentLeft;
            cl.contentTop = layer.contentTop;
            cl.contentRight = layer.contentRight;
            cl.contentBottom = layer.contentBottom;
        }

        if (layer.gpuTextureReady) {
            const auto [srcW, srcH] = logicalLayerSizes[li];
            cl.textureInfo = layer.gpuDescriptor;
            setLayerTransforms(cl, layer, srcW, srcH, outW, outH);
            temporalPrimarySourceInfos[li] = layer.gpuDescriptor;
        }
        if (layer.temporalGpuTextureReady)
            temporalSourceInfos[li] = layer.temporalGpuDescriptor;

        gpuLayers.push_back(cl);
    }

    // ── Upload mask textures (same logic as old monolithic path) ──
    // The evaluated mask state travels in layer.masks (snapshotted at
    // layer-build time). A per-slot hash cache skips the CPU rasterize +
    // upload when the mask state, clip, and dimensions are unchanged. The
    // raster lives in native clip space and the composite shader samples it
    // with the same layerUV used for the clip image.
    for (size_t li = 0; li < layers.size() && li < gpuLayers.size(); ++li) {
        const auto& layer = layers[li];
        if (layer.masks.empty())
            continue;
        const auto [srcW, srcH] = logicalLayerSizes[li];
        const uint64_t maskHash = layerMaskHash(
            layer.masks, layer, srcW, srcH, outW, outH);
        auto& cache = maskCache[li];
        if (cache.valid && cache.clipId == layer.clipId &&
            cache.stateHash == maskHash &&
            gpuMaskTextures[li] &&
            gpuMaskTextures[li]->image() != VK_NULL_HANDLE)
        {
            gpuLayers[li].hasMask = true;
            gpuLayers[li].maskTextureInfo = cache.desc;
            continue;
        }
        auto maskPixels = rasterizeLayerMasks(
            layer.masks, layer, srcW, srcH, outW, outH);
        VkDescriptorImageInfo maskDesc{};
        if (m_uploadManager->uploadMask(
                maskPixels, *gpuMaskTextures[li], srcW, srcH, maskDesc))
        {
            gpuLayers[li].hasMask = true;
            gpuLayers[li].maskTextureInfo = maskDesc;
            uploadsSeen = true;
            cache.clipId    = layer.clipId;
            cache.stateHash = maskHash;
            cache.desc      = maskDesc;
            cache.valid     = true;
        } else {
            cache.valid = false;
        }
    }

    // ── Upload effect-mask textures (Premiere Pro: a mask on an effect
    // limits where that effect applies). Effect masks are rasterized directly
    // in the clip's source-pixel grid. Uploaded here — before the pass walk — so
    // the global transfer→compute barrier covers these transfers exactly
    // like the clip-mask uploads above.  The Effect pass hands the
    // descriptors to EffectProcessor::process(), which mixes
    // original/effected per pixel after each masked effect.
    std::vector<std::vector<VkDescriptorImageInfo>> layerEffectMaskInfos(
        layers.size());
    for (size_t li = 0; li < layers.size() && li < gpuLayers.size(); ++li) {
        const auto& layer = layers[li];
        if (layer.effects.empty()) continue;
        bool anyEffectMask = false;
        for (const auto& fxSnap : layer.effects)
            if (!fxSnap.masks.empty()) { anyEffectMask = true; break; }
        if (!anyEffectMask || li >= effectMaskTextures.size()) continue;

        // Effect inputs and their masks share the same native clip grid.
        const auto [srcW, srcH] = logicalLayerSizes[li];

        auto& texPool   = effectMaskTextures[li];
        auto& cachePool = effectMaskCache[li];
        while (texPool.size() < layer.effects.size())
            texPool.push_back(std::make_unique<Texture>());
        if (cachePool.size() < layer.effects.size())
            cachePool.resize(layer.effects.size());

        auto& maskInfos = layerEffectMaskInfos[li];
        maskInfos.assign(layer.effects.size(), VkDescriptorImageInfo{});
        for (size_t fi = 0; fi < layer.effects.size(); ++fi) {
            const auto& fxSnap = layer.effects[fi];
            if (fxSnap.masks.empty()) continue;
            const uint64_t fxHash = layerMaskHash(
                fxSnap.masks, layer, srcW, srcH, outW, outH) ^ fxSnap.effectId;
            auto& fxCache = cachePool[fi];
            if (fxCache.valid && fxCache.clipId == layer.clipId &&
                fxCache.stateHash == fxHash &&
                texPool[fi] && texPool[fi]->image() != VK_NULL_HANDLE)
            {
                maskInfos[fi] = fxCache.desc;
                continue;
            }
            auto fxMaskPixels = rasterizeLayerMasks(
                fxSnap.masks, layer, srcW, srcH, outW, outH);
            VkDescriptorImageInfo fxMaskDesc{};
            if (m_uploadManager->uploadMask(
                    fxMaskPixels, *texPool[fi], srcW, srcH, fxMaskDesc))
            {
                maskInfos[fi] = fxMaskDesc;
                uploadsSeen = true;
                fxCache.clipId    = layer.clipId;
                fxCache.stateHash = fxHash;
                fxCache.desc      = fxMaskDesc;
                fxCache.valid     = true;
            } else {
                fxCache.valid = false;
            }
        }
    }

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
    bool uploadTimestampWritten = false;
    bool effectTimestampWritten = false;
    const bool timingActive = m_timingInitialized &&
                              m_timingPools[timingSlot] != VK_NULL_HANDLE;
    for (uint32_t passIdx : graph.topologicalOrder()) {
        const auto& pass = graph.pass(passIdx);

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
        switch (pass.type) {
        case PassType::Upload: {
            uploadsSeen = true;
            // Find which layer this upload belongs to
            for (size_t li = 0; li < layers.size(); ++li) {
                if (layerInfo[li].uploadPassIdx == passIdx) {
                    const auto& layer = layers[li];

                    if (layer.gpuTextureReady) {
                        // GPU-resident texture — set up CompositorLayer directly
                        uint32_t srcW = layer.frameWidth  ? layer.frameWidth  : outW;
                        uint32_t srcH = layer.frameHeight ? layer.frameHeight : outH;
                        if (layer.isPacked && srcH > 1) srcH /= 2;
                        gpuLayers[li].textureInfo = layer.gpuDescriptor;
                        setLayerTransforms(gpuLayers[li], layer,
                                           srcW, srcH, outW, outH);
                    } else {
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
                    }
                    break;
                }
                if (layerInfo[li].temporalUploadPassIdx == passIdx) {
                    const auto& layer = layers[li];
                    if (layer.temporalGpuTextureReady) {
                        temporalSourceInfos[li] =
                            layer.temporalGpuDescriptor;
                    } else if (layer.temporalFrame) {
                        LayerInfo temporalLayer = layer;
                        temporalLayer.frame = layer.temporalFrame;
                        temporalLayer.gpuTextureReady = false;
                        temporalLayer.temporalFrame.reset();
                        auto& key = m_gpuTemporalSourceTexKeys[timingSlot][li];
                        auto result = m_uploadManager->uploadLayer(
                            temporalLayer,
                            *m_gpuTemporalSourceTextures[timingSlot][li],
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
            break;
        }

        case PassType::Effect: {
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
                        *m_layerTemporalOutputs[timingSlot][li];
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
                        VkImageMemoryBarrier temporalBarrier{};
                        temporalBarrier.sType =
                            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        temporalBarrier.srcAccessMask =
                            VK_ACCESS_SHADER_WRITE_BIT;
                        temporalBarrier.dstAccessMask =
                            VK_ACCESS_SHADER_READ_BIT;
                        temporalBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        temporalBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        temporalBarrier.srcQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        temporalBarrier.dstQueueFamilyIndex =
                            VK_QUEUE_FAMILY_IGNORED;
                        temporalBarrier.image = temporalOut.image();
                        temporalBarrier.subresourceRange = {
                            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdPipelineBarrier(
                            cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr,
                            1, &temporalBarrier);

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
                EffectProcessor* activeEffectProcessor =
                    (effectProcessor &&
                     effectProcessor->outputWidth() == effectW &&
                     effectProcessor->outputHeight() == effectH)
                        ? effectProcessor
                        : ctx.effectProcessor(effectW, effectH);
                if (!activeEffectProcessor || !activeEffectProcessor->isInitialized())
                    break;

                for (const auto& snap : layer.effects) {
                    if (snap.type == EffectType::LUT && layer.clipPtr) {
                        for (size_t cfi = 0; cfi < layer.clipPtr->effects().effectCount(); ++cfi) {
                            auto& clipFx = layer.clipPtr->effects().effect(cfi);
                            if (clipFx.effectType() == EffectType::LUT && clipFx.isEnabled()) {
                                auto* lutFx = static_cast<LUT*>(&clipFx);
                                if (lutFx->hasLUT()) {
                                    activeEffectProcessor->uploadLUT3D(
                                        lutFx->lutData(), lutFx->lutSize());
                                }
                                break;
                            }
                        }
                        break;
                    }
                }

                VkDescriptorImageInfo srcInfo = gpuLayers[li].textureInfo;
                const std::vector<VkDescriptorImageInfo>* fxMasks =
                    (li < layerEffectMaskInfos.size() &&
                     !layerEffectMaskInfos[li].empty())
                        ? &layerEffectMaskInfos[li] : nullptr;
                if (activeEffectProcessor->process(cmd, srcInfo, layer.effects,
                                                   fxMasks)) {
                    gpuLayers[li].textureInfo = activeEffectProcessor->outputDescriptorInfo();
                    gpuLayers[li].needsSwapRB = false;
                    gpuLayers[li].isPacked = false;
                    gpuLayers[li].isPMA = false;

                    // Barrier: compute write → sampler read (#40 fix)
                    if (activeEffectProcessor->outputImage() != VK_NULL_HANDLE) {
                        VkImageMemoryBarrier b{};
                        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        b.image = activeEffectProcessor->outputImage();
                        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                    }

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
                        if (snap.image() == VK_NULL_HANDLE || snap.width() != effectW || snap.height() != effectH) {
                            snap.destroy();
                            TextureConfig cfg;
                            cfg.width = effectW; cfg.height = effectH;
                            cfg.format = VK_FORMAT_R8G8B8A8_UNORM;
                            cfg.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                            snap.create(ctx.allocator().handle(), ctx.vkDevice(), cfg);
                        }
                        if (snap.image() != VK_NULL_HANDLE) {
                            VkImage ei = activeEffectProcessor->outputImage();
                            VkImageMemoryBarrier toSrc{};
                            toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                            toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            toSrc.image = ei;
                            toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
                            snap.transitionLayout(cmd, snap.layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                            VkImageCopy r{};
                            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            r.extent = {effectW, effectH, 1};
                            vkCmdCopyImage(cmd, ei, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                snap.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                            VkImageMemoryBarrier toGen{};
                            toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                            toGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                            toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                            toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                            toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            toGen.image = ei;
                            toGen.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toGen);
                            snap.transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            gpuLayers[li].textureInfo = snap.descriptorInfo();
                        }
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
            break;
        }
        case PassType::Transition: {
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
            break;
        }

        case PassType::Composite: {
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

                // LUT data is stored on the live effect object, just as it is
                // for ordinary clip effects; snapshots carry only parameters.
                for (const auto& effect : layer.effects) {
                    if (effect.type == EffectType::LUT && layer.clipPtr) {
                        for (size_t fxIndex = 0;
                             fxIndex < layer.clipPtr->effects().effectCount();
                             ++fxIndex) {
                            auto& clipEffect =
                                layer.clipPtr->effects().effect(fxIndex);
                            if (clipEffect.effectType() == EffectType::LUT &&
                                clipEffect.isEnabled()) {
                                auto* lut = static_cast<LUT*>(&clipEffect);
                                if (lut->hasLUT())
                                    m_adjustmentEffectProcessor->uploadLUT3D(
                                        lut->lutData(), lut->lutSize());
                                break;
                            }
                        }
                        break;
                    }
                }

                ++effectLayerCount;
                effectPassCount += static_cast<int>(layer.effects.size());
                const std::vector<VkDescriptorImageInfo>* effectMasks =
                    (li < layerEffectMaskInfos.size() &&
                     !layerEffectMaskInfos[li].empty())
                        ? &layerEffectMaskInfos[li] : nullptr;
                if (!m_adjustmentEffectProcessor->process(
                        cmd, adjustmentSource, layer.effects, effectMasks)) {
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
            break;
        }

        case PassType::Readback: {
            if (compOk) {
                readbackOk = compositor->recordReadback(cmd);
            }
            break;
        }

        default:
            break;
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 4: Submit and build result (same as old path)
    // ══════════════════════════════════════════════════════════════════

    // Final GPU-TIMING marker (frame end).  Backfill any markers we did
    // not reach (e.g. no effects this frame) so deltas remain monotonic.
    if (timingActive) {
        if (!uploadTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 1);
        }
        if (!effectTimestampWritten) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                m_timingPools[timingSlot], 2);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            m_timingPools[timingSlot], 3);
        m_timingPoolUsed[timingSlot] = true;
    }

    // Per-pass fault isolation: a failed Upload pass disables only that
    // layer (see PassType::Upload case above, which sets gpuLayers[li].
    // enabled = false on failure).  The compositor renders the surviving
    // layers — the user sees one missing layer instead of a blank frame
    // or a CPU-fallback regression.  This matches RenderPass::optional=true.
    // The frame is only killed if the Composite pass itself failed below.
    if (!uploadOk) {
        spdlog::warn("[RENDER_GRAPH] one or more uploads failed — compositing "
                     "surviving layers (fault-isolated)");
    }

    // Composite and viewport presentation both submit to the graphics queue.
    // Queue order is sufficient; a one-shot binary semaphore was unsafe for
    // held frames and leaked whenever a produced frame was dropped.
    VkSemaphore frameSem = VK_NULL_HANDLE;
    const bool endOk = slot.endRecording();
    bool gpuSubmitOk = false;
    if (endOk) {
        // Note: no manual queue-mutex lock here.  GpuWorkSubmission::
        // submit routes through GpuScheduler (P1.1), which owns the
        // queue mutex.  Locking it manually would re-enter a non-
        // recursive std::mutex on the same thread and throw
        // resource_deadlock_would_occur — the regression that left
        // the program monitor blank in an earlier revision.
        //
        // UPGRADE_PLAN Path C (2026-05-22): target the graphics queue.
        // The composite cmd was allocated from graphicsCmdPool above;
        // submitting it to the graphics queue runs the composite work
        // on the 3D engine, in parallel with whatever the async-compute
        // engine (still owned by prefetch convert+copy) is doing.  This
        // removes the 130-468 ms submit= stalls observed in
        // logs/perf_log.txt at 2026-05-22 13:40:50, 13:41:01, etc.
        // graphicsQueue() falls back to the compute queue on devices
        // where the families coincide, preserving the previous path.
        //
        // Cross-queue memory visibility (Path C optimisation): walk the
        // layers vector and find the max producer timeline value across
        // every CachedFrame the compositor is about to sample.  We
        // assume one shared producer timeline semaphore — all
        // prefetch-produced frames currently signal MediaPool's
        // m_prefetchTimelineSem.  Layers whose frame was CPU-decoded
        // or composite-output (no producer) contribute 0 and are
        // ignored.  The wait is added GPU-side via a
        // VkTimelineSemaphoreSubmitInfo so the prefetch worker thread
        // does NOT block on vkWaitForFences — the previous per-frame
        // ~5 ms CPU stall that limited prefetch throughput under
        // seek-recovery and high playback rates is gone.
        VkSemaphore producerSem      = VK_NULL_HANDLE;
        uint64_t    producerMaxValue = 0;
        for (const auto& layer : layers) {
            if (!layer.frame) continue;
            const uint64_t semHandle = layer.frame->producerTimelineSem;
            const uint64_t v         = layer.frame->producerTimelineValue;
            if (semHandle == 0 || v == 0) continue;
            // Currently all prefetch frames share one timeline sem.  If
            // that ever changes, the per-sem map can replace this
            // single-value tracker; for now we sanity-check the handle.
            VkSemaphore sem = reinterpret_cast<VkSemaphore>(semHandle);
            if (producerSem == VK_NULL_HANDLE) producerSem = sem;
            if (sem == producerSem && v > producerMaxValue)
                producerMaxValue = v;
        }
        if (producerSem != VK_NULL_HANDLE && producerMaxValue != 0) {
            gpuSubmitOk = slot.submitWithTimelineWait(
                ctx.graphicsQueue(), frameSem,
                producerSem, producerMaxValue, nullptr);
        } else {
            gpuSubmitOk = slot.submit(ctx.graphicsQueue(), frameSem, nullptr);
        }
    }

    // Advance the composite epoch that drives PrefetchTexturePool's deferred-
    // reuse quarantine.  Bumped per composite submit (whether or not it
    // succeeded) so a recycled texture released during this frame is held out
    // of reuse until several more frames have been submitted — guaranteeing any
    // in-flight submit that sampled it has completed.
    ctx.bumpCompositeEpoch();

    if (!gpuSubmitOk) {
        // P2: no retry/backoff.  Submit failure means the device is lost
        // or wedged; signalling it propagates to the fatal-failure modal.
        spdlog::error("[RENDER_GRAPH] GPU submit failed — signalling device lost");
        GpuContext::get().signalDeviceLost();
        compOk = false;
        readbackOk = false;
    }

    // A/B harness (#18): GPU-signature the preview output. The composite was
    // submitted (async) just above; FrameSignature's own GpuScheduler submit
    // runs after it in graphics-queue submission order, and its acquire barrier
    // provides the cross-submission memory visibility. The Compositor output
    // rests in GENERAL (same as the export path). No-op unless ROUNDTABLE_FRAMEHASH
    // is set; when set, this serialises a per-frame signature submit (offline use).
    if (compOk && gpuSubmitOk && FrameSignatureLog::get().enabled()) {
        FrameSignatureLog::get().capture(
            "preview", tick,
            compositor->outputImageView(), compositor->outputSampler(),
            outW, outH, VK_IMAGE_LAYOUT_GENERAL);
    }

    m_uploadManager->endFrame();

    perfTgpuUp = std::chrono::high_resolution_clock::now();

    // ── Build result frame ───────────────────────────────────────────
    if (compOk) {
        auto result = std::make_shared<CachedFrame>();
        result->width  = outW;
        result->height = outH;
        result->stride = outW * 4;

        // forceSyncReadback (export / Export Preview): the caller consumes
        // the CPU pixels on the SAME thread immediately after this returns,
        // with no fence wait of its own.  The lazy/GPU-direct path below
        // defers the readback AND leaves the EffectProcessor's single shared
        // ping-pong storage + descriptor sets live; with up to kRingSize
        // export frames overlapping on the GPU, frame N+1's effect dispatch
        // then overwrites the storage/descriptor that frame N's still-pending
        // blur + composite reads — the "blur flickers / pulls in other
        // composited layers after a few seconds of export" bug.  Routing
        // export through the synchronous wait+readback path (below) drains
        // each frame before the next reuses the EffectProcessor, and reads
        // fully-rendered (non-stale) pixels.
        if (gpuDisplayMode && !forceSyncReadback) {
            // GPU-direct display path: used for BOTH playback and scrub.
            //
            // Previously this was gated on `!scrubMode`, which forced
            // scrub composites through the synchronous CPU-readback
            // path below (vkWaitForFences with a 5 s timeout +
            // mapAndCopyReadback).  That wait blocked the FrameProducer
            // thread until ALL GPU work queued ahead — including the
            // prefetch convert+copy submissions saturating the compute
            // queue — drained.  Result: `readback=` times of 71-307 ms
            // during scrub bursts, exactly the symptom in
            // logs/perf_log.txt at 2026-05-22 13:11:51–13:12:18.
            //
            // The GPU-direct path works identically for scrub: the
            // compositor produces the same outputImageView, the
            // gpuSemaphore signals completion to the presenter, and
            // ProgramMonitor::presentFrame's gpuReady branch displays
            // directly without ever touching CPU memory.  No reason to
            // pay the readback cost just because the user is scrubbing.
            result->gpuReady     = true;
            result->gpuImageView = reinterpret_cast<uint64_t>(compositor->outputImageView());
            result->gpuSampler   = reinterpret_cast<uint64_t>(compositor->outputSampler());
            result->gpuSemaphore = reinterpret_cast<uint64_t>(frameSem);
            // Keep the compositor's output texture alive until all frames
            // referencing it are consumed by the viewport.
            result->gpuTextureOwner = compositor->outputTextureOwner();
        }

        if (readbackOk && gpuDisplayMode && !forceSyncReadback) {
            auto compPtr = compositor;
            uint32_t rW = outW, rH = outH;
            const uint32_t readbackSlot = compPtr->outputSlot();
            result->lazyReadback = [compPtr, rW, rH, readbackSlot](std::vector<uint8_t>& px) -> bool {
                const size_t imgBytes = static_cast<size_t>(rW) * rH * 4;
                px.resize(imgBytes);
                return compPtr->mapAndCopyReadbackSlot(readbackSlot, px);
            };
        } else if (readbackOk) {
            const size_t imgBytes = static_cast<size_t>(outW) * outH * 4;
            if (m_compositeLru.size() >= kCacheSize) {
                auto& victim = m_compositeLru[m_compositeLruIdx];
                if (victim.frame && victim.frame.use_count() == 1 &&
                    victim.frame->pixels.size() == imgBytes)
                {
                    result->pixels = std::move(victim.frame->pixels);
                }
            }
            // Wait for the just-submitted composite + recordReadback to
            // finish on the GPU before reading staging memory.  Without
            // this, mapAndCopyReadback races the DMA into staging and
            // returns a mix of the new frame's pixels and whatever the
            // previous frame left behind — visible at cuts as "top of the
            // previous frame leaking into the next one" because the GPU
            // hasn't yet fully overwritten the staging buffer.  The
            // lazyReadback path (GPU display mode) is unaffected — it runs
            // from the FrameProducer after its own fence wait.
            if (gpuSubmitOk && m_gpuSubmission) {
                // 5-second timeout — same as other waits in this file;
                // generous enough to survive a heavy frame, short enough to
                // surface a wedged device instead of hanging the export.
                constexpr uint64_t kReadbackWaitNs = 5'000'000'000ull;
                if (!m_gpuSubmission->waitForCompletion(kReadbackWaitNs)) {
                    spdlog::warn("[RENDER_GRAPH] readback fence wait timed out — "
                                 "frame may contain partial/stale pixels");
                }
            }
            compositor->mapAndCopyReadback(result->pixels);
        }

        // A nearest-sampled temporal fallback is intentionally provisional:
        // the next request must rebuild once both exact endpoints arrive.
        // Caching that output by tick would make the clip remain sampled for
        // the lifetime of the LRU entry even though optical flow is selected.
        const bool hasPendingSourceFallback = std::any_of(
            layers.begin(), layers.end(), [](const LayerInfo& layer) {
                return layer.sourceFallbackPending;
            });

        // allowLruInsert is false for nested-sequence inner composites:
        // their CPU result is the inner timeline WITHOUT the SequenceClip
        // transform and would collide with the outer program tick in the
        // shared (tick,w,h)-keyed LRU, flickering the nested clip every
        // other frame.
        if (allowLruInsert && !hasPendingSourceFallback && !result->gpuReady) {
            insertLru(tick, outW, outH, result);
        }

        perfTcomp = std::chrono::high_resolution_clock::now();

        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const double totalMs   = ms(perfT0, perfTcomp);
        const double layersMs  = ms(perfT0, perfTlayers);
        const double submitMs  = ms(perfTlayers, perfTgpuUp);
        const double readbackMs = ms(perfTgpuUp, perfTcomp);

        // Always log when a frame breaches the 33ms budget (target 30 fps).
        // This is the most direct diagnostic for the clip-boundary cascade
        // and any other "FrameClock falls to ~10 fps" symptom: each slow
        // frame is one tick the FrameClock could not advance in real time,
        // so a flurry of these lines correlates exactly with the JUMP
        // stream in the same log.  Stage breakdown (layers / submit /
        // readback) is included so a slow frame can be attributed to
        // CPU-side layer build vs the GPU submit window vs CPU↔GPU
        // readback, without needing a second test run.  warn level: the
        // user runs warn+ filtered logging, and a >33 ms frame during
        // active playback is a real perf event worth surfacing every time.
        if (totalMs > 33.0) {
            spdlog::warn("[COMPOSITE-SLOW] tick={} TOTAL={:.1f}ms "
                         "layers={:.1f}ms submit={:.1f}ms readback={:.1f}ms "
                         "| layerCount={} effectLayers={} effectPasses={} "
                         "transitions={} gpuDisplay={}",
                         tick, totalMs, layersMs, submitMs, readbackMs,
                         layers.size(), effectLayerCount, effectPassCount,
                         transitionCount, gpuDisplayMode);
        } else if (perfLog) {
            spdlog::info("[RENDER_GRAPH] compositeFrame (DAG): layers={} | "
                         "gpu={:.1f}ms  TOTAL={:.1f}ms  "
                         "gpuDisplay={}  effectLayers={}  effectPasses={}  transitions={}",
                         layers.size(),
                         ms(perfTlayers, perfTcomp),
                         totalMs,
                         gpuDisplayMode,
                         effectLayerCount, effectPassCount, transitionCount);
        }

        if (m_cachePolicy)
            m_cachePolicy->onFrameCompleted();

        return result;
    }

    return nullptr;
}
