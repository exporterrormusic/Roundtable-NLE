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
 * Binder controllers (god-class decomposition, fable_cleanup.txt §3.1):
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
#include "panels/timeline/MediaWatchController.h"
#include "panels/timeline/PanelMaximizeController.h"
#include "panels/timeline/TimelinePanel.h"

#include "panels/monitors/ProgramMonitor.h"

#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace rt {

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
}

void TimelineWorkspace::togglePanelMaximize()
{
    m_panelMaximize->toggle();
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
