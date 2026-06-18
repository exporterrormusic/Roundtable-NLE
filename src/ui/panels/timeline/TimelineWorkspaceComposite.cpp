/*
 * TimelineWorkspaceComposite.cpp - Thin forwarding wrappers.
 * All compositing logic now lives in CompositeService (gpu/).
 */
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "cache/FrameCache.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "playback/PlaybackController.h"
#include "timeline/Timeline.h"

#include <QCoreApplication>
#include <QProgressDialog>

#include <spdlog/spdlog.h>

namespace rt {

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeFrame(
    int64_t tick, uint32_t outW, uint32_t outH, bool scrubMode)
{
    return m_compositeService
        ? m_compositeService->compositeFrame(tick, outW, outH, scrubMode)
        : nullptr;
}

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeFrame16f(
    int64_t tick, uint32_t outW, uint32_t outH)
{
    return m_compositeService
        ? m_compositeService->tryBuild16fPassthrough(tick, outW, outH)
        : nullptr;
}

void TimelineWorkspace::prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH)
{
    if (m_compositeService)
        m_compositeService->prewarmPlaybackResources(tick, outW, outH);
}

void TimelineWorkspace::cacheExportFrame(
    int64_t tick, const std::shared_ptr<CachedFrame>& frame)
{
    if (m_compositeService)
        m_compositeService->cacheExportFrame(tick, frame);
}

void TimelineWorkspace::refreshRenderBar()
{
    if (m_timelinePanel)
        m_timelinePanel->refreshRenderBar();
}

int TimelineWorkspace::renderInToOut()
{
    if (!m_compositeService || !m_timeline) return -2;

    const int64_t in  = m_timeline->inPoint();
    const int64_t out = m_timeline->outPoint();
    if (in < 0 || out < 0 || out <= in) {
        spdlog::warn("Render In to Out: no valid in/out range (set marks with "
                     "I and O first)");
        return -1;
    }

    // Pre-render at FULL quality + the sequence's full resolution: these
    // frames must serve EXPORT (which composites at Full) and full-res
    // playback.  Rendering at a reduced playback size would make export miss
    // (size mismatch) and could only ever feed reduced-res playback — the
    // case that's already fast.  effectiveCacheTier() keys them as Full.
    const uint32_t outW = m_timeline->settings().resolution().width;
    const uint32_t outH = m_timeline->settings().resolution().height;
    if (outW == 0 || outH == 0) return -2;

    // Mirror export: stop playback so we are the sole compositor driving
    // compositeFrame over the range.
    // Re-entrancy guard: the processEvents pump below can redispatch the
    // Ctrl+Shift+R action while a render is already running.
    if (m_renderingInToOut) return 0;
    m_renderingInToOut = true;
    struct Guard { bool& f; ~Guard() { f = false; } } guard{m_renderingInToOut};

    if (m_playbackController && m_playbackController->isPlaying())
        m_playbackController->stop();

    // NON-MODAL progress so the UI stays responsive and Cancel works.  A modal
    // dialog is forbidden here: compositeFrame()'s s_modalDialogActive guard
    // suppresses compositing during a modal exec — it would stall the very
    // render we're driving.  show() (never exec()) doesn't trip that guard.
    // minimumDuration hides the dialog entirely for short ranges (no flash).
    QProgressDialog progress(QObject::tr("Pre-rendering In to Out…"),
                             QObject::tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::NonModal);
    progress.setMinimumDuration(500);
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    // forceFullResolution makes the layer decodes Full and keys the cache at
    // the Full tier (effectiveCacheTier) — the same key export will consult.
    const bool prevForce = m_compositeService->forceFullResolution();
    m_compositeService->setForceFullResolution(true);

    const int rendered = m_compositeService->renderRangeToCache(
        in, out, outW, outH,
        [&progress]() { return progress.wasCanceled(); },
        [&](int done, int total) {
            if (progress.maximum() != total) progress.setMaximum(total);
            progress.setValue(done);
            // Live green-bar fill, throttled — refreshRenderBar re-probes the
            // cache, too costly to run every single frame.
            if (m_timelinePanel && (done % 30) == 0)
                m_timelinePanel->refreshRenderBar();
            QCoreApplication::processEvents();
        });

    m_compositeService->setForceFullResolution(prevForce);

    // Read-consult is default-on; ensure it's live even if a kill switch was
    // flipped earlier this session.
    m_compositeService->setSegmentCacheReadEnabled(true);

    progress.close();
    // Final repaint so the fully-cached span shows green.
    if (m_timelinePanel) m_timelinePanel->refreshRenderBar();

    spdlog::info("Render In to Out: [{}, {}] @ {}x{} -> {} frames pre-rendered "
                 "(full-res playback + export will reuse them)",
                 in, out, outW, outH, rendered);
    return rendered;
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

void TimelineWorkspace::setExportAlpha(bool keep)
{
    if (m_compositeService)
        m_compositeService->setExportAlpha(keep);
}

bool TimelineWorkspace::gpuDisplayMode() const noexcept
{
    return m_compositeService ? m_compositeService->gpuDisplayMode() : false;
}

} // namespace rt