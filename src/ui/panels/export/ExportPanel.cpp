/*
 * ExportPanel.cpp â€” Export settings and render queue UI implementation.
 */

#include "ExportPanel.h"
#include "ExportMiniTimeline.h"
#include "PathUtils.h"

#include "Theme.h"

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "Encoder.h"
#include "Muxer.h"
#include "RenderQueue.h"
#include "ExportRenderExecutor.h"
#include "ExportRenderSession.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QApplication>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>

#include "audio/AudioEngine.h"
#include "cache/FrameCache.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include "MainWindow.h"
#include "App.h"
#include "HardwareDiagnostics.h"

#include <spdlog/spdlog.h>

#include <QApplication>
#include <QMetaObject>

namespace rt {

ExportPanel::ExportPanel(QWidget* parent)
    : QWidget(parent)
    , m_renderQueue(std::make_unique<RenderQueue>())
{
    m_renderExecutor = new ExportRenderExecutor(this);
    setupUI();
    setFocusPolicy(Qt::StrongFocus);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(100); // 10 Hz progress updates
    connect(m_pollTimer, &QTimer::timeout, this, &ExportPanel::onPollProgress);
}

bool ExportPanel::isExporting() const noexcept
{
    return m_renderQueue && m_renderQueue->isRunning();
}

ExportPanel::~ExportPanel()
{
    shutdownExportRendering();

    // Unregister from timeline observer
    if (m_timeline)
        m_timeline->removeObserver(this);
}

void ExportPanel::shutdownExportRendering()
{
    m_destroying.store(true, std::memory_order_release);
    if (m_renderExecutor) m_renderExecutor->requestStop();
    if (m_pollTimer) m_pollTimer->stop();
    if (m_playbackTimer) m_playbackTimer->stop();

    // Stop the render queue and join the worker thread so the process
    // can release the session before shared media/GPU dependencies disappear.
    if (m_renderQueue) {
        m_renderQueue->cancelAll();
        m_renderQueue->waitForAll();
        m_renderQueue->setProgressCallback({});
        m_renderQueue->setCompleteCallback({});
        m_renderQueue->setSnapshotResourcePreflightCallback({});
        m_renderQueue->setSnapshotFrameRenderResultCallback({});
        m_renderQueue->setSnapshotFrameStoreCallback({});
    }

    m_exportFrameCallback = {};
    m_previewCallback = {};
    m_exportPreflightCallback = {};
    m_frameStoreCallback = {};
    m_exportRenderSession.reset();
    if (m_renderExecutor) m_renderExecutor->discardPendingWork();
    m_exportRenderSessionFactory = {};
}

void ExportPanel::onTimelineDestroyed(Timeline* tl)
{
    // The timeline is being freed. Drop our cached pointer so nothing here
    // dereferences it later. Do NOT call removeObserver() — the timeline is
    // mid-destruction and its observer list is going away with it.
    if (tl == m_timeline)
        m_timeline = nullptr;
}

void ExportPanel::setTimeline(Timeline* timeline)
{
    // Unregister from old timeline
    if (m_timeline)
        m_timeline->removeObserver(this);

    m_timeline = timeline;

    // Register as observer on new timeline so in/out point changes from
    // other panels (main timeline, Program Monitor) are reflected here.
    if (m_timeline)
        m_timeline->addObserver(this);

    spdlog::info("ExportPanel::setTimeline called, deferring refreshPreview");

    // When the timeline is cleared (project switch / close), reset the
    // output path so the next setTimeline/setProject call will regenerate
    // it from the new sequence name rather than keeping the stale path.
    if (m_outputPath && !timeline) {
        m_outputPath->clear();
        m_outputPath->setPlaceholderText(tr("Select output file..."));
    }

    // Default the output path to the sequence name (only if the field is
    // still empty / showing the placeholder — don't overwrite a user-set path).
    if (m_outputPath && m_outputPath->text().isEmpty() && timeline) {
        QString seqName = QString::fromStdString(timeline->name());
        if (!seqName.isEmpty())
            m_outputPath->setPlaceholderText(
                tr("e.g. %1.mp4").arg(seqName));
    }

    // Reflect any path change in the File Name / Location header fields.
    syncPartsFromOutputPath();

    // Defer refreshPreview to the next event-loop iteration.
    // Calling compositeFrame synchronously during setCurrentProject can
    // crash because GPU resources (VMA allocator / readback buffer) may
    // not be fully initialised when the timeline is wired to panels.
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        refreshPreview();
    });
}

void ExportPanel::setPlaybackController(PlaybackController* controller)
{
    m_playbackController = controller;
}

void ExportPanel::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;
}

void ExportPanel::setCompositor(Compositor* compositor)
{
    m_compositor = compositor;
}

