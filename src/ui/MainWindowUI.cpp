/*
 * MainWindowUI.cpp — Tabbed-page UI coordinator.
 *
 * Thin coordinator after extracting buildPanels → MainWindowUIBuild.cpp,
 * page navigation → MainWindowUINav.cpp, and workspace/accessors/statusbar
 * → MainWindowUIWorkspace.cpp.
 *
 * Contains: constructor, destructor, dependency injection setters.
 *
 * Sub-files (all in src/ui/):
 *   MainWindowUIBuild.cpp      — buildPanels() (all 5 pages + signal wiring)
 *   MainWindowUINav.cpp        — setupPageTabs(), setCurrentPage/currentPage,
 *                                 toggleNavRail(), onPageTabChanged()
 *   MainWindowUIWorkspace.cpp  — panel accessors, applyDefaultLayout(),
 *                                 saveWorkspace/restoreWorkspace, status bar
 *   MainWindowMenus.cpp        — all build*Menu() methods
 *   MainWindowProject.cpp      — project management (open/save/close)
 *   MainWindowRecovery.cpp     — crash recovery dialog
 */

#include "MainWindow.h"
#include "ProjectController.h"

#include "panels/characters/CharacterShotPanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/monitors/SourceMonitor.h"
#include "spine/ModelManager.h"
#include "project/Project.h"

#include "UiScale.h"
#include "Settings.h"

#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>


namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ═════════════════════════════════════════════════════════════════════════════

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setObjectName("MainWindow");
    setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));
    setMinimumSize(1280, 720);

    // Project lifecycle binder (open/save/recovery/auto-save/recents);
    // the MainWindow project methods are one-line shims to it.
    m_projectController = std::make_unique<ProjectController>(this);

    setDocumentMode(true);

    // Disable QMainWindow dock animations. The dock-undock + auto-resize
    // animations fire QPropertyAnimation steps that re-emit setGeometry on
    // child widgets ~60 times in 300ms. For the VulkanViewport that means
    // 60 swapchain recreations, which can wedge the NVIDIA driver and trip
    // VK_ERROR_DEVICE_LOST mid-playback.
    setAnimated(false);

    // Install global event filter for JKL transport keys
    if (qApp) qApp->installEventFilter(this);

    // Auto-save with configurable interval from preferences
    m_autoSaveTimer = new QTimer(this);
    {
        auto s = rt::appSettings();
        int minutes = s.value("AutosaveInterval", 5).toInt();
        m_autoSaveTimer->setInterval(minutes * 60 * 1000);
    }
    connect(m_autoSaveTimer, &QTimer::timeout, this, &MainWindow::onAutoSave);
    m_autoSaveTimer->start();

    setupPageTabs();
    setupStatusBar();

    // ── Per-monitor DPI relayout ────────────────────────────────────────
    // When the window is dragged across monitors with different DPI/scale,
    // Qt updates devicePixelRatio but does NOT automatically re-polish
    // stylesheets, re-evaluate font metrics, or re-run layouts that cached
    // pixel sizes from the old screen. Force a full relayout on every
    // screen change so controls stay the correct apparent size.
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto installScreenWatch = [this](QWindow* win) {
            if (!win) return;
            connect(win, &QWindow::screenChanged, this,
                    [this](QScreen* newScreen) {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (!newScreen) return;
                        spdlog::info("MainWindow: moved to screen '{}' ({}x{} dpr={:.2f} dpi={:.0f})",
                                     newScreen->name().toStdString(),
                                     newScreen->size().width(),
                                     newScreen->size().height(),
                                     newScreen->devicePixelRatio(),
                                     newScreen->logicalDotsPerInch());
                        // Update global UI scale factor for the new screen.
                        // This rescales every widget registered via
                        // UiScale::setScaledFixed*() and emits a signal so
                        // panels can rebuild stylesheet-driven sizes.
                        rt::UiScale::updateForScreen(newScreen);
                        // Re-polish the entire widget tree so any QSS that
                        // depends on font metrics or pixel sizes is rebuilt
                        // for the new DPI, then force a full relayout.
                        if (auto* st = style()) {
                            st->unpolish(this);
                            st->polish(this);
                        }
                        const auto kids = findChildren<QWidget*>();
                        for (QWidget* w : kids) {
                            if (auto* st = w->style()) {
                                st->unpolish(w);
                                st->polish(w);
                            }
                            w->updateGeometry();
                        }
                        updateGeometry();
                        adjustSize();
                        update();
                    });
            // Initialise scale for the screen the window is currently on.
            if (auto* s = win->screen()) {
                rt::UiScale::updateForScreen(s);
            }
        };
        installScreenWatch(windowHandle());
        if (!windowHandle()) {
            QTimer::singleShot(100, this, [this, installScreenWatch]() {
                installScreenWatch(windowHandle());
            });
        }
    });

    spdlog::debug("MainWindow constructed (tabbed pages)");
}

MainWindow::~MainWindow()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop timers before destroying children — prevents timer events
    // from firing into partially-destroyed object.
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }

    spdlog::debug("MainWindow destroyed");
}

// ═════════════════════════════════════════════════════════════════════════════
// ProjectController shims — keep the public/slot surface (menu + panel
// connects, main.cpp, App.cpp) unchanged while the implementations live in
// ProjectController*.cpp (god-class decomposition, cleanup audit §3.1).
// ═════════════════════════════════════════════════════════════════════════════

