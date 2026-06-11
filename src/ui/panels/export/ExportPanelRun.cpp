// ExportPanelRun.cpp
// Export job config, offline-media check, start/cancel/poll.
// Extracted from ExportPanel.cpp for file size (behavior-preserving).

#include "ExportPanel.h"
#include "ExportMiniTimeline.h"
#include "PathUtils.h"

#include "Theme.h"

#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "Encoder.h"
#include "Muxer.h"
#include "RenderQueue.h"

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

void ExportPanel::rememberExportDir(const std::string& outputPath)
{
    if (outputPath.empty()) return;
    QString dir = QFileInfo(QString::fromStdString(outputPath)).absolutePath();
    if (dir.isEmpty()) return;
    QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
    settings.setValue(QStringLiteral("export/lastOutputDir"), dir);
}

void ExportPanel::onBrowseOutput()
{
    // Use the CURRENT output path text as the starting point, so the dialog
    // pre-fills with what was already set. Falls back to the sequence name
    // (or project name) if the field is empty.
    QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
    QString currentPath = m_outputPath->text().trimmed();

    QString defaultPath;
    if (!currentPath.isEmpty()) {
        defaultPath = currentPath;
    } else {
        // Default filename from the active sequence name (or project name as fallback)
        QString defaultName;
        if (m_timeline) {
            defaultName = QString::fromStdString(m_timeline->name());
        } else if (m_project) {
            defaultName = QString::fromStdString(m_project->name());
        }
        if (defaultName.isEmpty())
            defaultName = QStringLiteral("export");

        QString lastDir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
        defaultPath = (lastDir.isEmpty() || !QDir(lastDir).exists())
                          ? defaultName
                          : lastDir + QStringLiteral("/") + defaultName;
    }

    QString filter = tr("Video Files (*.mp4 *.mov *.mkv *.webm *.avi);;All Files (*)");
    QString path = QFileDialog::getSaveFileName(this, tr("Export Output"), defaultPath, filter);
    if (!path.isEmpty()) {
        m_outputPath->setText(path);
        // Persist the chosen directory for next time
        QFileInfo fi(path);
        settings.setValue(QStringLiteral("export/lastOutputDir"), fi.absolutePath());
    }
}

ExportJobConfig ExportPanel::buildJobConfig() const
{
    ExportJobConfig cfg;

    cfg.outputPath  = m_outputPath->text().toStdString();
    cfg.outputWidth  = m_widthSpin->value();
    cfg.outputHeight = m_heightSpin->value();

    cfg.encoderConfig.width  = cfg.outputWidth;
    cfg.encoderConfig.height = cfg.outputHeight;
    cfg.encoderConfig.codec  = static_cast<EncoderCodec>(
        m_codecCombo->currentData().toInt());
    // Index 0 is the "Auto" item — resolve it to whatever hardware
    // encoder is actually available rather than blindly assuming NVENC.
    if (m_accelCombo->currentIndex() == 0) {
        cfg.encoderConfig.hwAccel =
            Encoder::detectBestHardware(cfg.encoderConfig.codec);
    } else {
        cfg.encoderConfig.hwAccel = static_cast<HardwareAccel>(
            m_accelCombo->currentData().toInt());
    }
    // Map quality slider (0-100) â†’ CRF value
    // 100 = Best (CRF 14), 75 = High (CRF 18), 50 = Medium (CRF 23),
    // 25 = Low (CRF 28), 0 = Lowest (CRF 35)
    {
        int q = m_crfSlider->value();
        // Linear interpolation: quality 0â†’CRF 35, quality 100â†’CRF 14
        int crf = 35 - (q * 21) / 100;  // 35..14
        cfg.encoderConfig.crf = crf;
    }
    cfg.encoderConfig.fpsNum = m_fpsCombo->currentData().toUInt();
    cfg.encoderConfig.fpsDen = 1;

    cfg.containerFormat = static_cast<uint8_t>(m_containerCombo->currentData().toInt());
    cfg.includeAudio = m_audioCheck->isChecked();
    cfg.preset = static_cast<ExportPreset>(m_presetCombo->currentData().toInt());

    // Range: convert In/Out ticks to frame indices and audio times
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1 && m_timeline) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        double fps = static_cast<double>(cfg.encoderConfig.fpsNum) /
                     std::max<uint32_t>(cfg.encoderConfig.fpsDen, 1u);
        double totalDur = ticksToSeconds(m_timeline->duration());

        // Use in-point if set, otherwise start from beginning
        if (inPt >= 0) {
            double inSec = ticksToSeconds(inPt);
            cfg.startFrame = static_cast<int64_t>(inSec * fps);
            cfg.audioConfig.startTime = inSec;
        } else {
            cfg.startFrame = 0;
            cfg.audioConfig.startTime = 0.0;
        }

        // Use out-point if set and > in-point, otherwise use full duration
        if (outPt > 0 && outPt > inPt) {
            double outSec = ticksToSeconds(outPt);
            cfg.endFrame = static_cast<int64_t>(outSec * fps);
            cfg.audioConfig.endTime = outSec;
        } else {
            cfg.endFrame = static_cast<int64_t>(totalDur * fps);
            cfg.audioConfig.endTime = totalDur;
        }
    } else {
        // Full timeline — audio also uses full duration
        if (m_timeline) {
            cfg.audioConfig.endTime = ticksToSeconds(m_timeline->duration());
        }
    }

    return cfg;
}

