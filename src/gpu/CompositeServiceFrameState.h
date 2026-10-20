/*
 * CompositeServiceFrameState.h -- one compositeFrameImpl request while it is
 * being answered. Internal to CompositeService; include only from
 * CompositeServiceFrame.cpp and CompositeServiceFrameStages.cpp.
 *
 * compositeFrameImpl (CompositeServiceFrame.cpp) runs the guards (shutdown,
 * modal dialog, recursion depth, the composite lock) and then these stages,
 * in CompositeServiceFrameStages.cpp. A stage that returns a FrameAnswer may
 * decide the frame (a null frame is an answer too); std::nullopt goes on.
 *
 *   consumeCacheInvalidation  a pending UI invalidation clears LRU + hold
 *   serveSegmentCache         a pre-rendered ("green") segment frame
 *   warmNewShotClips          lookahead prewarm + shot-boundary prefetch
 *   logPipelineConfigOnce     the one-time pipeline config log
 *   serveCompositeLru         the composite LRU
 *   resolveLayers             build the layers, briefly wait for arrivals
 *   answerIncompleteFrame     export/nested gates, settle hold, stale frame
 *   reportLayers              layer diagnostics, provisional mark
 *   serveStaticComposite      reuse for an unchanged all-still stack
 *   servePassthroughLayer     a lone identity layer is its own frame
 *   compositeOnGpu            the composite itself, remembered in the hold
 */

#pragma once

#include "CompositeEngine.h"   // CompositeCacheKey
#include "CompositeService.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace rt {

struct CompositeService::FrameState
{
    // ── The request ─────────────────────────────────────────────────────
    int64_t tick;
    uint32_t outW;   // raised to at least 64 x 36 after the LRU lookup
    uint32_t outH;
    bool scrubMode;
    bool isNestedRecursion;
    bool stillMode;
    /// stillMode is also used by the export callback: such requests bypass
    /// composite LRUs and every last-good substitution.
    bool exactStillMode;
    ResolutionTier requestTier;
    const RenderExecutionContext& context;
    const RenderExecutionPolicy& policy;
    Timeline* renderTimeline;
    Project* renderProject;
    FrameDiagnostics* realtimeDiagnostics;

    // ── Found while answering ───────────────────────────────────────────
    bool postInvalidate{false};
    // All composites use non-blocking tryGetFrame — the old blocking inline
    // NVDEC decode (which held m_compositeMutex for 50-500ms, freezing the
    // UI) was migrated to async prefetch workers.  The ProgramMonitor settle
    // mechanism retries until the frame arrives (~30-100ms).
    // The blocking lock on m_compositeMutex (instead of old try_to_lock)
    // ensures compositeFrame() ALWAYS produces the correct frame.
    // Only one GPU Spine render per compositeFrame — the shared FBO gets
    // cleared by beginFrame(), destroying previous renders.
    bool playbackNonBlocking{true};
    bool gpuSpineUsedThisFrame{false};
    bool perfLog{false};
    std::chrono::high_resolution_clock::time_point perfT0{};
    std::chrono::high_resolution_clock::time_point perfTlayers{};
    CompositeCacheKey cacheKey{};
    std::vector<LayerInfo> layers;
    int clipsAtTick{0};
    int resolvedClipsAtTick{0};
    std::optional<uint64_t> staticStateKey;
};

} // namespace rt
