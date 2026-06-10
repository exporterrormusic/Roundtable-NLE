/*
 * MainWindowProjectHandlers.cpp — Project CRUD handlers extracted from
 * MainWindowProject.cpp.
 *
 * Contains: onCreateProjectFromPanel, onOpenProjectFromPanel,
 * onDeleteProjectFromPanel, onRenameProjectFromPanel,
 * onDuplicateProjectFromPanel, onRevealProjectInExplorer,
 * onNewProjectForMedia, onOpenRecentProjectFromPanel,
 * onImportProject, onExportProject, onNewProject,
 * onOpenProject, onSaveProject, onSaveProjectAs.
 */

#include "MainWindow.h"
#include "PathUtils.h"

#include "panels/audio/AudioSync.h"
#include "panels/project/ProjectPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/TimelinePanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/export/ExportPanel.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/EffectsPanel.h"
#include "panels/library/LibraryPanel.h"
#include "panels/characters/CharactersPanel.h"

#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "media/PlaybackScheduler.h"
#include "timeline/Timeline.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "SrtIO.h"

#include "Settings.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QStatusBar>

#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Async project load primitive
// ═════════════════════════════════════════════════════════════════════════════

// Parse the .rtp on a worker thread (the heavy, UI-thread-freezing part —
// see ProjectSerializer::load), then hand the freshly-built Project back to
// `continuation` on the UI thread.  A freshly-loaded Project is not yet wired
// to the MediaPool, timeline, or any widget, so parsing it off-thread is safe;
// every step that touches those (setCurrentProject) stays on the UI thread in
// the continuation.  The full-window input lock is engaged for the duration so
// the user can't interact with the old/half-wired project mid-load.
void MainWindow::beginAsyncProjectLoad(
    const std::filesystem::path& path,
    const QString& busyMessage,
    std::function<void(std::unique_ptr<Project>)> continuation)
{
    engageLoadingOverlay(busyMessage);

    auto t0 = std::chrono::steady_clock::now();
    std::filesystem::path p = path;  // owned copy for the worker thread
    // shared_ptr keeps the move-only-payload continuation alive across the
    // thread hop while staying copyable for QMetaObject::invokeMethod.
    auto cont = std::make_shared<std::function<void(std::unique_ptr<Project>)>>(
        std::move(continuation));

    std::thread([this, p, cont, t0]() {
        ProjectSerializer serializer;
        Project* raw = serializer.load(p).release();
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        spdlog::warn("[OPEN-PERF] [ASYNC-OPEN] serializer.load '{}' off-thread in {:.0f}ms",
                     pathToUtf8(p), ms);
        QMetaObject::invokeMethod(qApp, [this, raw, cont]() {
            std::unique_ptr<Project> project(raw);
            if (m_destroying.load(std::memory_order_acquire)) return;
            (*cont)(std::move(project));
        }, Qt::QueuedConnection);
    }).detach();
}

