/*
 * TimelineWorkspaceComposite.cpp - Thin forwarding wrappers.
 * All compositing logic now lives in CompositeService (gpu/).
 */
#include "panels/timeline/TimelineWorkspace.h"
#include "CompositeService.h"
#include "Constants.h"
#include "Settings.h"
#include "cache/CachePolicy.h"
#include "cache/FrameCache.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/timeline/TimelinePanel.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "timeline/Timeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRegularExpression>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeFrame(
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool stillMode)
{
    return m_compositeService
        ? m_compositeService->compositeFrame(tick, outW, outH, scrubMode,
                                             /*isNestedRecursion=*/false,
                                             stillMode)
        : nullptr;
}

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeExportFrame(
    const std::shared_ptr<const Project>& projectSnapshot,
    const std::shared_ptr<const Timeline>& timelineSnapshot,
    int64_t tick, uint32_t outW, uint32_t outH,
    bool scrubMode, bool preserveAlpha)
{
    if (!projectSnapshot || !timelineSnapshot || outW == 0 || outH == 0)
        return nullptr;

    // Keep export state physically separate from the Program Monitor service.
    // Rebinding the live service for each frame would race its producer thread
    // and a project switch could overwrite the binding between queued frames.
    if (!m_exportCompositeService) {
        // CachePolicy is stateful and documented for one composite thread; a
        // separate instance keeps its LRU generations/VRAM callbacks from
        // racing or overwriting the Program Monitor's policy hooks.
        m_exportCachePolicy = std::make_unique<CachePolicy>();
        m_exportCompositeService = std::make_unique<CompositeService>();
        m_exportCompositeService->setMediaPool(m_mediaPool);
        m_exportCompositeService->setMediaSourceService(m_mediaSourceService);
        m_exportCompositeService->setModelManager(m_modelManager);
        m_exportCompositeService->setShotPresetManager(m_shotPresetManager);
        m_exportCompositeService->setCachePolicy(m_exportCachePolicy.get());
        m_exportCompositeService->setGpuDisplayMode(false);
        m_exportCompositeService->setForceFullResolution(true);
        m_exportCompositeService->setSegmentCacheReadEnabled(true);
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_mediaPool)
            m_exportCompositeService->initAnimVideoCache(m_mediaPool);
        m_exportCompositeService->setSpineLoadScheduler(
            [this](const std::string& c, const std::string& o,
                   int s, const std::string& a) {
                scheduleSpineSharedLoad(c, o, s, a);
            });
#endif
    }

    if (m_exportProjectSnapshot.get() != projectSnapshot.get() ||
        m_exportTimelineSnapshot.get() != timelineSnapshot.get()) {
        // Reset while the old strong references are still alive, then bind the
        // new immutable graph. CompositeService's historical setter surface is
        // non-const; its render path only reads these model objects.
        m_exportCompositeService->reset();
        m_exportProjectSnapshot = projectSnapshot;
        m_exportTimelineSnapshot = timelineSnapshot;
        m_exportCompositeService->setProject(
            const_cast<Project*>(m_exportProjectSnapshot.get()));
        m_exportCompositeService->setTimeline(
            const_cast<Timeline*>(m_exportTimelineSnapshot.get()));
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_mediaPool)
            m_exportCompositeService->initAnimVideoCache(m_mediaPool);
#endif
    }

#ifdef ROUNDTABLE_HAS_SPINE
    // Spine shared assets are loaded/integrated by the live UI scheduler. Copy
    // immutable decoded atlas/skeleton payloads into the isolated export
    // service; missing entries requested this frame arrive on a later frame.
    if (m_compositeService) {
        for (const auto& [key, data] : m_compositeService->spineSharedCache()) {
            if (data && !m_exportCompositeService->findSpineSharedData(key))
                m_exportCompositeService->storeSpineSharedData(key, data);
        }
    }
#endif

    m_exportCompositeService->setForceFullResolution(true);
    m_exportCompositeService->setExportAlpha(preserveAlpha);

    // Preserve the existing >8-bit single-clip fast path, but bind it to the
    // snapshot service instead of the live/current timeline.
    auto result = m_exportCompositeService->tryBuild16fPassthrough(tick, outW, outH);
    if (!result) {
        result = m_exportCompositeService->compositeFrame(
            tick, outW, outH, scrubMode,
            /*isNestedRecursion=*/false,
            /*stillMode=*/true);
    }
    if (result)
        result->preservesAlpha = preserveAlpha;
    return result;
}