QString MainWindow::projectsDirectory() const { return m_projectController->projectsDirectory(); }
bool MainWindow::checkUnsavedChanges()        { return m_projectController->checkUnsavedChanges(); }
void MainWindow::refreshProjectsList()        { m_projectController->refreshProjectsList(); }
void MainWindow::captureProjectThumbnail()    { m_projectController->captureProjectThumbnail(); }
void MainWindow::releaseOpenLock()            { m_projectController->releaseOpenLock(); }
void MainWindow::addToRecentFiles(const QString& filePath) { m_projectController->addToRecentFiles(filePath); }
void MainWindow::updateRecentFilesMenu()      { m_projectController->updateRecentFilesMenu(); }
void MainWindow::checkCrashRecovery()         { m_projectController->checkCrashRecovery(); }
void MainWindow::showGpuFatalError()          { m_projectController->showGpuFatalError(); }
void MainWindow::onAutoSave()                 { m_projectController->onAutoSave(); }
void MainWindow::resetAutoSaveTimer()         { if (m_autoSaveTimer) m_autoSaveTimer->start(); }
void MainWindow::onRestoreFromAutoSave()      { m_projectController->onRestoreFromAutoSave(); }
void MainWindow::onNewProject()               { m_projectController->onNewProject(); }
void MainWindow::onOpenProject()              { m_projectController->onOpenProject(); }
void MainWindow::onSaveProject()              { m_projectController->onSaveProject(); }
void MainWindow::onSaveProjectAs()            { m_projectController->onSaveProjectAs(); }
void MainWindow::onImportSrt()                { m_projectController->onImportSrt(); }
void MainWindow::onExportSrt()                { m_projectController->onExportSrt(); }

void MainWindow::setCurrentProject(std::unique_ptr<Project> project)
{
    m_projectController->setCurrentProject(std::move(project));
}

void MainWindow::beginAsyncProjectLoad(
    const std::filesystem::path& path,
    const QString& busyMessage,
    std::function<void(std::unique_ptr<Project>)> continuation)
{
    m_projectController->beginAsyncProjectLoad(path, busyMessage, std::move(continuation));
}

void MainWindow::onCreateProjectFromPanel(const QString& name, uint32_t resW, uint32_t resH,
                                          double fps, const QString& saveDir)
{
    m_projectController->onCreateProjectFromPanel(name, resW, resH, fps, saveDir);
}
void MainWindow::onOpenProjectFromPanel(const QString& name)   { m_projectController->onOpenProjectFromPanel(name); }
void MainWindow::onDeleteProjectFromPanel(const QString& name, const QString& filePath)
{
    m_projectController->onDeleteProjectFromPanel(name, filePath);
}
void MainWindow::onRenameProjectFromPanel(const QString& oldName, const QString& newName)
{
    m_projectController->onRenameProjectFromPanel(oldName, newName);
}
void MainWindow::onDuplicateProjectFromPanel(const QString& name) { m_projectController->onDuplicateProjectFromPanel(name); }
void MainWindow::onRevealProjectInExplorer(const QString& name)   { m_projectController->onRevealProjectInExplorer(name); }
void MainWindow::onNewProjectForMedia(const QString& filePath, int64_t atTick, size_t trackIndex)
{
    m_projectController->onNewProjectForMedia(filePath, atTick, trackIndex);
}
void MainWindow::onOpenRecentProjectFromPanel(const QString& filePath) { m_projectController->onOpenRecentProjectFromPanel(filePath); }
void MainWindow::onImportProject(const QString& srcPath)              { m_projectController->onImportProject(srcPath); }
void MainWindow::onExportProject(const QString& name, const QString& dstPath)
{
    m_projectController->onExportProject(name, dstPath);
}
void MainWindow::onProjectsDirChanged(const QString& newDir)          { m_projectController->onProjectsDirChanged(newDir); }

// ═════════════════════════════════════════════════════════════════════════════
// Dependency injection
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::setTimeline(Timeline* timeline) { m_timeline = timeline; }
void MainWindow::adoptCurrentProject(std::unique_ptr<Project> p) { m_currentProject = std::move(p); }
std::unique_ptr<Project> MainWindow::takeCurrentProject() { return std::move(m_currentProject); }
void MainWindow::setCommandStack(CommandStack* stack) { m_commandStack = stack; }
void MainWindow::setShortcutManager(ShortcutManager* mgr) { m_shortcutManager = mgr; }
void MainWindow::setAudioEngine(AudioEngine* engine) { m_audioEngine = engine; }
void MainWindow::setPlaybackController(PlaybackController* controller) { m_playbackController = controller; }
void MainWindow::setMediaPool(MediaPool* pool) {
    m_mediaPool = pool;
    // Also give the SourceMonitor a reference for drag-drop loads
    if (auto* sm = sourceMonitor())
        sm->setMediaPool(pool);
}
void MainWindow::setMediaSourceService(MediaSourceService* service) {
    m_mediaSourceService = service;
}
void MainWindow::setModelManager(ModelManager* mgr) {
    m_modelManager = mgr;
    spdlog::info("MainWindow::setModelManager — mgr={}, scanned={}, "
                 "csp={}, tw={}",
                 static_cast<const void*>(mgr),
                 mgr ? mgr->isScanned() : false,
                 static_cast<const void*>(m_characterShotPanel),
                 static_cast<const void*>(m_timelineWorkspace));
    // Propagate to CharacterShotPanel (and its children: CharacterBrowser,
    // ConversionPanel, ShotComposer) so they refresh after ModelManager
    // finishes its async scan.
    if (m_characterShotPanel)
        m_characterShotPanel->setModelManager(mgr);
    // Propagate to TimelineWorkspace so CharactersPanel refreshes its tree
    if (m_timelineWorkspace)
        m_timelineWorkspace->setModelManager(mgr);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
