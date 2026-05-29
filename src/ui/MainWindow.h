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
#include <memory>
#include <vector>

class QAction;
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
class AudioMixer;
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
    [[nodiscard]] CharacterShotPanel*   characterShotPanel()  const noexcept { return m_characterShotPanel; }
    [[nodiscard]] CharacterBrowser*     characterBrowser()    const noexcept;
    [[nodiscard]] ShotComposer*         shotComposer()        const noexcept;
    [[nodiscard]] TimelineWorkspace*    timelineWorkspace()   const noexcept { return m_timelineWorkspace; }
    [[nodiscard]] ExportPanel*          exportPanel()         const noexcept { return m_exportPanel; }

    // Delegate through TimelineWorkspace for backward compatibility
    [[nodiscard]] TimelinePanel*    timelinePanel()    const noexcept;
    [[nodiscard]] SourceMonitor*    sourceMonitor()    const noexcept;
    [[nodiscard]] ProgramMonitor*   programMonitor()   const noexcept;
    [[nodiscard]] ProjectBin*       projectBin()       const noexcept;
    [[nodiscard]] PropertiesPanel*  propertiesPanel()  const noexcept;
    [[nodiscard]] EffectControlsPanel* effectControlsPanel() const noexcept;
    [[nodiscard]] EffectsPanel*     effectsPanel()     const noexcept;
    [[nodiscard]] AudioMixer*       audioMixer()       const noexcept;
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
    /// playback, offers Restart / Quit / Continue-in-safe-mode.
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
    QAction*      m_undoAct{nullptr};
    QAction*      m_redoAct{nullptr};
    QMenu*        m_editMenu{nullptr};

    std::unique_ptr<Project> m_currentProject;
    std::vector<uint8_t>     m_lastSavedAudioSyncBlob;

    // ── Auto-update ─────────────────────────────────────────────────────
    rt::UpdateChecker* m_updateChecker{nullptr};
    bool               m_updatePromptShown{false};
};

} // namespace rt
