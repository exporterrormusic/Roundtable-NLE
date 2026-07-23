/*
 * TimelineWorkspacePanelsLayout.cpp — Dock layout arrangement
 * extracted from TimelineWorkspacePanels.cpp::buildPanels().
 *
 * Contains: arrangeDockLayout() — arranges all docks in Premiere Pro
 * default layout, installs edge splitter, wires toolbar tool buttons.
 */

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/DockBehavior.h"
#include "panels/timeline/DockLayoutManager.h"
#include "panels/timeline/TimelinePanel.h"

#include <QDockWidget>
#include <QTabBar>
#include <QToolButton>

namespace rt {

void TimelineWorkspace::arrangeDockLayout()
{
    auto* dockProjectBin      = m_dockWidgets.value("Project Bin");
    auto* dockSourceMonitor   = m_dockWidgets.value("Source Monitor");
    auto* dockProgramMonitor  = m_dockWidgets.value("Program Monitor");
    auto* dockEffectControls  = m_dockWidgets.value("Effect Controls");
    auto* dockEssentialGraphics = m_dockWidgets.value("Graphics Editor");
    auto* dockColorGrading    = m_dockWidgets.value("Color Correction");
    auto* dockEffects         = m_dockWidgets.value("Effects");
    auto* dockHistory         = m_dockWidgets.value("History");
    auto* dockScopes          = m_dockWidgets.value("Scopes");
    auto* dockProperties      = m_dockWidgets.value("Properties");
    auto* dockCharacters      = m_dockWidgets.value("Library");
    auto* dockCaptions        = m_dockWidgets.value("Captions");
    auto* dockTierList        = m_dockWidgets.value("Tier List");
    auto* dockAudioMeters     = m_dockWidgets.value("Audio Meters");
    auto* dockTools           = m_dockWidgets.value("Tools");

    if (!dockProjectBin || !dockSourceMonitor || !dockProgramMonitor) return;

    // =====================================================================
    //  DOCK LAYOUT -- Premiere Pro default arrangement
    //
    //  Top row:  [Project Bin | Source Monitor | Program Monitor | Effect Controls]
    //  Right of central: Audio Meters
    //  Effect Controls area: Effects, Keyframes, History tabbed
    // =====================================================================

    m_innerMainWindow->addDockWidget(Qt::TopDockWidgetArea, dockProjectBin);
    m_innerMainWindow->splitDockWidget(dockProjectBin, dockSourceMonitor, Qt::Horizontal);
    m_innerMainWindow->splitDockWidget(dockSourceMonitor, dockProgramMonitor, Qt::Horizontal);
    m_innerMainWindow->splitDockWidget(dockProgramMonitor, dockEffectControls, Qt::Horizontal);

    // Tab remaining panels onto Effect Controls
    if (dockEssentialGraphics)
        m_innerMainWindow->tabifyDockWidget(dockEffectControls, dockEssentialGraphics);
    if (dockColorGrading)
        m_innerMainWindow->tabifyDockWidget(dockEssentialGraphics ? dockEssentialGraphics : dockEffectControls, dockColorGrading);
    if (dockEffects)
        m_innerMainWindow->tabifyDockWidget(dockColorGrading ? dockColorGrading : dockEffectControls, dockEffects);
    if (dockHistory)
        m_innerMainWindow->tabifyDockWidget(dockEffects ? dockEffects : dockEffectControls, dockHistory);
    if (dockScopes)
        m_innerMainWindow->tabifyDockWidget(dockHistory ? dockHistory : dockEffectControls, dockScopes);
    if (dockProperties)
        m_innerMainWindow->tabifyDockWidget(dockScopes ? dockScopes : dockEffectControls, dockProperties);
    if (dockCharacters)
        m_innerMainWindow->tabifyDockWidget(dockProperties ? dockProperties : dockEffectControls, dockCharacters);
    // Captions must be placed explicitly here — otherwise Qt auto-tabifies the
    // un-positioned dock onto whatever it finds (the Source Monitor), which is
    // what produced the "merged into the Source Monitor" tab glitch.
    if (dockCaptions)
        m_innerMainWindow->tabifyDockWidget(dockCharacters ? dockCharacters : dockEffectControls, dockCaptions);
    if (dockTierList)
        m_innerMainWindow->tabifyDockWidget(dockCaptions ? dockCaptions : dockEffectControls, dockTierList);

    dockEffectControls->raise();

    // Enable tab drag-to-undock
    auto* tabDragFilter = new DockTabDragFilter(m_innerMainWindow, m_innerMainWindow);
    auto* tabBarWatcher = new DockTabBarWatcher(m_innerMainWindow, m_innerMainWindow);
    tabBarWatcher->setWorkspace(this);
    tabBarWatcher->setDragFilter(tabDragFilter);
    m_innerMainWindow->installEventFilter(tabBarWatcher);

    for (auto* tb : m_innerMainWindow->findChildren<QTabBar*>())
        tabBarWatcher->watchTabBar(tb);

    // -- Full-height edge docking -----------------------------------------
    m_edgeSplitter = new QSplitter(Qt::Horizontal, this);
    m_edgeSplitter->setHandleWidth(2);
    m_edgeSplitter->setChildrenCollapsible(false);
    m_edgeSplitter->addWidget(m_innerMainWindow);
    auto* edgeWatcher = new DockEdgeDragWatcher(m_innerMainWindow, m_edgeSplitter, m_edgeSplitter);
    tabDragFilter->setEdgeDragWatcher(edgeWatcher);

    // Audio Meters dock on the right
    if (dockAudioMeters)
        m_innerMainWindow->addDockWidget(Qt::RightDockWidgetArea, dockAudioMeters);

    // Tools dock on the left
    if (dockTools)
        m_innerMainWindow->addDockWidget(Qt::LeftDockWidgetArea, dockTools);

    // Set initial dock sizes
    dockProjectBin->setMinimumWidth(180);
    dockSourceMonitor->setMinimumWidth(200);
    dockProgramMonitor->setMinimumWidth(200);
    dockEffectControls->setMinimumWidth(400);
    if (dockAudioMeters) {
        dockAudioMeters->setMaximumWidth(200);
        dockAudioMeters->setMinimumWidth(80);
    }

    // -- Wire toolbar/tool buttons to TimelinePanel -----------------------
    auto connectTool = [this](QToolButton* btn, EditTool tool) {
        connect(btn, &QToolButton::clicked, this, [this, tool]() {
            if (m_timelinePanel) m_timelinePanel->setActiveTool(tool);
        });
    };

    if (m_toolButtons[0])
        connectTool(m_toolButtons[0], EditTool::Selection);
    if (m_toolButtons[1])
        connectTool(m_toolButtons[1], EditTool::Ripple);
    if (m_toolButtons[2])
        connectTool(m_toolButtons[2], EditTool::Rolling);
    if (m_toolButtons[3])
        connectTool(m_toolButtons[3], EditTool::Razor);
    if (m_toolButtons[4])
        connectTool(m_toolButtons[4], EditTool::Slip);
    if (m_toolButtons[5])
        connectTool(m_toolButtons[5], EditTool::Slide);
    if (m_toolButtons[6])
        connectTool(m_toolButtons[6], EditTool::Text);
    if (m_toolButtons[7])
        connectTool(m_toolButtons[7], EditTool::Zoom);
    if (m_toolButtons[8])
        connectTool(m_toolButtons[8], EditTool::PenMask);

    // Snap button retained in createPanelWidgets to be passed here
    // Find the snap button via toolbar children
    auto* toolbar = m_innerMainWindow->findChild<QWidget*>("timelineToolbar");
    if (toolbar) {
        auto* btnSnap = toolbar->findChild<QToolButton*>();
        // Connect snap toggle
        if (btnSnap) {
            connect(btnSnap, &QToolButton::toggled, this, [this](bool checked) {
                if (m_timelinePanel) m_timelinePanel->setSnappingEnabled(checked);
            });
        }
    }

    // Sync tool buttons when tool is changed via keyboard shortcut
    if (m_timelinePanel) {
        connect(m_timelinePanel, &TimelinePanel::toolChanged,
                this, [=](EditTool tool) {
            auto* btnSel  = m_toolButtons[0];
            auto* btnRip  = m_toolButtons[1];
            auto* btnRoll = m_toolButtons[2];
            auto* btnRaz  = m_toolButtons[3];
            auto* btnSlip = m_toolButtons[4];
            auto* btnSli2 = m_toolButtons[5];
            auto* btnTxt  = m_toolButtons[6];
            auto* btnZm   = m_toolButtons[7];
            auto* btnPen  = m_toolButtons[8];
            if (btnSel)  btnSel->setChecked(tool == EditTool::Selection);
            if (btnRip)  btnRip->setChecked(tool == EditTool::Ripple);
            if (btnRoll) btnRoll->setChecked(tool == EditTool::Rolling);
            if (btnRaz)  btnRaz->setChecked(tool == EditTool::Razor);
            if (btnSlip) btnSlip->setChecked(tool == EditTool::Slip);
            if (btnSli2) btnSli2->setChecked(tool == EditTool::Slide);
            if (btnTxt)  btnTxt->setChecked(tool == EditTool::Text);
            if (btnZm)   btnZm->setChecked(tool == EditTool::Zoom);
            if (btnPen)  btnPen->setChecked(tool == EditTool::PenMask);
        });
    }
}

void TimelineWorkspace::installEdgeGuard(QMainWindow* edgeCol)
{
    if (!edgeCol || !m_innerMainWindow) return;
    auto* guard = EdgeColumnGuard::forColumn(edgeCol);
    if (!guard)
        guard = new EdgeColumnGuard(edgeCol, m_innerMainWindow);
    for (auto* dock : edgeCol->findChildren<QDockWidget*>()) {
        if (!dock->isFloating())
            guard->watchDock(dock);
    }
}

} // namespace rt
