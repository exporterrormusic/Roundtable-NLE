/*
 * ProgramMonitor.cpp -- dependencies, render thread, display, and events.
 *
 * Constructor/setupUI --> ProgramMonitorUI.cpp
 */

#include "panels/monitors/ProgramMonitor.h"

#include "Theme.h"

#include "viewport/Viewport.h"
#include "viewport/VulkanViewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "GpuContext.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/PlaybackScheduler.h"
#include "media/AVSyncClock.h"
#include "media/FrameCache.h"
#include "media/MediaPool.h"
#include "timeline/Timeline.h"
#include "timeline/Marker.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QMenu>
#include <QCursor>
#include <QApplication>
#include <QShowEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QFrame>
#include <QThread>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {


// ═════════════════════════════════════════════════════════════════════════════
//  Dependencies
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::setController(PlaybackController* controller)
{
    m_controller = controller;

    if (m_pipeline)
        m_pipeline->setController(m_controller);

    if (m_controller)
    {
        m_miniTimeline->setFrameRate(m_controller->frameRate());
    }
}

void ProgramMonitor::setTimeline(Timeline* timeline)
{
    m_timeline = timeline;

    if (m_timeline)
    {
        // Update mini-timeline with the timeline duration
        m_miniTimeline->setDuration(m_timeline->duration());

        // Show in/out points if set
        if (m_timeline->inPoint() >= 0)
            m_miniTimeline->setInPoint(m_timeline->inPoint());
        if (m_timeline->outPoint() >= 0)
            m_miniTimeline->setOutPoint(m_timeline->outPoint());

        // Sync marker cue points
        const auto& markers = m_timeline->markers();
        std::vector<MarkerCue> cues;
        cues.reserve(markers.size());
        for (const auto& m : markers)
            cues.push_back({m.time, m.color});
        m_miniTimeline->setMarkers(cues);
    }
}

void ProgramMonitor::setMediaPool(MediaPool* pool)
{
    m_pool = pool;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Display control
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::startPolling(int intervalMs)
{
    ensurePipelineStarted();
    m_pollTimer->start(intervalMs);
}

void ProgramMonitor::stopPolling()
{
    m_pollTimer->stop();
    if (m_pipeline)
        m_pipeline->stop();
    spdlog::info("[PM-TRACE] stopPolling() called");
}

void ProgramMonitor::stopPlaybackPipeline()
{
    // A10: idempotent stop — safe to call from App::~App Phase 1 even
    // if stopPolling() was already invoked by other shutdown paths.
    if (m_pollTimer)
        m_pollTimer->stop();
    m_destroying.store(true, std::memory_order_release);
    if (m_pipeline) {
        m_pipeline->stop();
        // Drop the callbacks so any in-flight presenter notifications
        // can't re-enter this widget while MainWindow is being torn down.
        m_pipeline->setPresentCallback(nullptr);
        m_pipeline->setPresentNotify(nullptr);
        m_pipeline->setCompositeCallback(nullptr);
    }
    spdlog::info("[PM-TRACE] stopPlaybackPipeline() called");
}

void ProgramMonitor::refresh()
{
    m_lastRenderedTick = -1; // Force re-render
    m_lastRenderedFrame = -1;
    updateDisplay();
}

void ProgramMonitor::requestRefresh()
{
    m_lastRenderedTick = -1; // Force re-render
    m_lastRenderedFrame = -1;
    m_lastDirectFrame.reset(); // Clear stale playback frame

    // Kick a multi-cycle settle window so that cold decode misses
    // are retried.  Unlike scrub settle, edit settle does NOT reduce
    // resolution — the user expects crisp display after an edit.
    // 60 cycles @ 16ms = ~960ms: covers cold NVDEC init (~100-170ms
    // per character clip) and FFmpeg probe even when a shot-preset
    // swap opens several new clips at once.
    m_editSettleCounter = 60;

    // Composite + present immediately so the Program Monitor reflects
    // clip property changes, timeline edits, etc. without waiting for
    // the next 16ms poll timer tick.  Without this, there is a window
    // where the composite cache has been invalidated but the display
    // hasn't caught up yet — the user sees stale content until the
    // next poll cycle (or never, if the poll timer is delayed).
    //
    // The edit settle counter above ensures subsequent poll cycles
    // also re-render (for late decodes, settling quality), providing
    // defense-in-depth even if the first synchronous composite
    // returns a partial or null frame.
    updateDisplay();
}

void ProgramMonitor::onNewFrame(std::shared_ptr<CachedFrame> frame)
{
    m_pendingFrame = std::move(frame);
    m_newFrameAvailable.store(true);
    // The pollTimer will pick this up on its next tick
}

void ProgramMonitor::resetViewState()
{
    spdlog::info("ProgramMonitor::resetViewState — recovering from corrupt preview state");

    // 1. Reset render tracking so next poll produces a fresh frame.
    m_lastRenderedTick = -1;
    m_lastRenderedFrame = -1;
    m_lastDirectFrame.reset();
    m_scrubPending = false;
    m_isScrubbing = false;
    m_scrubSettleCounter = 0;
    m_editSettleCounter = 15;
    m_scrubSkipCounter = 0;

    // 2. Re-verify the view stack index is correct for the active display path.
    if (m_viewStack) {
        int expectedIndex = (m_gpuDisplay && m_vulkanViewport) ? 1 : 0;
        if (m_viewStack->currentIndex() != expectedIndex) {
            spdlog::info("ProgramMonitor::resetViewState: correcting viewStack index {} -> {}",
                         m_viewStack->currentIndex(), expectedIndex);
            m_viewStack->setCurrentIndex(expectedIndex);
        }
    }

    // 3. Clear any stale content from both viewports.
    if (m_viewport)
        m_viewport->clearFrame();
    if (m_vulkanViewport)
        m_vulkanViewport->clearFrame();

    // 4. Hide the transform overlay — it may show stale guides/reticles
    // from a previous selection that no longer applies after layout change.
    if (m_transformOverlay) {
        m_transformOverlay->clearTransformOverlay();
        m_transformOverlay->hide();
    }

    // 5. Re-sync overlay geometry to ensure native HWND clipping is correct.
    // This must happen AFTER the viewport is visible and has valid geometry.
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        syncOverlayGeometry();
    });

    // 6. Reset pipeline state: stop if idle, skip if playing.
    //    Do NOT stop the pipeline during active playback — that kills
    //    the FrameProducer/FramePresenter threads and requires a full
    //    restart cycle.  Stale queued frames are harmless since the
    //    next tick will overwrite them.
    if (m_controller && m_controller->isPlaying()) {
        spdlog::info("[PM-TRACE] resetViewState() — playback active, skipping pipeline stop");
    } else {
        spdlog::info("[PM-TRACE] resetViewState() stopping pipeline (not playing)");
        if (m_pipeline && m_pipeline->isRunning()) {
            m_pipeline->stop();
        }
    }

    // 7. Force a full composite refresh on the next poll cycle.
    // The poll timer must be running for this to take effect.
    if (m_pollTimer && !m_pollTimer->isActive()) {
        m_pollTimer->start(16);
    }
}