bool ExportPanel::checkOfflineMedia()
{
    if (!m_timeline) return true;

    std::vector<std::string> offlineClips;
    for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
        const auto* trk = m_timeline->track(ti);
        if (!trk || trk->isDivider()) continue;
        for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
            const auto* clip = trk->clip(ci);
            if (clip && clip->isOffline()) {
                std::string label = clip->label();
                if (label.empty())
                    label = "(unnamed)";
                offlineClips.push_back(label);
            }
        }
    }

    if (offlineClips.empty())
        return true;

    // Build a summary message listing the offline clips
    QString msg = tr("The following media files are offline (missing or unavailable):\n\n");
    for (size_t i = 0; i < offlineClips.size() && i < 20; ++i) {
        msg += QStringLiteral("  \u2022 ") + QString::fromStdString(offlineClips[i]) + QStringLiteral("\n");
    }
    if (offlineClips.size() > 20) {
        msg += tr("  ... and %1 more\n").arg(static_cast<int>(offlineClips.size() - 20));
    }
    msg += tr("\nThese clips will appear as black/missing in the export.\n"
              "Do you want to continue anyway?");

    auto result = QMessageBox::question(this, tr("Offline Media Detected"),
                                         msg, QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);
    return (result == QMessageBox::Yes);
}

