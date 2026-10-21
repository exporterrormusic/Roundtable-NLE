/*
 * ProgramMonitorUI.cpp -- constructor, destructor, and setupUI.
 *
 * Split from ProgramMonitor.cpp for maintainability.
 */


#include "panels/monitors/ProgramMonitor.h"
#include "playback/PlaybackScheduler.h"
#include "PerformanceProfile.h"

#include "Theme.h"
#include "UiScale.h"

#include "viewport/Viewport.h"
#include "viewport/VulkanViewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "GpuContext.h"
#include "panels/monitors/MonitorUiKit.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "playback/PlaybackController.h"
#include "cache/FrameCache.h"
#include "timeline/Timeline.h"

#include <QAction>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSettings>
#include <QStackedLayout>
#include <QShowEvent>
#include <QKeyEvent>
#include <QFrame>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QPainter>
#include <QPixmap>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

ProgramMonitor::ProgramMonitor(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setupUI();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    m_pollTimer->setInterval(16); // ~60fps
    connect(m_pollTimer, &QTimer::timeout, this, &ProgramMonitor::onPollTimer);

    connect(m_miniTimeline, &MiniTimeline::scrubbed, this, &ProgramMonitor::onScrub);
    connect(m_fitModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ProgramMonitor::onFitModeChanged);

    // Install event filter on child widgets so clicking anywhere inside
    // the Program Monitor gives us keyboard focus for I/O shortcuts.
    if (m_vulkanViewport) m_vulkanViewport->installEventFilter(this);
    if (m_transformOverlay) m_transformOverlay->installEventFilter(this);
    if (m_viewport) m_viewport->installEventFilter(this);
    if (m_miniTimeline) m_miniTimeline->installEventFilter(this);
}

