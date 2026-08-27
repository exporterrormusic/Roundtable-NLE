/*
 * MainWindow — Tabbed workspace (DaVinci Resolve–style page tabs).
 *
 * Step 26 (revised): 5 top-level tabs replace the single dock-everything view.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │ Menu:  File  Edit  View  Timeline  Audio  Help                  │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ [ PROJECTS ] [ AUDIO ] [ CHARACTERS ] [ TIMELINE ] [ EXPORT ]  │
 * ├══════════════════════════════════════════════════════════════════┤
 * │                                                                  │
 * │            << active page fills remaining space >>               │
 * │                                                                  │
 * └──────────────────────────────────────────────────────────────────┘
 * │ Status bar                                                       │
 *
 * Pages:
 *   0. PROJECTS   — ProjectPanel (new/open/save/import/settings + project table)
 *   1. AUDIO      — AudioSync panel (script → audio → transcribe → sync → export)
 *   2. CHARACTERS — CharacterShotPanel (LIBRARY + COMPOSE + SETTINGS sub-rail)
 *   3. TIMELINE   — TimelineWorkspace (nested QMainWindow with dockable NLE panels)
 *   4. EXPORT     — ExportPanel (render settings, queue, progress)
 *
 * The TIMELINE page uses a nested QMainWindow so it retains full dock support.
 */

#pragma once

#include <QMainWindow>
#include <QMap>

#include "project/Settings.h"  // for Resolution

#include "project/Settings.h"
#include <QString>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QAction;
class QWidget;
class QActionGroup;
class QDockWidget;
class QFileInfo;
class QMenu;
class QSettings;
class QButtonGroup;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QLabel;
class QProgressBar;
class QTimer;

namespace rt {

// Forward declarations
class UpdateChecker;
class AudioSync;
class VoiceGenerationPanel;
class VoiceGenerationService;
class CharacterBrowser;
class CharacterShotPanel;
class CommandStack;
class ExportPanel;
class MediaPool;
class MediaSourceService;
class PlaybackController;
class Project;
class ProjectPanel;
enum class ProjectTemplate : int;
class ShortcutManager;
class ShotComposer;
class TimelineWorkspace;
class AudioEngine;
class ModelManager;
class Timeline;

// Panel forward declarations (delegated through TimelineWorkspace)
class EffectControlsPanel;
class EffectsPanel;
class HistoryPanel;
class KeyframeEditor;
class PropertiesPanel;
class ProgramMonitor;
class ProjectBin;
class SourceMonitor;
class TimelinePanel;

/// Page index constants (5 top-level pages).
enum class Page : int
{
    Projects   = 0,
    Characters = 1,   ///< Combined CharacterShotPanel (LIBRARY + COMPOSE sub-rail)
    Audio      = 2,
    Timeline   = 3,
    Export     = 4
};

class ProjectController;

/// The main application window (tabbed pages).
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Show / hide a status-bar busy spinner with a message.
    void showBusyIndicator(const QString& message);
    void hideBusyIndicator();

    /// Whether a project open is in flight (off-thread load and/or background
    /// media warmup still running).  While true, the loading overlay is up and
    /// the qApp event filter swallows user input so half-wired project state
    /// cannot be touched.
    [[nodiscard]] bool isProjectLoading() const noexcept {
        return m_projectLoading.load(std::memory_order_acquire);
    }

    /// Check for auto-save recovery files on startup and offer recovery.
    void checkCrashRecovery();

    // ── Dependency injection ────────────────────────────────────────────

    void setTimeline(Timeline* timeline);
    void setCommandStack(CommandStack* stack);
    void setShortcutManager(ShortcutManager* mgr);
    void setAudioEngine(AudioEngine* engine);
    void setPlaybackController(PlaybackController* controller);
    void setMediaPool(MediaPool* pool);
    void setMediaSourceService(MediaSourceService* service);
    void setModelManager(ModelManager* mgr);

    // ── Build (called by App after dependencies are set) ────────────────

    /// Create all pages and their panels.
    void buildPanels();

    /// Build the menu bar and connect actions.
    void buildMenuBar();

    /// Apply the default workspace layout.
    void applyDefaultLayout();

    // ── Page navigation ─────────────────────────────────────────────────

    /// Switch to a page by index.
    void setCurrentPage(Page page);

    /// Get the current page.
    [[nodiscard]] Page currentPage() const noexcept;

    // ── Workspace ───────────────────────────────────────────────────────

    /// Save the current workspace layout to QSettings.
    void saveWorkspace(const QString& name = "default");

