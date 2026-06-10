/*
 * MainWindow.cpp - Tabbed workspace (DaVinci Resolve-style page tabs).
 * Step 26 (modularized)
 */

#include "MainWindow.h"
#include "PathUtils.h"
#include "ShortcutManager.h"
#include "Theme.h"
#include "widgets/DockTitleBar.h"
#include "dialogs/ProjectSettingsDialog.h"
#include "dialogs/KeyboardShortcutsDialog.h"
#include "dialogs/AppPreferencesDialog.h"
#include "dialogs/SequenceDialog.h"

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
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "CrashHandler.h"
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

#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "spine/ShotPreset.h"
#include "SrtIO.h"

#include "Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QEvent>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


namespace rt {


void MainWindow::onUndo()
{
    // If on the Characters page in COMPOSE mode, delegate to ShotComposer's own undo stack
    if (currentPage() == Page::Characters && m_characterShotPanel
        && m_characterShotPanel->currentMode() == CharacterShotPanel::Compose
        && m_characterShotPanel->shotComposer()) {
        m_characterShotPanel->shotComposer()->undo();
        statusBar()->showMessage("Undo (shot)", 2000);
        return;
    }
    if (m_commandStack && m_commandStack->canUndo()) {
        // Save the currently raised dock in the right-side panel group so
        // rebuildTracks() doesn't cause an unwanted dock tab switch.
        QDockWidget* raisedDock = nullptr;
        if (m_timelineWorkspace) {
            for (auto* dock : m_timelineWorkspace->dockWidgets().values()) {
                if (dock->isVisible())
                    raisedDock = dock;
            }
        }

        auto desc = m_commandStack->undoDescription();
        m_commandStack->undo();
        // Refresh timeline UI to reflect the undone change
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
            m_timelineWorkspace->timelinePanel()->rebuildTracks();
        // Flush composite cache and refresh monitor + overlay
        if (m_timelineWorkspace)
            m_timelineWorkspace->refreshAfterUndoRedo();
        // Reload audio sources so audio reflects the undone state
        if (m_timelineWorkspace)
            m_timelineWorkspace->invalidateAudioSources();

        // Restore the previously raised dock so undo doesn't switch tabs.
        if (raisedDock)
            raisedDock->raise();

        statusBar()->showMessage(
            QString("Undo: %1").arg(QString::fromStdString(desc)), 2000);
    }
}

void MainWindow::onRedo()
{
    // If on the Characters page in COMPOSE mode, delegate to ShotComposer's own redo stack
    if (currentPage() == Page::Characters && m_characterShotPanel
        && m_characterShotPanel->currentMode() == CharacterShotPanel::Compose
        && m_characterShotPanel->shotComposer()) {
        m_characterShotPanel->shotComposer()->redo();
        statusBar()->showMessage("Redo (shot)", 2000);
        return;
    }
    if (m_commandStack && m_commandStack->canRedo()) {
        // Save the currently raised dock so redo doesn't switch panel tabs.
        QDockWidget* raisedDock = nullptr;
        if (m_timelineWorkspace) {
            for (auto* dock : m_timelineWorkspace->dockWidgets().values()) {
                if (dock->isVisible())
                    raisedDock = dock;
            }
        }

        auto desc = m_commandStack->redoDescription();
        m_commandStack->redo();
        // Refresh timeline UI to reflect the redone change
        if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
            m_timelineWorkspace->timelinePanel()->rebuildTracks();
        // Flush composite cache and refresh monitor + overlay
        if (m_timelineWorkspace)
            m_timelineWorkspace->refreshAfterUndoRedo();
        // Reload audio sources so audio reflects the redone state
        if (m_timelineWorkspace)
            m_timelineWorkspace->invalidateAudioSources();

        // Restore the previously raised dock so redo doesn't switch tabs.
        if (raisedDock)
            raisedDock->raise();

        statusBar()->showMessage(
            QString("Redo: %1").arg(QString::fromStdString(desc)), 2000);
    }
}

void MainWindow::onToggleFullScreen()
{
    m_fullScreenPreview = !m_fullScreenPreview;

    if (m_fullScreenPreview) {
        showFullScreen();
    } else {
        showNormal();
    }

    emit fullScreenPreviewChanged(m_fullScreenPreview);
}

void MainWindow::onAbout()
{
    QString ver = QApplication::applicationVersion();
    QMessageBox::about(this, "About ROUNDTABLE NLE",
        "<h2>ROUNDTABLE NLE v" + ver + "</h2>"
        "<p>GPU-accelerated Non-Linear Video Editor for Spine Animation</p>"
        "<p>Created by <a href='http://youtube.com/@ExportErrorMusic/'>Export/Error Music</a></p>"
        "<p>GitHub: <a href='https://github.com/exporterrormusic/Roundtable-NLE'>https://github.com/exporterrormusic/Roundtable-NLE</a></p>"
        "<p>Support with donations via <a href='https://www.paypal.com/ncp/payment/7THEH3LWCTRZU'>PayPal</a></p>"
        "<p style='margin-top:12px;font-size:smaller;color:gray'>"
        "Licensed under AGPL-3.0. Bundles FFmpeg (LGPL), Qt 6 (LGPL), "
        "spine-cpp (Spine Runtimes License), and other components. "
        "See <i>Help &rarr; Third-Party Licenses</i> for full attribution.</p>");
}

// ═════════════════════════════════════════════════════════════════════════════
// Events
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Guard active export — the render queue is cancelled cleanly in
    // ExportPanel::~ExportPanel, but the user gets no warning that their
    // partially-written output is about to be abandoned.
    if (m_exportPanel && m_exportPanel->isExporting()) {
        auto reply = QMessageBox::question(
            this, QStringLiteral("Export in Progress"),
            QStringLiteral("An export is still running.\n\n"
                           "If you close ROUNDTABLE now, the export will be "
                           "cancelled and the partial output file will be "
                           "incomplete.\n\n"
                           "Cancel the export and close anyway?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    // Guard unsaved changes
    if (!checkUnsavedChanges()) {
        event->ignore();
        return;
    }

    // Capture project thumbnail before closing (screenshot of current playhead)
    captureProjectThumbnail();

    // Save audio sync state and active page for current project
    if (m_currentProject && m_audioSync) {
        m_audioSync->saveProjectState(
            QString::fromStdString(m_currentProject->name()));

        auto settings = rt::appSettings();
        settings.setValue("Project/" + QString::fromStdString(m_currentProject->name()) + "/activePage",
                          static_cast<int>(currentPage()));
    }

    // F12: Remember last project for auto-open on next launch
    {
        auto settings = rt::appSettings();
        if (m_currentProject && !m_currentProject->filePath().empty())
            settings.setValue("LastProjectPath",
                              QString::fromStdString(pathToUtf8(m_currentProject->filePath())));
        else
            settings.remove("LastProjectPath");
    }

    saveWorkspace("last_session");

    // Also save to the bundled layout file so the installer always captures
    // the latest panel arrangement.  When running from the build tree this
    // writes to assets/default_layout.bin; when installed it's silently
    // ignored (the file path won't be writable).
    {
        QString layoutFile = QCoreApplication::applicationDirPath()
            + QStringLiteral("/assets/default_layout.bin");
        // Only save if the file already exists OR we're in the dev tree
        // (to avoid spewing layout files in Program Files on first close).
        if (QFileInfo::exists(layoutFile) ||
            QDir(QCoreApplication::applicationDirPath()).exists("assets")) {
            saveWorkspaceToFile(layoutFile);
        }
    }

    spdlog::info("MainWindow closing — workspace saved");

    // Clear any stale crash marker so the next launch doesn't show a
    // spurious crash-recovery dialog.  The marker is also cleared by
    // checkCrashRecovery() when the user dismisses the dialog, but if
    // the file persists due to a mid-session crash after recovery, a
    // silent clearCrashMarker() failure, or a crash during shutdown
    // that writes a new marker, this ensures it's wiped on clean close.
    CrashHandler::clearCrashMarker();

    event->accept();

    // Close all windows (belt-and-suspenders) and quit the event loop.
    QApplication::closeAllWindows();
    QApplication::quit();
    QMetaObject::invokeMethod(QApplication::instance(), "quit",
                              Qt::QueuedConnection);

    // Hard shutdown watchdog: a detached thread that force-terminates
    // the process after 8 seconds if graceful teardown hangs (e.g. a
    // background thread blocked on I/O, a GPU waitIdle deadlock, or
    // an FFmpeg demuxer stuck on a network share).  This runs OUTSIDE
    // the Qt event loop, so it works even after the event loop exits
    // and ~App blocks on a hung thread join.
#ifdef _WIN32
    std::thread([]() {
        Sleep(8000);
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
#else
    QTimer::singleShot(8000, QApplication::instance(), []() {
        ::_exit(0);
    });
#endif
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange) return;

    // Window just transitioned out of minimized.  Minimizing destroys the
    // Vulkan swapchain — the last presented frame is gone — and while the
    // playhead is paused nothing else recomposites, so the Program Monitor
    // stays blank until the user scrubs or plays.  Kick a refresh once the
    // event loop has applied the new geometry, then a second time after
    // the swapchain has had time to recreate, so a real frame is presented.
    if (!isMinimized()) {
        QTimer::singleShot(0, this, [this]() {
            if (auto* pm = programMonitor()) pm->requestRefresh();
        });
        QTimer::singleShot(120, this, [this]() {
            if (auto* pm = programMonitor()) pm->requestRefresh();
        });
    }
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    // Try shortcut manager first
    if (m_shortcutManager &&
        m_shortcutManager->handleKeyPress(event->key(), event->modifiers()))
    {
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Global JKL transport routing — works regardless of which panel has focus,
    // but skip when a text input widget (QLineEdit, QTextEdit, QSpinBox) has focus,
    // and skip when the AudioSync panel is active (it has its own transport keys).
    const QEvent::Type t = event->type();

    // Keep the project-loading overlay sized to the window (its layout
    // re-centers the label + progress bar automatically).
    if (watched == this && t == QEvent::Resize && m_loadingOverlay
        && m_loadingOverlay->isVisible()) {
        m_loadingOverlay->setGeometry(rect());
    }

    // While a project open is in flight (off-thread parse + background media
    // warmup), swallow all user input.  The overlay covers mouse clicks to the
    // panels, but keyboard shortcuts travel through this qApp-level filter and
    // some clicks (dock title bars, etc.) can bypass a child overlay — so block
    // them here too.  Failure paths drop the lock before showing any dialog.
    if (m_projectLoading.load(std::memory_order_acquire)) {
        switch (t) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::Shortcut:
        case QEvent::ShortcutOverride:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            return true;   // consume
        default:
            break;
        }
    }

    const bool isKeyPress   = (t == QEvent::KeyPress);
    const bool isKeyRelease = (t == QEvent::KeyRelease);
    if ((isKeyPress || isKeyRelease) && m_playbackController) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->modifiers() == Qt::NoModifier) {
            // Don't intercept if focus is on a text input widget
            QWidget* focused = qApp->focusWidget();
            if (focused && (focused->inherits("QLineEdit") ||
                            focused->inherits("QTextEdit") ||
                            focused->inherits("QPlainTextEdit") ||
                            focused->inherits("QSpinBox") ||
                            focused->inherits("QDoubleSpinBox"))) {
                return QMainWindow::eventFilter(watched, event);
            }

            // Don't intercept transport keys when the AudioSync panel is
            // the active page — AudioSync has its own event filter that
            // routes Space/J/K/L to the selected audio clip.
            if (m_audioSync && focused) {
                if (m_audioSync->isAncestorOf(focused) || focused == m_audioSync) {
                    return QMainWindow::eventFilter(watched, event);
                }
            }

            // Don't intercept transport keys when a modal dialog (e.g.
            // ManualMatchDialog) is active — it handles its own keys.
            if (QWidget* modal = QApplication::activeModalWidget()) {
                if (modal != this) {
                    return QMainWindow::eventFilter(watched, event);
                }
            }

            // Don't forward J/K/L/Space to TimelineWorkspace when the
            // Export panel is the active page — ExportPanel has its own
            // transport controls and mini-timeline playback.
            if (currentPage() == Page::Export) {
                return QMainWindow::eventFilter(watched, event);
            }

            // J / K / L (and their releases) must reach
            // TimelineWorkspace::key{Press,Release}Event so the Premiere-style
            // K+J / K+L slow-scrub hold logic can track K's held state and
            // stop playback when J/L is released. Handling them directly here
            // would short-circuit that logic and produce normal fast shuttle.
            // The workspace's activeController() routes to the Source Monitor
            // when it owns focus, so we don't need a separate branch for it.
            // Guard against re-entry: sendEvent walks qApp's event filters
            // again, so without the flag we'd recurse on every J/K/L/Space.
            const int kkey = keyEvent->key();
            if (!m_forwardingTransportKey
                && (kkey == Qt::Key_J || kkey == Qt::Key_K ||
                    kkey == Qt::Key_L || kkey == Qt::Key_Space)
                && m_timelineWorkspace)
            {
                m_forwardingTransportKey = true;
                QApplication::sendEvent(m_timelineWorkspace, keyEvent);
                m_forwardingTransportKey = false;
                return true;
            }

            // Only key-press events drive the tool / maximize shortcuts; let
            // releases fall through to default propagation.
            if (!isKeyPress) {
                return QMainWindow::eventFilter(watched, event);
            }

            // FCP7-style tool shortcuts: must work whichever panel has
            // focus (Program Monitor, Timeline, etc.) — Premiere parity.
            // The text-input focus check at the top of this filter already
            // skips these when typing in a QLineEdit / QSpinBox / etc.
            auto setTool = [this](EditTool tool) -> bool {
                if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel()) {
                    m_timelineWorkspace->timelinePanel()->setActiveTool(tool);
                    return true;
                }
                return false;
            };

            switch (kkey) {
            case Qt::Key_QuoteLeft:
            case Qt::Key_AsciiTilde:
                if (m_timelineWorkspace)
                    m_timelineWorkspace->togglePanelMaximize();
                return true;
            case Qt::Key_A: if (setTool(EditTool::Selection)) return true; break;
            case Qt::Key_B: if (setTool(EditTool::Razor))     return true; break;
            case Qt::Key_R: if (setTool(EditTool::Rolling))   return true; break;
            case Qt::Key_S: if (setTool(EditTool::Slip))      return true; break;
            case Qt::Key_T: if (setTool(EditTool::Text))      return true; break;
            case Qt::Key_Z: if (setTool(EditTool::Zoom))      return true; break;
            default:
                break;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════════════
// Sequence switching
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::switchSequence(size_t index, int64_t seekTick)
{
    if (!m_currentProject) return;
    if (index >= m_currentProject->sequenceCount()) return;

    if (index == m_currentProject->activeSequenceIndex()) {
        // Already active — honour an explicit seek (e.g. re-opening a nested
        // sequence whose clip the playhead currently sits over).
        if (seekTick >= 0 && m_playbackController)
            m_playbackController->seekTo(seekTick);
        // Already active in the backend — but the tab may still need to be
        // (re-)added or raised. Happens when:
        //   - user closed the active sequence's tab: refreshSequenceTabs
        //     silently switches the backend's active to a remaining open
        //     sequence, then emits sequenceTabChanged → this function gets
        //     called with index == new active and used to early-return,
        //     leaving the timeline panel showing the old sequence.
        //   - user double-clicks the active sequence in the bin whose tab
        //     was closed, expecting it to re-appear.
        // Skip the heavy timeline rebuild (it's already correct) but make
        // sure the tab bar reflects the current active sequence.
        if (m_timelineWorkspace) {
            m_timelineWorkspace->openSequenceTab(index);
            m_timelineWorkspace->refreshSequenceTabs();
        }
        setCurrentPage(Page::Timeline);
        return;
    }

    // Stop playback before switching.  stop() rewinds the playhead to the
    // in-point/start, so capture the outgoing sequence's current position
    // first and restore it afterwards — otherwise switching away would
    // discard where the user was working and the sequence would always
    // re-open at tick 0.  Each Timeline stores its own playhead, so this
    // makes returning to a sequence resume from the same spot.
    int64_t outgoingTick = 0;
    Timeline* outgoing = m_timeline;
    if (m_playbackController) {
        outgoingTick = m_playbackController->currentTick();
        m_playbackController->stop();
    }
    if (outgoing)
        outgoing->setPlayheadPosition(outgoingTick);

    Timeline* newTimeline = m_currentProject->setActiveSequence(index);
    if (!newTimeline) return;

    // Premiere-style nested open: when an explicit seek was requested, move
    // the incoming sequence's playhead there BEFORE the workspace wires it
    // up, so the timeline panel's deferred playhead restore reads this value.
    if (seekTick >= 0)
        newTimeline->setPlayheadPosition(seekTick);

    // Update MainWindow's own pointer
    m_timeline = newTimeline;

    // Update workspace (TimelinePanel + compositeFrame + loadAudioSources)
    if (m_timelineWorkspace)
        m_timelineWorkspace->setTimeline(newTimeline);

    // Update PlaybackController and align the sync clock + audio engine with
    // the sequence's restored (or explicitly requested) playhead so the
    // Program Monitor shows the correct frame and play resumes from there.
    if (m_playbackController) {
        m_playbackController->setTimeline(newTimeline);
        m_playbackController->seekTo(newTimeline->playheadPosition());
    }

    // Update ExportPanel
    if (m_exportPanel) {
        m_exportPanel->setTimeline(newTimeline);
        if (m_currentProject) m_exportPanel->setProject(m_currentProject.get());
    }

    // Refresh the bin to update the bold/active indicator
    if (auto* bin = projectBin())
        bin->refreshSequences();

    // Ensure the sequence is in the open-tab set, then refresh the tab bar
    if (m_timelineWorkspace) {
        m_timelineWorkspace->openSequenceTab(index);
        m_timelineWorkspace->refreshSequenceTabs();
    }

    // Rebuild timeline UI
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->rebuildTracks();

    // Apply the newly-active sequence's own settings (resolution / fps) to the
    // Program Monitor, playback, and timeline — sequences are independent, so
    // switching must re-point the live UI at this sequence's settings.
    {
        const auto& ss = newTimeline->settings();
        applySequenceSettingsRefresh(ss.resolution().width,
                                     ss.resolution().height,
                                     ss.frameRate());
    }

    // Refresh ProgramMonitor so it shows the new sequence content
    if (auto* pm = programMonitor())
        pm->refresh();

    // Switch to Timeline page
    setCurrentPage(Page::Timeline);

    spdlog::info("MainWindow: switched to sequence {} '{}'",
                 index, newTimeline->name());
}

void MainWindow::onSequenceSettingsRequested(size_t seqIdx)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_currentProject) return;

    auto* seq = m_currentProject->sequence(seqIdx);
    if (!seq) return;

    SequenceDialog dlg(this);
    dlg.setWindowTitle(tr("Sequence Settings"));
    dlg.setAcceptButtonText(tr("Save Settings"));

    // Edit THIS sequence's own settings — sequences are independent.
    const auto& settings = seq->settings();
    dlg.setMediaProperties(settings.resolution().width,
                           settings.resolution().height,
                           settings.frameRate());
    dlg.setSequenceName(QString::fromStdString(seq->name()));

    if (dlg.exec() != QDialog::Accepted)
        return;

    auto oldRes = settings.resolution();
    double oldFps = settings.frameRate();
    auto newRes = Resolution{dlg.width(), dlg.height()};
    double newFps = dlg.frameRate();
    QString newName = dlg.sequenceName();
    QString oldName = QString::fromStdString(seq->name());

    if (newRes == oldRes && newFps == oldFps && newName == oldName)
        return; // nothing changed

    // Only refresh the live UI (Program Monitor / playback fps) when the
    // edited sequence is the one currently open.
    auto refreshIfActive = [this, seqIdx](uint32_t w, uint32_t h, double fps) {
        if (m_currentProject &&
            m_currentProject->activeSequenceIndex() == seqIdx)
            applySequenceSettingsRefresh(w, h, fps);
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Sequence Settings",
            [this, newRes, newFps, newName, seqIdx, oldRes, refreshIfActive]() {
                auto* seq = m_currentProject->sequence(seqIdx);
                if (!seq) return;
                if (oldRes != newRes && oldRes.width > 0 && oldRes.height > 0)
                    scaleClipsInSequence(seq, oldRes, newRes);
                seq->settings().setResolution(newRes);
                seq->settings().setFrameRate(newFps);
                seq->setName(newName.toStdString());
                m_currentProject->setModified(true);
                refreshIfActive(newRes.width, newRes.height, newFps);
            },
            [this, oldRes, oldFps, oldName, seqIdx, newRes, refreshIfActive]() {
                auto* seq = m_currentProject->sequence(seqIdx);
                if (!seq) return;
                if (oldRes != newRes && newRes.width > 0 && newRes.height > 0)
                    scaleClipsInSequence(seq, newRes, oldRes);
                seq->settings().setResolution(oldRes);
                seq->settings().setFrameRate(oldFps);
                seq->setName(oldName.toStdString());
                m_currentProject->setModified(true);
                refreshIfActive(oldRes.width, oldRes.height, oldFps);
            }));
    } else {
        if (oldRes != newRes && oldRes.width > 0 && oldRes.height > 0)
            scaleClipsInSequence(seq, oldRes, newRes);
        seq->settings().setResolution(newRes);
        seq->settings().setFrameRate(newFps);
        seq->setName(newName.toStdString());
        m_currentProject->setModified(true);
        refreshIfActive(newRes.width, newRes.height, newFps);
    }
}

void MainWindow::scaleClipsInSequence(Timeline* seq,
                                      const Resolution& from,
                                      const Resolution& to)
{
    // Intentionally a no-op.
    //
    // Clip positions are stored as pixel offsets from a fixed 1920×1080
    // reference and scaled to the output resolution at composite time
    // (CompositeServiceLayerBuild.cpp / OverlayMath.cpp), and clip scale
    // is applied on top of a resolution-independent cover/contain fit
    // (Compositor::buildViewportTransform).  Both are therefore already
    // resolution-independent: changing the sequence resolution preserves
    // the exact visual layout WITHOUT modifying any position/scale value.
    //
    // Rescaling them by the resolution ratio (as this previously did)
    // double-applies the scaling — zooming in when going up in resolution
    // and out when going down.  Leaving the values untouched is what keeps
    // every clip at the same on-screen position and size.
    (void)seq; (void)from; (void)to;
}

void MainWindow::applySequenceSettingsRefresh(uint32_t resW, uint32_t resH, double fps)
{
    if (auto* pm = programMonitor()) {
        pm->setOutputResolution(resW, resH);
        pm->requestRefresh();
    }
    if (auto* ec = effectControlsPanel())
        ec->setSequenceResolution(resW, resH);
    if (m_playbackController)
        m_playbackController->setFrameRate(fps);
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->setFrameRate(fps);
    if (auto* b = projectBin()) b->refreshSequences();
    if (m_timelineWorkspace)
        m_timelineWorkspace->refreshSequenceTabs();
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->rebuildTracks();
}

} // namespace rt
