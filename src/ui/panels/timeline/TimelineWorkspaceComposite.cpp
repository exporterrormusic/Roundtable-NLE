/*
 * TimelineWorkspaceComposite.cpp - Thin forwarding wrappers.
 * All compositing logic now lives in CompositeService (gpu/).
 */
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "cache/FrameCache.h"

namespace rt {

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeFrame(
    int64_t tick, uint32_t outW, uint32_t outH, bool scrubMode)
{
    return m_compositeService
        ? m_compositeService->compositeFrame(tick, outW, outH, scrubMode)
        : nullptr;
}

void TimelineWorkspace::prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_compositeService)
        m_compositeService->prewarmPlaybackResources(tick, outW, outH);
}

void TimelineWorkspace::setGpuDisplayMode(bool on)
{
    if (m_compositeService)
        m_compositeService->setGpuDisplayMode(on);
}

void TimelineWorkspace::setForceFullResolution(bool force)
{
    if (m_compositeService)
        m_compositeService->setForceFullResolution(force);
}

bool TimelineWorkspace::gpuDisplayMode() const noexcept
{
    return m_compositeService ? m_compositeService->gpuDisplayMode() : false;
}

} // namespace rt