void ExportPanel::onStartExport()
{
    if (m_outputPath->text().isEmpty()) {
        QMessageBox::warning(this, tr("Export"), tr("Please select an output file."));
        return;
    }

    if (!m_timeline) {
        QMessageBox::warning(this, tr("Export"), tr("No timeline loaded — nothing to export."));
        return;
    }

    if (!m_previewCallback) {
        QMessageBox::warning(this, tr("Export"), tr("No renderer available — cannot export."));
        return;
    }

    // Check for offline media and warn the user
    if (!checkOfflineMedia())
        return;

    auto config = buildJobConfig();

    // ── Pascal NVENC pre-flight ───────────────────────────────────────
    // NVIDIA Pascal consumer GPUs (GTX 10xx) cap concurrent NVENC
    // sessions at 2 in hardware.  If Discord / OBS / another encoder
    // app is running we'll fail NVENC init and silently fall back to
    // CPU encoding.  Warn the user upfront so they can close those
    // apps now instead of discovering it after a long, slow export.
    // Suppressed for one session via Cancel-then-tick, persisted via
    // QSettings if they choose "Don't warn again".
    if (config.encoderConfig.hwAccel != HardwareAccel::None) {
        const auto* app = App::instance();
        const bool isPascal = app && app->diagnosticsGpu().arch
                              == HardwareDiagnostics::GpuArchitecture::NvidiaPascal;
        if (isPascal) {
            QSettings cfg(QStringLiteral("RoundtableMedia"),
                          QStringLiteral("RoundtableNLE"));
            const bool suppressed = cfg.value(
                QStringLiteral("export/pascalNvencHintSuppressed"), false).toBool();
            if (!suppressed) {
                QMessageBox box(this);
                box.setIcon(QMessageBox::Information);
                box.setWindowTitle(tr("Pascal NVENC reminder"));
                box.setText(tr(
                    "Your GPU is a Pascal-class card (GTX 10xx). Pascal consumer "
                    "GPUs are limited to <b>2 concurrent NVENC encoder sessions</b> "
                    "in hardware.<br><br>"
                    "If <b>Discord</b>, <b>OBS</b>, or another screen-recording app "
                    "is running it may be holding an encoder session and this "
                    "export will fall back to CPU encoding (much slower).<br><br>"
                    "Close those apps for the fastest HW-accelerated export. "
                    "Continue anyway?"));
                auto* dontShow = new QCheckBox(tr("Don't show this again"), &box);
                box.setCheckBox(dontShow);
                box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Yes);
                int rc = box.exec();
                if (dontShow->isChecked()) {
                    cfg.setValue(
                        QStringLiteral("export/pascalNvencHintSuppressed"), true);
                }
                if (rc != QMessageBox::Yes) return;
            }
        }
    }

    rememberExportDir(pathToUtf8(config.outputPath));
    uint32_t jobId = m_renderQueue->addJob(config);
    m_activeJobId = jobId;

    // Update job list
    auto* item = new QListWidgetItem(
        QStringLiteral("\u25CB Job %1 \u2014 %2 \u2014 Queued")
            .arg(jobId)
            .arg(m_outputPath->text()));
    item->setData(Qt::UserRole, static_cast<qulonglong>(jobId));
    m_jobList->addItem(item);

    // Set up callbacks
    m_renderQueue->setProgressCallback(
        [this](uint32_t id, const JobProgress& /*prog*/) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            Q_UNUSED(id);
            // Progress stored in job, polled by timer
        });

    m_renderQueue->setCompleteCallback(
        [this](uint32_t id, bool success, const std::string& msg) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Snapshot the fallback state on the worker thread BEFORE
            // queueing back to the UI — the job record may be reset by
            // the time the lambda runs.
            bool fellBack = false;
            std::string fellBackReason;
            if (const auto* j = m_renderQueue->job(id)) {
                fellBack = j->fellBackToCpuEncoder;
                fellBackReason = j->fellBackReason;
            }
            QMetaObject::invokeMethod(this,
                [this, id, success, msg, fellBack, fellBackReason]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                m_pollTimer->stop();
                m_startButton->setEnabled(true);
                m_addQueueButton->setEnabled(true);
                m_cancelButton->setEnabled(false);
                m_cancelButton->setVisible(false);
                m_statusLabel->setText(success ? tr("Export complete!") : tr("Failed: %1").arg(QString::fromStdString(msg)));
                m_progressBar->setValue(success ? 100 : 0);
                // Update the job list item to reflect completion status
                const qulonglong targetId = static_cast<qulonglong>(id);
                for (int i = 0; i < m_jobList->count(); ++i) {
                    auto* listItem = m_jobList->item(i);
                    if (listItem && listItem->data(Qt::UserRole).toULongLong() == targetId) {
                        const QString line2 = listItem->text().section(QStringLiteral(" \u2014 "), 1, 1);
                        if (success) {
                            listItem->setText(QStringLiteral("\u2713 Job %1 \u2014 %2 \u2014 Complete")
                                .arg(id).arg(line2));
                            listItem->setForeground(Qt::darkGreen);
                        } else {
                            listItem->setText(QStringLiteral("\u2717 Job %1 \u2014 %2 \u2014 Failed")
                                .arg(id).arg(line2));
                            listItem->setForeground(Qt::red);
                        }
                        break;
                    }
                }
                if (success)
                    QApplication::beep();
                emit exportFinished(id, success, QString::fromStdString(msg));

                // Surface the silent HW→CPU fallback to the user.  The
                // export still succeeded (or failed for its own reason)
                // but the user is waiting much longer than expected and
                // deserves to know why and how to fix it for next time.
                if (fellBack && !fellBackReason.empty()) {
                    QMessageBox box(this);
                    box.setIcon(QMessageBox::Information);
                    box.setWindowTitle(tr("Hardware encoder unavailable"));
                    box.setText(QString::fromStdString(fellBackReason));
                    box.setStandardButtons(QMessageBox::Ok);
                    box.exec();
                }
            });
        });

    // Wire the frame render callback so export uses real compositing
    // with a composite/encode PIPELINE.  The worker thread encodes the
    // PREVIOUS frame while the main thread composites the CURRENT frame,
    // overlapping ~2ms of encode time with ~10ms of composite time.
    //
    // Each call to pipelineComposite:
    //   1) Submits THIS frame's composite to the main thread (QueuedConnection
    //      — non-blocking, returns immediately)
    //   2) Waits for the PREVIOUS frame's composite to finish (already
    //      started when pipelineComposite was last called)
    //   3) Returns the previous frame's pixels for encoding
    //
    // The first call composites synchronously since there's no previous frame.
    if (m_previewCallback) {
        m_renderQueue->setFrameRenderCallback(
            [this](int64_t tick, int64_t nextTick,
                   uint32_t w, uint32_t h, bool scrub)
                -> std::shared_ptr<CachedFrame> {
                if (m_destroying.load(std::memory_order_acquire)) return nullptr;
                return pipelineComposite(tick, nextTick, w, h, scrub);
            });
    }

    // Start rendering
    m_renderQueue->start(m_timeline, m_compositor);

    m_startButton->setEnabled(false);
    m_addQueueButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_cancelButton->setVisible(true);
    m_jobList->setVisible(true);
    m_pollTimer->start();

    emit exportStarted(jobId);
}

