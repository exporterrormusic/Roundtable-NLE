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
#include "cache/CachePolicy.h"
#include "audio/AudioPlaybackService.h"
#include "panels/timeline/DockLayoutManager.h"
#include "panels/timeline/DropController.h"
#include "panels/timeline/MediaWarmupController.h"
#include "panels/timeline/MediaWatchController.h"
#include "panels/timeline/OverlayController.h"
#include "panels/timeline/PanelMaximizeController.h"
#include "panels/timeline/SequenceTabsController.h"
#include "panels/timeline/ShortcutController.h"
#include "panels/timeline/TimelinePanel.h"

#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "playback/PlaybackController.h"
#include "playback/SnapshotPublisher.h"
#include "command/CommandStack.h"
#include "edit/EditSession.h"

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
    , m_snapshotPublisher(std::make_unique<SnapshotPublisher>())
    , m_compositeService(std::make_unique<CompositeService>())
{
    setObjectName("TimelineWorkspace");

    // Render threads read published snapshots, never the live model.
    m_compositeService->setSnapshotPublisher(m_snapshotPublisher.get());
    m_snapshotPublishTimer = new QTimer(this);
    m_snapshotPublishTimer->setSingleShot(true);
    m_snapshotPublishTimer->setInterval(0);
    connect(m_snapshotPublishTimer, &QTimer::timeout, this, [this] {
        const bool wasDirty = m_snapshotPublisher->isDirty();
        publishSnapshotNow();
        // An edit that only invalidated the cache (no requestRefresh) is
        // visible only after this publish: re-render the paused frame.
        if (wasDirty && m_programMonitor && m_playbackController
                && !m_playbackController->isPlaying()) {
            m_refreshFromSnapshotPublish = true;
            m_programMonitor->requestRefresh();
            m_refreshFromSnapshotPublish = false;
        }
    });
    m_snapshotVerifyTimer = new QTimer(this);
    m_snapshotVerifyTimer->setInterval(1000);
    connect(m_snapshotVerifyTimer, &QTimer::timeout, this, [this] { verifySnapshot(); });
    m_snapshotVerifyTimer->start();

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

    // Background media warm-up (worker threads; joined in the destructor).
    m_mediaWarmup = std::make_unique<MediaWarmupController>(MediaWarmupController::Config{
        /*timeline=*/  [this] { return m_timeline; },
        /*mediaPool=*/ [this] { return m_mediaPool; },
        /*canWarm=*/   [this] { return m_compositeService != nullptr; },
        /*destroying=*/[this] { return isDestroying(); },
        /*invalidateCompositeCache=*/[this] { invalidateCompositeCache(); },
        /*progress=*/  [this](int done, int total) { emit backgroundWarmupProgress(done, total); },
        /*warmed=*/    [this] { if (m_programMonitor) m_programMonitor->requestRefresh(); },
        /*finished=*/  [this] { emit backgroundWarmupFinished(); },
    });

    // Sequence tab bar (open sequences, tab close/context menu).
    m_sequenceTabs = std::make_unique<SequenceTabsController>(SequenceTabsController::Config{
        /*project=*/           [this] { return m_project; },
        /*destroying=*/        [this] { return isDestroying(); },
        /*sequencesChanged=*/  [this] { markSnapshotDirty(/*allSequences=*/true); },
        /*switchRequested=*/   [this](size_t i) { emit sequenceTabChanged(i); },
        /*renameRequested=*/   [this](size_t i) { emit sequenceTabRenameRequested(i); },
        /*settingsRequested=*/ [this](size_t i) { emit sequenceTabSettingsRequested(i); },
        /*duplicateRequested=*/[this](size_t i) { emit sequenceTabDuplicateRequested(i); },
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

    // MediaPool may already have copied its callback on another thread. Clear
    // future callbacks, close the controller's shared callback gate, then join
    // all stat/warmup workers before any non-owning dependency can disappear.
    if (m_mediaPool)
        m_mediaPool->setOnMediaOpened({});
    if (m_mediaWatch)
        m_mediaWatch->shutdown();
    cancelAndJoinBackgroundMediaWarmups();

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
        m_programMonitor->setBeforeRefreshHook({});
    }
    if (m_snapshotPublishTimer) m_snapshotPublishTimer->stop();
    if (m_snapshotVerifyTimer) m_snapshotVerifyTimer->stop();
    // The edit session and stack outlive this widget; drop the listeners that
    // capture us.
    if (EditSession* edits = m_commandStack ? m_commandStack->editSession() : nullptr)
        edits->setCommitListener({});
    if (m_commandStack && m_historyListenerId)
        m_commandStack->removeHistoryListener(m_historyListenerId);

    // Cancel any in-flight background audio decode before destroying
    if (m_audioPlayback) {
        m_audioPlayback->cancelWarm();
        m_audioPlayback->waitForWarm();
    }

    // Destroy the live compositor while its dependencies are still alive.
    // Export compositors are owned and torn down by ExportRenderSession.
    m_compositeService.reset();
}

void TimelineWorkspace::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    QWidget::mousePressEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
