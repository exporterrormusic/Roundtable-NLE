/*
 * MainWindowMenus.cpp — Menu bar construction for MainWindow.
 * Split from MainWindowUI.cpp for maintainability.
 *
 * Contains: buildMenuBar, buildFileMenu, buildEditMenu, buildViewMenu,
 * buildTimelineMenu, buildAudioMenu, buildWindowMenu, buildHelpMenu.
 */

#include "MainWindow.h"
#include "ShortcutManager.h"
#include "Theme.h"
#include "command/CommandStack.h"
#include "audio/AudioEngine.h"
#include "project/Project.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "panels/effects/EffectControlsPanel.h"
#include "dialogs/KeyboardShortcutsDialog.h"
#include "dialogs/AppPreferencesDialog.h"
#include "dialogs/RelinkMediaDialog.h"
#include "UpdateChecker.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelinePanel.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/TierListClip.h"
#include "command/commands/ClipCommands.h"
#include "Constants.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include "Settings.h"
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>

#include <iterator>

namespace rt {
// ═════════════════════════════════════════════════════════════════════════════
// Menu bar
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::buildMenuBar()
{
    auto* mb = menuBar();
    buildFileMenu(mb);
    buildEditMenu(mb);
    buildViewMenu(mb);
    buildTimelineMenu(mb);
    buildAudioMenu(mb);
    buildWindowMenu(mb);
    buildHelpMenu(mb);
}

void MainWindow::buildFileMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&File");

    auto* newAct = menu->addAction("&New Project", this, &MainWindow::onNewProject);
    newAct->setShortcut(QKeySequence::New);

    auto* openAct = menu->addAction("&Open Project...", this, &MainWindow::onOpenProject);
    openAct->setShortcut(QKeySequence::Open);

    menu->addSeparator();

    auto* saveAct = menu->addAction("&Save", this, &MainWindow::onSaveProject);
    saveAct->setShortcut(QKeySequence::Save);

    menu->addAction("Save &As...", this, &MainWindow::onSaveProjectAs);

    menu->addAction("Restore from Auto-Save...", this, &MainWindow::onRestoreFromAutoSave);

    menu->addSeparator();

    menu->addAction("Import Media...", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (auto* bin = projectBin())
            bin->importFiles();
    });

    menu->addSeparator();

    menu->addAction("Import SRT Subtitles...", this, &MainWindow::onImportSrt);
    menu->addAction("Export SRT Subtitles...", this, &MainWindow::onExportSrt);

    menu->addSeparator();

    menu->addAction("Link Media...", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_timelineWorkspace || !m_timelineWorkspace->project()) return;
        auto* assetDb = m_timelineWorkspace->project()->assets();
        RelinkMediaDialog dlg(assetDb, m_mediaPool, this);
        dlg.exec();
        if (!dlg.relinkedIds().empty()) {
            statusBar()->showMessage(
                tr("Relinked %1 media file(s)").arg(dlg.relinkedIds().size()), 5000);
        }
    });

    menu->addSeparator();

    m_recentProjectsMenu = menu->addMenu("Recent Projects");
    updateRecentFilesMenu();

    menu->addSeparator();

    auto* exitAct = menu->addAction("E&xit");
    exitAct->setShortcut(QKeySequence(Qt::ALT | Qt::Key_F4));
    connect(exitAct, &QAction::triggered, this, &QMainWindow::close);
}