void ProgramMonitor::requestPlaybackPreroll(int64_t tick)
{
    if (!usesAsyncPipelinePath())
        return;

    ensurePipelineStarted();
    if (!m_pipeline)
        return;

    m_pipeline->requestFrame(tick, compositeWidth(), compositeHeight(),
                             /*scrub=*/false);
}

void ProgramMonitor::setOutputResolution(uint32_t w, uint32_t h)
{
    m_outputWidth  = w;
    m_outputHeight = h;
    if (m_pipeline)
        m_pipeline->setOutputResolution(w, h, m_playbackResDivisor);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Compositing callback
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::setCompositeCallback(CompositeCallback cb)
{
    m_compositeCallback = std::move(cb);

    if (m_pipeline)
        m_pipeline->setCompositeCallback(m_compositeCallback);
}

void ProgramMonitor::setPlaybackTierCallback(PlaybackTierCallback cb)
{
    m_playbackTierCallback = std::move(cb);
    // Fire once so the compositor picks up the current dropdown state
    // (e.g. on startup the dropdown defaults to 1/2 before the callback
    // is wired — without this, the compositor would stay at its own
    // default until the user touches the dropdown).
    if (m_playbackTierCallback)
        m_playbackTierCallback(m_playbackResDivisor);
}

void ProgramMonitor::setGpuDisplayEnabled(bool enabled)
{
    if (enabled && (!m_vulkanViewport || !m_vulkanViewport->isGpuActive())) {
        spdlog::warn("ProgramMonitor::setGpuDisplayEnabled(true) but VulkanViewport not available; using CPU viewport fallback");
        enabled = false;
    }

    m_gpuDisplay = enabled && m_vulkanViewport && m_vulkanViewport->isGpuActive();

    if (m_viewStack) {
        if (m_gpuDisplay && m_vulkanViewport)
            m_viewStack->setCurrentIndex(1);
        else
            m_viewStack->setCurrentIndex(0);
    }

    if (!m_gpuDisplay && m_transformOverlay)
        m_transformOverlay->hide();

    // The async pipeline runs in all display modes now.  FrameProducer is the
    // sole compositor — the UI thread never calls compositeFrame() directly.
    // This eliminates the try_to_lock contention that returned stale frames.
    if (m_pipeline && !m_pipeline->isRunning())
        m_pipeline->start();
}

// CPU Safe Mode UI (Phase 6) was deleted in P2 of CLAUDE_IMPROVEMENT_PLAN.
// Device-lost is now fatal: GpuContext::tryRecover() fires the fatal-failure
// callback which the app translates into a modal restart dialog.
// ═════════════════════════════════════════════════════════════════════════════
//  PlaybackPipeline integration
// ═════════════════════════════════════════════════════════════════════════════

bool ProgramMonitor::usesAsyncPipelinePath() const noexcept
{
    // Always use the async pipeline — FrameProducer is the sole compositor.
    // GPU display mode previously ran compositing on the UI thread via
    // QTimer poll, causing try_to_lock contention.  Now the FrameProducer
    // thread handles all compositing, and FramePresenter calls presentFrame()
    // on its own thread (Vulkan is thread-safe for queue submission).
    return true;
}

void ProgramMonitor::ensurePipelineStarted()
{
    if (!usesAsyncPipelinePath() || !m_controller || !m_compositeCallback)
        return;

    if (!m_pipeline)
        initPipeline();

    if (m_pipeline && !m_pipeline->isRunning())
        m_pipeline->start();
}

void ProgramMonitor::initPipeline()
{
    if (m_pipeline) return;

    m_pipeline = std::make_unique<PlaybackScheduler>();
    m_pipeline->setController(m_controller);
    m_pipeline->setCompositeCallback(m_compositeCallback);
    m_pipeline->setOutputResolution(m_outputWidth, m_outputHeight,
                                    m_playbackResDivisor);

    // Forward adaptive tier transitions to the same callback the manual
    // dropdown uses, so the compositor's playback tier follows the
    // producer's auto-resolution decision in sync with the divisor.
    // The producer thread invokes this — m_playbackTierCallback writes
    // an atomic on CompositeService, so no extra thread hop is needed.
    m_pipeline->setAdaptiveTierCallback([this](int divisor) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_playbackTierCallback)
            m_playbackTierCallback(divisor);
    });

    // Apply the persisted Auto/manual mode now that the pipeline exists —
    // the combo-box slot fires only on user input, not on the lazy first
    // initPipeline() call after launch.
    if (m_playbackResAdaptive)
        m_pipeline->setAdaptiveEnabled(true);

    // Wire the present callback — called from the pipeline's present thread.
    // During playback, the UI thread does direct compositing for zero-latency
    // A/V sync, so the pipeline presenter must NOT touch the Vulkan viewport.
    m_pipeline->setPresentCallback(
        [this](const std::shared_ptr<CachedFrame>& frame) -> bool {
            // If destruction has started, bail out immediately instead of
            // accessing members of a partially-destroyed ProgramMonitor.
            if (m_destroying.load(std::memory_order_acquire))
                return false;

            // All QWidget/QImage display work must happen on the GUI thread.
            // Calling presentFrame from the pipeline present thread can crash
            // in QtGui/QtWidgets under load.
            // GPU-direct present (VulkanViewport::displayGpuImage) is safe
            // from any thread since Vulkan queue submission is thread-safe.
            if (QThread::currentThread() == thread())
                return presentFrame(frame);

            // ── Coalesced cross-thread present ────────────────────────
            // Store the latest frame and queue at most ONE invokeMethod.
            // Without this, every presenter frame queues an event on the
            // GUI thread.  If the GUI thread is busy (scrubbing, editing),
            // the event queue grows unboundedly — each event holds a
            // shared_ptr<CachedFrame> with pixel data — causing OOM.
            {
                std::lock_guard lock(m_queuedPresentMtx);
                m_queuedPresentFrame = frame;
                if (m_queuedPresentPending.load(std::memory_order_acquire))
                    return true;  // already queued — just updated the frame
                m_queuedPresentPending.store(true, std::memory_order_release);
            }

            QMetaObject::invokeMethod(this,
                                      [this]() { flushQueuedPresent(); },
                                      Qt::QueuedConnection);
            return true;
        });

    // Wire the present-notify callback — emits Qt signal from present thread.
    // During playback the UI thread composites directly and emits
    // frameDisplayed itself, so skip the background-thread emit to avoid
    // duplicate signals and stale-tick deliveries to scopes.
    m_pipeline->setPresentNotify(
        [this](int64_t tick) {
            if (m_destroying.load(std::memory_order_acquire))
                return;
            emit frameDisplayed(tick);
        });
}

