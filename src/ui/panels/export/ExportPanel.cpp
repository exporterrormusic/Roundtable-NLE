/*
 * ExportPanel.cpp â€” Export settings and render queue UI implementation.
 */

#include "ExportPanel.h"
#include "ExportMiniTimeline.h"

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

#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/PlaybackController.h"
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
    m_destroying.store(true);

    // Stop the render queue and join the worker thread so the process
    // can exit cleanly when the main window is closed during an export.
    m_renderQueue->cancelAll();
    m_renderQueue->waitForAll();

    // Unregister from timeline observer
    if (m_timeline)
        m_timeline->removeObserver(this);
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

    // Default the output path to the sequence name (only if the field is
    // still empty / showing the placeholder — don't overwrite a user-set path).
    if (m_outputPath && m_outputPath->text().isEmpty() && timeline) {
        QString seqName = QString::fromStdString(timeline->name());
        if (!seqName.isEmpty())
            m_outputPath->setPlaceholderText(
                tr("e.g. %1.mp4").arg(seqName));
    }

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
                    QString defaultPath = QString::fromStdString((projDir / (seqName + ".mp4").toStdString()).string());
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

std::shared_ptr<CachedFrame> ExportPanel::pipelineComposite(
    int64_t tick, int64_t nextTick,
    uint32_t w, uint32_t h, bool scrub)
{
    // ═══════════════════════════════════════════════════════════════════
    // PIPELINE using nextTick from RenderQueue:
    //
    //   Call 0 (first):  submit frame 0 (QueuedConn) → WAIT → return 0
    //                     submit frame 1 (QueuedConn) → store for next call
    //
    //   Call N (N>0):    wait for stored frame N (from prev Phase C) → return N
    //                     submit frame N+1 (QueuedConn) → store for next call
    //
    // Main thread processes frame N's event concurrently with the worker
    // encoding frame N, because frame N was already queued during the
    // PREVIOUS call's Phase C (right before returning).
    // ═══════════════════════════════════════════════════════════════════

    auto trySubmit = [&](int64_t targetTick, int slotIdx) {
        auto promise = std::make_shared<std::promise<std::shared_ptr<CachedFrame>>>();
        auto sf = promise->get_future().share();
        m_pipelineSlots[slotIdx].tick = targetTick;
        m_pipelineSlots[slotIdx].future = sf;
        if (m_previewCallback) {
            auto cb = m_previewCallback;
            QMetaObject::invokeMethod(this,
                [promise, cb, targetTick, w, h, scrub]() {
                    // /EHa is set on this TU (see ui/CMakeLists.txt) so
                    // catch(...) covers SEH access violations and the
                    // 0x000006BA hook-DLL exception that fires while the
                    // Vulkan ICD is mid-TDR.  Without this catch, an SEH
                    // raised on the main thread during the composite
                    // would (a) kill the process and (b) leave the
                    // worker thread blocked forever on promise.get_future().
                    // On failure we set the promise to nullptr; the
                    // worker treats that as "skip this frame".
                    try {
                        auto frame = cb(targetTick, w, h, scrub);
                        if (frame) frame->ensurePixels();
                        promise->set_value(std::move(frame));
                    } catch (...) {
                        spdlog::error("ExportPanel: SEH/exception during main-thread "
                                      "composite at tick={} — skipping frame", targetTick);
                        try { promise->set_value(nullptr); } catch (...) {}
                    }
                },
                Qt::QueuedConnection);
        } else {
            promise->set_value(nullptr);
        }
        return sf;
    };

    // ── Phase A: Wait for previously stored result ───────────────────────
    int cur = m_pipelineCurrentSlot;
    int prev = (cur + 1) % 2;
    bool firstCall = (m_pipelineSlots[prev].tick < 0);
    std::shared_ptr<CachedFrame> result;

    if (!firstCall && m_pipelineSlots[prev].future.valid()) {
        try {
            result = m_pipelineSlots[prev].future.get();
        } catch (const std::exception& e) {
            spdlog::error("ExportPanel: pipeline wait exception: {}", e.what());
        }
    }

    // ── Phase B: Submit this frame (first call only) / Submit next frame ─
    if (firstCall) {
        // First call: submit frame 0 and wait for it.
        auto sf0 = trySubmit(tick, cur);
        m_pipelineCurrentSlot = (cur + 1) % 2;
        result = sf0.get();

        // Also submit frame 1 for the next call.
        if (nextTick >= 0) {
            trySubmit(nextTick, m_pipelineCurrentSlot);
            m_pipelineCurrentSlot = (m_pipelineCurrentSlot + 1) % 2;
        }
    } else {
        // Subsequent calls: pre-submit next frame (overlap with encode).
        if (nextTick >= 0) {
            trySubmit(nextTick, cur);
            m_pipelineCurrentSlot = (cur + 1) % 2;
        } else {
            // Last frame: no next, leave slot as-is.
            m_pipelineCurrentSlot = cur;
        }
    }

    if (!result || result->pixels.empty()) {
        spdlog::warn("ExportPanel: pipeline empty pixels at tick={}", tick);
    }
    return result;
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

void ExportPanel::populatePresets()
{
    m_presetCombo->addItem(tr("Custom"), static_cast<int>(ExportPreset::Custom));
    m_presetCombo->addItem(tr("YouTube 1080p 30fps"), static_cast<int>(ExportPreset::YouTube1080p30));
    m_presetCombo->addItem(tr("YouTube 1080p 60fps"), static_cast<int>(ExportPreset::YouTube1080p60));
    m_presetCombo->addItem(tr("YouTube 4K 30fps"), static_cast<int>(ExportPreset::YouTube4K30));
    m_presetCombo->addItem(tr("YouTube 4K 60fps"), static_cast<int>(ExportPreset::YouTube4K60));
    m_presetCombo->addItem(tr("Broadcast 1080i"), static_cast<int>(ExportPreset::Broadcast1080i));
    m_presetCombo->addItem(tr("Archive ProRes HQ"), static_cast<int>(ExportPreset::ArchiveProRes));
    m_presetCombo->addItem(tr("Web Optimized"), static_cast<int>(ExportPreset::WebOptimized));
    loadCustomPresets();
}

QString ExportPanel::customPresetsDir() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/assets/presets/export");
}

