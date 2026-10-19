/*
 * TimelineWorkspacePanels.cpp — Coordinator for panel creation, dock layout,
 * playback wiring, and keyboard shortcuts.
 *
 * Split into sub-files for maintainability:
 *   - TimelineWorkspacePanelsCreate.cpp    createPanelWidgets()
 *   - TimelineWorkspacePanelsLayout.cpp    arrangeDockLayout(), installEdgeGuard()
 *   - TimelineWorkspacePanelsPlayback.cpp  wirePlaybackSignals()
 *   - TimelineWorkspacePanelsShortcuts.cpp registerKeyboardShortcuts()
 */
// **Must** come before any header that pulls in <vulkan/vulkan.h> so
// volk can define VK_NO_PROTOTYPES first.  Without this, calls like
// vkQueueWaitIdle resolve to a direct function symbol instead of a
// function-pointer variable, causing an ACCESS_VIOLATION crash.
#include <volk.h>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/ClipBindingController.h"
#include "panels/timeline/DockLayoutManager.h"
#include "Theme.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QSplitter>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::buildPanels()
{
    if (m_panelsBuilt) return;

    setUpdatesEnabled(false);

    spdlog::info("TimelineWorkspace::buildPanels() - dockable Premiere Pro-style layout");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // -- Nested QMainWindow for dock-widget support -----------------------
    m_innerMainWindow = new QMainWindow(this);
    m_innerMainWindow->setWindowFlags(Qt::Widget);
    m_innerMainWindow->setDockNestingEnabled(true);
    m_innerMainWindow->setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
    m_innerMainWindow->setAnimated(false);

    // Enable grouped dragging so docks can be separated from tab groups
    // and redocked into other areas (Premiere Pro-style panel rearranging).
    // Explicitly set all needed options: AllowNestedDocks enables splitting
    // within dock areas (dock below/above), AllowTabbedDocks enables tabify.
    m_innerMainWindow->setDockOptions(
        QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
        QMainWindow::GroupedDragging);

    // Phase 1: Create all panel widgets and central widget
    createPanelWidgets();
    m_clipBinding->setPanels({
        /*effectControls=*/m_effectControlsPanel,
        /*graphicsEditor=*/m_GraphicsEditorPanel,
        /*colorGrading=*/  m_ColorGradingPanel,
        /*properties=*/    m_propertiesPanel,
        /*tierList=*/      m_tierListPanel,
        /*effects=*/       m_effectsPanel,
    });

    // Phase 2: Arrange dock layout
    arrangeDockLayout();

    mainLayout->addWidget(m_edgeSplitter);

    // Phase 3: Wire playback signals
    wirePlaybackSignals();

    // Phase 4: Register keyboard shortcuts
    registerKeyboardShortcuts();

    // Phase 5: Final setup
    wirePanelSignals();

    m_dockLayoutManager = std::make_unique<DockLayoutManager>(
        DockLayoutManager::Config{
            m_innerMainWindow,
            m_edgeSplitter,
            &m_dockWidgets,
            this,
            [this](QMainWindow* col) { installEdgeGuard(col); }
        });

    m_panelsBuilt = true;

    if (m_innerMainWindow)
        m_defaultDockState = m_innerMainWindow->saveState(4);

    spdlog::info("TimelineWorkspace::buildPanels() - dockable layout with {} panels", m_dockWidgets.size());

    setUpdatesEnabled(true);
    updateGeometry();
    repaint();
}

} // namespace rt