void MainWindow::buildEditMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&Edit");
    m_editMenu = menu;

    auto* undoAct = menu->addAction("&Undo", this, &MainWindow::onUndo);
    undoAct->setShortcut(QKeySequence::Undo);
    m_undoAct = undoAct;

    auto* redoAct = menu->addAction("&Redo", this, &MainWindow::onRedo);
    redoAct->setShortcuts({QKeySequence::Redo,
                           QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    m_redoAct = redoAct;

    // Dynamic undo/redo text — update when the Edit menu is about to show
    connect(menu, &QMenu::aboutToShow, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_commandStack) {
            auto ud = m_commandStack->undoDescription();
            m_undoAct->setText(ud.empty() ? tr("&Undo")
                : tr("&Undo %1").arg(QString::fromStdString(ud)));
            m_undoAct->setEnabled(m_commandStack->canUndo());

            auto rd = m_commandStack->redoDescription();
            m_redoAct->setText(rd.empty() ? tr("&Redo")
                : tr("&Redo %1").arg(QString::fromStdString(rd)));
            m_redoAct->setEnabled(m_commandStack->canRedo());
        }
    });

    menu->addSeparator();

    auto* cutAct = menu->addAction("Cu&t");
    // Shortcut handled by TimelineWorkspace (Ctrl+X at widget scope)

    auto* copyAct = menu->addAction("&Copy");
    // Shortcut handled by TimelineWorkspace (Ctrl+C at widget scope)

    auto* pasteAct = menu->addAction("&Paste");
    // Shortcut handled by TimelineWorkspace (Ctrl+V at widget scope)

    auto* deleteAct = menu->addAction("&Delete");
    // Shortcut handled by TimelineWorkspace (Delete key in keyPressEvent)

    menu->addSeparator();

    auto* selectAllAct = menu->addAction("Select &All");
    // Shortcut handled by TimelineWorkspace (Ctrl+A at widget scope)

    menu->addSeparator();

    menu->addAction("Project &Settings...", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_timelineWorkspace || !m_timelineWorkspace->project()) return;
        auto& settings = m_timelineWorkspace->project()->settings();
        ProjectSettingsDialog dlg(settings, this);
        if (dlg.exec() == QDialog::Accepted) {
            // Update program monitor resolution
            if (auto* pm = m_timelineWorkspace->programMonitor()) {
                pm->setOutputResolution(
                    settings.resolution().width,
                    settings.resolution().height);
                pm->requestRefresh();
            }
            if (auto* ec = effectControlsPanel())
                ec->setSequenceResolution(
                    settings.resolution().width,
                    settings.resolution().height);
        }
    });

    menu->addAction("Keyboard &Shortcuts...", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_shortcutManager) return;
        KeyboardShortcutsDialog dlg(*m_shortcutManager, this);
        dlg.exec();
    });

    menu->addSeparator();

    menu->addAction("&Preferences...", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        std::vector<AudioDeviceInfo> devices;
        if (m_audioEngine) devices = m_audioEngine->enumerateDevices();
        AppPreferencesDialog dlg(this, devices);
        if (dlg.exec() == QDialog::Accepted) {
            // Apply autosave interval
            if (m_autoSaveTimer)
                m_autoSaveTimer->setInterval(dlg.autosaveMinutes() * 60 * 1000);

            // Apply scrollbar width override (re-set base stylesheet first to avoid accumulation)
            {
                int sbw = dlg.scrollbarWidth();
                QString base = Theme::stylesheet();
                if (sbw != 16) {
                    base += QString(
                        "QScrollBar:vertical { width: %1px; min-width: %1px; }"
                        "QScrollBar:horizontal { height: %1px; min-height: %1px; }"
                    ).arg(sbw);
                }
                qApp->setStyleSheet(base);
            }

            // Apply audio device change
            if (m_audioEngine) {
                int devIdx = dlg.audioDeviceIndex();
                auto s = rt::appSettings();
                s.setValue("AudioDeviceIndex", devIdx);
            }
        }
    });

    (void)cutAct; (void)copyAct; (void)pasteAct;
    (void)deleteAct; (void)selectAllAct;
}

void MainWindow::buildViewMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&View");

    // Page navigation shortcuts
    auto* pagesMenu = menu->addMenu("&Pages");
    pagesMenu->addAction("Projects",   this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; setCurrentPage(Page::Projects); });
    pagesMenu->addAction("Characters", this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; setCurrentPage(Page::Characters); });
    pagesMenu->addAction("Audio",      this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; setCurrentPage(Page::Audio); });
    pagesMenu->addAction("Timeline",   this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; setCurrentPage(Page::Timeline); });
    pagesMenu->addAction("Export",     this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; setCurrentPage(Page::Export); });

    menu->addSeparator();

    auto* fsAct = menu->addAction("Full Screen", this, &MainWindow::onToggleFullScreen);
    fsAct->setShortcut(QKeySequence(Qt::Key_F11));
    fsAct->setCheckable(true);
}