bool ProgramMonitor::presentFrame(const std::shared_ptr<CachedFrame>& frame)
{
    // Bail out immediately if the destructor has started.  The presenter
    // thread may still hold a reference to this lambda (captured `this`)
    // after m_destroying is set; continuing would access freed members.
    if (m_destroying.load(std::memory_order_acquire))
        return false;

    if (!frame) return false;

    // Refuse GPU-direct display path when the GPU has entered Failed state
    // (VK_ERROR_DEVICE_LOST). The raw gpuImageView/gpuSampler in `frame`
    // points at handles whose VkDevice no longer exists; using them would
    // crash inside nvoglv64.dll.  When in safe mode the frame's CPU pixels
    // are populated by the safe-mode compositor, so the CPU path works.
    //
    // NOTE: we do NOT check frame->gpuTextureOwner here. That field is only
    // set on media-layer frames (uploaded video / character textures from
    // CompositeServiceLayerBuild), not on the compositor's output frames
    // that reach the presenter. The device-lost gate above is sufficient.
    const bool gpuOk = GpuContext::get().isOperational();

    if (frame->width > 0) {
        if (gpuOk && m_gpuDisplay && m_vulkanViewport && frame->gpuReady) {
            // DIAG: log which display path we take
            {
                static int s_presPathLog = 0;
                if (++s_presPathLog % 5 == 0) {
                    spdlog::info("[DIAG-PRESENT-PATH] GPU-DIRECT gpuView=0x{:X} "
                                 "sampler=0x{:X} {}x{}",
                                 frame->gpuImageView, frame->gpuSampler,
                                 frame->width, frame->height);
                }
            }
            m_vulkanViewport->displayGpuImage(
                reinterpret_cast<VkImageView>(frame->gpuImageView),
                reinterpret_cast<VkSampler>(frame->gpuSampler),
                frame->width, frame->height,
                reinterpret_cast<VkSemaphore>(frame->gpuSemaphore.load()),
                frame->gpuTextureOwner);
            m_lastDirectFrame = frame;
            return true;
        }
        if (frame->ensurePixels()) {
            if (m_gpuDisplay && m_vulkanViewport) {
                spdlog::info("[DIAG-PRESENT-PATH] CPU-UPLOAD {}x{} pixelBytes={}",
                             frame->width, frame->height, frame->pixels.size());
                m_vulkanViewport->displayFrame(frame);
                m_lastDirectFrame = frame;
                return true;
            }
            if (!m_gpuDisplay && m_viewport) {
                static int s_cpuPresentLog = 0;
                ++s_cpuPresentLog;
                if (s_cpuPresentLog <= 5 || s_cpuPresentLog % 60 == 0) {
                    // Sample a few pixels: top-left, center, bottom-right.
                    // If they're all 0xFFFFFFFF the readback is producing
                    // white frames; if varied, the data is real and the
                    // problem is in display.
                    uint32_t px0 = 0, pxC = 0, pxE = 0;
                    if (!frame->pixels.empty() && frame->pixels.size() >= 4) {
                        const uint8_t* p = frame->pixels.data();
                        const size_t total = frame->pixels.size();
                        px0 = *reinterpret_cast<const uint32_t*>(p);
                        const size_t mid = (total / 8) * 4;
                        pxC = *reinterpret_cast<const uint32_t*>(p + mid);
                        const size_t end = total - 4;
                        pxE = *reinterpret_cast<const uint32_t*>(p + end);
                    }
                    spdlog::info("[DIAG-PRESENT-PATH] CPU-VIEWPORT {}x{} pixelBytes={} pix[0]=0x{:08X} pix[mid]=0x{:08X} pix[end]=0x{:08X}",
                                 frame->width, frame->height, frame->pixels.size(),
                                 px0, pxC, pxE);
                    const int stackIdx = m_viewStack ? m_viewStack->currentIndex() : -1;
                    const QSize vpSize = m_viewport->size();
                    const QSize pmSize = size();
                    spdlog::info("[DIAG-CPU-VIEW] stackIndex={} vpVisible={} vpSize={}x{} pmVisible={} pmSize={}x{} updatesEnabled={}",
                                 stackIdx,
                                 static_cast<int>(m_viewport->isVisible()),
                                 vpSize.width(), vpSize.height(),
                                 static_cast<int>(isVisible()),
                                 pmSize.width(), pmSize.height(),
                                 static_cast<int>(updatesEnabled()));
                }
                m_viewport->displayFrame(frame);
                m_lastDirectFrame = frame;
                return true;
            }
        }
        spdlog::warn("[DIAG-PRESENT-PATH] FAILED: gpuDisplay={} viewport={} gpuReady={} pixels={}",
                     m_gpuDisplay, (m_vulkanViewport != nullptr), frame->gpuReady,
                     frame->pixels.size());
        return false;
    }

    // width==0 -> clear
    if (m_gpuDisplay && m_vulkanViewport)
        m_vulkanViewport->clearFrame();
    else if (m_viewport)
        m_viewport->clearFrame();
    m_lastDirectFrame.reset();
    return true;
}