    /// Restore a previously saved workspace layout.
    bool restoreWorkspace(const QString& name = "default");

    /// Save the current workspace layout to a binary file (for bundling as default).
    void saveWorkspaceToFile(const QString& filePath);

    /// Restore a workspace layout from a binary file.
    bool restoreWorkspaceFromFile(const QString& filePath);

    /// Load a saved workspace preset (e.g. "DEFAULT") into the dock layout.
    void restoreWorkspacePreset(const QString& presetName);

    // ── Panel accessors (for testing / backward compat) ─────────────────

    // Top-level pages
    [[nodiscard]] ProjectPanel*         projectPanel()        const noexcept { return m_projectPanel; }
    [[nodiscard]] AudioSync*            audioSync()           const noexcept { return m_audioSync; }
    [[nodiscard]] VoiceGenerationPanel* voiceGenerationPanel() const noexcept { return m_voiceGenerationPanel; }
    [[nodiscard]] CharacterShotPanel*   characterShotPanel()  const noexcept { return m_characterShotPanel; }
    [[nodiscard]] CharacterBrowser*     characterBrowser()    const noexcept;
    [[nodiscard]] ShotComposer*         shotComposer()        const noexcept;
    [[nodiscard]] TimelineWorkspace*    timelineWorkspace()   const noexcept { return m_timelineWorkspace; }
    [[nodiscard]] ExportPanel*          exportPanel()         const noexcept { return m_exportPanel; }

    // ── Core-service accessors ───────────────────────────────────────────
    // The explicit surface ProjectController works through — replaces the
    // former `friend class ProjectController`.
    [[nodiscard]] Project*            currentProject()     const noexcept { return m_currentProject.get(); }
    [[nodiscard]] Timeline*           timeline()           const noexcept { return m_timeline; }
    [[nodiscard]] CommandStack*       commandStack()       const noexcept { return m_commandStack; }
    [[nodiscard]] PlaybackController* playbackController() const noexcept { return m_playbackController; }
    [[nodiscard]] MediaPool*          mediaPool()          const noexcept { return m_mediaPool; }
    [[nodiscard]] AudioEngine*        audioEngine()        const noexcept { return m_audioEngine; }
    [[nodiscard]] QMenu*              recentProjectsMenu() const noexcept { return m_recentProjectsMenu; }
    /// True once the destructor has started (guards async continuations).
    [[nodiscard]] bool isDestroying() const noexcept {
        return m_destroying.load(std::memory_order_acquire);
    }
    /// Transfer ownership of the current project in / out.  Project
    /// lifecycle only — ProjectController::setCurrentProject is the
    /// intended caller.  (Defined out-of-line: unique_ptr<Project> needs
    /// the complete type.)
    void adoptCurrentProject(std::unique_ptr<Project> p);
    [[nodiscard]] std::unique_ptr<Project> takeCurrentProject();

    /// Raise the full-window non-interactive overlay and set m_projectLoading.
    void engageLoadingOverlay(const QString& message);
    /// Drop the overlay and clear m_projectLoading (idempotent).
    void disengageLoadingOverlay();
    /// Update the loading-overlay message without touching its progress state.
    void setLoadingOverlayText(const QString& message);

    // Delegate through TimelineWorkspace for backward compatibility
    [[nodiscard]] TimelinePanel*    timelinePanel()    const noexcept;
    [[nodiscard]] SourceMonitor*    sourceMonitor()    const noexcept;
    [[nodiscard]] ProgramMonitor*   programMonitor()   const noexcept;
    [[nodiscard]] ProjectBin*       projectBin()       const noexcept;
    [[nodiscard]] PropertiesPanel*  propertiesPanel()  const noexcept;
    [[nodiscard]] EffectControlsPanel* effectControlsPanel() const noexcept;
    [[nodiscard]] EffectsPanel*     effectsPanel()     const noexcept;
    [[nodiscard]] KeyframeEditor*   keyframeEditor()   const noexcept;
    [[nodiscard]] HistoryPanel*     historyPanel()     const noexcept;

    /// Get a dock widget by panel name (delegates to TimelineWorkspace).
    [[nodiscard]] QDockWidget* dockForPanel(const QString& panelName) const;

    /// Is full-screen preview active?
    [[nodiscard]] bool isFullScreenPreview() const noexcept { return m_fullScreenPreview; }

    /// Number of docks in the Timeline workspace.
    [[nodiscard]] int dockCount() const noexcept;

    /// Number of top-level pages.
    [[nodiscard]] int pageCount() const noexcept { return 5; }

signals:
    /// Emitted when workspace layout changes.
    void workspaceChanged();