void ExportPanel::loadCustomPresets()
{
    QDir dir(customPresetsDir());
    if (!dir.exists()) return;
    const auto files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& fi : files) {
        // User presets get data value = -1 (not a built-in ExportPreset enum)
        m_presetCombo->addItem(fi.baseName(), -1);
    }
}

void ExportPanel::onSavePreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Save Export Preset"),
                                         tr("Preset name:"), QLineEdit::Normal,
                                         QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed();

    QDir dir(customPresetsDir());
    if (!dir.exists()) dir.mkpath(QStringLiteral("."));

    QJsonObject obj;
    obj[QStringLiteral("width")]     = m_widthSpin->value();
    obj[QStringLiteral("height")]    = m_heightSpin->value();
    obj[QStringLiteral("fps")]       = m_fpsCombo->currentData().toInt();
    obj[QStringLiteral("codec")]     = m_codecCombo->currentData().toInt();
    obj[QStringLiteral("accel")]     = m_accelCombo->currentData().toInt();
    obj[QStringLiteral("crf")]       = m_crfSlider->value();
    obj[QStringLiteral("container")] = m_containerCombo->currentData().toInt();
    obj[QStringLiteral("audio")]     = m_audioCheck->isChecked();

    QString path = dir.filePath(name + QStringLiteral(".json"));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.close();
    }

    // Refresh combo: check if already there
    bool found = false;
    for (int i = 0; i < m_presetCombo->count(); ++i) {
        if (m_presetCombo->itemText(i) == name) { found = true; break; }
    }
    if (!found)
        m_presetCombo->addItem(name, -1);

    // Select the saved preset
    for (int i = 0; i < m_presetCombo->count(); ++i) {
        if (m_presetCombo->itemText(i) == name) {
            m_presetCombo->setCurrentIndex(i);
            break;
        }
    }
}

void ExportPanel::onDeletePreset()
{
    int idx = m_presetCombo->currentIndex();
    if (idx < 0) return;
    int data = m_presetCombo->currentData().toInt();
    if (data != -1) return; // can only delete user presets

    QString name = m_presetCombo->currentText();
    auto answer = QMessageBox::question(this, tr("Delete Preset"),
                                        tr("Delete preset \"%1\"?").arg(name));
    if (answer != QMessageBox::Yes) return;

    QDir dir(customPresetsDir());
    dir.remove(name + QStringLiteral(".json"));
    m_presetCombo->removeItem(idx);
}