void ProgramMonitor::flushQueuedPresent()
{
    // Called on the GUI thread from a queued QMetaObject::invokeMethod.
    // Present the latest frame from the presenter thread and clear the
    // pending flag so the next presenter frame can queue a new invocation.

    // Skip GPU display when a modal dialog is active (QDialog::exec event
    // loop).  During modal dialogs, paint events still fire for widgets
    // behind the dialog, and invoking Vulkan viewport operations from a
    // paint event cascade can overflow the NVIDIA driver stack.
    if (QApplication::activeModalWidget() != nullptr) {
        std::lock_guard lock(m_queuedPresentMtx);
        m_queuedPresentPending.store(false, std::memory_order_release);
        m_queuedPresentFrame.reset();
        return;
    }

    std::shared_ptr<CachedFrame> frame;
    {
        std::lock_guard lock(m_queuedPresentMtx);
        frame = std::move(m_queuedPresentFrame);
        m_queuedPresentPending.store(false, std::memory_order_release);
    }
    if (frame)
        presentFrame(frame);
}

std::shared_ptr<CachedFrame> ProgramMonitor::lastDisplayedFrame() const
{
    if (m_lastDirectFrame)
        return m_lastDirectFrame;
    if (m_pipeline)
        return m_pipeline->lastDisplayedFrame();
    return nullptr;
}