    /// Emitted when the active page changes.
    void pageChanged(int index);

    /// Emitted when full-screen preview toggles.
    void fullScreenPreviewChanged(bool active);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    /// Restoring from minimized recreates the Vulkan swapchain; the last
    /// presented frame is gone and, when paused, nothing recomposites
    /// until the user scrubs/plays — the Program Monitor stays blank.
    /// Force a recomposite+present on un-minimize.
    void changeEvent(QEvent* event) override;

public slots:
    /// Make sequence `index` the active timeline.  When `seekTick` is >= 0 the
    /// new sequence's playhead is moved there (used when double-clicking a
    /// nested clip so the inner sequence opens at the matching position,
    /// Premiere-style); otherwise the sequence's own saved playhead is kept.
    void switchSequence(size_t index, int64_t seekTick = -1);

    /// Show the "GPU error — please restart" modal dialog.  Invoked via
    /// QMetaObject::invokeMethod(Qt::QueuedConnection) from the GpuContext
    /// fatal-failure callback (which may fire on any thread).  Stops
    /// playback, and offers Restart / Quit.
    void showGpuFatalError();

private slots:
    void onPageTabChanged(int index);
    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onRestoreFromAutoSave();
    void onCreateProjectFromPanel(const QString& name, uint32_t resW, uint32_t resH,
                                  double fps, const QString& saveDir);
    void onOpenProjectFromPanel(const QString& name);
    void onDeleteProjectFromPanel(const QString& name, const QString& filePath);
    void onRenameProjectFromPanel(const QString& oldName, const QString& newName);
    void onDuplicateProjectFromPanel(const QString& name);
    void onRevealProjectInExplorer(const QString& name);
    void onNewProjectForMedia(const QString& filePath, int64_t atTick, size_t trackIndex);
    void onSequenceSettingsRequested(size_t seqIdx);

    void scaleClipsInSequence(class Timeline* seq,
                              const Resolution& from,
                              const Resolution& to);

    /// Refresh the UI after sequence settings change.
    void applySequenceSettingsRefresh(uint32_t resW, uint32_t resH, double fps);
    void onOpenRecentProjectFromPanel(const QString& filePath);
    void onImportProject(const QString& srcPath);
    void onExportProject(const QString& name, const QString& dstPath);
    void onImportSrt();
    void onExportSrt();
    void onExportChapters();
    void onProjectsDirChanged(const QString& newDir);
    void onUndo();
    void onRedo();
    void onToggleFullScreen();
    void onAbout();
    void onShowThirdPartyLicenses();

public:
    // ── Auto-update (called from main.cpp via QTimer) ──────────────────
    void onCheckForUpdates();
    void onCheckForUpdatesSilent();
    void onAutoUpdateCheck(bool available, const QString &version);

    /// Restart the auto-save interval countdown from now. Called by the
    /// ProjectController whenever a project is loaded so a freshly-opened
    /// project gets a full interval before its first auto-save (opening the
    /// app "just to test" and closing within the interval never auto-saves).
    void resetAutoSaveTimer();

private:
    void buildFileMenu(QMenuBar* menuBar);
    void buildEditMenu(QMenuBar* menuBar);
    void buildViewMenu(QMenuBar* menuBar);
    void buildTimelineMenu(QMenuBar* menuBar);
    void buildAudioMenu(QMenuBar* menuBar);
    void buildWindowMenu(QMenuBar* menuBar);
    void buildHelpMenu(QMenuBar* menuBar);
    void setupStatusBar();
    void setupPageTabs();

    // ── Project management ──────────────────────────────────────────────
    [[nodiscard]] QString projectsDirectory() const;
    void refreshProjectsList();
    void setCurrentProject(std::unique_ptr<Project> project);
    /// Ask user to save/discard/cancel if current project has unsaved changes.
    /// Returns true if OK to proceed (saved, discarded, or nothing to save).
    [[nodiscard]] bool checkUnsavedChanges();

    /// Capture the current playhead frame and save as project thumbnail.
    void captureProjectThumbnail();

    // ── Dependencies (non-owning) ───────────────────────────────────────
    Timeline*             m_timeline{nullptr};
    CommandStack*          m_commandStack{nullptr};
    ShortcutManager*       m_shortcutManager{nullptr};
    AudioEngine*           m_audioEngine{nullptr};
    PlaybackController*    m_playbackController{nullptr};
    MediaPool*             m_mediaPool{nullptr};
    MediaSourceService*    m_mediaSourceService{nullptr};
    ModelManager*          m_modelManager{nullptr};