void ExportPanel::populateCodecs()
{
    m_codecCombo->addItem(QStringLiteral("H.264"), static_cast<int>(EncoderCodec::H264));
    m_codecCombo->addItem(QStringLiteral("H.265 (HEVC)"), static_cast<int>(EncoderCodec::H265));
    m_codecCombo->addItem(QStringLiteral("AV1"), static_cast<int>(EncoderCodec::AV1));
    m_codecCombo->addItem(QStringLiteral("ProRes"), static_cast<int>(EncoderCodec::ProRes));
    m_codecCombo->addItem(QStringLiteral("DNxHR"), static_cast<int>(EncoderCodec::DNxHR));
    m_codecCombo->addItem(QStringLiteral("Image Sequence"), static_cast<int>(EncoderCodec::ImageSequence));
}

void ExportPanel::populateContainers()
{
    m_containerCombo->addItem(QStringLiteral("MP4"), static_cast<int>(ContainerFormat::MP4));
    m_containerCombo->addItem(QStringLiteral("MOV"), static_cast<int>(ContainerFormat::MOV));
    m_containerCombo->addItem(QStringLiteral("MKV"), static_cast<int>(ContainerFormat::MKV));
    m_containerCombo->addItem(QStringLiteral("WebM"), static_cast<int>(ContainerFormat::WebM));
    m_containerCombo->addItem(QStringLiteral("AVI"), static_cast<int>(ContainerFormat::AVI));
}

void ExportPanel::populateAccel()
{
    m_accelCombo->addItem(QStringLiteral("Auto"), static_cast<int>(HardwareAccel::NVENC));
    m_accelCombo->addItem(QStringLiteral("CPU Only"), static_cast<int>(HardwareAccel::None));
    m_accelCombo->addItem(QStringLiteral("NVENC"), static_cast<int>(HardwareAccel::NVENC));
    m_accelCombo->addItem(QStringLiteral("Quick Sync"), static_cast<int>(HardwareAccel::QSV));
    m_accelCombo->addItem(QStringLiteral("AMF"), static_cast<int>(HardwareAccel::AMF));
}

void ExportPanel::onPresetChanged(int index)
{
    int data = m_presetCombo->itemData(index).toInt();
    bool isUserPreset = (data == -1);
    if (m_deletePresetBtn) m_deletePresetBtn->setEnabled(isUserPreset);

    if (isUserPreset) {
        // Load user preset from JSON
        QString name = m_presetCombo->itemText(index);
        QFile f(customPresetsDir() + QStringLiteral("/") + name + QStringLiteral(".json"));
        if (f.open(QIODevice::ReadOnly)) {
            QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            m_widthSpin->setValue(obj[QStringLiteral("width")].toInt(1920));
            m_heightSpin->setValue(obj[QStringLiteral("height")].toInt(1080));
            m_crfSlider->setValue(obj[QStringLiteral("crf")].toInt(50));
            m_audioCheck->setChecked(obj[QStringLiteral("audio")].toBool(true));
            int fps = obj[QStringLiteral("fps")].toInt(30);
            for (int i = 0; i < m_fpsCombo->count(); ++i) {
                if (m_fpsCombo->itemData(i).toInt() == fps) { m_fpsCombo->setCurrentIndex(i); break; }
            }
            int codec = obj[QStringLiteral("codec")].toInt();
            for (int i = 0; i < m_codecCombo->count(); ++i) {
                if (m_codecCombo->itemData(i).toInt() == codec) { m_codecCombo->setCurrentIndex(i); break; }
            }
            int accel = obj[QStringLiteral("accel")].toInt();
            for (int i = 0; i < m_accelCombo->count(); ++i) {
                if (m_accelCombo->itemData(i).toInt() == accel) { m_accelCombo->setCurrentIndex(i); break; }
            }
            int container = obj[QStringLiteral("container")].toInt();
            for (int i = 0; i < m_containerCombo->count(); ++i) {
                if (m_containerCombo->itemData(i).toInt() == container) { m_containerCombo->setCurrentIndex(i); break; }
            }
        }
    } else {
        auto preset = static_cast<ExportPreset>(data);
        if (preset != ExportPreset::Custom)
            updateUIFromPreset(preset);
    }
}