PlaybackScheduler* ProgramMonitor::pipeline() const noexcept
{
    return m_pipeline.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Size hint
// ═════════════════════════════════════════════════════════════════════════════

QSize ProgramMonitor::sizeHint() const
{
    return QSize(640, 400);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Slots
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::onPollTimer()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    // ── Dirty flag optimization (Phase 5.C) ──────────────────────────
    // If no new frame has arrived since the last poll AND we're not in
    // an active playback/scrub state, skip heavy work.  The timer keeps
    // running (handles resize, overlay updates) but the composite/present
    // path is avoided when the view is static.
    //
    // During playback, the poll timer drives frame generation directly
    // via the composite callback — the dirty flag is never set by this
    // path.  Without this guard, the early return would prevent the
    // playhead from advancing and the Program Monitor from updating.
    if (!m_newFrameAvailable.exchange(false)) {
        const bool isPlaying = m_controller && m_controller->isPlaying();
        // Edit-settle MUST keep this loop alive even when no new frame is
        // pending. Without this, requestRefresh() arms editSettleCounter=15
        // and fires one composite request, but if the pipeline's first
        // response is empty (e.g. media not yet open after a project-load
        // or a shot-preset swap) m_newFrameAvailable stays false and every
        // subsequent poll tick early-returns -- updateDisplay never runs,
        // editSettleCounter never decrements, and no further composites are
        // requested. Result: the monitor stays blank until the user nudges
        // the playhead. Letting edit-settle through the gate lets the loop
        // keep re-requesting the frame until late media open / decode
        // completes.
        if (!isPlaying && !m_scrubPending
                && m_scrubSettleCounter <= 0
                && m_editSettleCounter <= 0) {
            syncOverlayGeometry();
            return;
        }
    }

    auto pollStart = std::chrono::steady_clock::now();

    // Detect event-loop stalls (gap between consecutive polls)
    {
        static auto s_lastPoll = std::chrono::steady_clock::time_point{};
        if (s_lastPoll != std::chrono::steady_clock::time_point{}) {
            double sinceLastMs = std::chrono::duration<double, std::milli>(
                pollStart - s_lastPoll).count();
            if (sinceLastMs > 32.0) {
                spdlog::info("[PERF] pollTimer gap: {:.1f}ms (expected ~16ms)",
                             sinceLastMs);
            }
        }
        s_lastPoll = pollStart;
    }

    // Keep the floating overlay positioned over the Vulkan viewport.
    // syncOverlayGeometry() is cheap — it only calls setGeometry when
    // the position/size actually changed.
    syncOverlayGeometry();

    if (!m_controller) return;

    (void)m_controller->pollPosition();

    updateDisplay();

    if (m_droppedFrameLabel) {
        const bool asyncPlayback = m_pipeline && m_controller->isPlaying();
        const int dropped = asyncPlayback ? m_pipeline->droppedFrames() : 0;
        if (dropped > 0) {
            m_droppedFrameLabel->setText(QStringLiteral("Dropped %1").arg(dropped));
            m_droppedFrameLabel->setVisible(true);
        } else {
            m_droppedFrameLabel->setVisible(false);
        }
    }

    // ── VRAM pressure warning ────────────────────────────────────────
    if (m_vramQuery && m_resOverlayLabel) {
        const int vramPct = m_vramQuery();
        if (vramPct > 90) {
            m_resOverlayLabel->setStyleSheet(
                QStringLiteral("color: #FF4444; font-weight: bold; background: transparent;"));
            m_resOverlayLabel->setText(QStringLiteral("VRAM %1%").arg(vramPct));
            m_resOverlayLabel->setVisible(true);
        } else if (vramPct > 75) {
            m_resOverlayLabel->setStyleSheet(
                QStringLiteral("color: #FFD700; background: transparent;"));
            m_resOverlayLabel->setText(QStringLiteral("VRAM %1%").arg(vramPct));
            m_resOverlayLabel->setVisible(true);
        } else {
            m_resOverlayLabel->setVisible(false);
        }
    }

    // Log when the handler itself takes too long
    {
        double selfMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - pollStart).count();
        if (selfMs > 12.0) {
            spdlog::info("[PERF] onPollTimer self: {:.1f}ms (budget=16ms)", selfMs);
        }
    }
}

void ProgramMonitor::onScrub(int64_t tick)
{
    if (!m_controller) return;

    m_controller->seekTo(tick);

    // Delegate to updateDisplay() which has its own scrub throttle
    // (m_scrubSkipCounter — composites every 4th call during scrubbing).
    // We call it on every scrub event; the single throttle ensures the
    // GPU is not overwhelmed while keeping the display responsive.
    // notifyScrub() below ensures the poll timer retries until a valid
    // frame lands (handles cold decode cache after shot boundaries).
    updateDisplay();

    // Set settle counter so subsequent poll cycles retry if the first
    // composite returned a null/stale frame (cold decode cache after
    // export, shot boundary, etc.).  Without this the poll timer dedup
    // (tick == m_lastRenderedTick) skips rendering and the viewport
    // stays frozen on whatever was last displayed.
    notifyScrub();
    emit scrubbed(tick);
}

void ProgramMonitor::notifyScrub()
{
    m_scrubPending = true;
    m_scrubSettleCounter = 15; // re-render for ~240ms to pick up late decodes
}

