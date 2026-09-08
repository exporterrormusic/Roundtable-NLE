#include "playback/PlaybackTelemetry.h"

#include <algorithm>

namespace rt {

void PlaybackTelemetryAccumulator::add(
    const FrameDiagnostics& d, RenderResultStatus status) noexcept
{
    ++m_totals.samples;
    switch (status) {
    case RenderResultStatus::Ready:        ++m_totals.ready; break;
    case RenderResultStatus::HeldPrevious: ++m_totals.held; break;
    case RenderResultStatus::Pending:      ++m_totals.pending; break;
    case RenderResultStatus::Blank:        ++m_totals.blank; break;
    case RenderResultStatus::Failed:
    case RenderResultStatus::MissingMedia:
    case RenderResultStatus::Canceled:     ++m_totals.failed; break;
    }

    if (d.droppedFrame) ++m_totals.dropped;
    if (d.cacheHit) ++m_totals.cacheHits;
    if (d.compositeCacheHit) ++m_totals.compositeCacheHits;
    if (d.segmentCacheHit) ++m_totals.segmentCacheHits;

    m_totals.avgCacheLookupMs += d.cacheLookupMs;
    m_totals.avgPrewarmMs += d.prewarmMs;
    m_totals.avgShotBoundaryMs += d.shotBoundaryMs;
    m_totals.avgMediaResolveMs += d.mediaResolveMs;
    m_totals.avgGpuRecordSubmitMs += d.gpuRecordSubmitMs;
    m_totals.avgReadbackMs += d.readbackMs;
    m_totals.avgRenderMs += d.renderMs;
    m_totals.avgProducerMs += d.producerMs;
    m_totals.maxRenderMs = std::max(m_totals.maxRenderMs, d.renderMs);
    m_totals.maxProducerMs = std::max(m_totals.maxProducerMs, d.producerMs);

    if (d.gpuTimingsValid) {
        ++m_totals.gpuSamples;
        m_totals.avgGpuFrameMs += d.gpuFrameMs;
        m_totals.avgGpuUploadMs += d.gpuUploadMs;
        m_totals.avgGpuEffectMs += d.gpuEffectMs;
        m_totals.avgGpuCompositeMs += d.gpuCompositeMs;
    }
}

void PlaybackTelemetryAccumulator::addDropped(uint64_t count) noexcept
{
    m_totals.dropped += count;
}

bool PlaybackTelemetryAccumulator::ready(size_t window) const noexcept
{
    return window > 0 && m_totals.samples >= window;
}

PlaybackTelemetrySummary PlaybackTelemetryAccumulator::take() noexcept
{
    auto summary = m_totals;
    if (summary.samples != 0) {
        const double n = static_cast<double>(summary.samples);
        summary.avgCacheLookupMs /= n;
        summary.avgPrewarmMs /= n;
        summary.avgShotBoundaryMs /= n;
        summary.avgMediaResolveMs /= n;
        summary.avgGpuRecordSubmitMs /= n;
        summary.avgReadbackMs /= n;
        summary.avgRenderMs /= n;
        summary.avgProducerMs /= n;
    }
    if (summary.gpuSamples != 0) {
        const double n = static_cast<double>(summary.gpuSamples);
        summary.avgGpuFrameMs /= n;
        summary.avgGpuUploadMs /= n;
        summary.avgGpuEffectMs /= n;
        summary.avgGpuCompositeMs /= n;
    }
    reset();
    return summary;
}

void PlaybackTelemetryAccumulator::reset() noexcept
{
    m_totals = {};
}

} // namespace rt
