/*
 * CompositeEngineRenderGraph.cpp -- CompositeEngine::compositeViaRenderGraph(),
 * the DAG render-graph compositing path (the only composite path): the
 * stage order, and the stages that set a frame up (recording, texture
 * cache, graph, compositor layers, masks). The passes are recorded in
 * CompositeEnginePasses.cpp, submit and result in CompositeEngineSubmit.cpp;
 * CompositeEngineFrame.h lists every stage.
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
using rt::composite_detail::LogicalLayerSize;
using rt::composite_detail::setLayerTransforms;

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

void rt::composite_detail::setLayerTransforms(CompositorLayer& compositorLayer,
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

// One copy region covering a whole w x h colour image.
static std::vector<VkBufferImageCopy> fullImageRegion(uint32_t w, uint32_t h)
{
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    return {region};
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

    // A texture taller than the frame its source-local masks were drawn on
    // (LayerInfo::renderGrowthY) holds that frame in its middle 1/k.  The
    // rasterizer scales feather and expansion by the mapping's mean scale;
    // this mapping keeps the texture's pixel size, so undo that.
    MaskRasterTransform grownXf{};
    const MaskRasterTransform* sourceMap = nullptr;
    if (layer.renderGrowthY > 1.0f) {
        const float k = layer.renderGrowthY;
        const float h = static_cast<float>(srcH);
        grownXf = {{static_cast<float>(srcW), 0.0f, 0.0f,
                    0.0f, h / k, h * (1.0f - 1.0f / k) * 0.5f}};
        sourceMap = &grownXf;
        const float meanScale = 0.5f * (1.0f + 1.0f / k);
        for (auto& mask : sourceMasks) {
            mask.feather /= meanScale;
            mask.expansion /= meanScale;
        }
    }

    if (legacyMasks.empty())
        return rasterizeMasks(sourceMasks, srcW, srcH, sourceMap);

    const auto xf = legacyMaskTransform(layer, srcW, srcH, outW, outH);
    if (sourceMasks.empty())
        return rasterizeMasks(legacyMasks, srcW, srcH, &xf);

    auto pixels = rasterizeMasks(sourceMasks, srcW, srcH, sourceMap);
    const auto legacyPixels = rasterizeMasks(legacyMasks, srcW, srcH, &xf);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(std::min(
            255, static_cast<int>(pixels[i]) +
                 static_cast<int>(legacyPixels[i])));
    }
    return pixels;
}

// ============================================================================
// Render graph composite: stage order
// ============================================================================

std::shared_ptr<CachedFrame> CompositeEngine::compositeViaRenderGraph(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool gpuDisplayMode,
    const std::shared_ptr<Compositor>& compositor,
    const std::shared_ptr<EffectProcessor>& effectProcessor,
    const std::shared_ptr<TransitionRenderer>& transitionRenderer,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    std::chrono::high_resolution_clock::time_point& perfTgpuUp,
    std::chrono::high_resolution_clock::time_point& perfTcomp,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount,
    const CompositeCacheKey& cacheKey,
    bool allowLruInsert,
    bool forceSyncReadback)
{
    FrameRecording frame{
        .layers = layers,
        .outW = outW,
        .outH = outH,
        .tick = tick,
        .scrubMode = scrubMode,
        .gpuDisplayMode = gpuDisplayMode,
        .forceSyncReadback = forceSyncReadback,
        .compositor = compositor,
        .effectProcessor = effectProcessor,
        .transitionRenderer = transitionRenderer,
        .effectLayerCount = effectLayerCount,
        .effectPassCount = effectPassCount,
        .transitionCount = transitionCount,
    };

    // ── Ensure texture pool is large enough ──────────────────────────
    for (auto& resources : m_slots)
        resources.ensureLayerCount(layers.size());

    if (!beginCompositeRecording(frame))
        return nullptr;
    ensureGpuTextureCache();

    // ══════════════════════════════════════════════════════════════════
    // STEP 1: Build the Render Graph
    // ══════════════════════════════════════════════════════════════════
    buildCompositeGraph(frame);

    // ══════════════════════════════════════════════════════════════════
    // STEP 2: Compile the graph (topo sort + barriers)
    // ══════════════════════════════════════════════════════════════════

    if (!frame.graph.compile(GpuContext::get().vkDevice())) {
        spdlog::warn("[RENDER_GRAPH] Graph compilation failed — falling back");
        frame.slot->endRecording();
        m_uploadManager->endFrame();
        return nullptr;
    }

    // ══════════════════════════════════════════════════════════════════
    // STEP 3: Execute passes in topological order
    // ══════════════════════════════════════════════════════════════════
    prepareCompositorLayers(frame);
    uploadLayerMasks(frame);
    frame.splitPrep = FrameRecording::SplitClock::now();
    recordPasses(frame);

    // ══════════════════════════════════════════════════════════════════
    // STEP 4: Submit and build result (same as old path)
    // ══════════════════════════════════════════════════════════════════
    submitComposite(frame);
    perfTgpuUp = std::chrono::high_resolution_clock::now();
    if (!frame.compOk)
        return nullptr;

    auto result = buildCompositeResult(frame, cacheKey, allowLruInsert);
    perfTcomp = std::chrono::high_resolution_clock::now();
    logCompositeTimings(frame, perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp);
    if (m_cachePolicy)
        m_cachePolicy->onFrameCompleted();
    return result;
}

// ============================================================================
// Setup stages
// ============================================================================

bool CompositeEngine::beginCompositeRecording(FrameRecording& frame)
{
    auto& ctx = GpuContext::get();

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
        const VkCommandPool commandPool = m_renderCommandPool != VK_NULL_HANDLE
            ? m_renderCommandPool : ctx.graphicsCmdPool().handle();
        m_gpuSubmission->init(ctx.vkDevice(), commandPool);
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
        return false;
    }
    frame.splitBegin = FrameRecording::SplitClock::now();
    frame.slot = &slot;
    frame.cmd = slot.cmdBuffer();
    VkCommandBuffer cmd = frame.cmd;
    slot.retainForCurrentSlot(frame.compositor);
    slot.retainForCurrentSlot(frame.effectProcessor);
    slot.retainForCurrentSlot(frame.transitionRenderer);
    for (const auto& layer : frame.layers)
        slot.retainForCurrentSlot(layer.gpuResourceOwner);
    frame.timingSlot = slot.currentSlot();
    const int timingSlot = frame.timingSlot;
    frame.resources = &m_slots[static_cast<size_t>(timingSlot)];

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
    return true;
}

void CompositeEngine::ensureGpuTextureCache()
{
    auto& ctx = GpuContext::get();

    // ── Initialize GPU texture cache if needed (same logic as old path) ─
    if (m_gpuTexCache)
        return;
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
    if (m_sequentialRenderMode) {
        constexpr size_t kSequentialBudget = 256ull * 1024ull * 1024ull;
        budget = std::min(budget, kSequentialBudget);
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
        const size_t appliedMaxEntries = m_sequentialRenderMode
            ? std::min<size_t>(maxEntries, 32u)
            : maxEntries;
        m_gpuTexCache->setMaxEntries(appliedMaxEntries);
        spdlog::info("[PERF] GpuTexCache max entries: {} "
                     "(Premiere-style bounded working set, sequential={})",
                     appliedMaxEntries, m_sequentialRenderMode);
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

void CompositeEngine::buildCompositeGraph(FrameRecording& frame)
{
    using namespace render_graph;
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const bool scrubMode = frame.scrubMode;
    const bool gpuDisplayMode = frame.gpuDisplayMode;
    auto& slotResources = *frame.resources;
    auto& gpuLayerTextures = frame.resources->layerTextures;
    auto& gpuMaskTextures = frame.resources->maskTextures;

    auto& graph = frame.graph;

    // Masks are clip-local assets. Keep one authoritative logical size for
    // every layer so clip masks, effect masks, and the layer transform all use
    // the same normalized coordinate system.
    auto& logicalLayerSizes = frame.logicalSizes;
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
    auto& layerInfo = frame.layerPasses;
    layerInfo.assign(layers.size(), {});

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
            auto regions = fullImageRegion(outW, outH);

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
                    : (slotResources.temporalSourceTextures[li]
                        ? slotResources.temporalSourceTextures[li]->image()
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
                auto temporalRegions = fullImageRegion(
                    layer.frameWidth ? layer.frameWidth : outW,
                    layer.frameHeight ? layer.frameHeight : outH);
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

            auto maskRegions = fullImageRegion(maskSize.width, maskSize.height);

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
        auto readbackRegions = fullImageRegion(outW, outH);

        (void)graph.addReadbackPass(
            "readback", outputTexId,
            VK_NULL_HANDLE, 0, readbackRegions, true);
    }
}

void CompositeEngine::prepareCompositorLayers(FrameRecording& frame)
{
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    const auto& logicalLayerSizes = frame.logicalSizes;
    auto& gpuLayers = frame.gpuLayers;
    auto& temporalPrimarySourceInfos = frame.temporalPrimarySourceInfos;
    auto& temporalSourceInfos = frame.temporalSourceInfos;

    gpuLayers.reserve(layers.size());
    temporalPrimarySourceInfos.assign(layers.size(), VkDescriptorImageInfo{});
    temporalSourceInfos.assign(layers.size(), VkDescriptorImageInfo{});

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
}

void CompositeEngine::uploadLayerMasks(FrameRecording& frame)
{
    const auto& layers = frame.layers;
    const uint32_t outW = frame.outW;
    const uint32_t outH = frame.outH;
    auto& gpuMaskTextures = frame.resources->maskTextures;
    auto& maskCache = frame.resources->maskCache;
    auto& effectMaskTextures = frame.resources->effectMaskTextures;
    auto& effectMaskCache = frame.resources->effectMaskCache;
    const auto& logicalLayerSizes = frame.logicalSizes;
    auto& gpuLayers = frame.gpuLayers;
    auto& layerEffectMaskInfos = frame.effectMaskInfos;
    bool& uploadsSeen = frame.uploadsSeen;

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
    layerEffectMaskInfos.assign(layers.size(), {});
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
}