void ProgramMonitor::onFitModeChanged(int index)
{
    // Apply zoom to whichever viewport is active
    if (m_gpuDisplay && m_vulkanViewport) {
        switch (index)
        {
        case 0: m_vulkanViewport->resetZoomPan();    break;  // Fit = 1:1 + no pan
        case 1: m_vulkanViewport->zoomToFill();       break;  // Fill
        default: {
            static constexpr float zoomLevels[] = { 0.25f, 0.50f, 0.75f, 1.0f, 1.5f, 2.0f };
            int zi = index - 2;
            if (zi >= 0 && zi < static_cast<int>(std::size(zoomLevels)))
                m_vulkanViewport->setViewZoom(zoomLevels[zi]);
            break;
        }
        }
    } else {
        switch (index)
        {
        case 0: m_viewport->setFitMode(ViewportFitMode::Fit);    break;
        case 1: m_viewport->setFitMode(ViewportFitMode::Fill);   break;
        default: {
            static constexpr float zoomLevels[] = { 0.25f, 0.50f, 0.75f, 1.0f, 1.5f, 2.0f };
            int zi = index - 2;
            if (zi >= 0 && zi < static_cast<int>(std::size(zoomLevels))) {
                m_viewport->setFitMode(ViewportFitMode::Actual);
                m_viewport->setViewZoom(zoomLevels[zi]);
            }
            break;
        }
        }
        if (index <= 1)
            m_viewport->resetZoomPan();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Internal display update
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::updateDisplay()
{
    if (!m_controller) return;

    // Skip GPU compositing when a modal dialog is active (QDialog::exec
    // event loop).  During modal dialogs, paint events still fire for
    // widgets behind the dialog, causing repeated GPU compositing which
    // rapidly exhausts the C++ heap and triggers std::bad_alloc crashes.
    // The last-displayed frame remains on screen, which is fine since
    // the user is interacting with the dialog, not the timeline.
    if (QApplication::activeModalWidget() != nullptr) {
        // Still update lightweight UI (timecode, playhead position) even
        // when modal, so the display is ready when the dialog closes.
        m_lastRenderedTick = -1; // force re-render when modal dismisses
        updateTimecodeDisplay();
        return;
    }

    const bool playing = m_controller->isPlaying();
    int64_t tick = m_controller->currentTick();

    // ── Lightweight UI updates (always, regardless of mode) ──────────
    m_miniTimeline->setPlayhead(tick);

    if (m_timeline) {
        m_miniTimeline->setDuration(m_timeline->duration());

        // Keep timeline playhead in sync during playback so that the
        // timeline ruler, loop bounds, and auto-stop all work correctly.
        if (playing && tick != m_timeline->playheadPosition())
            m_timeline->setPlayheadPosition(tick);
    }

    if (m_btnPlayPause) {
        m_btnPlayPause->setText(playing ? QStringLiteral("\u23F8") : QStringLiteral("\u25B6"));
    }

    auto updateShuttleLabel = [this, playing]() {
        if (!m_shuttleSpeedLabel || !m_controller)
            return;

        const double speed = m_controller->shuttleSpeed();
        if (playing && speed != 0.0 && speed != 1.0) {
            QString speedText;
            if (speed == static_cast<int>(speed))
                speedText = QStringLiteral("%1x").arg(static_cast<int>(speed));
            else
                speedText = QStringLiteral("%1x").arg(speed, 0, 'g', 2);
            m_shuttleSpeedLabel->setText(speedText);
            m_shuttleSpeedLabel->show();
        } else {
            m_shuttleSpeedLabel->hide();
        }
    };

    // ── Frame drop indicator ──────────────────────────────────────────
    if (m_dropIndicator && m_pipeline) {
        int drops = m_pipeline->droppedFrames();
        if (drops <= 0) {
            m_dropIndicator->hide();
        } else {
            QString color = (drops <= 5) ? QStringLiteral("#FFC107")
                                         : QStringLiteral("#F44336");
            m_dropIndicator->setStyleSheet(
                QString("QLabel { background: %1; border-radius: 5px; }").arg(color));
            m_dropIndicator->setToolTip(
                tr("Dropped frames: %1").arg(drops));
            m_dropIndicator->show();
        }
    }

    // ── During playback: pipeline handles composite + present ──────
    // The FrameClock drives FrameProducer (compositor thread) and
    // FramePresenter (present thread).  The UI thread only updates
    // lightweight UI elements (timecode, playhead, shuttle label).
    // This eliminates the try_to_lock contention that occurred when
    // the UI thread called compositeFrame() directly.
    if (playing) {
        const double fps = m_controller->frameRate();
        if (m_pool && fps > 0.0)
            m_pool->setProjectFps(fps);

        // Keep poll rate at 16ms for smooth frame updates during playback
        if (m_pollTimer->interval() != 16)
            m_pollTimer->setInterval(16);

        ensurePipelineStarted();
        updateTimecodeDisplay();
        updateShuttleLabel();
        return;
    }

    // ── Not playing: restore fast poll rate for responsive scrubbing ──
    if (m_pollTimer->interval() != 16)
        m_pollTimer->setInterval(16);
    m_lastRenderedFrame = -1;

    // Handle scrub debounce
    if (m_scrubPending) {
        m_scrubPending = false;
        m_isScrubbing = true;
        m_lastRenderedTick = -1;  // force re-render immediately
    }

    // Post-scrub settle: keep re-rendering to pick up late prefetch frames.
    // Without this, the tick-dedup below blocks re-render and the stale
    // frame returned during fast scrubbing persists after the user stops.
    if (m_scrubSettleCounter > 0) {
        --m_scrubSettleCounter;
        m_lastRenderedTick = -1; // force re-render every cycle during settle
        m_isScrubbing = true;    // keep using blocking getFrame during settle

        // When scrub settle expires, transition to an edit-settle window
        // so the frame is re-rendered at full (non-halved) resolution.
        if (m_scrubSettleCounter == 0 && m_editSettleCounter == 0)
            m_editSettleCounter = 5;
    }

    // Post-edit settle: same re-render forcing but at FULL resolution.
    // Explicitly set m_isScrubbing=false so the viewport uses full
    // resolution (non-halved).  The scrub settle block above may have
    // left m_isScrubbing=true on the cycle where it expired (the same
    // cycle edit settle starts).  Without this reset, the first edit
    // settle cycle still gets a halved viewport and useScrub=true,
    // causing blurry output.
    bool editSettleActive = false;
    if (m_editSettleCounter > 0) {
        --m_editSettleCounter;
        m_lastRenderedTick = -1; // force re-render every cycle during settle
        m_isScrubbing = false;
        editSettleActive = true;
    }

    // Skip if tick hasn't changed (paused dedup)
    if (tick == m_lastRenderedTick) {
        m_isScrubbing = false;
        updateTimecodeDisplay();
        updateShuttleLabel();
        return;
    }

    // ── Scrub composite throttle ────────────────────────────────────
    // During scrubbing (including settle), composite only every 4th
    // poll cycle (~15fps effective) to reduce GPU driver load.
    // Lightweight UI (ruler, timecode, shuttle label) still updates
    // at 60fps.  This prevents the NVIDIA driver stack overflow that
    // occurs under rapid Vulkan resource churn from spine state
    // recreation during sustained scrubbing.  15fps matches Premiere
    // Pro and DaVinci Resolve scrub behavior — the user is moving
    // too fast to perceive individual frames.
    if (m_isScrubbing) {
        if (++m_scrubSkipCounter % 4 != 0) {
            updateTimecodeDisplay();
            updateShuttleLabel();
            return;
        }
    } else {
        m_scrubSkipCounter = 0;
    }

    // ── SYNC PATH: scrub / seek / paused ─────────────────────────────
    // All composite requests go through the pipeline.  FrameProducer is
    // the sole compositor — never call compositeFrame() from the UI thread.
    {
        const int resDivisor = m_isScrubbing
            ? std::min(m_playbackResDivisor * 2, 8)
            : m_playbackResDivisor;
        uint32_t vpW = m_outputWidth  / static_cast<uint32_t>(resDivisor);
        uint32_t vpH = m_outputHeight / static_cast<uint32_t>(resDivisor);
        vpW = std::clamp(vpW, 64u, 3840u);
        vpH = std::clamp(vpH, 36u, 2160u);
        // A7: edit-settle no longer forces scrub=true.  The old behavior
        // ran blocking getFrame() inside resolveMediaFrame, which held the
        // composite mutex for 50–500ms on cold decodes — the visible
        // "after-edit pause" that didn't feel Premiere-snappy.  Now we
        // keep non-blocking fetch, and the A1 composite settle-window
        // (CompositeService::kSettleWindowMs) holds the previous full
        // composite while the prefetch workers warm the cache.  Only
        // active user scrubbing forces scrub-mode.
        const bool useScrub = m_isScrubbing;
        (void)editSettleActive; // kept for future telemetry / diagnostics

        ensurePipelineStarted();
        if (m_pipeline)
            m_pipeline->requestFrame(tick, vpW, vpH, useScrub);
    }

    m_isScrubbing = false;
    updateTimecodeDisplay();
    updateShuttleLabel();
}

void ProgramMonitor::updateTimecodeDisplay()
{
    if (!m_controller) return;

    std::string tc = m_controller->currentTimecodeString();
    m_timecodeLabel->setText(QString::fromStdString(tc));

    // Update duration timecode
    if (m_durationLabel) {
        auto dur = tickToTimecode(m_controller->durationTicks(),
                                  m_controller->frameRate());
        m_durationLabel->setText(QString::fromStdString(dur.toString()));
    }
}

void ProgramMonitor::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // On Windows, a window-edge drag puts the main thread into a modal
    // WM_SIZE message loop where Qt's queued update() paint events do not
    // process — but the dock widget still receives WM_PAINT directly from
    // Windows and repaints itself with stale content over our control
    // strip, producing the "echo" gap between the Playback Resolution
    // bar and the transport buttons.  update() alone only resolves after
    // the user releases the drag and ambient paints (playback ticks)
    // arrive; that's why the issue appears stuck while paused.
    //
    // Three-pronged fix:
    //   1) repaint() — synchronous: bypasses the queued event path so the
    //      paint runs even inside the modal drag loop.
    //   2) updateDisplay() + editSettleCounter — forces a fresh composite
    //      at the new widget size AND keeps the poll timer re-rendering
    //      after the resize settles (paused path would otherwise early-
    //      return and never correct the native Vulkan child HWND position).
    //      Without this the "echo" persists until playback starts.
    //   3) singleShot(0, update) — fallback: re-invalidates once the
    //      event loop resumes, in case any late dock paint sneaks in
    //      after the WM_SIZE settles.
    auto forceRepaint = [this]() {
        repaint();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                // Skip the Vulkan viewport's host container — its paint
                // is owned by the native swapchain, not by Qt.
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->repaint();
            }
        }
    };
    forceRepaint();

    // Force a fresh composite at the new size so the viewport and native
    // Vulkan child HWND reposition to match.  This is critical when paused
    // — the poll timer would otherwise early-return without calling
    // updateDisplay(), leaving the echo visible indefinitely.
    m_lastRenderedTick = -1;
    updateDisplay();
    // Keep the poll timer active for a settle window after resize stops,
    // ensuring any late composite results or HWND corrections take effect.
    if (m_editSettleCounter < 15)
        m_editSettleCounter = 15;

    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        update();
        if (auto* lay = layout()) {
            for (int i = 0; i < lay->count(); ++i) {
                auto* item = lay->itemAt(i);
                auto* w = item ? item->widget() : nullptr;
                if (!w || !w->isVisible()) continue;
                if (m_viewport && w == m_viewport->parentWidget()) continue;
                w->update();
            }
        }
    });
}

