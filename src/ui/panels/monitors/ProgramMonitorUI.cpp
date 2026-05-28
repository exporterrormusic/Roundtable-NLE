/*
 * ProgramMonitorUI.cpp -- constructor, destructor, and setupUI.
 *
 * Split from ProgramMonitor.cpp for maintainability.
 */


#include "panels/monitors/ProgramMonitor.h"
#include "media/PlaybackScheduler.h"

#include "Theme.h"
#include "UiScale.h"

#include "viewport/Viewport.h"
#include "viewport/VulkanViewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "GpuContext.h"
#include "widgets/MiniTimeline.h"
#include "widgets/TransportButton.h"
#include "media/PlaybackController.h"
#include "media/FrameCache.h"
#include "timeline/Timeline.h"

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
    auto* viewSpacer = new QWidget(this);
    rt::UiScale::setScaledFixedHeight(viewSpacer, 8);
    if (useNativeViewportContainer) {
        viewSpacer->setAttribute(Qt::WA_NativeWindow);
        viewSpacer->winId();
    }
    viewSpacer->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; }")
        .arg(Theme::hex(Theme::colors().surface0)));
    mainLayout->addWidget(viewSpacer);

    // ── Mini-timeline scrub bar ─────────────────────────────────────────
    m_miniTimeline = new MiniTimeline(this);

    // ── Info/control bar (Premiere Pro style — above mini timeline) ──────
    auto* controlBar = new QWidget(this);
    controlBar->setObjectName(QStringLiteral("ControlBar"));
    rt::UiScale::setScaledFixedHeight(controlBar, 52);
    controlBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#ControlBar { background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(rt::UiScale::px(8), rt::UiScale::px(10),
                                      rt::UiScale::px(8), rt::UiScale::px(10));
    controlLayout->setSpacing(rt::UiScale::px(6));

    auto comboStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QComboBox { background: %1; border: 1px solid %2; "
        "border-radius: 3px; color: %3; font-size: 12px; "
        "padding: 4px 8px 4px 8px; }"
        "QComboBox::drop-down { border: none; width: 20px; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary)));

    // Timecode display (left side, Premiere Pro green style — click to edit)
    m_timecodeLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_timecodeLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 14px; "
        "font-weight: bold; color: #00CC88; background: transparent; "
        "padding: 0px 6px 0px 0px; }")));
    rt::UiScale::setScaledFixedWidth(m_timecodeLabel, 120);
    m_timecodeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timecodeLabel->setCursor(Qt::IBeamCursor);
    m_timecodeLabel->setToolTip(tr("Click to enter timecode"));
    m_timecodeLabel->installEventFilter(this);

    // Hidden editable timecode field (shown on click)
    m_timecodeEdit = new QLineEdit(this);
    m_timecodeEdit->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLineEdit { font-family: 'Consolas', monospace; font-size: 14px; "
        "font-weight: bold; color: #00CC88; background: %1; "
        "border: 1px solid #00CC88; border-radius: 3px; "
        "padding: 0px 6px 0px 0px; }")
        .arg(Theme::hex(Theme::colors().surface2))));
    rt::UiScale::setScaledFixedWidth(m_timecodeEdit, 120);
    m_timecodeEdit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timecodeEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("\\d{0,2}:?\\d{0,2}:?\\d{0,2}:?\\d{0,2}")), m_timecodeEdit));
    m_timecodeEdit->setPlaceholderText(QStringLiteral("HH:MM:SS:FF"));
    m_timecodeEdit->hide();

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

    controlLayout->addWidget(m_timecodeLabel, 0, Qt::AlignVCenter);
    controlLayout->addWidget(m_timecodeEdit, 0, Qt::AlignVCenter);
    controlLayout->addSpacing(rt::UiScale::px(12));

    // Fit mode / zoom presets combo box
    m_fitModeCombo = new QComboBox(this);
    m_fitModeCombo->addItem(tr("Fit"));     // 0
    m_fitModeCombo->addItem(tr("Fill"));    // 1
    m_fitModeCombo->addItem(tr("25%"));     // 2
    m_fitModeCombo->addItem(tr("50%"));     // 3
    m_fitModeCombo->addItem(tr("75%"));     // 4
    m_fitModeCombo->addItem(tr("100%"));    // 5
    m_fitModeCombo->addItem(tr("150%"));    // 6
    m_fitModeCombo->addItem(tr("200%"));    // 7
    m_fitModeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_fitModeCombo->setFocusPolicy(Qt::NoFocus);
    m_fitModeCombo->setStyleSheet(comboStyle);
    rt::UiScale::setScaledMinimumWidth(m_fitModeCombo, 80);
    rt::UiScale::setScaledFixedHeight(m_fitModeCombo, 24);
    controlLayout->addWidget(m_fitModeCombo, 0, Qt::AlignVCenter);

    controlLayout->addStretch();

    // Playback resolution dropdown.  Indices:
    //   0 = Full         — manual divisor 1
    //   1 = 1/2          — manual divisor 2
    //   2 = 1/4          — manual divisor 4
    //   3 = 1/8          — manual divisor 8 (composite-only; decode tier
    //                       still caps at Quarter)
    //   4 = Auto         — UPGRADE_PLAN item 1: FrameProducer adjusts the
    //                       divisor between Full/Half/Quarter from rolling
    //                       composite latency vs. frame budget.
    m_playbackResCombo = new QComboBox(this);
    m_playbackResCombo->addItem(tr("Full"));
    m_playbackResCombo->addItem(QStringLiteral("1/2"));
    m_playbackResCombo->addItem(QStringLiteral("1/4"));
    m_playbackResCombo->addItem(QStringLiteral("1/8"));
    m_playbackResCombo->addItem(tr("Auto"));
    m_playbackResCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_playbackResCombo->setFocusPolicy(Qt::NoFocus);
    m_playbackResCombo->setToolTip(tr("Playback Resolution"));
    m_playbackResCombo->setStyleSheet(comboStyle);
    rt::UiScale::setScaledMinimumWidth(m_playbackResCombo, 70);
    rt::UiScale::setScaledFixedHeight(m_playbackResCombo, 24);
    static constexpr int kAutoIdx = 4;
    // Restore the last-used playback resolution (Premiere-style: the
    // dropdown reopens with whatever it was closed at). Defaults to 1/2
    // (index 1, matches Half-tier decode) on first run. Set the divisor
    // member directly too — setPlaybackTierCallback() fires once with
    // m_playbackResDivisor on startup, so the restored tier must be in
    // place before the combo's currentIndexChanged slot is connected.
    {
        int savedIdx = QSettings().value(
            QStringLiteral("playback/resolutionIndex"), 1).toInt();
        if (savedIdx < 0 || savedIdx > kAutoIdx) savedIdx = 1;
        static constexpr int kDivisors[] = {1, 2, 4, 8};
        // Auto starts at the same effective divisor as 1/2; the producer
        // takes it from there.  Anything else uses the manual table.
        m_playbackResAdaptive = (savedIdx == kAutoIdx);
        m_playbackResDivisor  = m_playbackResAdaptive ? 2 : kDivisors[savedIdx];
        m_playbackResCombo->setCurrentIndex(savedIdx);
    }
    controlLayout->addWidget(m_playbackResCombo, 0, Qt::AlignVCenter);

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

    // Resolution/framerate overlay label (hidden — Premiere doesn't show inline)
    m_resOverlayLabel = new QLabel(QStringLiteral("1920\u00d71080 \u2022 24fps"), this);
    m_resOverlayLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-size: 10px; color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().textTertiary))));
    m_resOverlayLabel->hide();

    // Dropped frame counter (Premiere Pro style yellow indicator)
    m_droppedFrameLabel = new QLabel(this);
    m_droppedFrameLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-size: 10px; font-weight: bold; color: #FFD700; padding: 0 4px; background: transparent; }")));
    m_droppedFrameLabel->setToolTip(tr("Dropped frames during playback"));
    m_droppedFrameLabel->hide(); // Hidden until drops occur
    controlLayout->addWidget(m_droppedFrameLabel, 0, Qt::AlignVCenter);

    // Safe Area toggle button (visible, Premiere Pro style icon)
    auto checkedBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; padding: 3px; } "
        "QPushButton:hover { background: %2; } "
        "QPushButton:checked { background: %3; border-color: %3; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent)));

    // Draw a Premiere Pro-style safe margins icon (nested rectangles)
    auto makeSafeIcon = [](QColor fg) -> QIcon {
        QPixmap px(24, 24);
        px.fill(Qt::transparent);
        QPainter ip(&px);
        ip.setRenderHint(QPainter::Antialiasing, false);
        ip.setPen(QPen(fg, 1.5));
        ip.setBrush(Qt::NoBrush);
        ip.drawRect(QRectF(1.5, 1.5, 21, 21));   // outer frame
        ip.drawRect(QRectF(5.5, 5.5, 13, 13));    // action-safe
        ip.setPen(QPen(fg, 1.0, Qt::DashLine));
        ip.drawRect(QRectF(8.5, 8.5, 7, 7));      // title-safe
        ip.end();
        return QIcon(px);
    };
    QIcon safeIcon = makeSafeIcon(Theme::colors().textSecondary);

    m_btnSafeArea = new QPushButton(this);
    m_btnSafeArea->setIcon(safeIcon);
    m_btnSafeArea->setIconSize(QSize(rt::UiScale::px(16), rt::UiScale::px(16)));
    m_btnSafeArea->setCheckable(true);
    rt::UiScale::setScaledFixedSize(m_btnSafeArea, 24, 22);
    m_btnSafeArea->setFocusPolicy(Qt::NoFocus);
    m_btnSafeArea->setToolTip(tr("Toggle Safe Area Overlay"));
    m_btnSafeArea->setStyleSheet(checkedBtnStyle);
    controlLayout->addWidget(m_btnSafeArea, 0, Qt::AlignVCenter);

    connect(m_btnSafeArea, &QPushButton::toggled, this, [this](bool checked) {
        m_viewport->setSafeAreasVisible(checked);
        if (m_transformOverlay)
            m_transformOverlay->setSafeAreasVisible(checked);
    });

    // Export frame button (large, Premiere Pro style)
    auto exportBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2; "
        "border-radius: 4px; color: %3; font-size: 14px; padding: 1px; } "
        "QPushButton:hover { background: %4; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textBright))
        .arg(Theme::hex(Theme::colors().controlBgHover)));
    m_btnExportFrame = new QPushButton(QStringLiteral("📷"), this);
    m_btnExportFrame->setToolTip(tr("Export Frame (Ctrl+Shift+E)"));
    rt::UiScale::setScaledFixedSize(m_btnExportFrame, 28, 22);
    m_btnExportFrame->setFocusPolicy(Qt::NoFocus);
    m_btnExportFrame->setStyleSheet(exportBtnStyle);
    controlLayout->addWidget(m_btnExportFrame, 0, Qt::AlignVCenter);

    connect(m_btnExportFrame, &QPushButton::clicked, this, [this]() {
        emit exportFrameRequested();
    });

    // Zoom percentage label (hidden — shown in status bar instead)
    m_zoomLabel = new QLabel(QStringLiteral("100%"), this);
    m_zoomLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 11px; "
        "color: %1; padding: 0 4px; background: transparent; }")
        .arg(Theme::hex(Theme::colors().textSecondary))));
    rt::UiScale::setScaledMinimumWidth(m_zoomLabel, 60);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->hide();

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
    m_durationLabel = new QLabel(QStringLiteral("00:00:00:00"), this);
    m_durationLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 14px; "
        "color: %1; background: transparent; padding: 0px 8px 0px 6px; }")
        .arg(Theme::hex(Theme::colors().textSecondary))));
    rt::UiScale::setScaledFixedWidth(m_durationLabel, 120);
    m_durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    controlLayout->addWidget(m_durationLabel, 0, Qt::AlignVCenter);

    // Hidden checkable button for grid (used by settings menu)
    m_btnGrid = new QPushButton(this);
    m_btnGrid->setCheckable(true);
    m_btnGrid->hide();
    connect(m_btnGrid, &QPushButton::toggled, this, [this](bool checked) {
        if (m_transformOverlay)
            m_transformOverlay->setGridVisible(checked);
    });

    mainLayout->addWidget(controlBar);
    mainLayout->addSpacing(rt::UiScale::px(4));   // gap between control bar and mini-timeline

    // Give the mini-timeline a distinct background so it's clearly visible
    m_miniTimeline->setMinimumHeight(rt::UiScale::px(56));
    mainLayout->addWidget(m_miniTimeline);

    // ── Transport controls (Premiere Pro style) ─────────────────────────
    auto* transportBar = new QWidget(this);
    transportBar->setObjectName(QStringLiteral("TransportBar"));
    rt::UiScale::setScaledFixedHeight(transportBar, 36);
    transportBar->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "#TransportBar { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().border))));

    auto* transportLayout = new QHBoxLayout(transportBar);
    transportLayout->setContentsMargins(rt::UiScale::px(8), 0, rt::UiScale::px(8), 0);
    transportLayout->setSpacing(rt::UiScale::px(4));
    transportLayout->addStretch();

    m_btnGoStart     = new TransportButton(TransportButton::GoStart, transportBar);
    m_btnStepBack    = new TransportButton(TransportButton::StepBack, transportBar);
    m_btnPlayPause   = new TransportButton(TransportButton::Play, transportBar);
    m_btnStop        = new TransportButton(TransportButton::Stop, transportBar);
    m_btnStepForward = new TransportButton(TransportButton::StepForward, transportBar);
    m_btnGoEnd       = new TransportButton(TransportButton::GoEnd, transportBar);

    rt::UiScale::setScaledFixedSize(m_btnGoStart, 22, 22);
    m_btnGoStart->setToolTip(tr("Go to Start"));
    rt::UiScale::setScaledFixedSize(m_btnStepBack, 22, 22);
    m_btnStepBack->setToolTip(tr("Step Back"));
    rt::UiScale::setScaledFixedSize(m_btnPlayPause, 26, 26);
    m_btnPlayPause->setToolTip(tr("Play/Pause"));
    rt::UiScale::setScaledFixedSize(m_btnStop, 22, 22);
    m_btnStop->setToolTip(tr("Stop"));
    rt::UiScale::setScaledFixedSize(m_btnStepForward, 22, 22);
    m_btnStepForward->setToolTip(tr("Step Forward"));
    rt::UiScale::setScaledFixedSize(m_btnGoEnd, 22, 22);
    m_btnGoEnd->setToolTip(tr("Go to End"));

    transportLayout->addWidget(m_btnGoStart, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStepBack, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnPlayPause, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStop, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnStepForward, 0, Qt::AlignVCenter);
    transportLayout->addWidget(m_btnGoEnd, 0, Qt::AlignVCenter);

    // Vertical divider + screenshot button (right next to transport controls)
    auto* transportDivider = new QFrame(transportBar);
    transportDivider->setFrameShape(QFrame::VLine);
    rt::UiScale::setScaledFixedHeight(transportDivider, 16);
    transportDivider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; }").arg(Theme::hex(Theme::colors().border)));
    transportLayout->addSpacing(rt::UiScale::px(4));
    transportLayout->addWidget(transportDivider, 0, Qt::AlignVCenter);
    transportLayout->addSpacing(rt::UiScale::px(4));

    m_btnScreenshot = new TransportButton(TransportButton::Screenshot, transportBar);
    m_btnScreenshot->setToolTip(tr("Take Screenshot"));
    rt::UiScale::setScaledFixedSize(m_btnScreenshot, 22, 22);
    transportLayout->addWidget(m_btnScreenshot, 0, Qt::AlignVCenter);

    connect(m_btnScreenshot, &QPushButton::clicked, this, [this]() {
        emit exportFrameRequested();
    });

    // Loop toggle button
    auto loopBtnStyle = rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; "
        "border-radius: 3px; color: %2; font-size: 10px; font-weight: bold; "
        "padding: 2px 6px; } "
        "QPushButton:hover { background: %3; } "
        "QPushButton:checked { background: %4; color: %5; border-color: %4; }")
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().textBright)));

    auto* loopDivider = new QFrame(transportBar);
    loopDivider->setFrameShape(QFrame::VLine);
    rt::UiScale::setScaledFixedHeight(loopDivider, 16);
    loopDivider->setStyleSheet(QStringLiteral(
        "QFrame { color: %1; }").arg(Theme::hex(Theme::colors().border)));
    transportLayout->addSpacing(rt::UiScale::px(4));
    transportLayout->addWidget(loopDivider, 0, Qt::AlignVCenter);
    transportLayout->addSpacing(rt::UiScale::px(4));

    m_btnLoop = new QPushButton(tr("Loop"), transportBar);
    m_btnLoop->setCheckable(true);
    rt::UiScale::setScaledFixedHeight(m_btnLoop, 22);
    m_btnLoop->setFocusPolicy(Qt::NoFocus);
    m_btnLoop->setToolTip(tr("Toggle Loop Playback"));
    m_btnLoop->setStyleSheet(loopBtnStyle);
    transportLayout->addWidget(m_btnLoop, 0, Qt::AlignVCenter);

    connect(m_btnLoop, &QPushButton::toggled, this, [this](bool checked) {
        if (m_controller) m_controller->setLoopEnabled(checked);
    });

    // Shuttle speed display label
    m_shuttleSpeedLabel = new QLabel(transportBar);
    m_shuttleSpeedLabel->setStyleSheet(rt::UiScale::scaleStyleSheet(QStringLiteral(
        "QLabel { font-family: 'Consolas', monospace; font-size: 12px; "
        "font-weight: bold; color: #FFD700; background: transparent; "
        "padding: 0px 8px; }")));
    rt::UiScale::setScaledMinimumWidth(m_shuttleSpeedLabel, 50);
    m_shuttleSpeedLabel->setAlignment(Qt::AlignCenter);
    m_shuttleSpeedLabel->hide();
    transportLayout->addWidget(m_shuttleSpeedLabel);

    // Frame drop indicator — green/yellow/red dot, hidden when no drops
    m_dropIndicator = new QLabel(transportBar);
    m_dropIndicator->setFixedSize(10, 10);
    m_dropIndicator->setToolTip(tr("Dropped frames"));
    m_dropIndicator->hide();
    transportLayout->addWidget(m_dropIndicator);

    transportLayout->addStretch();

    // Connect transport buttons
    connect(m_btnGoStart, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->goToStart();
    });
    connect(m_btnStepBack, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->stepBackward();
    });
    connect(m_btnPlayPause, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->togglePlayPause();
    });
    connect(m_btnStop, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->stop();
    });
    connect(m_btnStepForward, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->stepForward();
    });
    connect(m_btnGoEnd, &QPushButton::clicked, this, [this]() {
        if (m_controller) m_controller->goToEnd();
    });

    mainLayout->addWidget(transportBar);
}

} // namespace rt
