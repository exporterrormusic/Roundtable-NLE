/*
 * TimelineWorkspace.cpp — Splitter-based NLE workspace coordinator.
 *
 * Thin coordinator after extracting setTimeline/refreshAfterUndoRedo →
 * TimelineWorkspaceIntegration.cpp, editing commands →
 * TimelineWorkspaceEditCommands.cpp, dependency injection →
 * TimelineWorkspaceDeps.cpp, and dock persistence →
 * TimelineWorkspaceDock.cpp.
 *
 * Contains: constructor (binder-controller construction), destructor,
 * togglePanelMaximize shim, mousePressEvent.
 *
 * Binder controllers (god-class decomposition, cleanup audit §3.1):
 *   MediaWatchController     — live media file-swap watching
 *   PanelMaximizeController  — tilde panel maximize/restore
 *
 * Sub-files (all in panels/timeline/):
 *   TimelineWorkspaceIntegration.cpp     — setTimeline(),
 *                                          invalidateCompositeCache(),
 *                                          refreshAfterUndoRedo()
 *   TimelineWorkspaceEditCommands.cpp    — setInPoint, setOutPoint,
 *                                          clearInOut,
 *                                          syncProgramMonitorInOut,
 *                                          refreshSequenceTabs,
 *                                          nestSequence
 *   TimelineWorkspaceDeps.cpp            — all dependency injection
 *                                          setters, dockForPanel
 *   TimelineWorkspaceDock.cpp            — saveDockLayout,
 *                                          restoreDockLayout,
 *                                          resetToDefaultDockLayout,
 *                                          doResetToDefaultDockLayout,
 *                                          showEvent
 */

#include "panels/timeline/TimelineWorkspace.h"

#include "CompositeService.h"
#include "audio/AudioPlaybackService.h"
#include "panels/timeline/DockLayoutManager.h"
#include "panels/timeline/DropController.h"
#include "panels/timeline/MediaWatchController.h"
#include "panels/timeline/OverlayController.h"
#include "panels/timeline/PanelMaximizeController.h"
#include "panels/timeline/ShortcutController.h"
#include "panels/timeline/TimelinePanel.h"

#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "playback/PlaybackController.h"

