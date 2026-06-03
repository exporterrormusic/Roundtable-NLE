/*
 * MainWindowRecovery.cpp - Auto-save, crash recovery, and recent files.
 * Split from MainWindowProject.cpp.
 */

#include "MainWindow.h"
#include "ShortcutManager.h"
#include "Theme.h"
#include "widgets/DockTitleBar.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "dialogs/KeyboardShortcutsDialog.h"
#include "dialogs/AppPreferencesDialog.h"

// Pages / panels
#include "panels/audio/AudioSync.h"
#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/CharacterShotPanel.h"
#include "panels/export/ExportPanel.h"
#include "panels/project/ProjectPanel.h"
#include "panels/characters/ShotComposer.h"
#include "panels/timeline/TimelineWorkspace.h"

// Delegated panel headers (for accessor forwarding)
#include "panels/effects/EffectsPanel.h"
#include "panels/project/HistoryPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "viewport/Viewport.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

// Core
#include "CrashHandler.h"
#include "command/CommandStack.h"
#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/PlaybackController.h"
#include "spine/ModelManager.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include "project/AutoSave.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"
#include "SrtIO.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QAbstractButton>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QUrl>
#include <QUrlQuery>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../version.h"
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>


namespace rt {

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Auto-save
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â


// ═══════════════════════════════════════════════════════════════════════════
//  Restore from Auto-Save
// ═══════════════════════════════════════════════════════════════════════════

void MainWindow::onRestoreFromAutoSave()
{
    if (!m_currentProject) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No project is open. Open a project first.");
        return;
    }

    auto projPath = m_currentProject->filePath();
    if (projPath.empty()) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "Project has not been saved yet — no auto-saves exist.");
        return;
    }

    auto folder = AutoSave::autoSaveFolder(projPath);
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No auto-save folder found for this project.");
        return;
    }

    // Collect all auto-save files sorted newest-first
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        uintmax_t size;
    };
    std::vector<Entry> files;
    for (auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".rtp") continue;
        auto wt = entry.last_write_time(ec);
        if (ec) continue;
        files.push_back({entry.path(), wt, entry.file_size(ec)});
    }

    if (files.empty()) {
        QMessageBox::information(this, "Restore from Auto-Save",
            "No auto-save files found.");
        return;
    }

    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.time > b.time; });

    // Build a list of display names
    QStringList items;
    for (const auto& f : files) {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            f.time - std::filesystem::file_time_type::clock::now()
            + std::chrono::system_clock::now());
        auto tt = std::chrono::system_clock::to_time_t(sctp);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &tt);
