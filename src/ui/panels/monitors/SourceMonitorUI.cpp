/*
 * SourceMonitorUI.cpp -- WaveformDisplayWidget, constructor, destructor, setupUI.
 *
 * Split from SourceMonitor.cpp for maintainability.
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/MonitorUiKit.h"
#include "panels/monitors/WaveformDisplayWidget.h"

#include "Theme.h"
#include "UiScale.h"
#include "Settings.h"

#include "viewport/Viewport.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "playback/PlaybackController.h"
#include "playback/MediaPool.h"
#include "audio/AudioFile.h"
#include "audio/AudioEngine.h"
#include "audio/AudioPlaybackService.h"
#include "playback/AVSyncClock.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStackedLayout>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QTreeWidget>
#include <QFrame>
#include <QLineEdit>
#include <QRegularExpressionValidator>

#include <algorithm>
#include <cmath>
#include <thread>



namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

SourceMonitor::SourceMonitor(QWidget* parent)
    : QWidget(parent)
    , m_controller(std::make_unique<PlaybackController>())
    , m_seqAudioPlayback(std::make_unique<AudioPlaybackService>())
{
    setAutoFillBackground(true);
    {
        QPalette p = palette();
        p.setColor(QPalette::Window, Theme::colors().surface0);
        setPalette(p);
    }
    setupUI();

    // Polling timer for playback position updates
    m_pollTimer = new QTimer(this);
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    m_pollTimer->setInterval(16); // ~60fps
    connect(m_pollTimer, &QTimer::timeout, this, &SourceMonitor::onPollTimer);

    // Wire mini-timeline scrub events
    connect(m_miniTimeline, &MiniTimeline::scrubbed, this, &SourceMonitor::onScrub);

    // Any controller-driven seek (e.g. arrow-key step from the global
    // shortcut, programmatic seekTo, etc.) needs to repaint the viewport —
    // the source monitor doesn't have a continuously-running poll timer
    // while paused, so without this hook arrow Left/Right looked like a
    // no-op even though the controller tick had moved.
    m_controller->onPositionChanged = [this](int64_t) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_controller && m_controller->isPlaying()) return;  // poll timer covers it
        updateFrameDisplay();
    };

    // Manage poll timer and audio from controller state changes
    // (covers both button clicks and keyboard shortcuts)
    m_controller->onStateChanged = [this](PlayState state) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (state == PlayState::Playing || state == PlayState::Shuttling) {
            m_pollTimer->start();
            startSourceAudio();
        } else {
            m_pollTimer->stop();
            stopSourceAudio();
            updateFrameDisplay();
        }
    };

    // Update audio engine speed when shuttle speed changes mid-playback
    m_controller->onSpeedChanged = [this](double speed) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_sourceAudioActive && m_audioEngine) {
            m_audioEngine->setPlaybackSpeed(speed);
            if (m_audioEngine->transportState() != TransportState::Playing)
                m_audioEngine->play();
        }
    };

    // Accept keyboard focus so JKL/Space route to this monitor
    setFocusPolicy(Qt::ClickFocus);

    // Accept drops from Project Bin
    setAcceptDrops(true);

    // Install event filter on interactive children so any click
    // within the Source Monitor grabs keyboard focus for JKL/Space.
    m_viewport->setAcceptDrops(true);
    m_viewport->installEventFilter(this);
    m_miniTimeline->installEventFilter(this);
    m_waveformWidget->installEventFilter(this);
}

SourceMonitor::~SourceMonitor()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop the poll timer — prevents it firing during destruction
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
}

void SourceMonitor::setAudioEngine(AudioEngine* engine)
{
    m_audioEngine = engine;
    // NOTE: Do NOT set audio engine on the PlaybackController.
    // The shared AudioEngine is wired to the timeline's sync clock.
    // If the controller calls audioEngine->play()/stop(), it would
    // start/reset the timeline's sync clock, corrupting timeline state.
    // Audio is managed directly by startSourceAudio()/stopSourceAudio().

    // Wire the sequence-preview audio service so it can push track sources
    // for the inner sequence when the user previews one in this monitor.
    if (m_seqAudioPlayback) {
        m_seqAudioPlayback->setAudioEngine(engine);
        m_seqAudioPlayback->setPlaybackController(m_controller.get());
    }
}

void SourceMonitor::setSequenceProject(Project* project)
{
    // Lets a previewed sequence that itself contains nested SequenceClips
    // on audio tracks expand them for playback.
    if (m_seqAudioPlayback)
        m_seqAudioPlayback->setProject(project);
}

// ═════════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void SourceMonitor::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    setMinimumWidth(rt::UiScale::px(200));
    setMinimumHeight(rt::UiScale::px(180));

    // ── Clip name label ──────────────────────────────────
    m_clipLabel = new QLabel(tr("No clip loaded"), this);
    rt::UiScale::setScaledFixedHeight(m_clipLabel, 28);
    m_clipLabel->setAlignment(Qt::AlignCenter);
    m_clipLabel->setTextFormat(Qt::PlainText);
    m_clipLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { background: %1; color: %2; "
        "font-size: %3px; padding: 4px 8px; }")
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::typography().sizeXs)));
    mainLayout->addWidget(m_clipLabel);

    // ── Viewport / Waveform stacked area ──────────────────────────────
    auto* viewContainer = new QWidget(this);
    viewContainer->setAutoFillBackground(true);
    {
        QPalette vp = viewContainer->palette();
        vp.setColor(QPalette::Window, Theme::colors().surface0);
        viewContainer->setPalette(vp);
    }
    m_viewStack = new QStackedLayout(viewContainer);
    m_viewStack->setContentsMargins(0, 0, 0, 0);

    m_viewport = new Viewport(viewContainer);
    m_viewStack->addWidget(m_viewport);           // index 0

    m_waveformWidget = new WaveformDisplayWidget(viewContainer);
    m_viewStack->addWidget(m_waveformWidget);     // index 1

    // Waveform click-to-scrub
    m_waveformWidget->setScrubCallback([this](double ratio) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_hasClip || m_clipDuration <= 0) return;
        int64_t tick = static_cast<int64_t>(ratio * m_clipDuration);
        m_controller->seekTo(tick);
        scrubAudioAt(tick);
        updateFrameDisplay();
        emit playheadChanged(tick);
    });

    m_viewStack->setCurrentIndex(0); // default: video viewport
    mainLayout->addWidget(viewContainer, 1); // stretch=1

    // Spacer between viewport and controls — prevents video spill
    mainLayout->addWidget(monitorui::makeViewSpacer(this, 4));

    // ── Mini-timeline scrub bar ─────────────────────────────────────────
    m_miniTimeline = new MiniTimeline(this);

    // ── Info/control bar (Premiere Pro style — above mini timeline) ──────
    QHBoxLayout* controlLayout = nullptr;
    auto* controlBar = monitorui::makeControlBar(this, &controlLayout);

    // Timecode display (left side, Premiere Pro style; click to edit)
    m_timecodeLabel = monitorui::makeTimecodeLabel(this);
    m_timecodeLabel->installEventFilter(this);

    // Hidden editable timecode field (shown on click)
    m_timecodeEdit = monitorui::makeTimecodeEdit(this);

    connect(m_timecodeEdit, &QLineEdit::returnPressed, this, [this]() {
        QString text = m_timecodeEdit->text().trimmed();
        QStringList parts = text.split(QChar(':'));
        if (parts.size() == 4 && m_controller && m_hasClip) {
            Timecode tc;
            tc.hours   = parts[0].toInt();
            tc.minutes = parts[1].toInt();
            tc.seconds = parts[2].toInt();
            tc.frames  = parts[3].toInt();
            int64_t tick = timecodeToTick(tc, m_controller->frameRate());
            m_controller->seekTo(tick);
            updateFrameDisplay();
        }
        m_timecodeEdit->hide();
        m_timecodeLabel->show();
    });
    connect(m_timecodeEdit, &QLineEdit::editingFinished, this, [this]() {
        m_timecodeEdit->hide();
        m_timecodeLabel->show();
    });

    controlLayout->addWidget(m_timecodeLabel, 0, Qt::AlignVCenter);
    controlLayout->addWidget(m_timecodeEdit, 0, Qt::AlignVCenter);
    controlLayout->addSpacing(rt::UiScale::px(12));

    // ── Premiere-style "drag video only" / "drag audio only" buttons ────
    // Sit just left of the zoom combo. Click-drag one to drop only that
    // stream of the loaded clip/sequence into the timeline (respecting the
    // source in/out points).
    auto dragBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; padding: 2px; } "
        "QPushButton:hover:enabled { background: %2; } "
        "QPushButton:disabled { border-color: %3; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().surface1)));

    auto makeFilmstripIcon = [](QColor fg) -> QIcon {
        QPixmap px(20, 20);
        px.fill(Qt::transparent);
        QPainter ip(&px);
        ip.setRenderHint(QPainter::Antialiasing, false);
        ip.setPen(QPen(fg, 1.2));
        ip.setBrush(Qt::NoBrush);
        ip.drawRect(QRectF(3.5, 4.5, 13, 11));     // film body
        for (int i = 0; i < 4; ++i) {              // sprocket holes
            ip.fillRect(QRectF(4.5 + i * 3.0, 5.5, 1.4, 1.4), fg);
            ip.fillRect(QRectF(4.5 + i * 3.0, 12.5, 1.4, 1.4), fg);
        }
        ip.end();
        return QIcon(px);
    };
    auto makeWaveformIcon = [](QColor fg) -> QIcon {
        QPixmap px(20, 20);
        px.fill(Qt::transparent);
        QPainter ip(&px);
        ip.setRenderHint(QPainter::Antialiasing, false);
        ip.setPen(QPen(fg, 1.4));
        const float h[7] = { 3, 7, 11, 14, 9, 5, 2 };
        for (int i = 0; i < 7; ++i) {
            float x = 3.0f + i * 2.4f;
            ip.drawLine(QPointF(x, 10 - h[i] / 2.0f),
                        QPointF(x, 10 + h[i] / 2.0f));
        }
        ip.end();
        return QIcon(px);
    };

    m_btnDragVideo = new QPushButton(this);
    m_btnDragVideo->setIcon(makeFilmstripIcon(Theme::colors().textSecondary));
    m_btnDragVideo->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
    rt::UiScale::setScaledFixedSize(m_btnDragVideo, 26, 22);
    m_btnDragVideo->setFocusPolicy(Qt::NoFocus);
    m_btnDragVideo->setCursor(Qt::OpenHandCursor);
    m_btnDragVideo->setToolTip(tr("Drag Video Only — drag to timeline"));
    m_btnDragVideo->setStyleSheet(dragBtnStyle);
    m_btnDragVideo->setEnabled(false);  // nothing loaded yet
    m_btnDragVideo->installEventFilter(this);
    controlLayout->addWidget(m_btnDragVideo, 0, Qt::AlignVCenter);

    m_btnDragAudio = new QPushButton(this);
    m_btnDragAudio->setIcon(makeWaveformIcon(Theme::colors().textSecondary));
    m_btnDragAudio->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
    rt::UiScale::setScaledFixedSize(m_btnDragAudio, 26, 22);
    m_btnDragAudio->setFocusPolicy(Qt::NoFocus);
    m_btnDragAudio->setCursor(Qt::OpenHandCursor);
    m_btnDragAudio->setToolTip(tr("Drag Audio Only — drag to timeline"));
    m_btnDragAudio->setStyleSheet(dragBtnStyle);
    m_btnDragAudio->setEnabled(false);  // nothing loaded yet
    m_btnDragAudio->installEventFilter(this);
    controlLayout->addWidget(m_btnDragAudio, 0, Qt::AlignVCenter);

    // Premiere-style single monitor row: playback controls share this bar
    // with timecode and display controls, below the permanent mini-timeline.
    auto transport = monitorui::makeTransportBar(controlBar, controlLayout);
    m_btnGoStart     = transport.goStart;
    m_btnStepBack    = transport.stepBack;
    m_btnPlayPause   = transport.playPause;
    m_btnStop        = transport.stop;
    m_btnStepForward = transport.stepForward;
    m_btnGoEnd       = transport.goEnd;
    m_btnLoop        = transport.loop;
    m_shuttleSpeedLabel = transport.shuttleSpeed;

    controlLayout->addStretch();

    // Fit mode / zoom presets combo box. Added to the layout later, next
    // to the playback-resolution combo on the right side of the bar.
    m_fitModeCombo = monitorui::makeFitModeCombo(this);

    connect(m_fitModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        switch (index) {
        case 0: m_viewport->setFitMode(ViewportFitMode::Fit);  break;
        case 1: m_viewport->setFitMode(ViewportFitMode::Fill); break;
        default: {
            static constexpr float zoomLevels[] = { 0.25f, 0.50f, 0.75f, 1.0f, 1.5f, 2.0f };
            int zi = index - 2;
            if (zi >= 0 && zi < static_cast<int>(std::size(zoomLevels))) {
                m_viewport->setFitMode(ViewportFitMode::Actual);
                m_viewport->setViewZoom(zoomLevels[zi]);
            }
            break;
        }
        }
        if (index <= 1)
            m_viewport->resetZoomPan();
    });

    // Playback resolution dropdown
    m_playbackResCombo = monitorui::makePlaybackResCombo(this, /*withAuto=*/false);
    m_playbackResCombo->setCurrentIndex(1); // default 1/2
    // Wire the dropdown: changing source playback resolution re-renders the
    // current frame at the new tier.  (Previously this combo was created but
    // never connected, so it did nothing — updateFrameDisplay reads its index.)
    connect(m_playbackResCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateFrameDisplay(); });
    // Zoom level sits directly next to the playback-resolution combo.
    controlLayout->addWidget(m_fitModeCombo, 0, Qt::AlignVCenter);
    controlLayout->addWidget(m_playbackResCombo, 0, Qt::AlignVCenter);

    // Safe Area toggle button (Premiere Pro style icon)
    m_btnSafeArea = monitorui::makeSafeAreaButton(this);
    controlLayout->addWidget(m_btnSafeArea, 0, Qt::AlignVCenter);
    connect(m_btnSafeArea, &QPushButton::toggled, this, [this](bool checked) {
        m_viewport->setSafeAreasVisible(checked);
    });

    // Export frame button (large, Premiere Pro style)
    m_btnExportFrame = monitorui::makeExportFrameButton(this);
    controlLayout->addWidget(m_btnExportFrame, 0, Qt::AlignVCenter);
    connect(m_btnExportFrame, &QPushButton::clicked, this, [this]() {
        exportViewportFrame();
    });

    // Zoom percentage label (hidden — matches Premiere layout)
    m_zoomLabel = monitorui::makeZoomLabel(this);

    connect(m_viewport, &Viewport::viewZoomChanged, this, [this](float zoom) {
        m_zoomLabel->setText(QString::number(static_cast<int>(std::round(zoom * 100))) + QStringLiteral("%"));
    });

    // Duration timecode display (right side)
    m_durationLabel = monitorui::makeDurationLabel(this);
    controlLayout->addWidget(m_durationLabel, 0, Qt::AlignVCenter);

    // ── Mini-timeline scrub bar ─────────────────────────────────────────
    m_miniTimeline->setMinimumHeight(rt::UiScale::px(56));
    mainLayout->addWidget(m_miniTimeline);
    mainLayout->addWidget(controlBar);

    // Unified-row transport connections.
    connect(m_btnLoop, &QPushButton::toggled, this, [this](bool checked) {
        if (m_controller) m_controller->setLoopEnabled(checked);
    });

    // Connect transport buttons
    connect(m_btnGoStart, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->goToStart(); updateFrameDisplay(); }
    });
    connect(m_btnStepBack, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepBackward(); updateFrameDisplay(); }
    });
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_hasClip)
            m_controller->togglePlayPause();
    });
    connect(m_btnStop, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stop(); updateFrameDisplay(); }
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepForward(); updateFrameDisplay(); }
    });
    connect(m_btnGoEnd, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->goToEnd(); updateFrameDisplay(); }
    });

    // Install event filter on transport bar and its children so that
    // clicking playback buttons (which have Qt::NoFocus) or the bar
    // background still grabs keyboard focus for JKL/Space shortcuts.
    controlBar->installEventFilter(this);
    for (auto* child : controlBar->findChildren<QWidget*>())
        child->installEventFilter(this);
}