void MainWindow::buildTimelineMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&Timeline");

    menu->addAction("Add Video Track");
    menu->addAction("Add Audio Track");
    // Shared placement: drop a (possibly pre-seeded) tier-list clip on the
    // LOWEST available video track at the playhead — the tier list is a
    // background/base layer. Search from the bottom-most (base) video track
    // upward for the first with free space over the clip's span, so we never
    // overwrite an existing clip; if all are occupied, add a fresh base track.
    auto placeTierListClip = [this](std::unique_ptr<TierListClip> clip) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_timelineWorkspace || !clip) return;
        auto* timeline = m_timelineWorkspace->timeline();
        if (!timeline) return;

        const int64_t start = timeline->playheadPosition();
        const int64_t dur   = clip->duration() > 0 ? clip->duration() : secondsToTicks(10.0);
        const int64_t end   = start + dur;

        auto rangeFreeOnTrack = [](Track* t, int64_t s, int64_t e) -> bool {
            for (size_t c = 0; c < t->clipCount(); ++c) {
                const Clip* cl = t->clip(c);
                if (!cl) continue;
                const int64_t cs = cl->timelineIn();
                const int64_t ce = cs + cl->duration();
                if (s < ce && cs < e) return false;   // half-open interval overlap
            }
            return true;
        };

        std::vector<Track*> videoTracks;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (t && t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack())
                videoTracks.push_back(t);
        }

        // videoTracks is in index order (top→bottom); the base layer is last.
        Track* track = nullptr;
        for (auto it = videoTracks.rbegin(); it != videoTracks.rend(); ++it) {
            if (rangeFreeOnTrack(*it, start, end)) { track = *it; break; }
        }
        bool createdTrack = false;
        if (!track) { track = timeline->addVideoTrack(""); createdTrack = true; }
        if (!track) return;

        clip->setTimelineIn(start);
        if (clip->duration() <= 0) clip->setDuration(dur);

        if (auto* stack = m_timelineWorkspace->commandStack())
            stack->execute(std::make_unique<AddClipCommand>(track, std::move(clip)));
        else
            track->addClip(std::move(clip));

        if (auto* panel = m_timelineWorkspace->timelinePanel()) {
            if (createdTrack) panel->rebuildTracks();
            else              panel->refreshTrackContents();
        }
        m_timelineWorkspace->invalidateCompositeCache();
        if (auto* pm = m_timelineWorkspace->programMonitor()) pm->requestRefresh();
    };

    menu->addAction("Add Tier List", this, [placeTierListClip]() {
        auto clip = std::make_unique<TierListClip>();
        clip->setDuration(secondsToTicks(10.0));
        clip->setLabel("Tier List");
        placeTierListClip(std::move(clip));
    });

    menu->addSeparator();
    menu->addAction("Split at Playhead")->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    menu->addAction("Ripple Delete")->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
    menu->addSeparator();
    // In/Out — QAction text includes shortcut hint for display.
    // Actual key handling is done by TimelineWorkspace::keyPressEvent
    // (I, O) and QShortcut (Ctrl+Shift+X) so they work with context
    // sensitivity and don't fire when QLineEdit has focus.
    {
        auto* act = menu->addAction("Set In Point\tI");
        connect(act, &QAction::triggered, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace) m_timelineWorkspace->setInPoint();
        });
    }
    {
        auto* act = menu->addAction("Set Out Point\tO");
        connect(act, &QAction::triggered, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace) m_timelineWorkspace->setOutPoint();
        });
    }
    {
        auto* act = menu->addAction("Clear In/Out\tCtrl+Shift+X");
        connect(act, &QAction::triggered, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelineWorkspace) m_timelineWorkspace->clearInOut();
        });
    }
    menu->addSeparator();
    {
        // Render In to Out (§4.6 2d) — pre-render the marked range into the
        // segment cache for smoother playback.  Shortcut set on the action so
        // it works whenever the window is active (Ctrl+Shift+R doesn't clash
        // with text entry, unlike the single-letter I/O marks).
        auto* act = menu->addAction("Render In to Out");
        act->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
        connect(act, &QAction::triggered, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelineWorkspace) return;
            // renderInToOut drives its own non-modal progress dialog + cancel,
            // so no wait cursor / success box here — the progress dialog and the
            // green render bar are the feedback.  Only surface the error cases.
            const int r = m_timelineWorkspace->renderInToOut();
            if (r == -1) {
                QMessageBox::information(this, "Render In to Out",
                    "Set an In and Out point first (press I and O on the "
                    "timeline to mark the range to pre-render).");
            } else if (r == -2) {
                QMessageBox::warning(this, "Render In to Out",
                    "Could not render — no active sequence or output size.");
            }
            // r >= 0 (incl. a cancelled partial render) and the busy re-entry
            // sentinel: progress dialog already gave feedback; nothing to show.
        });
    }

}