void ExportPanel::onCancelExport()
{
    m_renderQueue->cancelAll();
    m_pollTimer->stop();
    m_startButton->setEnabled(true);
    m_addQueueButton->setEnabled(true);
    m_cancelButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_statusLabel->setText(tr("Cancelled"));
}

void ExportPanel::onPollProgress()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_renderQueue->isRunning()) return;

    const auto* j = m_renderQueue->job(m_activeJobId);
    if (!j) return;

    int pct = static_cast<int>(j->progress.percent);
    m_progressBar->setValue(pct);
    // Update the active job's list item to show "Running"
    const qulonglong activeId = static_cast<qulonglong>(m_activeJobId);
    for (int i = 0; i < m_jobList->count(); ++i) {
        auto* listItem = m_jobList->item(i);
        if (listItem && listItem->data(Qt::UserRole).toULongLong() == activeId) {
            const QString line2 = listItem->text().section(QStringLiteral(" \u2014 "), 1, 1);
            listItem->setText(QStringLiteral("\u21BB Job %1 \u2014 %2 \u2014 Running %3%")
                .arg(m_activeJobId).arg(line2).arg(pct));
            listItem->setForeground(QColor(0, 120, 212)); // blue
            break;
        }
    }
    // Build a rich status string: "Rendering â€” 245/800 frames Â· 14.2 fps Â· ETA 0:39"
    int64_t curFrame   = j->progress.currentFrame;
    int64_t totalFrame = j->progress.totalFrames;
    double  elapsed    = j->progress.elapsedSeconds;
    double  fps        = (elapsed > 0.5) ? (curFrame / elapsed) : 0.0;

    QString status;
    if (pct < 100) {
        status = tr("Rendering \u2014 %1/%2 frames").arg(curFrame).arg(totalFrame);
        if (fps > 0.1) {
            status += QStringLiteral("  \u00B7  %1 fps").arg(QString::number(fps, 'f', 1));
            // ETA
            int64_t remaining = totalFrame - curFrame;
            double etaSec = remaining / fps;
            int etaMin = static_cast<int>(etaSec) / 60;
            int etaS   = static_cast<int>(etaSec) % 60;
            if (etaMin > 0)
                status += QStringLiteral("  \u00B7  ETA %1:%2")
                    .arg(etaMin).arg(etaS, 2, 10, QChar('0'));
            else
                status += QStringLiteral("  \u00B7  ETA %1s").arg(etaS);
        }
    } else {
        status = QString::fromStdString(j->progress.statusText);
    }
    m_statusLabel->setText(status);

    // Update preview with the current frame being rendered
    if (m_previewCallback && m_timeline && m_previewImageLabel) {
        int64_t frameIdx = curFrame + j->config.startFrame;
        double frameFps = static_cast<double>(j->config.encoderConfig.fpsNum) /
                     std::max<uint32_t>(j->config.encoderConfig.fpsDen, 1u);
        double timeSec = (frameFps > 0) ? frameIdx / frameFps : 0.0;
        int64_t tick = static_cast<int64_t>(timeSec * 48000.0);

        uint32_t prevW = static_cast<uint32_t>(j->config.outputWidth);
        uint32_t prevH = static_cast<uint32_t>(j->config.outputHeight);
        auto frame = m_previewCallback(tick, prevW, prevH, true);
        if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
            uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
            QImage img(frame->pixels.data(), static_cast<int>(frame->width),
                       static_cast<int>(frame->height), static_cast<int>(stride),
                       QImage::Format_ARGB32);
            QPixmap pix = QPixmap::fromImage(img).scaled(
                m_previewImageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_previewImageLabel->setPixmap(pix);
        }

        if (m_miniTimeline)
            m_miniTimeline->setPlayhead(tick);
    }

    emit exportProgress(m_activeJobId, j->progress.percent);
}
} // namespace rt
