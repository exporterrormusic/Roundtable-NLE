/*
 * SourceMonitorUI.cpp -- WaveformDisplayWidget, constructor, destructor, setupUI.
 *
 * Split from SourceMonitor.cpp for maintainability.
 */

#include "panels/monitors/SourceMonitor.h"
#include "panels/monitors/MonitorUiKit.h"
#include "panels/monitors/WaveformDisplayWidget.h"
#include "MediaTaskQueue.h"

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

#include <QAction>
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
    m_audioTaskOwner = MediaTaskQueue::instance().createOwner();
    m_waveformTaskOwner = MediaTaskQueue::instance().createOwner();
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
        if (m_scrubPending) return; // the scrub timer coalesces expensive decoding
        if (m_controller && m_controller->isPlaying()) return;  // poll timer covers it
        updateFrameDisplay();
    };

    // Manage poll timer and audio from controller state changes
    // (covers both button clicks and keyboard shortcuts)
    m_controller->onStateChanged = [this](PlayState state) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        // Premiere's Play-Stop Toggle shows a stop square while playing.
        const bool playing = (state == PlayState::Playing || state == PlayState::Shuttling);
        if (m_btnPlayPause)
            m_btnPlayPause->setTransportIcon(playing ? TransportButton::Stop
                                                     : TransportButton::Play);
        if (playing) {
            // The scrub target is already committed to the controller. Do not
            // let a queued scrub burst replace the audio we are starting now.
            m_scrubPending = false;
            m_scrubQualityActive = false;
            m_scrubSettleCounter = 0;
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
    auto& tasks = MediaTaskQueue::instance();
    tasks.cancelOwner(m_audioTaskOwner);
    tasks.cancelOwner(m_waveformTaskOwner);

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

    // The clip name lives in the panel tab ("Source: clip.mp4"), as in
    // Premiere — see panelTitle() / clipChanged().

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

    // ── Premiere's Source Monitor controls (see MonitorUiKit.h) ──────────
    //   info row    timecode · Fit · drag video / drag audio · Full · wrench · duration
    //   mini ruler  (with the clip's markers)
    //   button row  marker/in/out | playback | Export Frame, Mute
    m_miniTimeline = new MiniTimeline(this);

    QHBoxLayout* infoLayout = nullptr;
    auto* infoRow = monitorui::makeInfoRow(this, &infoLayout);

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

    infoLayout->addWidget(m_timecodeLabel, 0, Qt::AlignVCenter);
    infoLayout->addWidget(m_timecodeEdit, 0, Qt::AlignVCenter);

    // Fit mode / zoom presets combo box, right after the timecode.
    m_fitModeCombo = monitorui::makeFitModeCombo(this);
    infoLayout->addWidget(m_fitModeCombo, 0, Qt::AlignVCenter);

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

    m_shuttleSpeedLabel = monitorui::makeShuttleSpeedLabel(infoRow);
    infoLayout->addWidget(m_shuttleSpeedLabel, 0, Qt::AlignVCenter);

    infoLayout->addStretch();

    // ── Premiere-style "drag video only" / "drag audio only" buttons ────
    // In the middle of the info row. Click-drag one to drop only that
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
    infoLayout->addWidget(m_btnDragVideo, 0, Qt::AlignVCenter);

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
    infoLayout->addWidget(m_btnDragAudio, 0, Qt::AlignVCenter);

    infoLayout->addStretch();

    // Playback resolution dropdown
    m_playbackResCombo = monitorui::makePlaybackResCombo(this, /*withAuto=*/false);
    m_playbackResCombo->setCurrentIndex(1); // default 1/2
    // Wire the dropdown: changing source playback resolution re-renders the
    // current frame at the new tier.  (Previously this combo was created but
    // never connected, so it did nothing — updateFrameDisplay reads its index.)
    connect(m_playbackResCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateFrameDisplay(); });
    infoLayout->addWidget(m_playbackResCombo, 0, Qt::AlignVCenter);

    // Wrench: the settings menu (Loop, Safe Margins), as in Premiere.
    auto settings = monitorui::makeSettingsMenu(infoRow);
    m_btnSettings    = settings.button;
    m_actLoop        = settings.loop;
    m_actSafeMargins = settings.safeMargins;
    infoLayout->addWidget(m_btnSettings, 0, Qt::AlignVCenter);

    connect(m_actLoop, &QAction::toggled, this, [this](bool checked) {
        if (m_controller) m_controller->setLoopEnabled(checked);
    });
    connect(m_actSafeMargins, &QAction::toggled, this, [this](bool checked) {
        m_viewport->setSafeAreasVisible(checked);
    });

    // Zoom percentage label (hidden — matches Premiere layout)
    m_zoomLabel = monitorui::makeZoomLabel(this);

    connect(m_viewport, &Viewport::viewZoomChanged, this, [this](float zoom) {
        m_zoomLabel->setText(QString::number(static_cast<int>(std::round(zoom * 100))) + QStringLiteral("%"));
    });

    // Duration timecode display (right side)
    m_durationLabel = monitorui::makeDurationLabel(this);
    infoLayout->addWidget(m_durationLabel, 0, Qt::AlignVCenter);

    // ── Button row ───────────────────────────────────────────────────────
    QHBoxLayout* buttonLayout = nullptr;
    auto* buttonRow = monitorui::makeButtonRow(this, &buttonLayout);

    const auto marks = monitorui::addMarkGroup(buttonRow, buttonLayout);
    m_btnAddMarker = marks.addMarker;
    m_btnMarkIn    = marks.markIn;
    m_btnMarkOut   = marks.markOut;
    monitorui::addButtonGroupGap(buttonLayout);

    const auto playback = monitorui::addPlaybackGroup(buttonRow, buttonLayout);
    m_btnGoToIn      = playback.goToIn;
    m_btnStepBack    = playback.stepBack;
    m_btnPlayPause   = playback.playPause;
    m_btnStepForward = playback.stepForward;
    m_btnGoToOut     = playback.goToOut;
    monitorui::addButtonGroupGap(buttonLayout);

    m_btnExportFrame = monitorui::makeRowButton(buttonRow, TransportButton::Screenshot,
                                                tr("Export Frame"), {},
                                                QStringLiteral("Ctrl+Shift+E"));
    m_btnMute = monitorui::makeRowButton(buttonRow, TransportButton::Speaker,
                                         tr("Mute Source Monitor"));
    m_btnMute->setCheckable(true);
    buttonLayout->addWidget(m_btnExportFrame, 0, Qt::AlignVCenter);
    buttonLayout->addWidget(m_btnMute, 0, Qt::AlignVCenter);
    monitorui::finishButtonRow(buttonLayout);

    m_miniTimeline->setMinimumHeight(rt::UiScale::px(56));
    mainLayout->addWidget(infoRow);
    mainLayout->addWidget(m_miniTimeline);
    mainLayout->addWidget(buttonRow);

    // Button-row connections.
    connect(m_btnAddMarker, &QPushButton::clicked, this, &SourceMonitor::requestAddMarker);
    connect(m_btnMarkIn, &QPushButton::clicked, this, &SourceMonitor::markIn);
    connect(m_btnMarkOut, &QPushButton::clicked, this, &SourceMonitor::markOut);
    connect(m_btnGoToIn, &QPushButton::clicked, this, [this]() {
        if (!m_hasClip) return;
        m_controller->seekTo(hasInPoint() ? inPoint() : 0);
        updateFrameDisplay();
    });
    connect(m_btnStepBack, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepBackward(); updateFrameDisplay(); }
    });
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_hasClip)
            m_controller->togglePlayPause();
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_hasClip) { m_controller->stepForward(); updateFrameDisplay(); }
    });
    connect(m_btnGoToOut, &QPushButton::clicked, this, [this]() {
        if (!m_hasClip) return;
        m_controller->seekTo(hasOutPoint() ? outPoint() : m_clipDuration);
        updateFrameDisplay();
    });
    connect(m_btnExportFrame, &QPushButton::clicked, this, [this]() {
        exportViewportFrame();
    });
    connect(m_btnMute, &QPushButton::toggled, this, &SourceMonitor::setMuted);

    // Install event filter on both control rows and their children so that
    // clicking playback buttons (which have Qt::NoFocus) or a row's
    // background still grabs keyboard focus for JKL/Space shortcuts.
    for (auto* row : {infoRow, buttonRow}) {
        row->installEventFilter(this);
        for (auto* child : row->findChildren<QWidget*>())
            child->installEventFilter(this);
    }
}

QString SourceMonitor::panelTitle() const
{
    return m_clipName.isEmpty() ? tr("Source Monitor")
                                : tr("Source: %1").arg(m_clipName);
}

void SourceMonitor::setShortcutManager(const ShortcutManager* shortcuts)
{
    monitorui::applyShortcutTooltips(this, shortcuts);
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