void MainWindow::buildAudioMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&Audio");

    menu->addAction("Go to Audio Sync", this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        setCurrentPage(Page::Audio);
    });
}

void MainWindow::buildWindowMenu(QMenuBar* menuBar)
{
    m_windowMenu = menuBar->addMenu("&Window");

    // Populate dynamically each time the menu is shown so that checkmarks
    // reflect the current visibility of each dock panel.
    connect(m_windowMenu, &QMenu::aboutToShow, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        m_windowMenu->clear();

        // ── Workspaces section ──────────────────────────────────────────
        QMenu* wsMenu = m_windowMenu->addMenu("Workspaces");

        // Built-in workspace presets (Premiere Pro style)
        struct BuiltinPreset {
            const char* name;
            QKeySequence shortcut;
            // Panels to show (all others hidden); empty = show all
            QStringList visible;
            // Which tab to raise in the Effect Controls dock area
            QString raiseTab;
        };
        // clang-format off
        const BuiltinPreset builtins[] = {
            {"Assembly",  QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_1),
             {"Project Bin", "Source Monitor", "Program Monitor"},
             "Project Bin"},
            {"Editing",   QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_2),
             {"Project Bin", "Source Monitor", "Program Monitor",
              "Effect Controls", "Audio Meters"},
             "Effect Controls"},
            {"Color",     QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_3),
             {"Program Monitor", "Color Grading", "Video Scopes"},
             "Color Grading"},
             {"Effects",   QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_4),
              {"Source Monitor", "Program Monitor", "Effect Controls",
               "Effects", "Text Graphics"},
              "Effect Controls"},
            {"Audio",     QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_5),
             {"Source Monitor", "Program Monitor", "Audio Meters"},
             "Audio Meters"},
        };
        // clang-format on

        for (const auto& bp : builtins) {
            auto* act = wsMenu->addAction(bp.name, this,
                [this, visible = bp.visible, raiseTab = bp.raiseTab]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (!m_timelineWorkspace) return;
                    setCurrentPage(Page::Timeline);
                    const auto& docks = m_timelineWorkspace->dockWidgets();
                    for (auto it = docks.constBegin(); it != docks.constEnd(); ++it) {
                        bool show = visible.isEmpty() || visible.contains(it.key());
                        it.value()->setVisible(show);
                    }
                    if (!raiseTab.isEmpty()) {
                        if (auto* d = docks.value(raiseTab))
                            d->raise();
                    }
                    // Recovery: refresh Program Monitor after dock visibility change
                    if (auto* pm = m_timelineWorkspace->programMonitor())
                        pm->resetViewState();
                    statusBar()->showMessage(
                        QString("Workspace applied"), 2000);
                });
            act->setShortcut(bp.shortcut);
        }

        wsMenu->addSeparator();

        // Reset to Default Layout (stock Premiere Pro-like arrangement)
        wsMenu->addAction("Reset to Default Layout", this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelineWorkspace) return;
            setCurrentPage(Page::Timeline);
            m_timelineWorkspace->resetToDefaultDockLayout();
            statusBar()->showMessage("Layout reset to default", 2000);
        });

        // Reset to Saved Layout
        wsMenu->addAction("Reset to Saved Layout", this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelineWorkspace) return;
            auto s = rt::appSettings();
            s.beginGroup("workspace/last_session");
            if (m_timelineWorkspace->restoreDockLayout(s))
                statusBar()->showMessage("Layout restored", 2000);
            else
                statusBar()->showMessage("No saved layout found", 2000);
            s.endGroup();
            // Recovery: refresh Program Monitor after dock layout change
            if (auto* pm = m_timelineWorkspace->programMonitor())
                pm->resetViewState();
        });

        // ── Refresh Program Monitor ────────────────────────────────────
        wsMenu->addAction("Refresh Program Monitor", this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelineWorkspace) return;
            setCurrentPage(Page::Timeline);
            if (auto* pm = m_timelineWorkspace->programMonitor()) {
                pm->resetViewState();
                statusBar()->showMessage("Program Monitor refreshed", 2000);
            } else {
                statusBar()->showMessage("Program Monitor not available", 2000);
            }
        });

        wsMenu->addSeparator();

        // Save Workspace as...
        wsMenu->addAction("Save Workspace as...", this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            bool ok = false;
            QString name = QInputDialog::getText(
                this, "Save Workspace", "Workspace name:",
                QLineEdit::Normal, QString(), &ok);
            if (!ok || name.isEmpty()) return;

            auto settings = rt::appSettings();
            settings.beginGroup("WorkspacePresets/" + name);
            m_timelineWorkspace->saveDockLayout(settings);
            settings.endGroup();
            statusBar()->showMessage("Workspace '" + name + "' saved", 3000);
        });

        // List saved custom presets
        {
            auto settings = rt::appSettings();
            settings.beginGroup("WorkspacePresets");
            QStringList presets = settings.childGroups();
            settings.endGroup();

            if (!presets.isEmpty()) {
                wsMenu->addSeparator();
                for (const QString& preset : presets) {
                    QMenu* presetMenu = wsMenu->addMenu(preset);
                    presetMenu->addAction("Load", this, [this, preset]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        auto s = rt::appSettings();
                        s.beginGroup("WorkspacePresets/" + preset);
                        m_timelineWorkspace->restoreDockLayout(s);
                        s.endGroup();
                        // Recovery: refresh Program Monitor after dock layout change
                        if (auto* pm = m_timelineWorkspace->programMonitor())
                            pm->resetViewState();
                        statusBar()->showMessage(
                            "Workspace '" + preset + "' loaded", 3000);
                    });
                    presetMenu->addAction("Save current", this, [this, preset]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        auto s = rt::appSettings();
                        s.beginGroup("WorkspacePresets/" + preset);
                        m_timelineWorkspace->saveDockLayout(s);
                        s.endGroup();
                        statusBar()->showMessage(
                            "Workspace '" + preset + "' updated", 3000);
                    });
                    presetMenu->addAction("Delete", this, [this, preset]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        auto s = rt::appSettings();
                        s.remove("WorkspacePresets/" + preset);
                        statusBar()->showMessage(
                            "Workspace '" + preset + "' deleted", 3000);
                    });
                }
            }
        }

        // ── Panel visibility toggles ────────────────────────────────────
        //
        // Organised by category (Premiere Pro style) with keyboard
        // shortcuts Shift+1..9 for the most-used panels.
        // ─────────────────────────────────────────────────────────────────

        if (!m_timelineWorkspace) return;
        const auto& docks = m_timelineWorkspace->dockWidgets();

        // Panel definitions: {display name, dock key, shortcut}
        struct PanelEntry {
            const char* label;       // menu text
            const char* dockKey;     // key in m_dockWidgets QMap
            QKeySequence shortcut;   // may be empty
        };

        // clang-format off
        // -- Editing panels --
        const PanelEntry editingPanels[] = {
            {"Source Monitor",    "Source Monitor",    QKeySequence(Qt::SHIFT | Qt::Key_2)},
            {"Program Monitor",   "Program Monitor",   QKeySequence(Qt::SHIFT | Qt::Key_4)},
            {"Project Bin",       "Project Bin",       QKeySequence(Qt::SHIFT | Qt::Key_1)},
            {"Effect Controls",   "Effect Controls",   QKeySequence(Qt::SHIFT | Qt::Key_5)},
            {"Effects",           "Effects",           QKeySequence(Qt::SHIFT | Qt::Key_7)},
             {"Text Graphics",     "Text Graphics", {}},
            {"Properties",        "Properties",        {}},
            {"History",           "History",           QKeySequence(Qt::SHIFT | Qt::Key_9)},
            {"Library",           "Library",           {}},
        };

        // -- Color panels --
        const PanelEntry colorPanels[] = {
            {"Color Correction", "Color Correction", {}},
            {"Graphics Editor", "Graphics Editor", {}},
            {"Color Grading",  "Color Grading",  {}},
            {"Video Scopes", "Video Scopes", {}},
        };

        // -- Audio panels --
        const PanelEntry audioPanels[] = {
            {"Audio Meters", "Audio Meters", {}},
        };
        // clang-format on

        auto addPanelSection = [&](const PanelEntry* entries, int count) {
            m_windowMenu->addSeparator();

            for (int i = 0; i < count; ++i) {
                const auto& pe = entries[i];
                QDockWidget* dock = docks.value(QString::fromLatin1(pe.dockKey));
                if (!dock) continue;

                auto* act = m_windowMenu->addAction(QString::fromLatin1(pe.label));
                act->setCheckable(true);
                act->setChecked(dock->isVisible());
                if (!pe.shortcut.isEmpty())
                    act->setShortcut(pe.shortcut);

                connect(act, &QAction::triggered, this,
                    [this, dock](bool checked) {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (checked) {
                            setCurrentPage(Page::Timeline);
                            dock->show();
                            dock->raise();
                        } else {
                            dock->close();
                        }
                    });
            }
        };

        addPanelSection(
            editingPanels, static_cast<int>(std::size(editingPanels)));
        addPanelSection(
            colorPanels, static_cast<int>(std::size(colorPanels)));
        addPanelSection(
            audioPanels, static_cast<int>(std::size(audioPanels)));
    });
}