// Common tail for the async open paths: drop the input lock now, or keep the
// overlay up until the background media warmup finishes (whichever applies).
// Centralized so every entry point releases the lock identically.
void MainWindow::releaseOpenLock()
{
    if (m_timelineWorkspace && m_timelineWorkspace->isBackgroundWarmupActive()) {
        // Overlay stays up; backgroundWarmupFinished() drops it once the
        // timeline's media is warm and safe to interact with.
        showBusyIndicator(tr("Loading media…"));
        if (m_loadingOverlayLabel) m_loadingOverlayLabel->setText(tr("Loading media…"));
    } else {
        disengageLoadingOverlay();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Project CRUD — panel-backed operations
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::onCreateProjectFromPanel(const QString& name, uint32_t resW, uint32_t resH,
                                          double fps, const QString& saveDir)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("Creating project: {} ({}x{} @ {} fps)",
                 name.toStdString(), resW, resH, fps);

    // Create the project object
    auto project = Project::createNew(name.toStdString());

    // Apply the user's settings to the first sequence + the default template
    // (so later "New Sequence" defaults inherit this choice).
    project->settings().setResolution(resW, resH);
    project->settings().setFrameRate(fps);
    project->defaultSettings().setResolution(resW, resH);
    project->defaultSettings().setFrameRate(fps);

    // Save it to disk — each project gets its own subfolder
    QString projDir = saveDir.isEmpty() ? projectsDirectory() : saveDir;
    QString projectFolder = projDir + "/" + name;
    QDir projectDir(projectFolder);
    if (!projectDir.exists() && !projectDir.mkpath(".")) {
        spdlog::error("Failed to create project folder: {}",
                      projectFolder.toStdString());
        QMessageBox::warning(this, "Error",
            QString("Failed to create project folder.\n"
                    "The save location may be read-only or the path invalid:\n%1")
                .arg(projDir));
        return;
    }
    // Use wide-string conversion to preserve Unicode characters on Windows
    std::filesystem::path path =
        (projectFolder + "/" + name + ".rtp").toStdWString();
    project->setFilePath(path);

    ProjectSerializer serializer;
    if (serializer.save(*project, path)) {
        spdlog::info("Project saved to: {}", pathToUtf8(path));
        setCurrentProject(std::move(project));
        // Reset the Timeline dock layout to the canonical default
        // (loads the "USE_AS_DEFAULT" workspace preset from QSettings)
        // so new projects start with the correct panel arrangement.
        if (m_timelineWorkspace)
            m_timelineWorkspace->resetToDefaultDockLayout();
        addToRecentFiles(QString::fromStdString(pathToUtf8(path)));
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' created").arg(name), 3000);
    } else {
        spdlog::error("Failed to save new project: {}", pathToUtf8(path));
        QMessageBox::warning(this, "Error",
            QString("Failed to save project '%1'.\n\n"
                    "Check that the destination folder is writable and has\n"
                    "enough free space:\n%2")
                .arg(name, projDir));
    }
}

void MainWindow::onOpenProjectFromPanel(const QString& name)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("=== OPEN PROJECT START: {} ===", name.toStdString());
    auto t0 = std::chrono::steady_clock::now();

    // Save state of current project before opening new one.  This runs on the
    // UI thread up-front, while the old project is still fully wired — the
    // input lock (engaged inside beginAsyncProjectLoad) prevents the user from
    // touching it during the off-thread parse that follows.
    if (m_currentProject && m_audioSync) {
        spdlog::info("OPEN: saving current project audio sync state");
        m_audioSync->saveProjectState(
            QString::fromStdString(m_currentProject->name()));

        // Save which page was active — but NOT if we're on the Projects page,
        // because we're here specifically to switch projects.  Saving 0 (Projects)
        // would cause the next open to stay on the Projects tab.
        Page curPage = currentPage();
        if (curPage != Page::Projects) {
            auto settings = rt::appSettings();
            settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                              static_cast<int>(curPage));
        }
    }

    // Use the precise file path from the project panel when available,
    // falling back to the standard projects-directory convention.  This
    // ensures projects saved to custom locations or discovered via the
    // recent-files list are opened at their actual on-disk location.
    QString filePath;
    if (m_projectPanel)
        filePath = m_projectPanel->projectFilePath(name);
    if (filePath.isEmpty())
        filePath = projectsDirectory() + "/" + name + "/" + name + ".rtp";

    std::filesystem::path path = filePath.toStdWString();

    spdlog::info("OPEN: dispatching async load for {}", pathToUtf8(path));
    beginAsyncProjectLoad(path, tr("Opening project…"),
        [this, name, path, t0](std::unique_ptr<Project> project) {
            if (!project) {
                disengageLoadingOverlay();
                spdlog::error("Failed to load project: {}", pathToUtf8(path));
                QMessageBox::warning(this, "Error",
                    QString("Failed to open project '%1'").arg(name));
                return;
            }

            // Normalize display name to the selected project entry.
            if (project->name() != name.toStdString())
                project->setName(name.toStdString());
            project->setFilePath(path);

            spdlog::info("OPEN: calling setCurrentProject");
            setCurrentProject(std::move(project));

            spdlog::info("OPEN: calling restoreWorkspace");
            // Prefer the project's own saved layout; fall back to the last
            // session snapshot.  If neither exists (new project / fresh
            // install), resetToDefaultDockLayout() loads the user's
            // "USE_AS_DEFAULT" workspace preset from QSettings.
            if (!restoreWorkspace("project/" + name)
                && !restoreWorkspace("last_session")) {
                spdlog::info("OPEN: no saved workspace — resetting to default layout");
                if (m_timelineWorkspace)
                    m_timelineWorkspace->resetToDefaultDockLayout();
            }

            // Stay on the current tab (Projects) instead of restoring the
            // last active page for this project.
            setCurrentPage(Page::Projects);

            spdlog::info("OPEN: restoring audio sync state");
            // Prefer the blob embedded in the .rtp file (backed up + versioned)
            // over QSettings (which has no backup).
            if (m_audioSync) {
                const auto& blob = m_currentProject->audioSyncBlob();
                spdlog::info("OPEN: AudioSync blob size={}", blob.size());
                if (!blob.empty()) {
                    m_audioSync->deserializeFromBlob(blob);
                    spdlog::info("OPEN: after deserialize — audioPaths.size={}",
                                 m_audioSync->audioPaths().size());
                } else {
                    m_audioSync->restoreProjectState(name);
                }
                // Re-baseline the dirty tracker to the canonical in-memory
                // serialization. The on-disk bytes can differ (unordered_map
                // session ordering, filtered missing audio paths), so comparing
                // against them would falsely mark the project dirty right after
                // opening — triggering spurious auto-saves / unsaved prompts.
                m_lastSavedAudioSyncBlob = m_audioSync->serializeToBlob();
            } else {
                spdlog::warn("OPEN: m_audioSync is null — cannot restore audio state");
            }

            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            spdlog::info("=== OPEN PROJECT COMPLETE: {} total ms ===", dt);

            addToRecentFiles(QString::fromStdString(pathToUtf8(path)));
            statusBar()->showMessage(QString("Opened '%1'").arg(name), 3000);

            // Release the input lock — or keep the overlay up until the
            // background media warmup finishes if it's still running.
            releaseOpenLock();
        });
}