ProgramMonitor::~ProgramMonitor()
{
    // Set the atomic flag FIRST so the present callback and presentFrame()
    // can bail out immediately instead of accessing freed memory.
    // The callback lambda captures `this`, so without this guard the
    // presenter thread can call into a partially-destroyed ProgramMonitor
    // even after setPresentCallback(nullptr) — std::function assignment
    // is not atomic and the presenter may already hold a copy of the old
    // function object.
    m_destroying.store(true, std::memory_order_release);

    if (m_pipeline) {
        spdlog::info("[PM-TRACE] destructor stopping pipeline");
        // Stop pipeline threads BEFORE clearing callbacks.  stop() joins
        // all threads, guaranteeing no more callbacks will fire.
        m_pipeline->stop();

        // Now it's safe to clear callbacks (no threads left to call them).
        m_pipeline->setPresentNotify(nullptr);
        m_pipeline->setPresentCallback(nullptr);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void ProgramMonitor::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

#ifdef _WIN32
    // Windows currently forces CPU viewport rendering in Program Monitor.
    // Keep the viewport container as a regular QWidget (non-native) to avoid
    // child-window z-order artifacts after dock/tab rearrangement.
    const bool useNativeViewportContainer = false;
#else
    const bool useNativeViewportContainer = true;
#endif

    // ── Viewport (same pattern as SourceMonitor: QStackedLayout container) ──
    auto* viewContainer = new QWidget(this);
    if (useNativeViewportContainer) {
        viewContainer->setAttribute(Qt::WA_NativeWindow);
        viewContainer->winId(); // force native HWND so it clips Vulkan child
    }
    m_viewStack = new QStackedLayout(viewContainer);
    m_viewStack->setContentsMargins(0, 0, 0, 0);

    m_viewport = new Viewport(viewContainer);
    m_viewStack->addWidget(m_viewport);             // index 0

    // Try GPU-direct display via VulkanViewport (zero-copy from compositor).
    // Enabled on all platforms — the VulkanViewport uses QWindow::createWindowContainer()
    // which handles native child-window embedding correctly.  CPU readback is
    // significantly slower and causes frame-drops during playback.
    const bool allowGpuDisplay = true;

    if (allowGpuDisplay && GpuContext::get().isInitialized()) {
        m_vulkanViewport = new VulkanViewport(viewContainer);
        if (m_vulkanViewport->isGpuActive()) {
            m_gpuDisplay = true;
            m_viewStack->addWidget(m_vulkanViewport); // index 1
            m_viewStack->setCurrentIndex(1);

            // Transform overlay — a top-level frameless transparent tool
            // window that floats above the Vulkan surface.  ProgramMonitor
            // keeps it positioned to exactly cover the viewport.
            m_transformOverlay = new TransformOverlayWidget(m_vulkanViewport, this);

            // Defer initial positioning until the event loop settles
            QTimer::singleShot(0, this, [this]() { syncOverlayGeometry(); });

            connect(m_vulkanViewport, &VulkanViewport::resized, this, [this]() {
                syncOverlayGeometry();
            });

            // Update transform overlay when render thread presents a frame
            connect(this, &ProgramMonitor::frameDisplayed, this, [this](int64_t) {
                if (m_transformOverlay && m_transformOverlay->transformOverlay().visible)
                    m_transformOverlay->update();
            });

            spdlog::info("ProgramMonitor: GPU display active (zero-copy)");
        } else {
            delete m_vulkanViewport;
            m_vulkanViewport = nullptr;
            m_viewStack->setCurrentIndex(0);
        }
    } else {
        m_viewStack->setCurrentIndex(0);
    }

    // GPU display may be disabled by preferences or if VulkanViewport init fails.
    if (!m_gpuDisplay) {
        spdlog::warn("ProgramMonitor: GPU display unavailable, using CPU viewport path");
    }

    mainLayout->addWidget(viewContainer, 1); // stretch=1

    // Spacer between viewport and controls — prevents video spill
    auto* viewSpacer = monitorui::makeViewSpacer(this, 8);
    if (useNativeViewportContainer) {
        viewSpacer->setAttribute(Qt::WA_NativeWindow);
        viewSpacer->winId();
    }
    mainLayout->addWidget(viewSpacer);

    // ── Premiere's Program Monitor controls (see MonitorUiKit.h) ─────────
    //   info row    timecode · Fit · · · Full · wrench · duration
    //   mini ruler
    //   button row  marker/in/out | playback | Lift, Extract | Export Frame
    m_miniTimeline = new MiniTimeline(this);

    QHBoxLayout* infoLayout = nullptr;
    auto* infoRow = monitorui::makeInfoRow(this, &infoLayout);

    // Timecode display (left side; click to edit)
    m_timecodeLabel = monitorui::makeTimecodeLabel(this);
    m_timecodeLabel->installEventFilter(this);

    // Hidden editable timecode field (shown on click)
    m_timecodeEdit = monitorui::makeTimecodeEdit(this);

    connect(m_timecodeEdit, &QLineEdit::returnPressed, this, [this]() {
        QString text = m_timecodeEdit->text().trimmed();
        // Parse HH:MM:SS:FF
        QStringList parts = text.split(QChar(':'));
        if (parts.size() == 4 && m_controller) {
            Timecode tc;
            tc.hours   = parts[0].toInt();
            tc.minutes = parts[1].toInt();
            tc.seconds = parts[2].toInt();
            tc.frames  = parts[3].toInt();
            int64_t tick = timecodeToTick(tc, m_controller->frameRate());
            m_controller->seekTo(tick);
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

    m_dropIndicator = new QLabel(infoRow);
    m_dropIndicator->setFixedSize(10, 10);
    m_dropIndicator->setToolTip(tr("Dropped frames"));
    m_dropIndicator->hide();
    infoLayout->addWidget(m_dropIndicator, 0, Qt::AlignVCenter);

    // Fit mode / zoom presets combo box
    m_fitModeCombo = monitorui::makeFitModeCombo(this);
    infoLayout->addWidget(m_fitModeCombo, 0, Qt::AlignVCenter);

    m_shuttleSpeedLabel = monitorui::makeShuttleSpeedLabel(infoRow);
    infoLayout->addWidget(m_shuttleSpeedLabel, 0, Qt::AlignVCenter);

    infoLayout->addStretch();

    // Dropped frame counter (Premiere Pro style yellow indicator)
    m_droppedFrameLabel = new QLabel(this);
    m_droppedFrameLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-size: 10px; font-weight: bold; color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().warning))));
    m_droppedFrameLabel->setToolTip(tr("Dropped frames during playback"));
    m_droppedFrameLabel->hide(); // Hidden until drops occur
    infoLayout->addWidget(m_droppedFrameLabel, 0, Qt::AlignVCenter);

    // Playback resolution dropdown.  Indices:
    //   0 = Full         — manual divisor 1
    //   1 = 1/2          — manual divisor 2
    //   2 = 1/4          — manual divisor 4
    //   3 = 1/8          — manual divisor 8 (composite-only; decode tier
    //                       still caps at Quarter)
    //   4 = Auto         — UPGRADE_PLAN item 1: FrameProducer adjusts the
    //                       divisor between Full/Half/Quarter from rolling
    //                       composite latency vs. frame budget.
    m_playbackResCombo = monitorui::makePlaybackResCombo(this, /*withAuto=*/true);
    static constexpr int kAutoIdx = 4;
    // Restore the last-used playback resolution (Premiere-style: the
    // dropdown reopens with whatever it was closed at). The FIRST-RUN
    // default comes from the machine profile: Workstation-tier machines
    // (editProxyScale >= 1.0) start at Full, everyone else at 1/2
    // (index 1, matches Half-tier decode). Set the divisor member
    // directly too — setPlaybackTierCallback() fires once with
    // m_playbackResDivisor on startup, so the restored tier must be in
    // place before the combo's currentIndexChanged slot is connected.
    {
        const int firstRunIdx = (perfProfile().editProxyScale >= 1.0f) ? 0 : 1;
        int savedIdx = QSettings().value(
            QStringLiteral("playback/resolutionIndex"), firstRunIdx).toInt();
        if (savedIdx < 0 || savedIdx > kAutoIdx) savedIdx = firstRunIdx;
        static constexpr int kDivisors[] = {1, 2, 4, 8};
        // Auto starts at the same effective divisor as 1/2; the producer
        // takes it from there.  Anything else uses the manual table.
        m_playbackResAdaptive = (savedIdx == kAutoIdx);
        m_playbackResDivisor  = m_playbackResAdaptive ? 2 : kDivisors[savedIdx];
        m_playbackResCombo->setCurrentIndex(savedIdx);
    }
    infoLayout->addWidget(m_playbackResCombo, 0, Qt::AlignVCenter);

    // Connect playback resolution selection
    connect(m_playbackResCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        static constexpr int divisors[] = {1, 2, 4, 8};
        const bool isAuto = (index == kAutoIdx);
        m_playbackResAdaptive = isAuto;
        if (isAuto) {
            // Auto starts at Half — close to what most projects need.  The
            // producer's feedback loop bumps from there on every-frame
            // composite latency.
            m_playbackResDivisor = 2;
        } else {
            m_playbackResDivisor = (index >= 0 && index < 4) ? divisors[index] : 1;
        }

        // Persist so the next launch reopens at this resolution.
        if (index >= 0 && index <= kAutoIdx)
            QSettings().setValue(
                QStringLiteral("playback/resolutionIndex"), index);

        if (m_pipeline) {
            // Toggle adaptive mode BEFORE re-seeding the manual divisor so a
            // transition from Auto→manual lands at exactly the user's choice
            // instead of the last auto-chosen value.
            m_pipeline->setAdaptiveEnabled(isAuto);
            m_pipeline->setOutputResolution(m_outputWidth, m_outputHeight,
                                            m_playbackResDivisor);
        }

        // Tell the compositor to match the new preview resolution at decode
        // time — otherwise the dropdown only shrinks composite output while
        // the decoder still produces full-resolution frames, wasting both
        // decode cycles and FrameCache bytes.  For Auto, this seeds the
        // starting tier; subsequent adaptive changes route through the
        // pipeline's adaptive-tier callback wired in initPipeline().
        if (m_playbackTierCallback)
            m_playbackTierCallback(m_playbackResDivisor);

        if (m_gpuDisplay && m_vulkanViewport)
            m_vulkanViewport->clearFrame();
        else if (m_viewport)
            m_viewport->clearFrame();

        m_lastRenderedTick = -1;
        updateDisplay();
    });

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
        if (m_transformOverlay)
            m_transformOverlay->setSafeAreasVisible(checked);
    });

    // Resolution/framerate overlay label (hidden — Premiere doesn't show inline)
    m_resOverlayLabel = new QLabel(QStringLiteral("1920×1080 • 24fps"), this);
    m_resOverlayLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-size: 10px; color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().textTertiary))));
    m_resOverlayLabel->hide();

    // Zoom percentage label (hidden — shown in status bar instead)
    m_zoomLabel = monitorui::makeZoomLabel(this);

    connect(m_viewport, &Viewport::viewZoomChanged, this, [this](float zoom) {
        m_zoomLabel->setText(QString::number(static_cast<int>(std::round(zoom * 100))) + QStringLiteral("%"));
    });

    if (m_vulkanViewport) {
        connect(m_vulkanViewport, &VulkanViewport::viewZoomChanged, this, [this](float zoom) {
            m_zoomLabel->setText(QString::number(static_cast<int>(std::round(zoom * 100))) + QStringLiteral("%"));
            if (m_transformOverlay) m_transformOverlay->update();
        });
        connect(m_vulkanViewport, &VulkanViewport::resized, this, [this]() {
            m_lastRenderedTick = -1;
            updateDisplay();
        });
    }

    // Duration timecode display (right side, Premiere Pro style)
    m_durationLabel = monitorui::makeDurationLabel(this);
    infoLayout->addWidget(m_durationLabel, 0, Qt::AlignVCenter);

    // Hidden checkable button for grid (used by settings menu)
    m_btnGrid = new QPushButton(this);
    m_btnGrid->setCheckable(true);
    m_btnGrid->hide();
    connect(m_btnGrid, &QPushButton::toggled, this, [this](bool checked) {
        if (m_transformOverlay)
            m_transformOverlay->setGridVisible(checked);
    });

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

    m_btnLift = monitorui::makeRowButton(buttonRow, TransportButton::Lift, tr("Lift"),
                                         {}, QStringLiteral(";"));
    m_btnExtract = monitorui::makeRowButton(buttonRow, TransportButton::Extract, tr("Extract"),
                                            {}, QStringLiteral("'"));
    buttonLayout->addWidget(m_btnLift, 0, Qt::AlignVCenter);
    buttonLayout->addWidget(m_btnExtract, 0, Qt::AlignVCenter);
    monitorui::addButtonGroupGap(buttonLayout);

    m_btnExportFrame = monitorui::makeRowButton(buttonRow, TransportButton::Screenshot,
                                                tr("Export Frame"), {},
                                                QStringLiteral("Ctrl+Shift+E"));
    buttonLayout->addWidget(m_btnExportFrame, 0, Qt::AlignVCenter);
    monitorui::finishButtonRow(buttonLayout);

    // Give the mini-timeline a distinct background so it's clearly visible
    m_miniTimeline->setMinimumHeight(rt::UiScale::px(56));
    mainLayout->addWidget(infoRow);
    mainLayout->addWidget(m_miniTimeline);
    mainLayout->addWidget(buttonRow);

    // Button-row connections.
    connect(m_btnAddMarker, &QPushButton::clicked, this, &ProgramMonitor::addMarkerRequested);
    connect(m_btnMarkIn, &QPushButton::clicked, this, &ProgramMonitor::inPointRequested);
    connect(m_btnMarkOut, &QPushButton::clicked, this, &ProgramMonitor::outPointRequested);
    connect(m_btnGoToIn, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->goToInPoint();
    });
    connect(m_btnStepBack, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->stepBackward();
    });
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->togglePlayPause();
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->stepForward();
    });
    connect(m_btnGoToOut, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->goToOutPoint();
    });
    connect(m_btnLift, &QPushButton::clicked, this, &ProgramMonitor::liftRequested);
    connect(m_btnExtract, &QPushButton::clicked, this, &ProgramMonitor::extractRequested);
    connect(m_btnExportFrame, &QPushButton::clicked, this, &ProgramMonitor::exportFrameRequested);
}

void ProgramMonitor::setShortcutManager(const ShortcutManager* shortcuts)
{
    monitorui::applyShortcutTooltips(this, shortcuts);
}

} // namespace rt
