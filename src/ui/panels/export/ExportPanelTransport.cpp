// ExportPanelTransport.cpp
// Preview transport: play/step/skip, in-out points, queue, file estimate.
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

void ExportPanel::onRangeChanged(int /*index*/)
{
    refreshPreview();
}

void ExportPanel::onInOutChanged()
{
    // Lightweight sync: update mini timeline markers and range combo
    // whenever in/out points change from any panel (main timeline, etc.)
    // without re-rendering the full preview (that's expensive).
    if (!m_miniTimeline || !m_timeline) return;

    bool hasEitherInOut = (m_timeline->inPoint() >= 0 ||
                           m_timeline->outPoint() > 0);

    // Auto-switch range combo when any point is set or all cleared
    if (hasEitherInOut && m_rangeCombo && m_rangeCombo->currentIndex() == 0) {
        m_rangeCombo->setCurrentIndex(1); // "In to Out"
    } else if (!hasEitherInOut && m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
        m_rangeCombo->setCurrentIndex(0); // "Entire Sequence"
    }

    // Update mini timeline markers
    if (hasEitherInOut) {
        m_miniTimeline->setInOutRange(m_timeline->inPoint(),
                                       m_timeline->outPoint());
    } else {
        m_miniTimeline->setInOutRange(-1, -1);
    }
}

// â”€â”€ Transport control slots â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

/// Helper: render one preview frame at the given tick and display it.
/// \p halfRes â€” if true, renders at Â½ resolution for faster playback preview.
static void renderPreviewFrame(ExportPanel* /*self*/,
                               QLabel* label,
                               const ExportPanel::PreviewCallback& cb,
                               QSpinBox* wSpin, QSpinBox* hSpin,
                               int64_t tick,
                               bool scrub = true)
{
    if (!cb || !label) return;

    uint32_t fullW = wSpin  ? static_cast<uint32_t>(wSpin->value())  : 1920;
    uint32_t fullH = hSpin ? static_cast<uint32_t>(hSpin->value()) : 1080;

    auto frame = cb(tick, fullW, fullH, scrub);
    if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
        uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
        QImage img(frame->pixels.data(), static_cast<int>(frame->width),
                   static_cast<int>(frame->height), static_cast<int>(stride),
                   QImage::Format_ARGB32);
        QPixmap pix = QPixmap::fromImage(img).scaled(
            label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(pix);
    }
}

void ExportPanel::onPlayPause()
{
    if (!m_miniTimeline || !m_timeline) return;

    if (m_playing) {
        // â”€â”€ Pause â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        m_playing = false;
        m_playbackTimer->stop();
        m_playPauseBtn->setText(QStringLiteral("\u25B6")); // â–¶

        if (m_playbackController) {
            m_playbackController->pause();

            // Restore the main timeline's playhead to where it was before
            // export preview playback started.
            if (m_savedMainPlayhead >= 0) {
                m_playbackController->seekTo(m_savedMainPlayhead);
                m_savedMainPlayhead = -1;
            }
        }

        // Render one high-quality frame at current position
        renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                           m_widthSpin, m_heightSpin,
                           m_miniTimeline->playhead());

        // Wake the Program Monitor's pipeline back up — it suspended
        // itself on Play to avoid double-composite contention.
        emit previewPlaybackToggled(false);
    } else {
        // -- Play ---------------------------------------------------------
        m_playing = true;
        m_playPauseBtn->setText(QStringLiteral("\u23F8")); // ⏸

        // Save the main timeline's current playhead so we can restore it
        // when export playback stops.
        if (m_playbackController)
            m_savedMainPlayhead = m_playbackController->pollPosition();

        // Suspend the Program Monitor's playback pipeline BEFORE we
        // call m_playbackController->play().  The shared controller
        // drives the main FrameClock too; without suspension the
        // FrameProducer thread would race the UI-thread renderPreview
        // path on m_compositeMutex, doubling effective work and
        // producing the choppy-video / smooth-audio symptom users see
        // on 60 fps ProRes preview playback.
        emit previewPlaybackToggled(true);

        int fps = m_fpsCombo ? m_fpsCombo->currentData().toInt() : 30;
        if (fps <= 0) fps = 30;

        // Render the FIRST frame immediately so the user sees video
        // without waiting for the timer to fire (~33ms delay).
        // scrub=true so the compositor's settle-window hold is BYPASSED
        // during real-time preview playback — otherwise every tick where
        // a Full-tier decode isn't ready (typical at 30+fps in export
        // preview) returns the prior m_lastGoodComposite and the preview
        // appears frozen.  Accepting partial composites for one or two
        // frames during a cold decoder warm-up is far better than the
        // visible freeze the settle window produces here.
        renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                           m_widthSpin, m_heightSpin,
                           m_miniTimeline->playhead(),
                           /*scrub=*/true);

        // Seek PlaybackController to mini-timeline position and play (audio)
        if (m_playbackController) {
            m_playbackController->setFrameRate(static_cast<double>(fps));
            m_playbackController->seekTo(m_miniTimeline->playhead());
            m_playbackController->play();
        }

        // Poll timer for visual preview updates (half the framerate to
        // avoid overwhelming the compositor — audio is exact via PortAudio)
        int intervalMs = std::max(1000 / fps, 16);
        m_playbackTimer->start(intervalMs);
    }
}