void MainWindow::buildHelpMenu(QMenuBar* menuBar)
{
    auto* menu = menuBar->addMenu("&Help");
    menu->addAction("&Check for Updates...", this, &MainWindow::onCheckForUpdates);
    menu->addSeparator();
    menu->addAction("Third-Party &Licenses...", this, &MainWindow::onShowThirdPartyLicenses);
    menu->addAction("&About ROUNDTABLE", this, &MainWindow::onAbout);
}

void MainWindow::onShowThirdPartyLicenses()
{
    // Find the licenses doc: prefer the installed copy next to the
    // executable; fall back to walking up from the exe to find the
    // dev-tree copy at docs/THIRD_PARTY_LICENSES.md.
    const QString exeDir = QCoreApplication::applicationDirPath();
    QString docPath;
    {
        const QString installed = exeDir + QStringLiteral("/docs/THIRD_PARTY_LICENSES.md");
        if (QFileInfo::exists(installed)) {
            docPath = installed;
        } else {
            QDir d(exeDir);
            for (int i = 0; i < 5 && docPath.isEmpty(); ++i) {
                const QString candidate = d.absoluteFilePath(
                    QStringLiteral("docs/THIRD_PARTY_LICENSES.md"));
                if (QFileInfo::exists(candidate)) {
                    docPath = candidate;
                    break;
                }
                if (!d.cdUp()) break;
            }
        }
    }

    if (docPath.isEmpty()) {
        QMessageBox::warning(this, tr("Third-Party Licenses"),
            tr("Could not find THIRD_PARTY_LICENSES.md.\n\n"
               "It should be installed alongside roundtable.exe in the "
               "docs/ subfolder."));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(docPath));
}