    // ── Nav rail + page stack ────────────────────────────────────────────
    QWidget*         m_navRail{nullptr};
    QButtonGroup*    m_navGroup{nullptr};
    QPushButton*     m_navButtons[5]{};        // one per Page enum value
    QStackedWidget*  m_pageStack{nullptr};
    QPushButton*     m_navCollapseBtn{nullptr};
    QPushButton*     m_navExpandBtn{nullptr};
    bool             m_navCollapsed{false};
    void             toggleNavRail();
    // ── Pages (owned by QStackedWidget) ─────────────────────────────────
    ProjectPanel*        m_projectPanel{nullptr};
    AudioSync*           m_audioSync{nullptr};
    VoiceGenerationPanel* m_voiceGenerationPanel{nullptr};
    VoiceGenerationService* m_voiceGenerationService{nullptr};
    CharacterShotPanel*  m_characterShotPanel{nullptr};
    TimelineWorkspace*   m_timelineWorkspace{nullptr};
    ExportPanel*         m_exportPanel{nullptr};

    // ── State ───────────────────────────────────────────────────────────
    bool m_fullScreenPreview{false};
    bool m_panelsBuilt{false};
    // Re-entry guard for the qApp event filter: when it forwards a JKL/Space
    // event into TimelineWorkspace via QApplication::sendEvent, qApp re-runs
    // its event filters on the same event — without this flag we'd recurse.
    bool m_forwardingTransportKey{false};

    // ── Auto-save ───────────────────────────────────────────────────────
    QTimer* m_autoSaveTimer{nullptr};
    void onAutoSave();

    // ── Destruction guard ────────────────────────────────────────────────
    std::atomic<bool> m_destroying{false};

    // ── Recent files ────────────────────────────────────────────────────
    QMenu* m_recentProjectsMenu{nullptr};
    QMenu* m_windowMenu{nullptr};
    void addToRecentFiles(const QString& filePath);
    void updateRecentFilesMenu();

    // ── Status bar busy spinner ────────────────────────────────────────
    QProgressBar* m_busySpinner{nullptr};
    QLabel*       m_busyLabel{nullptr};

    // ── Project-open input lock ─────────────────────────────────────────
    // serializer.load() runs on a worker thread; the heavy parse no longer
    // freezes the UI.  While the parse and the subsequent background media
    // warmup run, a full-window overlay blocks interaction so the user can't
    // scrub / select / play against a half-populated timeline (which the
    // compositor & warmup threads are concurrently touching → use-after-free).
    std::atomic<bool> m_projectLoading{false};
    QWidget*          m_loadingOverlay{nullptr};
    QLabel*           m_loadingOverlayLabel{nullptr};
    QProgressBar*     m_loadingOverlayBar{nullptr};
    QTimer*           m_loadingWatchdog{nullptr};
    // Smoothing: progress arrives in bursts (6 warmup threads); a timer eases
    // the displayed bar value toward the real target so it fills smoothly.
    QTimer*           m_loadingBarTimer{nullptr};
    int               m_loadingBarTarget{0};
    int               m_loadingBarTotal{0};

    /// Load a project off the UI thread, then run `continuation` on the UI
    /// thread with the loaded project (null on failure).  Engages the input
    /// lock for the duration.
    void beginAsyncProjectLoad(
        const std::filesystem::path& path,
        const QString& busyMessage,
        std::function<void(std::unique_ptr<Project>)> continuation);

    /// Slot: background media warmup finished — release the overlay if a load
    /// was pending on it.
    void onBackgroundWarmupFinished();
    /// Slot: background media warmup progress — drive the loading bar.
    void setLoadingProgress(int done, int total);
    /// Release the open input lock now, or keep the overlay up until the
    /// background media warmup completes (shared by the async open paths).
    void releaseOpenLock();
    QAction*      m_undoAct{nullptr};
    QAction*      m_redoAct{nullptr};
    QMenu*        m_editMenu{nullptr};

    std::unique_ptr<Project> m_currentProject;
    // (the last-saved AudioSync blob moved into ProjectController — it is
    //  project-lifecycle state)

    /// Project lifecycle binder — owns the implementation of the project/
    /// recovery methods shimmed above (cleanup audit §3.1).
    std::unique_ptr<ProjectController> m_projectController;

    // ── Auto-update ─────────────────────────────────────────────────────
    rt::UpdateChecker* m_updateChecker{nullptr};
    bool               m_updatePromptShown{false};
};

} // namespace rt