void ExportPanel::onPlaybackTick()
{
    if (!m_miniTimeline || !m_timeline) {
        onPlayPause(); // Stop
        return;
    }

    // Use the mini-timeline's own playhead for independent frame stepping.
    // This keeps the export preview's position separate from the main
    // timeline (which we save/restore in onPlayPause).
    int fps = m_fpsCombo ? m_fpsCombo->currentData().toInt() : 30;
    if (fps <= 0) fps = 30;
    int64_t ticksPerFrame = 48000 / fps;
    int64_t tick = m_miniTimeline->playhead() + ticksPerFrame;

    // Determine the end-of-range
    int64_t endTick = m_timeline->duration();
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
        int64_t outPt = m_timeline->outPoint();
        if (outPt > 0) endTick = outPt;
    }

    if (tick >= endTick) {
        // Reached the end â€” stop playback
        m_miniTimeline->setPlayhead(endTick);
        renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                           m_widthSpin, m_heightSpin, endTick);
        onPlayPause();
        return;
    }

    m_miniTimeline->setPlayhead(tick);

    // Render the preview at full output resolution.  scrub=true bypasses
    // the compositor's settle-window hold so the preview actually advances
    // each tick.  With scrub=false (the prior setting), every tick where a
    // Full-tier decode wasn't ready re-returned m_lastGoodComposite — at
    // 30 fps real-time playback that is virtually every tick, so the
    // preview window froze on the first frame while the timeline's audio
    // played on.  forceFullResolution=true is preserved by the preview
    // callback, so charVideoTier is still Full (the scrub-time Half
    // override only fires when forceFullResolution is false).
    static int s_tickCount = 0;
    if (++s_tickCount <= 5 || s_tickCount % 30 == 0) {
        spdlog::info("[EXPORT-PLAYBACK] tick={} endTick={} fps={}",
                     tick, endTick, fps);
    }
    renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                       m_widthSpin, m_heightSpin, tick, /*scrub=*/true);
}

void ExportPanel::onStepForward()
{
    if (!m_miniTimeline || !m_timeline) return;

    // Stop playback if playing
    if (m_playing) onPlayPause();

    int fps = m_fpsCombo ? m_fpsCombo->currentData().toInt() : 30;
    if (fps <= 0) fps = 30;
    int64_t ticksPerFrame = 48000 / fps;

    int64_t next = m_miniTimeline->playhead() + ticksPerFrame;
    int64_t maxTick = m_timeline->duration();
    if (next > maxTick) next = maxTick;

    m_miniTimeline->setPlayhead(next);
    renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                       m_widthSpin, m_heightSpin, next);
}