void ExportPanel::setProject(Project* project)
{
    m_project = project;

    // Auto-fill a default output path from the sequence name if still empty
    if (m_outputPath && m_outputPath->text().isEmpty() && project && project->timeline()) {
        QString seqName = QString::fromStdString(project->timeline()->name());
        QString projName = QString::fromStdString(project->name());
        if (!seqName.isEmpty()) {
            // Prefer the last-used export directory (if it still exists)
            // over the project directory — this is what the user expects
            // and runs before showEvent(), which would otherwise be
            // blocked by the field already holding the project default.
            QSettings settings(QStringLiteral("RoundtableMedia"),
                               QStringLiteral("RoundtableNLE"));
            QString lastDir =
                settings.value(QStringLiteral("export/lastOutputDir")).toString();

            if (!lastDir.isEmpty() && QDir(lastDir).exists()) {
                m_outputPath->setText(lastDir + QStringLiteral("/")
                                      + seqName + QStringLiteral(".mp4"));
            } else {
                // Fall back to a path inside the project's directory
                std::filesystem::path projDir = project->filePath().parent_path();
                if (!projDir.empty()) {
                    QString defaultPath = QString::fromStdString(pathToUtf8(projDir / (seqName + ".mp4").toStdString()));
                    m_outputPath->setText(defaultPath);
                } else {
                    // No project path yet — just show a placeholder hint
                    m_outputPath->setPlaceholderText(
                        tr("e.g. %1.mp4").arg(seqName));
                }
            }
        }
    }

    // Sync match-sequence settings now that we have project settings
    if (m_matchSequenceCheck && m_matchSequenceCheck->isChecked())
        syncMatchSequenceSettings();

    // Reflect the (possibly auto-filled) path in the File Name / Location fields.
    syncPartsFromOutputPath();
}

void ExportPanel::syncMatchSequenceSettings()
{
    if (!m_project)
        return;
    auto& settings = m_project->settings();
    m_widthSpin->setValue(static_cast<int>(settings.resolution().width));
    m_heightSpin->setValue(static_cast<int>(settings.resolution().height));
    int seqFps = static_cast<int>(std::round(settings.frameRate()));
    for (int i = 0; i < m_fpsCombo->count(); ++i) {
        if (m_fpsCombo->itemData(i).toInt() == seqFps) {
            m_fpsCombo->setCurrentIndex(i);
            break;
        }
    }
}

void ExportPanel::setPreviewCallback(PreviewCallback cb)
{
    m_previewCallback = std::move(cb);
}

void ExportPanel::setExportFrameCallback(ExportFrameCallback cb)
{
    if (!cb) {
        m_exportFrameCallback = nullptr;
        return;
    }
    m_exportFrameCallback = [cb = std::move(cb)](
        const std::shared_ptr<const ExportRenderSnapshot>& snapshot,
        int64_t tick, uint32_t w, uint32_t h, bool scrub, bool preserveAlpha) {
        RenderResult result;
        result.timelineTick = tick;
        result.frame = cb(snapshot, tick, w, h, scrub, preserveAlpha);
        if (!result.frame)
            result.status = RenderResultStatus::Pending;
        else if (result.frame->width == 0 || result.frame->height == 0)
            result.status = RenderResultStatus::Blank;
        else
            result.status = RenderResultStatus::Ready;
        result.diagnostics.status = result.status;
        return result;
    };
}

void ExportPanel::setExportResultCallback(ExportResultCallback cb)
{
    m_exportFrameCallback = std::move(cb);
}

void ExportPanel::setExportPreflightCallback(ExportPreflightCallback cb)
{
    m_exportPreflightCallback = std::move(cb);
}

void ExportPanel::setExportRenderSessionFactory(
    ExportRenderSessionFactory factory)
{
    m_exportRenderSessionFactory = std::move(factory);
}

void ExportPanel::applyInOutPointEdit(const std::string& description,
                                       int64_t newInPoint,
                                       int64_t newOutPoint,
                                       int     newRangeComboIdx)
{
    if (!m_timeline) return;

    const int64_t oldInPoint  = m_timeline->inPoint();
    const int64_t oldOutPoint = m_timeline->outPoint();
    const int     oldRangeIdx = m_rangeCombo ? m_rangeCombo->currentIndex() : 0;

    if (oldInPoint == newInPoint && oldOutPoint == newOutPoint
        && oldRangeIdx == newRangeComboIdx) {
        return; // no observable change
    }

    auto apply = [this](int64_t in, int64_t out, int rangeIdx) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_timeline) return;
        if (in < 0 && out < 0) {
            m_timeline->clearInOutPoints();
        } else {
            // setInPoint/setOutPoint accept -1 as "not set", so this also
            // covers the "only one of them is set" case.
            if (in >= 0)
                m_timeline->setInPoint(in);
            else
                m_timeline->clearInOutPoints();
            if (out >= 0)
                m_timeline->setOutPoint(out);
        }
        if (m_rangeCombo)
            m_rangeCombo->setCurrentIndex(rangeIdx);
        refreshPreview();
    };

    apply(newInPoint, newOutPoint, newRangeComboIdx);

    if (m_commandStack) {
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            description,
            [apply, newInPoint, newOutPoint, newRangeComboIdx]() {
                apply(newInPoint, newOutPoint, newRangeComboIdx);
            },
            [apply, oldInPoint, oldOutPoint, oldRangeIdx]() {
                apply(oldInPoint, oldOutPoint, oldRangeIdx);
            }));
    }
}

void ExportPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // Restore the last used output directory, preferring it over the
    // project-directory default so the user's last export location is preserved.
    QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
    QString lastDir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
    // Only restore it if the directory still exists on disk (it may have
    // been on a removed drive or deleted since the last session).
    if (!lastDir.isEmpty() && QDir(lastDir).exists() && m_outputPath && m_timeline) {
        QString seqName = QString::fromStdString(m_timeline->name());
        if (!seqName.isEmpty()) {
            QString preferredPath = lastDir + QStringLiteral("/") + seqName + QStringLiteral(".mp4");
            // Only override if the current path is not a user-set absolute path
            QString cur = m_outputPath->text().trimmed();
            if (cur.isEmpty() || cur == m_outputPath->placeholderText()) {
                m_outputPath->setText(preferredPath);
            }
        }
    }

    // Reflect the resolved path in the File Name / Location header fields.
    syncPartsFromOutputPath();

    // Defer refreshPreview to the next event-loop iteration to avoid
    // triggering GPU composition + widget state changes synchronously
    // during a show event (which can happen during QDialog::exec event loops).
    QTimer::singleShot(0, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        refreshPreview();
    });
    // Grab keyboard focus so Space/Left/Right/I/O work immediately
    // without requiring the user to click in the panel first.
    setFocus(Qt::OtherFocusReason);
}

