/*
 * CompositeEngineFrame.h -- the state of ONE render-graph composite while it
 * is being recorded. Internal to CompositeEngine; include only from the
 * CompositeEngine*.cpp files.
 *
 * compositeViaRenderGraph() runs these stages in order, each a private
 * CompositeEngine member that reads and updates the FrameRecording:
 *
 *   beginCompositeRecording  submission slot, command buffer, GPU timing start
 *   ensureGpuTextureCache    first-frame texture cache + VRAM pressure hooks
 *   buildCompositeGraph      resources and passes for every layer
 *   prepareCompositorLayers  one CompositorLayer per layer
 *   uploadLayerMasks         clip masks and effect masks (cached by hash)
 *                                      -- CompositeEngineRenderGraph.cpp
 *   recordPasses             every pass in topological order: upload,
 *                            effect (+ temporal), transition, composite
 *                            (+ adjustment layers), readback
 *                                      -- CompositeEnginePasses.cpp
 *   submitComposite          end markers, graphics-queue submit
 *   buildCompositeResult     the CachedFrame, readback, LRU
 *   logCompositeTimings      COMPOSITE-SLOW / SUBMIT-SPLIT lines
 *                                      -- CompositeEngineSubmit.cpp
 */

#pragma once

#include "CompositeEngine.h"
#include "render_graph/GpuRenderGraph.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt::composite_detail {

/// Dimensions of a layer image as the compositor's layerUV sees it. Packed-
/// alpha sources store the matte below the RGB image, so their logical
/// height is half the backing texture height.
struct LogicalLayerSize
{
    uint32_t width;
    uint32_t height;
};

/// The graph resources and passes one layer contributes.
struct LayerPasses
{
    render_graph::ResourceId layerTexId{render_graph::kInvalidResource};
    render_graph::ResourceId temporalTexId{render_graph::kInvalidResource};
    render_graph::ResourceId maskTexId{render_graph::kInvalidResource};
    uint32_t uploadPassIdx{UINT32_MAX};
    uint32_t temporalUploadPassIdx{UINT32_MAX};
    uint32_t effectPassIdx{UINT32_MAX};
    uint32_t transitionPassIdx{UINT32_MAX};
    uint32_t transitionTargetSlot{UINT32_MAX};
};

/// Set the compositor transform (and motion-blur end transforms) that places
/// `layer`'s srcW x srcH image in the outW x outH frame.
void setLayerTransforms(CompositorLayer& compositorLayer, const LayerInfo& layer,
                        uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH);

} // namespace rt::composite_detail

struct CompositeEngine::FrameRecording
{
    using SplitClock = std::chrono::steady_clock;
    using PerfClock = std::chrono::high_resolution_clock;

    // ── What to composite ───────────────────────────────────────────────
    const std::vector<rt::LayerInfo>& layers;
    uint32_t outW;
    uint32_t outH;
    int64_t tick;
    bool scrubMode;
    bool gpuDisplayMode;
    bool forceSyncReadback;
    const std::shared_ptr<rt::Compositor>& compositor;
    const std::shared_ptr<rt::EffectProcessor>& effectProcessor;
    const std::shared_ptr<rt::TransitionRenderer>& transitionRenderer;

    // ── Counters reported to the caller ─────────────────────────────────
    int& effectLayerCount;
    int& effectPassCount;
    int& transitionCount;

    // ── Where it is recorded (beginCompositeRecording) ──────────────────
    rt::GpuWorkSubmission* slot{nullptr};
    VkCommandBuffer cmd{VK_NULL_HANDLE};
    int timingSlot{0};
    SlotLayerResources* resources{nullptr};
    bool timingActive{false};

    // ── The graph (buildCompositeGraph) ─────────────────────────────────
    rt::render_graph::GpuRenderGraph graph;
    std::vector<rt::composite_detail::LogicalLayerSize> logicalSizes;
    std::vector<rt::composite_detail::LayerPasses> layerPasses;

    // ── Per-layer compositor state the passes update ────────────────────
    std::vector<rt::CompositorLayer> gpuLayers;
    std::vector<VkDescriptorImageInfo> temporalPrimarySourceInfos;
    std::vector<VkDescriptorImageInfo> temporalSourceInfos;
    std::vector<std::vector<VkDescriptorImageInfo>> effectMaskInfos;

    // ── Outcome ─────────────────────────────────────────────────────────
    bool uploadOk{true};
    bool compOk{false};
    bool readbackOk{false};
    bool nv12ReadbackOk{false};   // the readback is NV12 (export), not BGRA
    bool uploadsSeen{false};
    bool gpuSubmitOk{false};
    VkSemaphore frameSem{VK_NULL_HANDLE};

    // ── GPU timing markers written so far (recordPasses) ────────────────
    bool uploadTimestampWritten{false};
    bool effectTimestampWritten{false};

    // ── [SUBMIT-SPLIT] CPU sub-timings ───────────────────────────────────
    // CPU laps of the COMPOSITE-SLOW "submit=" window (perfTlayers →
    // perfTgpuUp). A steady ~33 ms submit with ~1.7 ms of GPU work was
    // observed (perf_log 2026-09-26) only on frames with an effect layer;
    // these laps attribute it without guessing.
    SplitClock::time_point splitT0{SplitClock::now()};
    SplitClock::time_point splitBegin{splitT0};
    SplitClock::time_point splitPrep{splitT0};
    SplitClock::time_point splitWalk{splitT0};
    SplitClock::time_point splitEnd{splitT0};
    SplitClock::time_point splitSubmit{splitT0};
    double passUploadMs{0};
    double passEffectMs{0};
    double passCompositeMs{0};
    double passOtherMs{0};
};