void ExportPanel::onStepBack()
{
    if (!m_miniTimeline || !m_timeline) return;

    if (m_playing) onPlayPause();

    int fps = m_fpsCombo ? m_fpsCombo->currentData().toInt() : 30;
    if (fps <= 0) fps = 30;
    int64_t ticksPerFrame = 48000 / fps;

    int64_t prev = m_miniTimeline->playhead() - ticksPerFrame;
    if (prev < 0) prev = 0;

    m_miniTimeline->setPlayhead(prev);
    renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                       m_widthSpin, m_heightSpin, prev);
}

void ExportPanel::onSkipToStart()
{
    if (!m_miniTimeline || !m_timeline) return;

    if (m_playing) onPlayPause();

    int64_t startTick = 0;
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
        int64_t inPt = m_timeline->inPoint();
        if (inPt >= 0) startTick = inPt;
    }

    m_miniTimeline->setPlayhead(startTick);
    renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                       m_widthSpin, m_heightSpin, startTick);
}

void ExportPanel::onSkipToEnd()
{
    if (!m_miniTimeline || !m_timeline) return;

    if (m_playing) onPlayPause();

    int64_t endTick = m_timeline->duration();
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
        int64_t outPt = m_timeline->outPoint();
        if (outPt > 0) endTick = outPt;
    }

    m_miniTimeline->setPlayhead(endTick);
    renderPreviewFrame(this, m_previewImageLabel, m_previewCallback,
                       m_widthSpin, m_heightSpin, endTick);
}

// â”€â”€ File size estimation â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void ExportPanel::updateFileEstimate()
{
    if (!m_estimateLabel || !m_timeline) return;

    // Calculate duration in seconds
    double durSec = 0.0;
    if (m_rangeCombo && m_rangeCombo->currentIndex() == 1) {
        int64_t inPt  = m_timeline->inPoint();
        int64_t outPt = m_timeline->outPoint();
        if (inPt >= 0 && outPt > inPt)
            durSec = ticksToSeconds(outPt - inPt);
        else
            durSec = ticksToSeconds(m_timeline->duration());
    } else {
        durSec = ticksToSeconds(m_timeline->duration());
    }

    if (durSec <= 0) {
        m_estimateLabel->setText(QString());
        return;
    }

    auto humanSize = [](double bytes) -> QString {
        if (bytes >= 1024.0 * 1024.0 * 1024.0)
            return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
        if (bytes >= 1024.0 * 1024.0)
            return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
        return QString::number(bytes / 1024.0, 'f', 0) + " KB";
    };

    // Audio-only export (VIDEO switch off): estimate from the audio config,
    // not a video bitrate.
    if (!videoEnabled()) {
        AudioMixdownConfig ac;
        ac.codec = m_audioFormatCombo
            ? static_cast<AudioCodec>(m_audioFormatCombo->currentData().toInt())
            : AudioCodec::AAC;
        ac.sampleRate = 48000;
        ac.channels = 2;
        if (m_audioBitrateCombo && m_audioBitrateCombo->currentData().isValid())
            ac.bitrate = m_audioBitrateCombo->currentData().toInt();
        const double sizeBytes = static_cast<double>(
            AudioMixdown::estimateFileSize(ac, durSec));
        m_estimateLabel->setText(tr("Est. %1 — %2s (audio only)")
            .arg(humanSize(sizeBytes))
            .arg(QString::number(durSec, 'f', 1)));
        return;
    }

    int w = m_widthSpin->value();
    int h = m_heightSpin->value();
    int fps = m_fpsCombo->currentData().toInt();
    if (fps <= 0) fps = 30;
    auto codec = static_cast<EncoderCodec>(m_codecCombo->currentData().toInt());

    // Estimate bitrate in kbps based on codec + quality + resolution
    double pixelsPerFrame = static_cast<double>(w) * h;
    double qualityFactor = 0.5 + (m_crfSlider->value() / 100.0) * 1.5; // 0.5..2.0

    double bitrateKbps = 0;
    switch (codec) {
    case EncoderCodec::H264:
        // H.264: ~5-15 Mbps for 1080p at high quality
        bitrateKbps = (pixelsPerFrame / (1920.0 * 1080.0)) * 8000 * qualityFactor;
        break;
    case EncoderCodec::H265:
        // H.265 is ~40% more efficient than H.264
        bitrateKbps = (pixelsPerFrame / (1920.0 * 1080.0)) * 5000 * qualityFactor;
        break;
    case EncoderCodec::AV1:
        // AV1 is ~30% more efficient than H.265
        bitrateKbps = (pixelsPerFrame / (1920.0 * 1080.0)) * 3500 * qualityFactor;
        break;
    case EncoderCodec::ProRes:
        // ProRes 422 HQ: ~220 Mbps for 1080p30
        bitrateKbps = (pixelsPerFrame / (1920.0 * 1080.0)) * (fps / 30.0) * 220000;
        break;
    case EncoderCodec::DNxHR:
        // DNxHR HQ: ~220 Mbps for 1080p30
        bitrateKbps = (pixelsPerFrame / (1920.0 * 1080.0)) * (fps / 30.0) * 220000;
        break;
    case EncoderCodec::ImageSequence:
        // PNG: ~30 MB/frame for 1080p (lossless, varies widely)
        bitrateKbps = (pixelsPerFrame * 4 * 8) / 1000.0 * 0.3; // ~30% of raw
        break;
    default:
        bitrateKbps = 8000 * qualityFactor;
        break;
    }

    // Calculate estimated file size
    double sizeBytes = (bitrateKbps * 1000.0 / 8.0) * durSec;
    // Add ~10% for audio if included
    if (audioEnabled())
        sizeBytes *= 1.10;

    // Format human-readable
    QString sizeStr = humanSize(sizeBytes);

    int64_t totalFrames = static_cast<int64_t>(durSec * fps);
    m_estimateLabel->setText(
        tr("Est. %1 - %2 frames - %3s")
            .arg(sizeStr)
            .arg(totalFrames)
            .arg(QString::number(durSec, 'f', 1)));
}