#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::setSourceTransportActive(bool active)
{
    const bool wasSourceActive = m_sourceTransportActive;
    m_sourceTransportActive = active;

    if (active) {
        // A click in the Source Monitor takes ownership immediately. Do not
        // leave the sequence controller or its audio clock running until
        // Source Monitor playback happens to start later.
        if (m_playbackController && m_playbackController->isPlaying())
            m_playbackController->pause();
        return;
    }

    bool sourceWasPlaying = false;
    if (m_sourceMonitor && m_sourceMonitor->controller()) {
        sourceWasPlaying = m_sourceMonitor->controller()->isPlaying();
        if (sourceWasPlaying)
            m_sourceMonitor->controller()->pause();
    }

    // Source playback and scrubbing replace or clear the shared AudioEngine's
    // track sources. AudioPlaybackService may still believe its old sources
    // are installed, so force a reload on the handoff back to the sequence.
    if (wasSourceActive || sourceWasPlaying) {
        invalidateAudioSources();
        ensureAudioSourcesLoaded();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

TimelineWorkspace::TimelineWorkspace(QWidget* parent)
    : QWidget(parent)
    , m_audioPlayback(std::make_unique<AudioPlaybackService>())
    , m_compositeService(std::make_unique<CompositeService>())
{
    setObjectName("TimelineWorkspace");

    // Live media file-swap watcher.  Collaborators are injected as accessor
    // functions because timeline/pool/bin are (re)set over our lifetime.
    m_mediaWatch = std::make_unique<MediaWatchController>(MediaWatchController::Config{
        /*timeline=*/      [this] { return m_timeline; },
        /*mediaPool=*/     [this] { return m_mediaPool; },
        /*projectBin=*/    [this] { return m_projectBin; },
        /*onMediaChanged=*/[this](const std::filesystem::path& p) { refreshChangedMedia(p); },
    });

    // Premiere-style tilde panel maximize/restore.
    m_panelMaximize = std::make_unique<PanelMaximizeController>(PanelMaximizeController::Config{
        /*ready=*/          [this] { return m_panelsBuilt; },
        /*innerMainWindow=*/[this] { return m_innerMainWindow; },
        /*edgeSplitter=*/   [this] { return m_edgeSplitter; },
        /*dockWidgets=*/    &m_dockWidgets,
        /*fallbackPanel=*/  [this] { return static_cast<QWidget*>(m_timelinePanel); },
    });

    // Program Monitor transform-overlay binder (drag handlers, overlay
    // sync, drag-session state).
    m_overlay = std::make_unique<OverlayController>(this);

    // Drag-and-drop binder (media / effect / nest drops).
    m_drop = std::make_unique<DropController>(this);

    // Keyboard routing binder (key handling + QShortcut registration).
    m_shortcuts = std::make_unique<ShortcutController>(this);
}

void TimelineWorkspace::togglePanelMaximize()
{
    m_panelMaximize->toggle();
}

// ── OverlayController shims ──────────────────────────────────────────────
// Keep the many intra-workspace call sites unchanged while the overlay
// logic lives in OverlayController.

void TimelineWorkspace::updateTransformOverlay()       { m_overlay->updateTransformOverlay(); }
void TimelineWorkspace::scheduleOverlayRefresh()       { m_overlay->scheduleOverlayRefresh(); }
void TimelineWorkspace::wireTransformOverlaySignals()  { m_overlay->wireTransformOverlaySignals(); }
void TimelineWorkspace::wireViewportTransformSignals() { m_overlay->wireViewportTransformSignals(); }
void TimelineWorkspace::wireOverlayToolSignals()       { m_overlay->wireOverlayToolSignals(); }

// ── DropController shims ─────────────────────────────────────────────────
void TimelineWorkspace::wireMediaDropSignals()         { m_drop->wireMediaDropSignals(); }
void TimelineWorkspace::wireEffectDropSignals()        { m_drop->wireEffectDropSignals(); }
void TimelineWorkspace::wireNestSignals()              { m_drop->wireNestSignals(); }

// ── ShortcutController shims / wrappers ──────────────────────────────────
// The controller accepts events it handles and leaves unhandled ones
// ignored; only those fall through to the QWidget default.

void TimelineWorkspace::registerKeyboardShortcuts()    { m_shortcuts->registerKeyboardShortcuts(); }

void TimelineWorkspace::keyPressEvent(QKeyEvent* event)
{
    m_shortcuts->handleKeyPress(event);
    if (!event->isAccepted())
        QWidget::keyPressEvent(event);
}

void TimelineWorkspace::keyReleaseEvent(QKeyEvent* event)
{
    m_shortcuts->handleKeyRelease(event);
    if (!event->isAccepted())
        QWidget::keyReleaseEvent(event);
}

TimelineWorkspace::~TimelineWorkspace()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop timers before destroying members
    if (m_meterTimer) {
        m_meterTimer->stop();
    }

    // Stop ProgramMonitor's async render thread BEFORE our members are
    // destroyed — the render thread's compositeCallback captures `this`
    // and accesses m_timeline, m_mediaPool, GPU resources, etc.
    if (m_programMonitor) {
        m_programMonitor->stopPolling();
        m_programMonitor->setCompositeCallback(nullptr);
    }

    // Safe-mode callback cleanup removed in P2 of CLAUDE_IMPROVEMENT_PLAN.

    // Cancel any in-flight background audio decode before destroying
    if (m_audioPlayback) {
        m_audioPlayback->cancelWarm();
        m_audioPlayback->waitForWarm();
    }

    // Destroy composite service (flushes GPU caches, destroys composite slot)
    m_compositeService.reset();
}

void TimelineWorkspace::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    QWidget::mousePressEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
