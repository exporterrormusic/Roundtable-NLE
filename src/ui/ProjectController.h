/*
 * ProjectController.h — project lifecycle binder for MainWindow.
 *
 * Owns the implementation of everything project-file related:
 *   - new / open / save / save-as / import / export / rename / delete /
 *     duplicate / reveal, the recent-files list, and the projects dir
 *   - the async off-thread project load (beginAsyncProjectLoad) and the
 *     open input lock release
 *   - setCurrentProject (the big wiring step that points every panel at
 *     the new project)
 *   - auto-save + crash/auto-save recovery (checkCrashRecovery,
 *     onRestoreFromAutoSave) and the SRT import/export entry points
 *
 * Extracted from MainWindowProject*.cpp + MainWindowRecovery.cpp
 * (god-class decomposition, cleanup audit §3.1, second half).
 * NARROWED (no friend): the controller works exclusively through
 * MainWindow's public surface — the panel accessors plus the
 * core-service block (currentProject / adoptCurrentProject /
 * takeCurrentProject, timeline / setTimeline, commandStack,
 * playbackController, mediaPool, audioEngine, recentProjectsMenu,
 * isDestroying, and the loading-overlay methods).  MainWindow keeps
 * its public/slot surface as one-line shims so menu/panel connects
 * and main.cpp are untouched.  Dialogs are parented to the MainWindow
 * widget (m_mw).
 *
 * Definitions: ProjectController.cpp, ProjectControllerSet.cpp,
 * ProjectControllerHandlers.cpp, ProjectControllerMisc.cpp,
 * ProjectControllerRecovery.cpp (one per former MainWindow* TU).
 */
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace rt {

class MainWindow;
class Project;

class ProjectController : public QObject {
public:
    explicit ProjectController(MainWindow* mw) : m_mw(mw) {}

    // ── New / open / save ───────────────────────────────────────────────
    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();

    // ── ProjectPanel handlers ───────────────────────────────────────────
    void onCreateProjectFromPanel(const QString& name, uint32_t resW, uint32_t resH,
                                  double fps, const QString& saveDir);
    void onOpenProjectFromPanel(const QString& name);
    void onDeleteProjectFromPanel(const QString& name, const QString& filePath);
    void onRenameProjectFromPanel(const QString& oldName, const QString& newName);
    void onDuplicateProjectFromPanel(const QString& name);
    void onRevealProjectInExplorer(const QString& name);
    void onNewProjectForMedia(const QString& filePath, int64_t atTick, size_t trackIndex);
    void onOpenRecentProjectFromPanel(const QString& filePath);
    void onImportProject(const QString& srcPath);
    void onExportProject(const QString& name, const QString& dstPath);
    void onProjectsDirChanged(const QString& newDir);

    // ── SRT subtitles ───────────────────────────────────────────────────
    void onImportSrt();
    void onExportSrt();

    // ── Core lifecycle ──────────────────────────────────────────────────
    [[nodiscard]] QString projectsDirectory() const;
    void refreshProjectsList();
    void setCurrentProject(std::unique_ptr<Project> project);
    [[nodiscard]] bool checkUnsavedChanges();
    void captureProjectThumbnail();
    void beginAsyncProjectLoad(
        const std::filesystem::path& path,
        const QString& busyMessage,
        std::function<void(std::unique_ptr<Project>)> continuation);
    void releaseOpenLock();

    // ── Recent files ────────────────────────────────────────────────────
    void addToRecentFiles(const QString& filePath);
    void updateRecentFilesMenu();

    // ── Auto-save / recovery ────────────────────────────────────────────
    void onAutoSave();
    void onRestoreFromAutoSave();
    void checkCrashRecovery();
    void showGpuFatalError();

    /// Forget the AudioSync blob captured at last save (called when the
    /// Audio tab is wiped, e.g. on auto-created projects).
    void clearLastSavedAudioSyncBlob() { m_lastSavedAudioSyncBlob.clear(); }

private:
    MainWindow* m_mw{nullptr};

    /// Serialized AudioSync state captured at last save — compared on
    /// auto-save to detect Audio-tab changes (project-lifecycle state,
    /// moved here from MainWindow during friend-narrowing).
    std::vector<uint8_t> m_lastSavedAudioSyncBlob;
};

} // namespace rt
