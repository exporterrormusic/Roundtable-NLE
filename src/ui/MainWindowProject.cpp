/*
 * MainWindowProject.cpp — Project lifecycle coordinator.
 *
 * Thin coordinator after extracting setCurrentProject → MainWindowProjectSet.cpp,
 * CRUD handlers → MainWindowProjectHandlers.cpp, and SRT/misc →
 * MainWindowProjectMisc.cpp.
 *
 * Contains: projectsDirectory(), checkUnsavedChanges(), refreshProjectsList().
 *
 * Sub-files (all in src/ui/):
 *   MainWindowProjectSet.cpp       — setCurrentProject()
 *   MainWindowProjectHandlers.cpp  — all on* CRUD handlers (create, open,
 *                                     delete, rename, duplicate, save, etc.)
 *   MainWindowProjectMisc.cpp      — SRT import/export, captureProjectThumbnail()
 */

#include "MainWindow.h"

#include "panels/audio/AudioSync.h"
#include "panels/project/ProjectPanel.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelineWorkspace.h"

#include "QtHelpers.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "timeline/Timeline.h"

#include "project/Project.h"
#include "project/ProjectSerializer.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSet>
#include <QStatusBar>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>


namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Project management helpers
// ═════════════════════════════════════════════════════════════════════════════

QString MainWindow::projectsDirectory() const
{
    // F14: Check for user-configured projects directory
    auto settings = rt::appSettings();
    QString customDir = settings.value("ProjectsDirectory").toString();
    if (!customDir.isEmpty() && QDir(customDir).exists())
        return customDir;

    // Use a "projects" folder next to the executable
    QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    // Walk up from bin/Release to the workspace root
    // (exe is at build/bin/Release/roundtable.exe)
    // Try to find a "projects" folder by walking up
    for (int i = 0; i < 5; ++i) {
        if (dir.exists("projects")) {
            return dir.absoluteFilePath("projects");
        }
        dir.cdUp();
    }
    // Fallback: create next to exe
    QString defaultDir = appDir + "/projects";
    QDir defDir(defaultDir);
    if (!defDir.exists()) {
        defDir.mkpath(".");
    }
    // Verify it's writable — if not, fall back to user data dir
    QFileInfo fi(defaultDir + "/.wtest");
    QFile testFile(defaultDir + "/.wtest");
    if (testFile.open(QIODevice::WriteOnly)) {
        testFile.close();
        testFile.remove();
        return defaultDir;
    }
    spdlog::warn("projectsDirectory: {} is not writable, falling back to userDataDir",
                 defaultDir.toStdString());
    return rt::userDataDir() + "/projects";
}

