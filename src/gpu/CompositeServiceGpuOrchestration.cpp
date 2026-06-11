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
    bool isNestedRecursion)
{
    if (!m_engine)
        return nullptr;

    auto& ctx = GpuContext::get();
    auto* compositor = static_cast<Compositor*>(ctx.compositor(outW, outH));
    auto* effectProcessor = ctx.effectProcessor(outW, outH);
    auto* transitionRenderer = ctx.transitionRenderer(outW, outH);

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
        layers, outW, outH, tick, scrubMode, m_gpuDisplayMode,
        compositor, effectProcessor, transitionRenderer,
        perfLog, perfT0, perfTlayers, perfTgpuUp, perfTcomp,
        effectLayerCount, effectPassCount, transitionCount,
        /*allowLruInsert=*/!isNestedRecursion,
        /*forceSyncReadback=*/m_forceFullResolution.load());

    return result;
}

} // namespace rt