void ProgramMonitor::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (!m_firstShowDone) {
        m_firstShowDone = true;
        // Defer additional setup until the event loop settles so the
        // VulkanViewport has its swapchain and HWNDs fully created.
        QTimer::singleShot(50, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            syncOverlayGeometry();
            refresh();
        });
    } else {
        // On subsequent show events (e.g. the dock was hidden and reshown
        // via workspace preset toggles), the viewport stack, overlay
        // geometry, and native HWND clipping may be stale.  Recover by
        // resetting the view state once the widget has valid geometry.
        QTimer::singleShot(50, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            resetViewState();
        });
    }
}

void ProgramMonitor::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    const auto mod = event->modifiers();
    const bool noMod = (mod == Qt::NoModifier);

    if (noMod && key == Qt::Key_I) {
        emit inPointRequested();
        event->accept();
        return;
    }
    if (noMod && key == Qt::Key_O) {
        emit outPointRequested();
        event->accept();
        return;
    }
    // Space, J, K, L — forward transport keys to TimelineWorkspace
    if (noMod && (key == Qt::Key_Space || key == Qt::Key_J ||
                  key == Qt::Key_K || key == Qt::Key_L)) {
        event->ignore();
        return;
    }

    QWidget::keyPressEvent(event);
}

bool ProgramMonitor::eventFilter(QObject* obj, QEvent* event)
{
    // When the user clicks inside any child widget, grab focus for keyboard.
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonDblClick) {
        // Timecode label click → show editable timecode entry
        if (obj == m_timecodeLabel && event->type() == QEvent::MouseButtonPress) {
            m_timecodeLabel->hide();
            m_timecodeEdit->setText(m_timecodeLabel->text());
            m_timecodeEdit->show();
            m_timecodeEdit->setFocus();
            m_timecodeEdit->selectAll();
            return true;
        }
        setFocus();
    }
    // Intercept key events from children to handle I/O shortcuts.
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->modifiers() == Qt::NoModifier) {
            if (ke->key() == Qt::Key_I) {
                emit inPointRequested();
                return true;
            }
            if (ke->key() == Qt::Key_O) {
                emit outPointRequested();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ProgramMonitor::syncOverlayGeometry()
{
    if (!m_vulkanViewport)
        return;

    const bool hasOverlay = (m_transformOverlay != nullptr);

#ifdef _WIN32
    QWindow* nw = m_vulkanViewport->nativeWindow();
    HWND nativeHwnd = nw ? reinterpret_cast<HWND>(nw->winId()) : nullptr;
#endif

    // Hide overlay when the viewport is not visible (e.g. tab switched)
    if (!m_vulkanViewport->isVisible()) {
        if (hasOverlay && m_transformOverlay->isVisible())
            m_transformOverlay->hide();
#ifdef _WIN32
        // Prevent the native Vulkan child from bleeding over sibling widgets
        // while this panel is hidden/tabbed out.
        if (nativeHwnd) {
            HRGN emptyRgn = CreateRectRgn(0, 0, 0, 0);
            SetWindowRgn(nativeHwnd, emptyRgn, TRUE);
            // Windows takes ownership of emptyRgn.
        }
#endif
        return;
    }

    // Always anchor geometry to the Qt viewport widget rect; this is the
    // logical preview area inside Program Monitor and remains stable across
    // tab/dock regrouping. Native HWND rects can transiently report stale
    // sizes/positions and overdraw controls if trusted directly.
    const QPoint vpGlobal = m_vulkanViewport->mapToGlobal(QPoint(0, 0));
    const QRect vpWidgetRect(vpGlobal, m_vulkanViewport->size());

    QPoint panelGlobal = mapToGlobal(QPoint(0, 0));
    QRect  panelRect(panelGlobal, size());

    QRect  desired = vpWidgetRect.intersected(panelRect);
    if (desired.isEmpty()) {
        if (hasOverlay && m_transformOverlay->isVisible())
            m_transformOverlay->hide();
        return;
    }

    // Tell the overlay how much the clipping shifted it relative to the
    // viewport's origin, so computeFrameRect() uses the viewport's full
    // dimensions with the correct origin offset.
    if (hasOverlay) {
        QPoint vpOffset(desired.left() - vpWidgetRect.left(),
                        desired.top()  - vpWidgetRect.top());
        m_transformOverlay->setViewportOffset(vpOffset);
    }

    if (hasOverlay && m_transformOverlay->geometry() != desired)
        m_transformOverlay->setGeometry(desired);

    if (hasOverlay && !m_transformOverlay->isVisible())
        m_transformOverlay->show();

#ifdef _WIN32
    // The native Vulkan QWindow HWND from createWindowContainer() renders
    // above all regular Qt widgets.  The TransformOverlayWidget is a
    // top-level Qt::Tool window, but Windows may still place the embedded
    // native HWND visually on top of it.  Use SetWindowPos to explicitly
    // keep the overlay above the Vulkan surface so QPainter-rendered
    // transform handles, safe-area guides, and grid are visible.
    {
        if (hasOverlay) {
            HWND overlayHwnd = reinterpret_cast<HWND>(m_transformOverlay->winId());
            if (overlayHwnd)
                SetWindowPos(overlayHwnd, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // Clip the native Vulkan QWindow HWND so it can't paint outside the
    // ProgramMonitor panel area.  createWindowContainer() embeds a native
    // HWND that ignores Qt widget clipping, so we must constrain it
    // manually via SetWindowRgn.
    if (nativeHwnd) {
        // Get the native window's screen position.
        RECT nativeScreenRect;
        GetWindowRect(nativeHwnd, &nativeScreenRect);

        // Clip to the *visible viewport rect* (desired), not the whole panel.
        // This ensures the native Vulkan child can never overdraw Program
        // Monitor controls (control bar, mini timeline, transport bar).
        int clipLeft   = std::max(0, (int)(desired.left()   - nativeScreenRect.left));
        int clipTop    = std::max(0, (int)(desired.top()    - nativeScreenRect.top));
        int clipRight  = std::min((int)(nativeScreenRect.right - nativeScreenRect.left),
                                 (int)(desired.right()  - nativeScreenRect.left));
        int clipBottom = std::min((int)(nativeScreenRect.bottom - nativeScreenRect.top),
                                 (int)(desired.bottom() - nativeScreenRect.top));

        if (clipRight > clipLeft && clipBottom > clipTop) {
            HRGN rgn = CreateRectRgn(clipLeft, clipTop, clipRight, clipBottom);
            SetWindowRgn(nativeHwnd, rgn, TRUE);
            // Windows takes ownership of rgn.
        } else {
            HRGN emptyRgn = CreateRectRgn(0, 0, 0, 0);
            SetWindowRgn(nativeHwnd, emptyRgn, TRUE);
            // Windows takes ownership of emptyRgn.
        }
    }
#endif
}


} // namespace rt