void MainWindow::onCheckForUpdates()
{
    // Manual check from Help menu — always show the result
    if (!m_updateChecker) {
        m_updateChecker = new UpdateChecker(this);
        connect(m_updateChecker, &UpdateChecker::checkComplete,
                this, &MainWindow::onAutoUpdateCheck);
    }
    m_updatePromptShown = true;
    m_updateChecker->check("exporterrormusic", "Roundtable-NLE");
}

void MainWindow::onCheckForUpdatesSilent()
{
    // Auto-check on startup — only show dialog if an update IS available
    if (!m_updateChecker) {
        m_updateChecker = new UpdateChecker(this);
        connect(m_updateChecker, &UpdateChecker::checkComplete,
                this, &MainWindow::onAutoUpdateCheck);
    }
    m_updatePromptShown = false;
    m_updateChecker->check("exporterrormusic", "Roundtable-NLE");
}

void MainWindow::onAutoUpdateCheck(bool available, const QString &version)
{
    if (!available) {
        if (m_updatePromptShown) {
            QMessageBox::information(this, tr("No Updates"),
                tr("You're already running the latest version (v%1).")
                .arg(QApplication::applicationVersion()));
        }
        return;
    }

    // New version available — prompt the user
    auto result = QMessageBox::question(this, tr("Update Available"),
        tr("A new version of ROUNDTABLE is available!\n\n"
           "Current: v%1\nLatest:  v%2\n\n"
           "Download and install now?")
        .arg(QApplication::applicationVersion(), version),
        QMessageBox::Yes | QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_updateChecker->doUpdate(); // starts download
    }
}

// ═════════════════════════════════════════════════════════════════════════════
} // namespace rt