// â”€â”€ Add to Queue â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

void ExportPanel::onSetInPoint()
{
    if (m_timeline && m_miniTimeline) {
        const int64_t playhead = m_miniTimeline->playhead();
        applyInOutPointEdit("Set in point",
                            playhead, m_timeline->outPoint(), 1);
        spdlog::info("ExportPanel: In point set at tick={} (button)", playhead);
    }
}

void ExportPanel::onSetOutPoint()
{
    if (m_timeline && m_miniTimeline) {
        const int64_t playhead = m_miniTimeline->playhead();
        applyInOutPointEdit("Set out point",
                            m_timeline->inPoint(), playhead, 1);
        spdlog::info("ExportPanel: Out point set at tick={} (button)", playhead);
    }
}

void ExportPanel::onClearInOut()
{
    if (m_timeline) {
        applyInOutPointEdit("Clear in/out points", -1, -1, 0);
        spdlog::info("ExportPanel: In/Out points cleared (button)");
    }
}

void ExportPanel::onAddToQueue()
{
    if (m_outputPath->text().isEmpty()) {
        QMessageBox::warning(this, tr("Export"), tr("Please select an output file first."));
        return;
    }

    auto config = buildJobConfig();
    rememberExportDir(pathToUtf8(config.outputPath));
    // Snapshot the timeline (main thread) so the worker's audio mixdown
    // never reads the live timeline while the user keeps editing.
    uint32_t jobId = m_renderQueue->addJob(config, m_timeline);

    // Show job in list; "Start Queue" arms now that a job is pending.
    setJobRowState(jobId, JobRowState::Queued);
    updateStartQueueEnabled();

    m_statusLabel->setText(tr("Job %1 added to queue").arg(jobId));
    spdlog::info("ExportPanel: added job {} to queue: {}", jobId, pathToUtf8(config.outputPath));
}
} // namespace rt
