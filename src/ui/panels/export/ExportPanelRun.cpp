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
#include <cmath>


namespace rt {

void ExportPanel::rememberExportDir(const std::string& outputPath)
{
    if (outputPath.empty()) return;
    QString dir = QFileInfo(QString::fromStdString(outputPath)).absolutePath();
    if (dir.isEmpty()) return;
    QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
    settings.setValue(QStringLiteral("export/lastOutputDir"), dir);
}

void ExportPanel::syncPartsFromOutputPath()
{
    if (!m_outputPath) return;
    const QString full = m_outputPath->text().trimmed();
    QFileInfo fi(full);

    if (m_fileNameEdit) {
        const QString name = fi.fileName();
        if (m_fileNameEdit->text() != name)
            m_fileNameEdit->setText(name);
    }
    if (m_locationLabel) {
        const QString dir = fi.path();
        if (full.isEmpty() || dir.isEmpty() || dir == QStringLiteral(".")) {
            m_locationLabel->setText(tr("<a href=\"#\">(choose a location)</a>"));
            m_locationLabel->setToolTip(tr("Output folder — click to change"));
        } else {
            const QString shown = QDir::toNativeSeparators(dir);
            m_locationLabel->setText(QStringLiteral("<a href=\"#\">%1</a>")
                                         .arg(shown.toHtmlEscaped()));
            m_locationLabel->setToolTip(shown);
        }
    }
}

void ExportPanel::syncOutputPathFromParts()
{
    if (!m_outputPath || !m_fileNameEdit) return;
    const QString name = m_fileNameEdit->text().trimmed();
    if (name.isEmpty()) return;

    QString dir = QFileInfo(m_outputPath->text()).path();
    if (dir.isEmpty() || dir == QStringLiteral(".")) {
        QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
        dir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
    }
    const QString full = dir.isEmpty() ? name : (dir + QStringLiteral("/") + name);
    if (m_outputPath->text() != full) {
        m_outputPath->setText(full);
        syncPartsFromOutputPath();   // dir may have come from settings
    }
}

void ExportPanel::onBrowseOutput()
{
    // Use the CURRENT output path text as the starting point, so the dialog
    // pre-fills with what was already set. Falls back to the sequence name
    // (or project name) if the field is empty.
    QSettings settings(QStringLiteral("RoundtableMedia"), QStringLiteral("RoundtableNLE"));
    QString currentPath = m_outputPath->text().trimmed();

    // Audio-only export (VIDEO switch off) → suggest the matching audio
    // extension + filter.
    const bool audioOnly = !videoEnabled();
    const QString audioExt = (audioOnly && m_audioFormatCombo)
        ? QString::fromLatin1(audioCodecExtension(
              static_cast<AudioCodec>(m_audioFormatCombo->currentData().toInt())))
        : QString();

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
        if (audioOnly && QFileInfo(defaultName).suffix().isEmpty())
            defaultName += audioExt;

        QString lastDir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
        defaultPath = (lastDir.isEmpty() || !QDir(lastDir).exists())
                          ? defaultName
                          : lastDir + QStringLiteral("/") + defaultName;
    }