void SourceMonitor::exportViewportFrame()
{
    if (!m_viewport || !m_viewport->hasFrame() || m_audioOnly) {
        QMessageBox::information(this, tr("Export Frame"),
                                 tr("There is no video frame to export."));
        return;
    }

    auto settings = appSettings();
    QString lastDir = settings.value(QStringLiteral("export/lastOutputDir")).toString();
    if (lastDir.isEmpty() || !QDir(lastDir).exists())
        lastDir = QDir::homePath();

    const int64_t tick = m_controller ? m_controller->currentTick() : 0;
    const QString defaultName = QStringLiteral("source_frame_%1.png")
        .arg(tick, 6, 10, QLatin1Char('0'));
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Frame"), QDir(lastDir).filePath(defaultName),
        tr("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;BMP Image (*.bmp)"),
        &selectedFilter);
    if (path.isEmpty()) return;

    if (QFileInfo(path).suffix().isEmpty()) {
        if (selectedFilter.startsWith(QStringLiteral("JPEG")))
            path += QStringLiteral(".jpg");
        else if (selectedFilter.startsWith(QStringLiteral("BMP")))
            path += QStringLiteral(".bmp");
        else
            path += QStringLiteral(".png");
    }

    const QPixmap shot = m_viewport->grab();
    if (shot.isNull() || !shot.save(path)) {
        QMessageBox::warning(this, tr("Export Frame"),
                             tr("Could not save the frame to:\n%1").arg(path));
        return;
    }

    settings.setValue(QStringLiteral("export/lastOutputDir"),
                      QFileInfo(path).absolutePath());
    settings.sync();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Media loading
// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