void ExportPanel::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        onPlayPause();
        event->accept();
        return;
    case Qt::Key_Right:
        onStepForward();
        event->accept();
        return;
    case Qt::Key_Left:
        onStepBack();
        event->accept();
        return;
    case Qt::Key_Home:
        onSkipToStart();
        event->accept();
        return;
    case Qt::Key_End:
        onSkipToEnd();
        event->accept();
        return;
    case Qt::Key_Escape: {
        // Navigate back to the timeline page.
        // Find the MainWindow by walking up the widget hierarchy.
        for (QWidget* w = parentWidget(); w; w = w->parentWidget()) {
            if (auto* mw = qobject_cast<MainWindow*>(w)) {
                mw->setCurrentPage(Page::Timeline);
                break;
            }
        }
        event->accept();
        return;
    }
    case Qt::Key_I:
        // Set In point at current playhead position
        if (m_timeline && m_miniTimeline) {
            const int64_t playhead = m_miniTimeline->playhead();
            applyInOutPointEdit("Set in point",
                                playhead, m_timeline->outPoint(), 1);
            spdlog::info("ExportPanel: In point set at tick={}", playhead);
        }
        event->accept();
        return;
    case Qt::Key_O:
        // Set Out point at current playhead position
        if (m_timeline && m_miniTimeline) {
            const int64_t playhead = m_miniTimeline->playhead();
            applyInOutPointEdit("Set out point",
                                m_timeline->inPoint(), playhead, 1);
            spdlog::info("ExportPanel: Out point set at tick={}", playhead);
        }
        event->accept();
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        // Clear in/out points
        if (m_timeline) {
            applyInOutPointEdit("Clear in/out points", -1, -1, 0);
            spdlog::info("ExportPanel: In/Out points cleared");
        }
        event->accept();
        return;
    case Qt::Key_X:
        if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            // Ctrl+Shift+X — clear in/out points
            if (m_timeline) {
                applyInOutPointEdit("Clear in/out points", -1, -1, 0);
                spdlog::info("ExportPanel: In/Out points cleared via Ctrl+Shift+X");
            }
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void ExportPanel::refreshPreview()
{
    // Re-entrancy guard: prevent recursive paint cycles when refreshPreview
    // is triggered during paint event processing (e.g. during QDialog::exec).
    if (m_refreshing) {
        spdlog::warn("ExportPanel::refreshPreview: re-entrancy detected, skipping");
        return;
    }
    m_refreshing = true;

    // Skip GPU compositing when a modal dialog is active (QDialog::exec
    // event loop).  The GPU compositing + QPixmap::setPixmap cascade
    // during nested event loops exhausts the C++ heap.  The existing
    // preview remains on screen, which is fine — the user is in a dialog.
    if (QApplication::activeModalWidget() != nullptr) {
        m_refreshing = false;
        return;
    }

    if (!m_previewImageLabel || !m_timeline) { m_refreshing = false; return; }
    spdlog::info("ExportPanel::refreshPreview starting");

    // Update mini timeline with sequence info
    if (m_miniTimeline) {
        m_miniTimeline->setDuration(m_timeline->duration());

            // Auto-select "In to Out" range when AT LEAST ONE in/out point is set,
        // unless the user explicitly changed the range combo.
        bool hasEitherInOut = (m_timeline->inPoint() >= 0 || m_timeline->outPoint() > 0);
        if (hasEitherInOut && m_rangeCombo && m_rangeCombo->currentIndex() == 0) {
            m_rangeCombo->setCurrentIndex(1); // "In to Out"
        } else if (!hasEitherInOut && m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
            m_rangeCombo->setCurrentIndex(0); // "Entire Sequence"
        }

        // Always show in/out markers on the mini timeline bar,
        // regardless of range combo — matching TIMELINE tab behaviour.
        // Only clear markers when neither point is set.
        if (hasEitherInOut) {
            m_miniTimeline->setInOutRange(m_timeline->inPoint(), m_timeline->outPoint());
        } else {
            m_miniTimeline->setInOutRange(-1, -1);
        }
    }

    // Update info label text
    if (m_previewInfoLabel) {
        double durSec = ticksToSeconds(m_timeline->duration());
        int mins = static_cast<int>(durSec) / 60;
        int secs = static_cast<int>(durSec) % 60;
        int frames = static_cast<int>((durSec - static_cast<int>(durSec)) * 30);
        QString infoText = QString("%1x%2  |  %3:%4:%5")
            .arg(m_widthSpin ? m_widthSpin->value() : 1920)
            .arg(m_heightSpin ? m_heightSpin->value() : 1080)
            .arg(mins, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'))
            .arg(frames, 2, 10, QChar('0'));

        // Show in/out range info if set
        if (m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
            int64_t inPt = m_timeline->inPoint();
            int64_t outPt = m_timeline->outPoint();
            if (inPt >= 0 && outPt > 0 && outPt > inPt) {
                double rangeSec = ticksToSeconds(outPt - inPt);
                int rm = static_cast<int>(rangeSec) / 60;
                int rs = static_cast<int>(rangeSec) % 60;
                infoText += QString("  (In/Out: %1:%2)").arg(rm, 2, 10, QChar('0')).arg(rs, 2, 10, QChar('0'));
            } else if (inPt >= 0 && outPt <= 0) {
                infoText += "  (In point set, no out point)";
            } else if (outPt > 0 && inPt < 0) {
                infoText += "  (Out point set, no in point)";
            } else {
                infoText += "  (In/out not usable)";
            }
        }
        m_previewInfoLabel->setText(infoText);
    }

    if (!m_previewCallback) {
        m_previewImageLabel->setText("No preview available");
        m_refreshing = false;
        return;
    }

    // Render first frame — preserve the current playhead position instead of
    // always jumping to 0 or to the in-point (fixes bug where setting an out-point
    // with the O key would snap the playhead back to the in-point or beginning).
    int64_t previewTick = m_miniTimeline ? m_miniTimeline->playhead() : 0;

    // If switching to "In to Out" range and the playhead is outside the range,
    // snap to the in-point so the preview shows valid content.
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1 && m_timeline->inPoint() >= 0) {
        int64_t outPt = m_timeline->outPoint();
        if (previewTick < m_timeline->inPoint() || (outPt > 0 && previewTick > outPt))
            previewTick = m_timeline->inPoint();
    }

    // Set mini timeline playhead to match
    if (m_miniTimeline) {
        m_miniTimeline->setPlayhead(previewTick);
    }

    // Render at the actual output resolution for a crisp preview
    uint32_t renderW = m_widthSpin  ? static_cast<uint32_t>(m_widthSpin->value())  : 1920;
    uint32_t renderH = m_heightSpin ? static_cast<uint32_t>(m_heightSpin->value()) : 1080;

    spdlog::info("ExportPanel::refreshPreview calling compositeFrame at tick={} res={}x{}",
                 previewTick, renderW, renderH);
    auto t0 = std::chrono::steady_clock::now();
    auto frame = m_previewCallback(previewTick, renderW, renderH, true);
    auto t1 = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    spdlog::info("ExportPanel::compositeFrame took {} ms, frame={}", dt, (bool)frame);
    if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
        uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
        QImage img(frame->pixels.data(), static_cast<int>(frame->width),
                   static_cast<int>(frame->height), static_cast<int>(stride),
                   QImage::Format_ARGB32);
        QPixmap pix = QPixmap::fromImage(img).scaled(
            m_previewImageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_previewImageLabel->setPixmap(pix);
    } else {
        m_previewImageLabel->setText("No clips at playhead");
    }

    m_refreshing = false;
}

} // namespace rt