void MainWindow::onDeleteProjectFromPanel(const QString& name, const QString& filePath)
{
    auto reply = QMessageBox::question(this, "Delete Project",
        QString("Delete project '%1'? This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    // If the deleted project is the currently open project, close it first
    // so subsequent operations (create, open) don't think it's still open.
    // Important: clean up all references BEFORE destroying the project,
    // otherwise dangling pointers in subsystems cause use-after-free crashes.
    if (m_currentProject) {
        QString currentName = QString::fromStdString(m_currentProject->name());
        if (currentName == name) {
            // 1. Stop audio playback / transport
            if (m_playbackController && m_playbackController->isPlaying())
                m_playbackController->stop();

            // 2. Stop the async composite pipeline (FrameProducer thread)
            if (m_timelineWorkspace) {
                if (auto* pm = m_timelineWorkspace->programMonitor()) {
                    pm->stopPolling();
                    if (auto* pl = pm->pipeline())
                        pl->stop();
                }
            }
            if (m_timelineWorkspace) {
                if (auto* sm = m_timelineWorkspace->sourceMonitor()) {
                    if (auto* ctrl = sm->controller()) {
                        if (ctrl->isPlaying()) ctrl->stop();
                    }
                }
            }

            // 3. Disconnect old timeline from all consumers so no subsystem
            //    accesses destroyed project data.
            if (m_timelineWorkspace) {
                if (auto* pm = m_timelineWorkspace->programMonitor())
                    pm->setCompositeCallback(nullptr);
                m_timelineWorkspace->setTimeline(nullptr);
            }
            if (m_playbackController)
                m_playbackController->setTimeline(nullptr);
            m_timeline = nullptr;

            // 4. Release all media from the old project and clear the frame cache
            if (m_mediaPool)
                m_mediaPool->closeAll();

            // 5. Reset per-project panels
            if (m_audioSync)
                m_audioSync->resetForNewProject();

            // 6. Clear undo/redo history
            if (m_commandStack)
                m_commandStack->clear();

            // 7. Clear export panel references
            if (m_exportPanel) {
                m_exportPanel->setTimeline(nullptr);
                m_exportPanel->setProject(nullptr);
            }

            // 8. Clear project bin items and project references so stale
            //    media items and sequences don't linger in the bin UI
            //    after project deletion.
            if (auto* bin = projectBin()) {
                bin->clearAll();
                bin->setProject(nullptr);
            }

            // 9. Clear stale clip / project references from detail panels
            //    that may still point into the about-to-be-destroyed project.
            if (m_timelineWorkspace) {
                m_timelineWorkspace->setProject(nullptr);
                if (auto* props = m_timelineWorkspace->propertiesPanel())
                    props->clearClip();
                if (auto* ecp = m_timelineWorkspace->effectControlsPanel())
                    ecp->clearClip();
                if (auto* eff = m_timelineWorkspace->effectsPanel())
                    eff->setClip(nullptr);
                if (auto* sm = m_timelineWorkspace->sourceMonitor())
                    sm->clearClip();
                if (auto* lib = m_timelineWorkspace->libraryPanel())
                    lib->refresh();
                if (auto* chars = m_timelineWorkspace->charactersPanel())
                    chars->refresh();
            }

            // 10. Update UI state
            if (m_projectPanel)
                m_projectPanel->setCurrentProjectName({});
            if (auto* bin = projectBin())
                bin->setProjectName({});
            setWindowTitle(QString("ROUNDTABLE NLE %1").arg(ROUNDTABLE_VERSION));

            // 11. Now safe to destroy the old project
            m_lastSavedAudioSyncBlob = {};
            m_currentProject.reset();
        }
    }

    // Determine the project folder:
    // - If filePath is known (external drive project), use its parent folder.
    // - Otherwise fall back to projectsDirectory/name.
    QString projectFolder;
    if (!filePath.isEmpty()) {
        projectFolder = QFileInfo(filePath).absolutePath();
    } else {
        projectFolder = projectsDirectory() + "/" + name;
    }

    bool deleted = QDir(projectFolder).removeRecursively();

    if (deleted) {
        spdlog::info("Deleted project: {} (folder: {})",
                     name.toStdString(), projectFolder.toStdString());
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' deleted").arg(name), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            QString("Failed to delete '%1'").arg(name));
    }
}

void MainWindow::onRenameProjectFromPanel(const QString& oldName, const QString& newName)
{
    spdlog::info("Renaming project '{}' -> '{}'", oldName.toStdString(), newName.toStdString());

    QString projDir = projectsDirectory();

    if (QDir(projDir + "/" + newName).exists()) {
        QMessageBox::warning(this, "Error",
            QString("A project named '%1' already exists.").arg(newName));
        return;
    }

    QString oldFolder = projDir + "/" + oldName;
    // Rename files inside, then rename the folder
    QString oldRtp = oldFolder + "/" + oldName + ".rtp";
    QString newRtp = oldFolder + "/" + newName + ".rtp";
    QFile::rename(oldRtp, newRtp);
    QFile::rename(oldRtp + ".bak", newRtp + ".bak");
    QFile::rename(oldFolder + "/" + oldName + ".png", oldFolder + "/" + newName + ".png");
    QFile::rename(oldFolder + "/" + oldName + ".jpg", oldFolder + "/" + newName + ".jpg");
    QString newFolder = projDir + "/" + newName;
    QString newFilePath = newFolder + "/" + newName + ".rtp";
    bool renamed = QDir().rename(oldFolder, newFolder);

    if (renamed) {
        // If the renamed project is the currently loaded one, update it
        if (m_currentProject &&
            QString::fromStdString(m_currentProject->name()) == oldName) {
            m_currentProject->setName(newName.toStdString());
            m_currentProject->setFilePath(newFilePath.toStdWString());
            if (m_projectPanel) m_projectPanel->setCurrentProjectName(newName);
            if (auto* bin = projectBin()) bin->setProjectName(newName);
            setWindowTitle(QString("ROUNDTABLE NLE %1 — %2").arg(ROUNDTABLE_VERSION).arg(newName));
        }
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Renamed '%1' to '%2'").arg(oldName, newName), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            QString("Failed to rename '%1'").arg(oldName));
    }
}

void MainWindow::onDuplicateProjectFromPanel(const QString& name)
{
    spdlog::info("Duplicating project: {}", name.toStdString());

    QString projDir = projectsDirectory();
    QString srcPath = projDir + "/" + name + "/" + name + ".rtp";

    // Find a unique name
    QString newName = name + " (Copy)";
    int counter = 2;
    while (QDir(projDir + "/" + newName).exists()) {
        newName = name + QString(" (Copy %1)").arg(counter++);
    }

    // Create subfolder for duplicate
    QString newFolder = projDir + "/" + newName;
    QDir().mkpath(newFolder);
    QString dstPath = newFolder + "/" + newName + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        // Also copy thumbnail if it exists
        QString srcThumb = projDir + "/" + name + "/" + name + ".png";
        if (QFile::exists(srcThumb))
            QFile::copy(srcThumb, newFolder + "/" + newName + ".png");

        refreshProjectsList();
        statusBar()->showMessage(
            QString("Duplicated as '%1'").arg(newName), 3000);
    } else {
        QDir(newFolder).removeRecursively();
        QMessageBox::warning(this, "Error",
            QString("Failed to duplicate '%1'").arg(name));
    }
}

void MainWindow::onRevealProjectInExplorer(const QString& name)
{
    // Use the actual file path from the project info, not a reconstructed
    // path that assumes <projDir>/<name>/<name>.rtp — the folder or file
    // name may differ from the project's display name (e.g. after a rename,
    // import, or manual move).
    QString filePath = m_projectPanel
        ? m_projectPanel->projectFilePath(name)
        : QString();
    if (filePath.isEmpty()) {
        // Fallback to the conventional layout if the panel lookup fails
        QString projDir = projectsDirectory();
        filePath = projDir + "/" + name + "/" + name + ".rtp";
    }
    QFileInfo fi(filePath);
    if (fi.exists()) {
        QProcess::startDetached("explorer.exe",
            {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// New project from dropped media
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::onNewProjectForMedia(const QString& filePath, int64_t atTick, size_t trackIndex)
{
    if (!checkUnsavedChanges()) return;

    // Read media properties to set sequence resolution / frame rate
    uint32_t mediaW = 1920, mediaH = 1080;
    double mediaFps = 30.0;
    if (!filePath.isEmpty() && m_mediaPool) {
        uint64_t h = m_mediaPool->open(filePath.toStdString());
        if (h != 0) {
            const auto* info = m_mediaPool->getInfo(h);
            if (info) {
                if (info->width  > 0) mediaW = info->width;
                if (info->height > 0) mediaH = info->height;
                if (info->fps    > 0) mediaFps = info->fps;
            }
        }
    }

    // Default to 30 fps for still images
    if (mediaFps <= 0.0) mediaFps = 30.0;

    // Derive project name from the dropped file
    QString baseName = filePath.isEmpty()
        ? QStringLiteral("New Project")
        : QFileInfo(filePath).completeBaseName();
    QString projName = baseName;
    // Ensure unique name in the projects directory
    QString projDir = projectsDirectory();
    int counter = 0;
    while (QDir(projDir + "/" + projName).exists()) {
        ++counter;
        projName = baseName + "_" + QString::number(counter);
    }

    spdlog::info("Creating project from dropped media: {} ({}x{} @ {} fps)",
                 projName.toStdString(), mediaW, mediaH, mediaFps);

    auto project = Project::createNew(projName.toStdString());
    project->settings().setResolution(mediaW, mediaH);
    project->settings().setFrameRate(mediaFps);
    project->defaultSettings().setResolution(mediaW, mediaH);
    project->defaultSettings().setFrameRate(mediaFps);

    // Save the project
    QString projectFolder = projDir + "/" + projName;
    QDir().mkpath(projectFolder);
    // Use wide-string conversion to preserve Unicode characters on Windows
    std::filesystem::path path =
        (projectFolder + "/" + projName + ".rtp").toStdWString();
    project->setFilePath(path);

    ProjectSerializer serializer;
    if (serializer.save(*project, path)) {
        spdlog::info("Project saved to: {}", pathToUtf8(path));
        setCurrentProject(std::move(project));
        // Reset the Timeline dock layout to the canonical default
        // (loads the "USE_AS_DEFAULT" workspace preset from QSettings).
        if (m_timelineWorkspace)
            m_timelineWorkspace->resetToDefaultDockLayout();
        refreshProjectsList();
        statusBar()->showMessage(
            QString("Project '%1' created from dropped media").arg(projName), 3000);

        // Switch to the TIMELINE page so the user sees the result
        setCurrentPage(Page::Timeline);

        // ── Place the dropped media on the timeline now that a project/sequence exists ──
        if (!filePath.isEmpty()) {
            // Add the file to the Project Bin
            if (auto* bin = projectBin()) {
                namespace fs = std::filesystem;
                bin->addFiles({ fs::path(filePath.toStdWString()) });
            }

            // Open in MediaPool to get a handle for the clip
            uint64_t handle = 0;
            if (m_mediaPool)
                handle = m_mediaPool->open(filePath.toStdString());

            // Re-emit mediaDropped so the normal clip-creation path places the
            // asset on the timeline at the exact position the user dragged it to.
            if (auto* tlp = timelinePanel())
                emit tlp->mediaDropped(filePath, handle, atTick, trackIndex);
        }
    } else {
        spdlog::error("Failed to save new project: {}", pathToUtf8(path));
        QMessageBox::warning(this, "Error",
            QString("Failed to create project '%1'").arg(projName));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Recent / Import / Export project handlers
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::onOpenRecentProjectFromPanel(const QString& filePath)
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("Opening recent project: {}", filePath.toStdString());

    std::filesystem::path path = filePath.toStdWString();
    beginAsyncProjectLoad(path, tr("Opening project…"),
        [this, filePath, path](std::unique_ptr<Project> project) {
            if (!project) {
                disengageLoadingOverlay();
                QMessageBox::warning(this, "Error", "Failed to open " + filePath);
                return;
            }
            const QString loadedName = QFileInfo(filePath).baseName();
            if (project->name() != loadedName.toStdString())
                project->setName(loadedName.toStdString());
            project->setFilePath(path);
            setCurrentProject(std::move(project));
            addToRecentFiles(filePath);

            // Stay on the current tab (Projects) instead of restoring the
            // last active page for this project.
            setCurrentPage(Page::Projects);

            // Restore audio sync state — prefer blob embedded in .rtp over QSettings
            if (m_audioSync) {
                const auto& blob = m_currentProject->audioSyncBlob();
                if (!blob.empty())
                    m_audioSync->deserializeFromBlob(blob);
                else
                    m_audioSync->restoreProjectState(loadedName);
                // Re-baseline the dirty tracker (see onOpenProjectFromPanel).
                m_lastSavedAudioSyncBlob = m_audioSync->serializeToBlob();
            }

            statusBar()->showMessage("Opened: " + QFileInfo(filePath).fileName(), 3000);
            releaseOpenLock();
        });
}

void MainWindow::onImportProject(const QString& srcPath)
{
    spdlog::info("Importing project from: {}", srcPath.toStdString());

    QString projDir = projectsDirectory();
    QDir().mkpath(projDir);
    QString baseName = QFileInfo(srcPath).baseName();

    // Find a unique project name
    QString name = baseName;
    int n = 2;
    while (QDir(projDir + "/" + name).exists() ||
           QFile::exists(projDir + "/" + name + ".rtp")) {
        name = baseName + QString(" (%1)").arg(n++);
    }

    // Create project subfolder and copy into it
    QString projectFolder = projDir + "/" + name;
    QDir().mkpath(projectFolder);
    QString dstPath = projectFolder + "/" + name + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        // Normalize internal project metadata to the new imported name.
        ProjectSerializer serializer;
        if (auto imported = serializer.load(dstPath.toStdWString())) {
            imported->setName(name.toStdString());
            imported->setFilePath(dstPath.toStdWString());
            imported->setModified(false);
            if (!serializer.save(*imported, dstPath.toStdWString())) {
                spdlog::warn("Import: copied project but failed to rewrite internal name for '{}'",
                             name.toStdString());
            }
        } else {
            spdlog::warn("Import: copied project but could not reload '{}' to normalize metadata",
                         dstPath.toStdString());
        }

        refreshProjectsList();
        statusBar()->showMessage(
            "Imported: " + name, 3000);
    } else {
        QDir(projectFolder).removeRecursively();
        QMessageBox::warning(this, "Error",
            "Failed to import project from " + srcPath);
    }
}

void MainWindow::onExportProject(const QString& name, const QString& dstPath)
{
    spdlog::info("Exporting project '{}' to: {}", name.toStdString(), dstPath.toStdString());

    QString projDir = projectsDirectory();

    // Locate source .rtp (subfolder or flat)
    QString srcPath = projDir + "/" + name + "/" + name + ".rtp";

    if (QFile::copy(srcPath, dstPath)) {
        statusBar()->showMessage(
            "Exported '" + name + "' to " + QFileInfo(dstPath).dir().path(), 3000);
    } else {
        QMessageBox::warning(this, "Error",
            "Failed to export project '" + name + "'");
    }
}

void MainWindow::onProjectsDirChanged(const QString& newDir)
{
    spdlog::info("Projects directory changed to: {}", newDir.toStdString());
    auto settings = rt::appSettings();
    settings.setValue("ProjectsDirectory", newDir);
    refreshProjectsList();
    statusBar()->showMessage(
        "Projects folder: " + newDir, 3000);
}

void MainWindow::onNewProject()
{
    spdlog::info("File > New Project");
    // Switch to Projects page so user can name the project
    setCurrentPage(Page::Projects);
    if (m_projectPanel)
        m_projectPanel->nameInput()->setFocus();
}

void MainWindow::onOpenProject()
{
    if (!checkUnsavedChanges()) return;

    spdlog::info("File > Open Project");
    QString path = QFileDialog::getOpenFileName(
        this, "Open Project", projectsDirectory(),
        "ROUNDTABLE Projects (*.rtp);;All Files (*)");

    if (path.isEmpty()) return;

    std::filesystem::path fsPath = path.toStdWString();
    beginAsyncProjectLoad(fsPath, tr("Opening project…"),
        [this, fsPath](std::unique_ptr<Project> project) {
            if (!project) {
                disengageLoadingOverlay();
                QMessageBox::warning(this, "Error",
                    "Failed to open the selected project file.");
                return;
            }
            const QString loadedName =
                QFileInfo(QString::fromStdWString(fsPath.wstring())).baseName();
            if (project->name() != loadedName.toStdString())
                project->setName(loadedName.toStdString());
            project->setFilePath(fsPath);
            setCurrentProject(std::move(project));
            if (!restoreWorkspace("project/" + loadedName)
                && !restoreWorkspace("last_session")) {
                if (m_timelineWorkspace)
                    m_timelineWorkspace->resetToDefaultDockLayout();
            }
            addToRecentFiles(QString::fromStdWString(fsPath.wstring()));
            // Stay on the current tab (Projects) instead of switching to Timeline
            setCurrentPage(Page::Projects);

            // Restore audio sync state
            if (m_audioSync) {
                const auto& blob = m_currentProject->audioSyncBlob();
                if (!blob.empty())
                    m_audioSync->deserializeFromBlob(blob);
                else
                    m_audioSync->restoreProjectState(loadedName);
                // Re-baseline the dirty tracker (see onOpenProjectFromPanel).
                m_lastSavedAudioSyncBlob = m_audioSync->serializeToBlob();
            }

            statusBar()->showMessage("Project opened", 3000);
            releaseOpenLock();
        });
}

void MainWindow::onSaveProject()
{
    spdlog::info("File > Save");
    if (!m_currentProject) {
        statusBar()->showMessage("No project to save", 3000);
        return;
    }

    auto path = m_currentProject->filePath();
    if (path.empty()) {
        onSaveProjectAs();
        return;
    }

    // ── Capture bin state into the project before serialization ─────────
    {
        // Persist only what is explicitly in the Project Bin.
        std::vector<std::filesystem::path> binFiles;
        if (auto* bin = projectBin()) {
            binFiles = bin->allFiles();
            m_currentProject->setBinItems(bin->exportBinItems());
        }

        m_currentProject->setBinFiles(binFiles);

        // Capture bin folder structure
        if (auto* bin = projectBin()) {
            auto uiFolders = bin->binFolderState();
            std::vector<Project::BinFolder> projFolders;
            projFolders.reserve(uiFolders.size());
            for (auto& f : uiFolders) {
                Project::BinFolder pf;
                pf.name      = std::move(f.name);
                pf.expanded  = f.expanded;
                pf.childKeys = std::move(f.childKeys);
                projFolders.push_back(std::move(pf));
            }
            m_currentProject->setBinFolders(std::move(projFolders));
            spdlog::info("onSaveProject: captured bin state — {} files, {} folders",
                         binFiles.size(), uiFolders.size());
        } else {
            spdlog::warn("onSaveProject: projectBin() returned nullptr — bin state NOT saved");
        }
    }

    // Capture AudioSync state into the project blob BEFORE serializing
    if (m_audioSync) {
        auto blob = m_audioSync->serializeToBlob();
        spdlog::info("onSaveProject: AudioSync blob {} bytes", blob.size());
        m_currentProject->setAudioSyncBlob(std::move(blob));
    } else {
        spdlog::warn("onSaveProject: m_audioSync is null");
    }

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, path)) {
        m_currentProject->setModified(false);
        m_lastSavedAudioSyncBlob = m_currentProject->audioSyncBlob();

        saveWorkspace("project/" + QString::fromStdString(m_currentProject->name()));
        saveWorkspace("last_session");

        // Auto-capture thumbnail from current playhead frame
        captureProjectThumbnail();

        // Save audio sync state (transcriptions, matches, clips)
        if (m_audioSync)
            m_audioSync->saveProjectState(QString::fromStdString(m_currentProject->name()));

        // Save active page per project
        auto settings = rt::appSettings();
        settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                          static_cast<int>(currentPage()));

        addToRecentFiles(QString::fromStdString(pathToUtf8(path)));
        statusBar()->showMessage("Project saved", 3000);
    } else {
        QMessageBox::warning(this, "Error", "Failed to save project.");
    }
}

void MainWindow::onSaveProjectAs()
{
    spdlog::info("File > Save As");
    if (!m_currentProject) {
        statusBar()->showMessage("No project to save", 3000);
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, "Save Project As", projectsDirectory(),
        "ROUNDTABLE Projects (*.rtp)");

    if (path.isEmpty()) return;

    if (!path.endsWith(".rtp"))
        path += ".rtp";

    // Place the file inside a project subfolder named after the project
    QFileInfo fi(path);
    QString projectName = fi.baseName();
    QString parentDir   = fi.absolutePath();
    QString projectFolder = parentDir + "/" + projectName;
    QDir().mkpath(projectFolder);
    path = projectFolder + "/" + projectName + ".rtp";

    m_currentProject->setFilePath(path.toStdString());
    m_currentProject->setName(projectName.toStdString());

    // ── Capture bin state into the project before serialization ─────────
    {
        std::vector<std::filesystem::path> binFiles;
        if (auto* bin = projectBin()) {
            binFiles = bin->allFiles();
            m_currentProject->setBinItems(bin->exportBinItems());
        }
        m_currentProject->setBinFiles(binFiles);

        if (auto* bin = projectBin()) {
            auto uiFolders = bin->binFolderState();
            std::vector<Project::BinFolder> projFolders;
            projFolders.reserve(uiFolders.size());
            for (auto& f : uiFolders) {
                Project::BinFolder pf;
                pf.name      = std::move(f.name);
                pf.childKeys = std::move(f.childKeys);
                projFolders.push_back(std::move(pf));
            }
            m_currentProject->setBinFolders(std::move(projFolders));
            spdlog::info("onSaveProjectAs: captured bin state — {} files, {} folders",
                         binFiles.size(), uiFolders.size());
        }
    }

    // Capture AudioSync state into blob before serializing
    if (m_audioSync)
        m_currentProject->setAudioSyncBlob(m_audioSync->serializeToBlob());

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, path.toStdString())) {
        m_currentProject->setModified(false);
        m_lastSavedAudioSyncBlob = m_currentProject->audioSyncBlob();

        saveWorkspace("project/" + QString::fromStdString(m_currentProject->name()));
        saveWorkspace("last_session");

        // Auto-capture thumbnail from current playhead frame
        captureProjectThumbnail();

        // Save audio sync state BEFORE moving the project (transcriptions, matches, clips)
        if (m_audioSync)
            m_audioSync->saveProjectState(QFileInfo(path).baseName());

        setCurrentProject(std::move(m_currentProject)); // refresh title

        // Save active page per project (use the new name from path)
        auto settings = rt::appSettings();
        settings.setValue("Project/" + QFileInfo(path).baseName() + "/activePage",
                          static_cast<int>(currentPage()));

        refreshProjectsList();
        addToRecentFiles(path);
        statusBar()->showMessage("Project saved", 3000);
    } else {
        QMessageBox::warning(this, "Error", "Failed to save project.");
    }
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
