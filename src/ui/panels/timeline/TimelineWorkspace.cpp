/*
 * TimelineWorkspace.cpp — Splitter-based NLE workspace coordinator.
 *
 * Thin coordinator after extracting togglePanelMaximize →
 * TimelineWorkspaceToggleMaximize.cpp, setTimeline/refreshAfterUndoRedo →
 * TimelineWorkspaceIntegration.cpp, editing commands →
 * TimelineWorkspaceEditCommands.cpp, dependency injection →
 * TimelineWorkspaceDeps.cpp, and dock persistence →
 * TimelineWorkspaceDock.cpp.
 *
 * Contains: constructor, destructor, mousePressEvent.
 *
 * Sub-files (all in panels/timeline/):
 *   TimelineWorkspaceToggleMaximize.cpp  — togglePanelMaximize()
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
