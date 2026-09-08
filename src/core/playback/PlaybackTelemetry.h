/*
 * PlaybackTelemetry - low-overhead aggregation for clock-driven preview.
 */

#pragma once

#include "playback/EngineContracts.h"

#include <cstddef>
#include <cstdint>

namespace rt {

struct PlaybackTelemetrySummary
{
    uint64_t samples{0};
    uint64_t ready{0};
    uint64_t held{0};
    uint64_t pending{0};
    uint64_t blank{0};
    uint64_t failed{0};
    uint64_t dropped{0};
    uint64_t cacheHits{0};
    uint64_t compositeCacheHits{0};
    uint64_t segmentCacheHits{0};
    uint64_t gpuSamples{0};

    double avgCacheLookupMs{0.0};
    double avgPrewarmMs{0.0};
    double avgShotBoundaryMs{0.0};
    double avgMediaResolveMs{0.0};
    double avgGpuRecordSubmitMs{0.0};
    double avgReadbackMs{0.0};
    double avgRenderMs{0.0};
    double avgProducerMs{0.0};
    double avgGpuFrameMs{0.0};
    double avgGpuUploadMs{0.0};
    double avgGpuEffectMs{0.0};
    double avgGpuCompositeMs{0.0};
    double maxRenderMs{0.0};
    double maxProducerMs{0.0};
};

class PlaybackTelemetryAccumulator
{
public:
    static constexpr size_t kDefaultWindow = 120;

    void add(const FrameDiagnostics& diagnostics,
             RenderResultStatus status) noexcept;
    void addDropped(uint64_t count = 1) noexcept;
    [[nodiscard]] bool ready(size_t window = kDefaultWindow) const noexcept;
    [[nodiscard]] PlaybackTelemetrySummary take() noexcept;
    void reset() noexcept;

private:
    PlaybackTelemetrySummary m_totals{};
};

} // namespace rt