#else
        localtime_r(&tt, &local);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
        double sizeMB = static_cast<double>(f.size) / (1024.0 * 1024.0);
        items.append(QString("%1  (%2 MB)")
            .arg(buf)
            .arg(sizeMB, 0, 'f', 1));
    }

    bool ok = false;
    QString selected = QInputDialog::getItem(
        this, "Restore from Auto-Save",
        "Select an auto-save to restore:\n"
        "(Current unsaved changes will be lost)",
        items, 0, false, &ok);

    if (!ok || selected.isEmpty()) return;

    int idx = items.indexOf(selected);
    if (idx < 0 || idx >= static_cast<int>(files.size())) return;

    auto reply = QMessageBox::warning(this, "Restore from Auto-Save",
        QString("This will replace the current project state with:\n\n%1\n\n"
                "Any unsaved changes will be lost. Continue?")
            .arg(QString::fromStdString(files[static_cast<size_t>(idx)].path.filename().string())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    showBusyIndicator(tr("Restoring auto-save..."));
    ProjectSerializer serializer;
    auto project = serializer.load(files[static_cast<size_t>(idx)].path);
    if (project) {
        project->setFilePath(projPath);
        project->setModified(true);
        setCurrentProject(std::move(project));
        hideBusyIndicator();
        statusBar()->showMessage("Restored from auto-save", 5000);
        spdlog::info("Restored project from auto-save: {}",
                     files[static_cast<size_t>(idx)].path.string());
    } else {
        hideBusyIndicator();
        QMessageBox::warning(this, "Restore Failed",
            "Could not load the selected auto-save file.");
    }
}
void MainWindow::onAutoSave()
{
    if (!m_currentProject) return;

    bool audioSyncDirty = false;
    std::vector<uint8_t> currentAudioSyncBlob;
    if (m_audioSync) {
        currentAudioSyncBlob = m_audioSync->serializeToBlob();
        audioSyncDirty = (currentAudioSyncBlob != m_lastSavedAudioSyncBlob);
    }

    if (!m_currentProject->isModified() && !audioSyncDirty) return;

    auto projPath = m_currentProject->filePath();
    if (projPath.empty()) return;  // not yet saved to disk

    // Save to "Roundtable Auto-Save" folder with timestamped filename
    auto folder = AutoSave::autoSaveFolder(projPath);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        spdlog::warn("Auto-save: failed to create folder '{}': {}",
                     folder.string(), ec.message());
        return;
    }

    auto savePath = AutoSave::makeTimestampedPath(
        folder, m_currentProject->name());

    // Capture AudioSync state into blob before serializing
    if (m_audioSync)
        m_currentProject->setAudioSyncBlob(std::move(currentAudioSyncBlob));

    ProjectSerializer serializer;
    if (serializer.save(*m_currentProject, savePath)) {
        // Prune old auto-saves (keep max 20)
        auto s = rt::appSettings();
        size_t maxKeep = static_cast<size_t>(s.value("MaxAutoSaves", 20).toInt());
        AutoSave::pruneAutoSaves(folder, maxKeep);

        spdlog::info("Auto-saved to {}", savePath.string());

        statusBar()->showMessage("Auto-saved", 2000);
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Crash recovery
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void MainWindow::checkCrashRecovery()
{
    auto settings = rt::appSettings();
    QString lastPath = settings.value("LastProjectPath").toString();

    // ── Phase 7.A: Crash marker recovery ───────────────────────────────
    // Check if ROUNDTABLE crashed on the previous launch.  If so, show a
    // dialog offering to reset dock layout and workspace to safe defaults
    // plus three help-the-user actions: copy error, open logs folder,
    // and report on GitHub with a pre-filled issue.
    if (CrashHandler::hasCrashMarker()) {
        auto crashInfo = CrashHandler::readCrashMarker();
        spdlog::warn("Crash marker detected: {}", crashInfo.summary);

        const QString summary = QString::fromStdString(crashInfo.summary);
        const QString stack   = QString::fromStdString(crashInfo.stackTrace);
        const QString logDir  = QString::fromStdString(
            CrashHandler::crashDirectory().string());

        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Crash Recovery"));
        auto* vbox = new QVBoxLayout(&dlg);

        auto* msg = new QLabel(
            QStringLiteral("ROUNDTABLE crashed on the last launch.\n\n"
                           "Crash details: %1\n\n"
                           "Would you like to reset the dock layout and workspace "
                           "to safe defaults?\n\n"
                           "• Reset & Continue — clears GPU cache, resets layout,\n"
                           "  and starts with GPU acceleration disabled.\n"
                           "• Continue — tries to restore your previous session.")
                .arg(summary),
            &dlg);
        msg->setWordWrap(true);
        vbox->addWidget(msg);

        auto* infoRow = new QHBoxLayout;
        auto* copyBtn      = new QPushButton(QStringLiteral("Copy Error Message"), &dlg);
        auto* openLogsBtn  = new QPushButton(QStringLiteral("Open Logs Folder"), &dlg);
        auto* reportBtn    = new QPushButton(QStringLiteral("Report on GitHub"), &dlg);
        infoRow->addWidget(copyBtn);
        infoRow->addWidget(openLogsBtn);
        infoRow->addWidget(reportBtn);
        vbox->addLayout(infoRow);

        auto* actionRow = new QHBoxLayout;
        auto* resetBtn = new QPushButton(QStringLiteral("Reset && Continue"), &dlg);
        auto* contBtn  = new QPushButton(QStringLiteral("Continue"), &dlg);
        resetBtn->setDefault(true);
        actionRow->addStretch();
        actionRow->addWidget(resetBtn);
        actionRow->addWidget(contBtn);
        vbox->addLayout(actionRow);

        // Format the error text once for both clipboard and report flows.
        const QString errorText = QStringLiteral("Crash: %1\n\nStack trace:\n%2")
                                      .arg(summary, stack);

        QObject::connect(copyBtn, &QPushButton::clicked, [&, errorText]() {
            QGuiApplication::clipboard()->setText(errorText);
            copyBtn->setText(QStringLiteral("Copied!"));
        });

        QObject::connect(openLogsBtn, &QPushButton::clicked, [logDir]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(logDir));
        });

        QObject::connect(reportBtn, &QPushButton::clicked, [&, errorText, summary, logDir]() {
            const QString desc = QInputDialog::getMultiLineText(
                &dlg, QStringLiteral("Report Crash on GitHub"),
                QStringLiteral("What were you doing when ROUNDTABLE crashed?\n"
                               "(Optional, but very helpful.)"),
                QString());
            if (desc.isNull()) return;  // user cancelled

            // Copy the full crash log so the user can paste it into the issue.
            QGuiApplication::clipboard()->setText(errorText);

            QString title = QStringLiteral("Crash: %1").arg(summary);
            if (title.length() > 80) title = title.left(77) + QStringLiteral("...");

            const QString body = QStringLiteral(
                "**What were you doing when it crashed?**\n%1\n\n"
                "**Crash log (copied to your clipboard — press Ctrl+V below):**\n\n"
                "<!-- Paste the full crash log here. -->\n\n"
                "---\n"
                "Roundtable %2\n"
                "Logs folder: %3")
                .arg(desc.isEmpty() ? QStringLiteral("(no description)") : desc,
                     QString::fromLatin1(ROUNDTABLE_VERSION),
                     logDir);

            QUrl url(QStringLiteral("https://github.com/ROUNDTABLE-TALK/roundtable/issues/new"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("title"),  title);
            q.addQueryItem(QStringLiteral("body"),   body);
            q.addQueryItem(QStringLiteral("labels"), QStringLiteral("crash"));
            url.setQuery(q);
            QDesktopServices::openUrl(url);
        });

        QObject::connect(resetBtn, &QPushButton::clicked, [&]() { dlg.done(QDialog::Accepted); });
        QObject::connect(contBtn,  &QPushButton::clicked, [&]() { dlg.done(QDialog::Rejected); });

        const bool resetChosen = (dlg.exec() == QDialog::Accepted);

        if (resetChosen) {
            spdlog::info("Crash recovery: user chose to reset layout");

            // ── Reset dock layout to defaults ──────────────────────────
            applyDefaultLayout();

            // ── Clear GPU cache settings ──────────────────────────────
            // Force safe mode on next composite by clearing GPU state
            // in the settings, so the compositor starts fresh.
            settings.setValue("GpuEnabled", false);

            // ── Reset workspace to default ────────────────────────────
            settings.remove("WorkspaceState");
            settings.remove("WorkspaceGeometry");

            // ── Clear any stored GPU cache paths ──────────────────────
            settings.remove("GpuCachePath");

            statusBar()->showMessage(
                QStringLiteral("Layout reset to defaults (previous crash detected)"), 5000);

            // ── Shortcut config diagnostic ────────────────────────────
            // Store a hash of the current shortcut config so unexpected
            // changes between launches (could indicate corruption from
            // crash) can be detected.
            {
                QStringList shortcuts = settings.value("Shortcuts").toStringList();
                size_t hash = 0;
                for (const auto& s : shortcuts) {
                    hash ^= std::hash<std::string>{}(s.toStdString()) + 0x9e3779b9
                          + (hash << 6) + (hash >> 2);
                }
                settings.setValue("ShortcutConfigHash",
                                  QVariant::fromValue(static_cast<quint64>(hash)));
            }
        } else {
            spdlog::info("Crash recovery: user chose to continue without reset");
        }

        // Clear the marker regardless — we've handled it
        CrashHandler::clearCrashMarker();
    }

    // ── Auto-save recovery (existing) ──────────────────────────────────
    if (lastPath.isEmpty()) return;

    auto projPath = std::filesystem::path(lastPath.toStdWString());

    bool recovered = false;
    if (AutoSave::hasRecoverableAutoSave(projPath)) {
        auto newestAutoSave = AutoSave::findNewestAutoSave(projPath);
        if (!newestAutoSave.empty()) {
            auto reply = QMessageBox::question(
                this, "Recover Auto-Save",
                QString("An auto-save was found for:\n\n%1\n\n"
                        "Auto-save: %2\n\n"
                        "This auto-save is newer than your last saved version.\n"
                        "\n"
                        "Click Yes to restore your unsaved work.\n"
                        "Click No to load the last manually saved version "
                        "(changes since then will be lost).")
                    .arg(lastPath)
                    .arg(QString::fromStdString(newestAutoSave.filename().string())),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

            if (reply == QMessageBox::Yes) {
                ProjectSerializer serializer;
                auto project = serializer.load(newestAutoSave);
                if (project) {
                    project->setFilePath(projPath);
                    setCurrentProject(std::move(project));
                    statusBar()->showMessage("Recovered from auto-save", 5000);
                    spdlog::info("Recovered project from auto-save: {}",
                                 newestAutoSave.string());
                    recovered = true;
                } else {
                    QMessageBox::warning(this, "Recovery Failed",
                        "Could not load the auto-save file. Opening the last saved version.");
                }
            }
        }
    }

}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Recent files
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void MainWindow::addToRecentFiles(const QString& filePath)
{
    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();

    recent.removeAll(filePath);
    recent.prepend(filePath);

    // Keep at most 10 entries
    while (recent.size() > 10)
        recent.removeLast();

    settings.setValue("RecentFiles", recent);
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
    if (!m_recentProjectsMenu) return;
    m_recentProjectsMenu->clear();

    auto settings = rt::appSettings();
    QStringList recent = settings.value("RecentFiles").toStringList();

    if (recent.isEmpty()) {
        m_recentProjectsMenu->setEnabled(false);
        return;
    }

    m_recentProjectsMenu->setEnabled(true);
    for (const QString& path : recent) {
        QFileInfo fi(path);
#if !defined(ROUNDTABLE_DEBUG) && !defined(ROUNDTABLE_DEV_BUILD)
        // Skip dev-only projects in release builds
        if (QFileInfo::exists(fi.absolutePath() + "/_dev"))
            continue;
#endif
        auto* act = m_recentProjectsMenu->addAction(fi.fileName());
        act->setData(path);
        connect(act, &QAction::triggered, this, [this, path]() {
            ProjectSerializer serializer;
            auto proj = serializer.load(path.toStdString());
            if (proj) {
                setCurrentProject(std::move(proj));
                addToRecentFiles(path);
                statusBar()->showMessage("Opened: " + QFileInfo(path).fileName(), 3000);
            } else {
                QMessageBox::warning(this, "Error", "Failed to open " + path);
            }
        });
    }

    m_recentProjectsMenu->addSeparator();
    m_recentProjectsMenu->addAction("Clear Recent", this, [this]() {
        auto s = rt::appSettings();
        s.remove("RecentFiles");
        updateRecentFilesMenu();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Fatal GPU failure — VK_ERROR_DEVICE_LOST or any non-recoverable Vulkan
//  error reported by GpuContext::tryRecover().  See GpuContext.cpp for why
//  we never try to re-init in-place.
// ─────────────────────────────────────────────────────────────────────────────

void MainWindow::showGpuFatalError()
{
    static bool s_alreadyShown = false;
    if (s_alreadyShown) return;
    s_alreadyShown = true;

    // Stop playback immediately so the FrameProducer / FramePresenter stop
    // hammering Vulkan with stale handles while the user reads the dialog.
    if (m_playbackController) {
        m_playbackController->pause();
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle("GPU Error");
    box.setText("The GPU renderer has crashed and cannot continue.");
    box.setInformativeText(
        "Roundtable detected an unrecoverable Vulkan device error "
        "(typically VK_ERROR_DEVICE_LOST from the graphics driver). "
        "The application will continue running in CPU safe mode for "
        "this session, but playback and effects will be very slow.\n\n"
        "Please save your work and restart Roundtable.\n\n"
        "If this happens repeatedly, update your graphics driver or "
        "disable third-party overlays (NVIDIA App, OBS, Discord overlay).");
    QPushButton* btnRestart  = box.addButton("Restart Now",       QMessageBox::AcceptRole);
    QPushButton* btnQuit     = box.addButton("Quit",              QMessageBox::DestructiveRole);
    box.addButton("Continue in safe mode",                        QMessageBox::RejectRole);
    box.setDefaultButton(btnRestart);
    box.exec();

    QAbstractButton* clicked = box.clickedButton();
    if (clicked == btnRestart) {
        const QString exe  = QCoreApplication::applicationFilePath();
        const QStringList args = QCoreApplication::arguments().mid(1);
        QProcess::startDetached(exe, args);
        QCoreApplication::quit();
    } else if (clicked == btnQuit) {
        QCoreApplication::quit();
    }
    // Continue: drop back into the running app; compositor will use safe mode.
}

} // namespace rt
