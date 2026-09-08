/*
 * CompositeServiceGpuOrchestration.cpp - GPU composite orchestration.
 * All GPU compositing logic has moved to CompositeEngine::composite().
 * This file now delegates to the engine.
 */

#include "CompositeService.h"
#include "CompositeEngine.h"
#include "Compositor.h"
#include "GpuContext.h"
#include "TransitionRenderer.h"
#include "EffectProcessor.h"

#include "cache/FrameCache.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace rt {

std::shared_ptr<CachedFrame> CompositeService::tryCompositeOnGpu(
    const std::vector<LayerInfo>& layers,
    uint32_t outW, uint32_t outH,
    int64_t tick, bool scrubMode,
    bool perfLog,
    std::chrono::high_resolution_clock::time_point perfT0,
    std::chrono::high_resolution_clock::time_point& perfTlayers,
    int& effectLayerCount, int& effectPassCount,
    int& transitionCount,
    const RenderExecutionContext& context,
    const ::CompositeCacheKey& cacheKey,
    bool isNestedRecursion)
{
    if (!m_engine)
        return nullptr;

    auto compositor = renderCompositor(outW, outH);
    auto effectProcessor = renderEffectProcessor(outW, outH);
    auto transitionRenderer = renderTransitionRenderer(outW, outH);
    const auto& policy = context.policy;

    // Alpha export: tell this service's compositor whether to keep a
    // straight-alpha transparent background.  Set every composite so it can't
    // leak into a later non-alpha composite on the same cached instance.
    if (compositor)
        compositor->setPreserveAlpha(policy.preserveAlpha);

    auto perfTgpuUp = perfT0;
    auto perfTcomp = perfT0;

    // isNestedRecursion → don't let the inner sequence composite write its
    // (untransformed, CPU) result into the shared composite LRU; that frame
    // collides with the outer program tick and flickers the nested clip.
    // forceFullResolution is set only by the Export Panel preview / export
    // render, which consumes the CPU pixels inline with no fence wait of its
    // own.  Route those frames through the synchronous wait+readback path so
    // the shared EffectProcessor ping-pong storage is drained before the next
    // frame reuses it (otherwise overlapping export frames corrupt each
    // other's effect output — the "blur flickers after a few seconds" bug).
    auto result = m_engine->composite(
        layers, outW, outH, tick, scrubMode, policy.preferGpuOutput,
        compositor, effectProcessor, transitionRenderer,
        perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp,
        effectLayerCount, effectPassCount, transitionCount,
        cacheKey,
        /*allowLruInsert=*/!isNestedRecursion,
        /*forceSyncReadback=*/policy.forceFullResolution);

    if (context.diagnostics && policy.measureRealtimeCost &&
        !isNestedRecursion && result) {
        const auto ms = [](auto start, auto end) {
            return std::chrono::duration<double, std::milli>(end - start).count();
        };
        context.diagnostics->gpuRecordSubmitMs = ms(perfTlayers, perfTgpuUp);
        context.diagnostics->readbackMs = ms(perfTgpuUp, perfTcomp);

        // Timestamp results lag by the submission ring depth. They are not
        // attributed to this exact tick, but averaging them over the same
        // playback window cleanly separates GPU saturation from CPU stalls.
        const auto gpu = m_engine->lastGpuTimings();
        if (gpu.valid) {
            context.diagnostics->gpuTimingsValid = true;
            context.diagnostics->gpuFrameMs = gpu.frameMs;
            context.diagnostics->gpuUploadMs = gpu.uploadMs;
            context.diagnostics->gpuEffectMs = gpu.effectMs;
            context.diagnostics->gpuCompositeMs = gpu.composeMs;
        }
    }

    return result;
}

} // namespace rt