    QString filter = audioOnly
        ? tr("Audio Files (*.wav *.mp3 *.m4a *.flac);;All Files (*)")
        : tr("Video Files (*.mp4 *.mov *.mkv *.webm *.avi);;All Files (*)");
    QString path = QFileDialog::getSaveFileName(this, tr("Export Output"), defaultPath, filter);
    if (!path.isEmpty()) {
        m_outputPath->setText(path);
        syncPartsFromOutputPath();    // refresh File Name + Location header fields
        updateFileEstimate();
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
    // The Format dropdown holds video codecs (EncoderCodec) plus audio-only
    // formats (kAudioFormatBase + AudioCodec).  Only the video entries map to
    // an encoder codec; audio-only exports ignore encoderConfig entirely.
    const int formatData = m_codecCombo->currentData().toInt();
    if (!isAudioFormatData(formatData))
        cfg.encoderConfig.codec = static_cast<EncoderCodec>(formatData);
    // Codec profile (ProRes / DNxHR brand) from the profile combo, which
    // onCodecChanged populates for exactly those two codecs.
    if (m_profileCombo && m_profileCombo->currentData().isValid()) {
        if (cfg.encoderConfig.codec == EncoderCodec::ProRes)
            cfg.encoderConfig.proresProfile =
                static_cast<ProResProfile>(m_profileCombo->currentData().toInt());
        else if (cfg.encoderConfig.codec == EncoderCodec::DNxHR)
            cfg.encoderConfig.dnxhrProfile =
                static_cast<DNxHRProfile>(m_profileCombo->currentData().toInt());
    }
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
    {
        const double fps = m_fpsCombo->currentData().toDouble();
        if (std::abs(fps - std::round(fps)) < 0.005) {
            cfg.encoderConfig.fpsNum = static_cast<uint32_t>(std::lround(fps));
            cfg.encoderConfig.fpsDen = 1;
        } else { // NTSC fractional rate: 23.976 → 24000/1001, 29.97 → 30000/1001, …
            cfg.encoderConfig.fpsNum = static_cast<uint32_t>(std::lround(fps * 1.001) * 1000);
            cfg.encoderConfig.fpsDen = 1001;
        }
    }

    cfg.containerFormat = static_cast<uint8_t>(m_containerCombo->currentData().toInt());
    cfg.preset = static_cast<ExportPreset>(m_presetCombo->currentData().toInt());

    // The VIDEO / AUDIO enable switches drive what's exported:
    //   video off  → audio-only file (WAV/MP3/AAC/FLAC from the AUDIO section)
    //   video on   → normal video export; audio switch = include muxed audio
    const bool videoOn = videoEnabled();
    const bool audioOn = audioEnabled();
    cfg.includeAudio = audioOn;
    if (!videoOn) {
        cfg.audioOnly = true;
        cfg.includeAudio = true;
        cfg.audioConfig.codec = m_audioFormatCombo
            ? static_cast<AudioCodec>(m_audioFormatCombo->currentData().toInt())
            : AudioCodec::AAC;
        cfg.audioConfig.sampleRate = 48000;
        cfg.audioConfig.channels = 2;
        if (m_audioBitrateCombo && m_audioBitrateCombo->currentData().isValid())
            cfg.audioConfig.bitrate = m_audioBitrateCombo->currentData().toInt();
    }

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

    auto config = buildJobConfig();

    // Video-export pre-flight (skipped entirely for audio-only exports, which
    // need neither the compositor nor a hardware video encoder).
    if (!config.audioOnly) {
        if (!m_previewCallback) {
            QMessageBox::warning(this, tr("Export"), tr("No renderer available — cannot export."));
            return;
        }

        // Check for offline media and warn the user
        if (!checkOfflineMedia())
            return;

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
    }

    rememberExportDir(pathToUtf8(config.outputPath));
    // Pass the timeline so addJob (main thread) deep-clones it for the
    // audio mixdown — the worker then never reads the live timeline for
    // audio while the user keeps editing.
    uint32_t jobId = m_renderQueue->addJob(config, m_timeline);
    m_activeJobId = jobId;

    setJobRowState(jobId, JobRowState::Queued);

    armQueueAndRun();
    emit exportStarted(jobId);
}

void ExportPanel::onStartQueue()
{
    if (!m_renderQueue || m_renderQueue->isRunning()) return;
    if (m_renderQueue->pendingCount() == 0) return;
    if (!m_timeline) {
        QMessageBox::warning(this, tr("Export"),
                             tr("No timeline loaded \u2014 nothing to export."));
        return;
    }

    // Video jobs need the renderer + the offline-media check; audio-only
    // jobs need neither. (Per-job encode settings were captured at Add time.)
    bool anyVideo = false;
    for (const auto& j : m_renderQueue->jobs())
        if (j && j->status.load() == JobStatus::Queued && !j->config.audioOnly)
            anyVideo = true;
    if (anyVideo) {
        if (!m_previewCallback) {
            QMessageBox::warning(this, tr("Export"),
                                 tr("No renderer available \u2014 cannot export."));
            return;
        }
        if (!checkOfflineMedia())
            return;
    }

    // Follow the first job the worker will pick up.
    for (const auto& j : m_renderQueue->jobs()) {
        if (j && j->status.load() == JobStatus::Queued) {
            m_activeJobId = j->id;
            break;
        }
    }

    armQueueAndRun();
    emit exportStarted(m_activeJobId);
}

void ExportPanel::armQueueAndRun()
{
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
            // Snapshot the fallback + cancelled state on the worker thread
            // BEFORE queueing back to the UI — the job record may be reset
            // by the time the lambda runs.
            bool fellBack = false;
            std::string fellBackReason;
            bool cancelled = false;
            if (const auto j = m_renderQueue->job(id)) {
                fellBack = j->fellBackToCpuEncoder;
                fellBackReason = j->fellBackReason;
                cancelled = (j->status.load() == JobStatus::Cancelled);
            }
            QMetaObject::invokeMethod(this,
                [this, id, success, msg, fellBack, fellBackReason, cancelled]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                m_statusLabel->setText(success ? tr("Export complete!")
                    : cancelled ? tr("Cancelled")
                    : tr("Failed: %1").arg(QString::fromStdString(msg)));
                m_progressBar->setValue(success ? 100 : 0);
                // Queue row pill; Done rows also get the output file size.
                if (success) {
                    QString size;
                    if (const auto j2 = m_renderQueue->job(id)) {
                        QFileInfo fi(QString::fromStdString(
                            pathToUtf8(j2->config.outputPath)));
                        if (fi.exists()) {
                            const double mb = fi.size() / (1024.0 * 1024.0);
                            size = mb >= 1024.0
                                ? tr("%1 GB").arg(QString::number(mb / 1024.0, 'f', 1))
                                : tr("%1 MB").arg(QString::number(mb, 'f', 0));
                        }
                    }
                    setJobRowState(id, JobRowState::Done, -1, size);
                } else if (cancelled) {
                    // Distinct, neutral state \u2014 a cancel is not a failure.
                    setJobRowState(id, JobRowState::Cancelled);
                } else {
                    setJobRowState(id, JobRowState::Failed, -1,
                                   QString::fromStdString(msg));
                }
                // A Start Queue run renders several jobs in one worker pass \u2014
                // only drop out of the running state when nothing is left.
                // (The poll timer is the fallback finalizer for the race
                // where the worker clears its running flag after this fires.)
                const bool moreWork = m_renderQueue->pendingCount() > 0
                                   || m_renderQueue->isRunning();
                if (!moreWork) {
                    m_pollTimer->stop();
                    setRunningUiState(false);
                } else {
                    // Follow the next job the worker picks up.
                    for (const auto& j2 : m_renderQueue->jobs()) {
                        if (!j2) continue;
                        const auto st = j2->status.load();
                        if (st == JobStatus::Running || st == JobStatus::Queued) {
                            m_activeJobId = j2->id;
                            break;
                        }
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
    // §4.6 export write-through: store each finished full-res frame into the
    // segment cache so a re-export reuses it (called on the worker thread with
    // pixels already present).
    if (m_frameStoreCallback)
        m_renderQueue->setFrameStoreCallback(m_frameStoreCallback);

    // Reset the composite pipeline between exports: the slots still hold the
    // previous export's last tick + shared_future, which broke first-call
    // detection on the second export (frame 0 reused the previous export's
    // last composited frame).  Only touch the slots when no worker is
    // running — while a worker drains the queue they belong to its thread
    // (pipelineComposite's tick-match check covers that case).
    if (!m_renderQueue->isRunning()) {
        m_pipelineSlots[0] = CompositeSlot{};
        m_pipelineSlots[1] = CompositeSlot{};
        m_pipelineCurrentSlot = 0;
    }

    // Start rendering
    m_renderQueue->start(m_timeline, m_compositor);

    setRunningUiState(true);
    m_jobList->setVisible(true);
    m_pollTimer->start();
}

void ExportPanel::setRunningUiState(bool running)
{
    if (m_startButton)    m_startButton->setEnabled(!running);
    if (m_addQueueButton) m_addQueueButton->setEnabled(!running);
    if (m_cancelButton)   m_cancelButton->setEnabled(running);
    if (m_runRow)         m_runRow->setVisible(running);
    if (m_renderPip && !running) m_renderPip->setVisible(false);
    updateStartQueueEnabled();
}

void ExportPanel::onCancelExport()
{
    // Cancel everything; the per-job Cancelled callbacks (and the poll
    // timer's queue-idle check) take the UI out of the running state once
    // the worker actually stops — an instant reset here would lie while
    // the current frame batch drains.
    m_renderQueue->cancelAll();
    m_statusLabel->setText(tr("Cancelling…"));
    m_cancelButton->setEnabled(false);
}

void ExportPanel::onPollProgress()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_renderQueue->isRunning()) {
        // Queue went idle \u2014 the last job's completion callback may have
        // raced the worker's running-flag clear; finalize from here.
        m_pollTimer->stop();
        setRunningUiState(false);
        return;
    }

    auto j = m_renderQueue->job(m_activeJobId);
    // A queue run advances to the next job inside one worker pass \u2014 follow
    // whichever job is actually Running.
    if (!j || j->status.load() != JobStatus::Running) {
        for (const auto& cand : m_renderQueue->jobs()) {
            if (cand && cand->status.load() == JobStatus::Running) {
                m_activeJobId = cand->id;
                j = cand;
                break;
            }
        }
    }
    if (!j) return;

    int pct = static_cast<int>(j->progress.percent.load());
    m_progressBar->setValue(pct);
    setJobRowState(m_activeJobId, JobRowState::Running, pct);
    // Build a rich status string: "Rendering â€” 245/800 frames Â· 14.2 fps Â· ETA 0:39"
    int64_t curFrame   = j->progress.currentFrame.load();
    int64_t totalFrame = j->progress.totalFrames.load();
    double  elapsed    = j->progress.elapsedSeconds.load();
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
        // statusText is guarded by the queue's mutex — use the locked accessor.
        status = QString::fromStdString(m_renderQueue->jobStatusText(m_activeJobId));
    }
    m_statusLabel->setText(status);

    // "RENDERING FRAME n / m" pip over the preview (video jobs only).
    if (m_renderPip) {
        if (!j->config.audioOnly && pct < 100) {
            m_renderPip->setText(tr("RENDERING FRAME %1 / %2")
                                     .arg(curFrame).arg(totalFrame));
            m_renderPip->setVisible(true);
        } else {
            m_renderPip->setVisible(false);
        }
    }

    // Update preview with the current frame being rendered (video only —
    // an audio-only export has no frames to composite).
    if (!j->config.audioOnly && m_previewCallback && m_timeline && m_previewImageLabel) {
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

    emit exportProgress(m_activeJobId, j->progress.percent.load());
}
} // namespace rt