void ExportPanel::updateUIFromPreset(ExportPreset preset)
{
    ExportJobConfig cfg;
    cfg.applyPreset(preset);

    m_widthSpin->setValue(cfg.outputWidth);
    m_heightSpin->setValue(cfg.outputHeight);
    // Map CRF back to quality slider (0-100)
    {
        int crf = cfg.encoderConfig.crf;
        int q = ((35 - crf) * 100) / 21;
        q = std::clamp(q, 0, 100);
        m_crfSlider->setValue(q);
    }

    // Set codec combo
    for (int i = 0; i < m_codecCombo->count(); ++i) {
        if (m_codecCombo->itemData(i).toInt() == static_cast<int>(cfg.encoderConfig.codec)) {
            m_codecCombo->setCurrentIndex(i);
            break;
        }
    }

    // Set FPS combo
    int fpsVal = static_cast<int>(cfg.encoderConfig.fpsNum / std::max(cfg.encoderConfig.fpsDen, 1));
    for (int i = 0; i < m_fpsCombo->count(); ++i) {
        if (m_fpsCombo->itemData(i).toInt() == fpsVal) {
            m_fpsCombo->setCurrentIndex(i);
            break;
        }
    }

    // Set container combo
    for (int i = 0; i < m_containerCombo->count(); ++i) {
        if (m_containerCombo->itemData(i).toInt() == cfg.containerFormat) {
            m_containerCombo->setCurrentIndex(i);
            break;
        }
    }
}

void ExportPanel::onCodecChanged(int /*index*/)
{
    auto codec = static_cast<EncoderCodec>(m_codecCombo->currentData().toInt());

    // ProRes, DNxHR and Image Sequence don't use CRF â€” disable the quality slider
    bool usesCrf = (codec != EncoderCodec::ProRes &&
                    codec != EncoderCodec::DNxHR &&
                    codec != EncoderCodec::ImageSequence);
    m_crfSlider->setEnabled(usesCrf);
    m_crfLabel->setEnabled(usesCrf);
    if (!usesCrf) {
        m_crfLabel->setText((codec == EncoderCodec::ProRes || codec == EncoderCodec::DNxHR)
            ? tr("N/A") : tr("N/A"));
    } else {
        onCrfChanged(m_crfSlider->value()); // Refresh label text
    }

    // Auto-switch container to match codec
    if (codec == EncoderCodec::ProRes || codec == EncoderCodec::DNxHR) {
        // ProRes/DNxHR â†’ MOV
        for (int i = 0; i < m_containerCombo->count(); ++i) {
            if (m_containerCombo->itemData(i).toInt() == static_cast<int>(ContainerFormat::MOV)) {
                m_containerCombo->setCurrentIndex(i);
                break;
            }
        }
    } else if (codec == EncoderCodec::AV1) {
        // AV1 â†’ WebM (modern) or MKV
        for (int i = 0; i < m_containerCombo->count(); ++i) {
            if (m_containerCombo->itemData(i).toInt() == static_cast<int>(ContainerFormat::WebM)) {
                m_containerCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    // Hardware accel only works with H.264/H.265
    bool hwAvailable = (codec == EncoderCodec::H264 || codec == EncoderCodec::H265);
    m_accelCombo->setEnabled(hwAvailable);
    if (!hwAvailable) {
        // Set to CPU only
        for (int i = 0; i < m_accelCombo->count(); ++i) {
            if (m_accelCombo->itemData(i).toInt() == static_cast<int>(HardwareAccel::None)) {
                m_accelCombo->setCurrentIndex(i);
                break;
            }
        }
    }

    updateFileEstimate();
}

void ExportPanel::onCrfChanged(int value)
{
    // Map 0-100 slider to friendly label
    QString label;
    if      (value >= 88) label = tr("Best");
    else if (value >= 63) label = tr("High");
    else if (value >= 38) label = tr("Medium");
    else if (value >= 13) label = tr("Low");
    else                  label = tr("Lowest");
    m_crfLabel->setText(label);
}

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

    rememberExportDir(config.outputPath.string());
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
    if (m_audioCheck && m_audioCheck->isChecked())
        sizeBytes *= 1.10;

    // Format human-readable
    QString sizeStr;
    if (sizeBytes >= 1024.0 * 1024.0 * 1024.0)
        sizeStr = QString::number(sizeBytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
    else if (sizeBytes >= 1024.0 * 1024.0)
        sizeStr = QString::number(sizeBytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    else
        sizeStr = QString::number(sizeBytes / 1024.0, 'f', 0) + " KB";

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
    rememberExportDir(config.outputPath.string());
    uint32_t jobId = m_renderQueue->addJob(config);

    // Show job in list
    m_jobList->setVisible(true);
    auto* item = new QListWidgetItem(
        QStringLiteral("\u25CB Job %1 \u2014 %2 \u2014 Queued")
            .arg(jobId)
            .arg(QString::fromStdString(config.outputPath.string())));
    item->setData(Qt::UserRole, static_cast<qulonglong>(jobId));
    m_jobList->addItem(item);

    m_statusLabel->setText(tr("Job %1 added to queue").arg(jobId));
    spdlog::info("ExportPanel: added job {} to queue: {}", jobId, config.outputPath.string());
}

} // namespace rt