void TimelineWorkspace::exportCurrentFrame()
{
    if (!m_timeline || !m_compositeService) {
        QMessageBox::information(this, tr("Export Frame"),
                                 tr("There is no active sequence to export."));
        return;
    }

    const int64_t tick = m_playbackController
        ? m_playbackController->currentTick()
        : m_timeline->playheadPosition();

    const double fps = m_timeline->settings().frameRate();
    const int64_t frameNumber = fps > 0.0
        ? static_cast<int64_t>(std::llround(
              (static_cast<double>(tick) / kTicksPerSecond) * fps))
        : 0;

    QString sequenceName = QString::fromStdString(m_timeline->name()).trimmed();
    if (sequenceName.isEmpty()) sequenceName = QStringLiteral("frame");
    sequenceName.replace(QRegularExpression(QStringLiteral(R"([<>:\"/\\|?*])")),
                         QStringLiteral("_"));
    const QString defaultName = QStringLiteral("%1_frame_%2.png")
        .arg(sequenceName)
        .arg(frameNumber, 6, 10, QLatin1Char('0'));

    auto settings = appSettings();
    QString lastDir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
    if (lastDir.isEmpty() || !QDir(lastDir).exists())
        lastDir = QDir::homePath();

    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Frame"), QDir(lastDir).filePath(defaultName),
        tr("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;BMP Image (*.bmp)"),
        &selectedFilter);
    if (path.isEmpty()) return;

    if (QFileInfo(path).suffix().isEmpty()) {
        if (selectedFilter.startsWith(QStringLiteral("JPEG")))
            path += QStringLiteral(".jpg");
        else if (selectedFilter.startsWith(QStringLiteral("BMP")))
            path += QStringLiteral(".bmp");
        else
            path += QStringLiteral(".png");
    }

    settings.setValue(QStringLiteral("export/lastOutputDir"),
                      QFileInfo(path).absolutePath());
    settings.sync();

    // Export a deterministic still rather than a potentially reduced preview.
    // GPU display mode intentionally skips CPU readback, so temporarily use the
    // CPU-output path and force full-resolution layer decoding for this frame.
    if (m_playbackController && m_playbackController->isPlaying())
        m_playbackController->pause();

    const bool wasGpuMode = m_compositeService->gpuDisplayMode();
    const bool wasForceFull = m_compositeService->forceFullResolution();
    m_compositeService->setGpuDisplayMode(false);
    m_compositeService->setForceFullResolution(true);

    struct CompositeStateGuard {
        CompositeService* service;
        bool gpuMode;
        bool forceFull;
        ~CompositeStateGuard()
        {
            service->setForceFullResolution(forceFull);
            service->setGpuDisplayMode(gpuMode);
        }
    } restore{m_compositeService.get(), wasGpuMode, wasForceFull};

    const auto resolution = m_timeline->settings().resolution();
    const uint32_t width = resolution.width;
    const uint32_t height = resolution.height;
    if (width == 0 || height == 0) {
        QMessageBox::warning(this, tr("Export Frame"),
                             tr("The active sequence has an invalid resolution."));
        return;
    }

    auto frame = compositeFrame(tick, width, height,
                                /*scrubMode=*/true,
                                /*stillMode=*/true);
    if (!frame || !frame->ensurePixels() || frame->pixels.empty()
        || frame->width == 0 || frame->height == 0) {
        QMessageBox::warning(this, tr("Export Frame"),
                             tr("The current frame could not be rendered."));
        return;
    }

    const uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
    const QImage image(frame->pixels.data(), static_cast<int>(frame->width),
                       static_cast<int>(frame->height), static_cast<int>(stride),
                       QImage::Format_ARGB32);
    if (image.isNull() || !image.save(path)) {
        QMessageBox::warning(this, tr("Export Frame"),
                             tr("Could not save the frame to:\n%1").arg(path));
        return;
    }

    spdlog::info("Export Frame: saved tick {} (frame {}) at {}x{} to '{}'",
                 tick, frameNumber, frame->width, frame->height,
                 path.toStdString());
}

std::shared_ptr<CachedFrame> TimelineWorkspace::compositeFrameAtTier(
    int64_t tick, uint32_t outW, uint32_t outH, bool scrubMode,
    bool stillMode, ResolutionTier tier)
{
    return m_compositeService
        ? m_compositeService->compositeFrame(tick, outW, outH, scrubMode,
                                             /*isNestedRecursion=*/false,
                                             stillMode, tier)
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

void TimelineWorkspace::cacheSnapshotExportFrame(
    const std::shared_ptr<const Project>& projectSnapshot,
    const std::shared_ptr<const Timeline>& timelineSnapshot,
    int64_t tick, const std::shared_ptr<CachedFrame>& frame)
{
    // The first composite for a job performs the binding on the main thread.
    // Never rebind from this worker-thread callback: if identities differ,
    // caching is skipped rather than hashing a frame against the wrong graph.
    if (!m_exportCompositeService || !frame ||
        m_exportProjectSnapshot.get() != projectSnapshot.get() ||
        m_exportTimelineSnapshot.get() != timelineSnapshot.get()) {
        return;
    }
    m_exportCompositeService->cacheExportFrame(tick, frame);
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

void TimelineWorkspace::shutdownCompositeServices()
{
    if (m_exportCompositeService)
        m_exportCompositeService->shutdown();
    if (m_compositeService)
        m_compositeService->shutdown();
}

} // namespace rt