bool MainWindow::checkUnsavedChanges()
{
    bool audioSyncDirty = false;
    if (m_currentProject && m_audioSync)
        audioSyncDirty = (m_audioSync->serializeToBlob() != m_lastSavedAudioSyncBlob);

    if (!m_currentProject || (!m_currentProject->isModified() && !audioSyncDirty))
        return true; // nothing to save

    QString name = QString::fromStdString(m_currentProject->name());
    auto reply = QMessageBox::question(
        this, "Unsaved Changes",
        QString("Project '%1' has unsaved changes.\n\n"
                "Do you want to save before continuing?").arg(name),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (reply == QMessageBox::Cancel)
        return false; // user cancelled

    if (reply == QMessageBox::Save) {
        onSaveProject();
    }
    // Discard or Save — OK to proceed
    return true;
}

void MainWindow::refreshProjectsList()
{
    if (!m_projectPanel) return;

    QString projDir = projectsDirectory();
    QDir dir(projDir);
    if (!dir.exists()) {
        dir.mkpath(".");
        m_projectPanel->setProjects({});
        return;
    }

    // Migrate any flat-layout project files into subfolders
    {
        QStringList flatFilters;
        flatFilters << "*.rtp";
        auto flatFiles = dir.entryInfoList(flatFilters, QDir::Files);
        for (const auto& fi : flatFiles) {
            QString name = fi.baseName();
            QString subFolder = projDir + "/" + name;
            QDir().mkpath(subFolder);
            QFile::rename(fi.absoluteFilePath(), subFolder + "/" + fi.fileName());
            // Move .bak if it exists
            QString bakPath = fi.absoluteFilePath() + ".bak";
            if (QFile::exists(bakPath))
                QFile::rename(bakPath, subFolder + "/" + fi.fileName() + ".bak");
            // Move thumbnail from thumbs/ if it exists
            QString thumbsDir = projDir + "/thumbs";
            for (const auto& ext : {".png", ".jpg"}) {
                QString thumbPath = thumbsDir + "/" + name + ext;
                if (QFile::exists(thumbPath))
                    QFile::rename(thumbPath, subFolder + "/" + name + ext);
            }
            spdlog::info("Migrated project '{}' into subfolder", name.toStdString());
        }
        // Remove empty thumbs/ directory
        QDir thumbsDir(projDir + "/thumbs");
        if (thumbsDir.exists() && thumbsDir.isEmpty())
            thumbsDir.removeRecursively();
    }

    // Scan for .rtp files inside project subfolders
    QStringList filters;
    filters << "*.rtp";

    QFileInfoList entries;
    for (const auto& subDir : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        // Skip dev-only projects in release builds — a project folder that
        // contains a `_dev` marker file is only shown when ROUNDTABLE_DEBUG
        // or ROUNDTABLE_DEV_BUILD is defined.
#if !defined(ROUNDTABLE_DEBUG) && !defined(ROUNDTABLE_DEV_BUILD)
        if (QFileInfo::exists(subDir.absoluteFilePath() + "/_dev"))
            continue;
#endif
        QDir sub(subDir.absoluteFilePath());
        entries.append(sub.entryInfoList(filters, QDir::Files, QDir::Time));
    }

    // Also include recent projects that live outside the default projects
    // directory (e.g. projects created on external drives).
    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();
    {
        QSet<QString> seen;
        for (const auto& fi : entries)
            seen.insert(QFileInfo(fi.absoluteFilePath()).absoluteFilePath().toLower());

        const QString projDirPrefix = QDir::toNativeSeparators(projDir).toLower();
        for (const auto& rp : recent) {
            QFileInfo rfi(rp);
            QString normPath = rfi.absoluteFilePath().toLower();
            if (seen.contains(normPath)) continue;
            if (rfi.exists() && rfi.fileName().endsWith(".rtp", Qt::CaseInsensitive)) {
                if (normPath.startsWith(projDirPrefix))
                    continue;
#if !defined(ROUNDTABLE_DEBUG) && !defined(ROUNDTABLE_DEV_BUILD)
                // Skip dev-only projects in release builds
                if (QFileInfo::exists(rfi.absolutePath() + "/_dev"))
                    continue;
#endif
                entries.append(rfi);
                seen.insert(normPath);
            }
        }
    }

    // Sort by modification time (newest first)
    std::sort(entries.begin(), entries.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.lastModified() > b.lastModified();
              });

    // Build rich ProjectInfo structs
    QVector<ProjectInfo> projects;
    projects.reserve(entries.size());

    QString currentName;
    if (m_currentProject)
        currentName = QString::fromStdString(m_currentProject->name());

    for (const auto& entry : entries) {
        ProjectInfo info;
        info.name         = entry.baseName();
        info.filePath     = entry.absoluteFilePath();
        info.fileSize     = entry.size();
        info.lastModified = entry.lastModified();
        info.isCurrent    = (!currentName.isEmpty() && info.name == currentName);

        // Read resolution/fps from the project header without
        // fully deserializing (P3: header-only metadata read).
        ProjectSerializer::Metadata meta;
        if (ProjectSerializer::readMetadata(
                entry.absoluteFilePath().toStdString(), meta)) {
            info.resW = meta.resW;
            info.resH = meta.resH;
            info.fps  = meta.fps;
        }
        // For the currently-open project prefer the in-memory show (may be
        // assigned but not yet saved); otherwise read it from the file.
        if (info.isCurrent && m_currentProject)
            info.show = QString::fromStdString(m_currentProject->show());
        else
            info.show = QString::fromStdString(ProjectSerializer::readProjectShow(
                entry.absoluteFilePath().toStdWString()));

        projects.append(info);
    }

    m_projectPanel->setProjects(projects);

    // Also feed recent projects
    m_projectPanel->setRecentProjects(recent);

    spdlog::info("Refreshed projects list: {} projects found", entries.size());
    statusBar()->showMessage(
        QString("%1 project(s) found").arg(entries.size()), 3000);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
