/*
 * MainWindowProjectMisc.cpp — SRT import/export and thumbnail capture
 * extracted from MainWindowProject.cpp.
 *
 * Contains: onImportSrt(), onExportSrt(), captureProjectThumbnail().
 */

#include "MainWindow.h"

#include "panels/project/ProjectPanel.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/TimelinePanel.h"

#include "media/FrameCache.h"
#include "media/PlaybackController.h"
#include "timeline/Timeline.h"

#include "project/Project.h"
#include "Settings.h"
#include "SrtIO.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>

#include <spdlog/spdlog.h>

#include <filesystem>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// SRT subtitle import / export
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::onImportSrt()
{
    auto settings = rt::appSettings();
    QString lastDir = settings.value("Import/lastDir", QString()).toString();
    if (lastDir.isEmpty())
        lastDir = QDir::homePath();

    QString path = QFileDialog::getOpenFileName(
        this, "Import SRT Subtitles", lastDir,
        "SRT Files (*.srt);;All Files (*)");
    if (path.isEmpty()) return;

    QString dir = QFileInfo(path).absolutePath();
    settings.setValue("Import/lastDir", dir);
    settings.sync();

    auto entries = parseSrt(std::filesystem::path(path.toStdWString()));
    if (entries.empty()) {
        QMessageBox::information(this, "Import SRT", "No subtitle entries found.");
        return;
    }

    int count = importSrt(*m_timeline, entries);
    statusBar()->showMessage(
        QString("Imported %1 subtitle(s)").arg(count), 3000);
    if (m_timelineWorkspace && m_timelineWorkspace->timelinePanel())
        m_timelineWorkspace->timelinePanel()->rebuildTracks();
}

void MainWindow::onExportSrt()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Export SRT Subtitles", QString(),
        "SRT Files (*.srt)");
    if (path.isEmpty()) return;

    int count = exportSrt(*m_timeline, std::filesystem::path(path.toStdWString()));
    if (count > 0)
        statusBar()->showMessage(
            QString("Exported %1 subtitle(s)").arg(count), 3000);
    else
        QMessageBox::information(this, "Export SRT",
            "No text/graphic clips found to export.");
}

// ═════════════════════════════════════════════════════════════════════════════
// Auto-capture project thumbnail from current playhead frame
// ═════════════════════════════════════════════════════════════════════════════

void MainWindow::captureProjectThumbnail()
{
    if (!m_currentProject) return;
    if (!m_projectPanel) return;

    QString projectName = QString::fromStdString(m_currentProject->name());

    // Always composite a 16:9 (project-aspect) frame at the current playhead.
    // Previously a Strategy-1 fallback grabbed the program-monitor viewport widget,
    // which captured letterbox bars when the monitor dock was in portrait orientation.
    // The composited path below renders at exactly the project resolution, so the
    // thumbnail aspect always matches the project (e.g. 1920x1080 -> 480x270).
    if (!m_timelineWorkspace || !m_playbackController) return;

    int64_t tick = m_playbackController->currentTick();

    uint32_t projW = m_currentProject->settings().resolution().width;
    uint32_t projH = m_currentProject->settings().resolution().height;
    if (projW == 0 || projH == 0) { projW = 1920; projH = 1080; }

    double aspect = static_cast<double>(projW) / static_cast<double>(projH);
    uint32_t thumbW = 480;
    uint32_t thumbH = static_cast<uint32_t>(thumbW / aspect);
    thumbW &= ~1u;
    thumbH &= ~1u;

    // Force CPU readback — GPU display mode skips pixel readback entirely,
    // leaving CachedFrame::pixels empty and ensurePixels() failing.
    const bool wasGpuMode = m_timelineWorkspace->gpuDisplayMode();
    if (wasGpuMode)
        m_timelineWorkspace->setGpuDisplayMode(false);

    auto frame = m_timelineWorkspace->compositeFrame(tick, thumbW, thumbH, true);

    if (wasGpuMode)
        m_timelineWorkspace->setGpuDisplayMode(true);
    if (!frame) {
        spdlog::warn("captureProjectThumbnail: compositeFrame returned null");
        return;
    }

    if (!frame->ensurePixels()) {
        spdlog::warn("captureProjectThumbnail: ensurePixels failed");
        return;
    }
    if (frame->pixels.empty() || frame->width == 0 || frame->height == 0) {
        spdlog::warn("captureProjectThumbnail: empty frame data");
        return;
    }

    m_projectPanel->setThumbnailFromPixels(
        projectName,
        frame->pixels.data(),
        frame->width,
        frame->height);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
