/*
 * ExportPanelUI.cpp - UI setup for ExportPanel.
 * Split from ExportPanel.cpp for maintainability.
 */
#include "ExportPanel.h"
#include "ExportMiniTimeline.h"
#include "Theme.h"
#include "PathUtils.h"

#include "Encoder.h"
#include "Muxer.h"

#include "media/AudioEngine.h"
#include "media/FrameCache.h"
#include "media/PlaybackController.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSplitter>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

void ExportPanel::setupUI()
{
    const auto& m = Theme::metrics();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(m.spacingXl, m.spacingXl, m.spacingXl, m.spacingXl);
    mainLayout->setSpacing(m.spacingLg);

    // â”€â”€ Title â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* title = new QLabel(tr("Export"), this);
    title->setObjectName(QStringLiteral("PanelTitle"));
    mainLayout->addWidget(title);

    // â”€â”€ Horizontal splitter: Settings (left) | Preview (right) â”€â”€â”€â”€â”€
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // LEFT: Settings panel (scrollable)
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* settingsScroll = new QScrollArea;
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* settingsWidget = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsWidget);
    settingsLayout->setContentsMargins(0, 0, 10, 0);
    settingsLayout->setSpacing(m.spacingSm);

    // â”€â”€ Output Path â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* outputGroup = new QGroupBox(tr("OUTPUT"), settingsWidget);
    auto* outputLayout = new QVBoxLayout(outputGroup);
    outputLayout->setSpacing(m.spacingSm);

    auto* outputRow = new QHBoxLayout();
    m_outputPath = new QLineEdit(outputGroup);
    m_outputPath->setToolTip(tr("Output file path for the exported video"));
    m_outputPath->setPlaceholderText(tr("Select output file..."));
    m_browseButton = new QPushButton(tr("Browse"), outputGroup);
    m_browseButton->setToolTip(tr("Browse for output file location"));
    m_browseButton->setObjectName(QStringLiteral("BrowseBtn"));
    connect(m_browseButton, &QPushButton::clicked, this, &ExportPanel::onBrowseOutput);
    outputRow->addWidget(m_outputPath, 1);
    outputRow->addWidget(m_browseButton);
    outputLayout->addLayout(outputRow);

    // Estimated file size
    m_estimateLabel = new QLabel(tr(""), outputGroup);
    m_estimateLabel->setObjectName(QStringLiteral("EstimateLbl"));
    outputLayout->addWidget(m_estimateLabel);

    settingsLayout->addWidget(outputGroup);

    // â”€â”€ Preset â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* presetGroup = new QGroupBox(tr("PRESET"), settingsWidget);
    auto* presetLayout = new QHBoxLayout(presetGroup);
    m_presetCombo = new QComboBox(presetGroup);
    m_presetCombo->setToolTip(tr("Select an export preset"));
    populatePresets();
    presetLayout->addWidget(m_presetCombo, 1);
    m_savePresetBtn = new QPushButton(tr("Save..."), presetGroup);
    m_savePresetBtn->setToolTip(tr("Save current export settings as a new preset"));
    m_deletePresetBtn = new QPushButton(tr("Delete"), presetGroup);
    m_deletePresetBtn->setToolTip(tr("Delete the selected custom preset"));
    m_deletePresetBtn->setEnabled(false);
    presetLayout->addWidget(m_savePresetBtn);
    presetLayout->addWidget(m_deletePresetBtn);
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onPresetChanged);
    connect(m_savePresetBtn, &QPushButton::clicked, this, &ExportPanel::onSavePreset);
    connect(m_deletePresetBtn, &QPushButton::clicked, this, &ExportPanel::onDeletePreset);
    settingsLayout->addWidget(presetGroup);
    // Match Sequence Settings checkbox (Premiere Pro style)
    m_matchSequenceCheck = new QCheckBox(tr("Match Sequence Settings"), settingsWidget);
    m_matchSequenceCheck->setToolTip(
        tr("When checked, resolution and frame rate are locked to the active sequence settings."));
    connect(m_matchSequenceCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            syncMatchSequenceSettings();
        // Null-check guards: the checkbox is created before the video
        // widgets in setupUI(), so the toggled signal may fire before
        // m_widthSpin/m_heightSpin/m_fpsCombo are constructed.
        if (m_widthSpin)  m_widthSpin->setEnabled(!checked);
        if (m_heightSpin) m_heightSpin->setEnabled(!checked);
        if (m_fpsCombo)   m_fpsCombo->setEnabled(!checked);
    });
    m_matchSequenceCheck->setChecked(true);
    settingsLayout->addWidget(m_matchSequenceCheck);

    // â”€â”€ Video Settings â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* videoGroup = new QGroupBox(tr("VIDEO"), settingsWidget);
    auto* videoForm  = new QFormLayout(videoGroup);
    videoForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    videoForm->setVerticalSpacing(8);
    videoForm->setHorizontalSpacing(10);

    // Resolution
    auto* resLayout = new QHBoxLayout();
    resLayout->setSpacing(m.spacingSm);
    m_widthSpin = new QSpinBox(videoGroup);
    m_widthSpin->setToolTip(tr("Output video width in pixels"));
    m_widthSpin->setRange(128, 7680);
    m_widthSpin->setValue(1920);
    m_widthSpin->setSingleStep(2);
    m_heightSpin = new QSpinBox(videoGroup);
    m_heightSpin->setToolTip(tr("Output video height in pixels"));
    m_heightSpin->setRange(128, 4320);
    m_heightSpin->setValue(1080);
    m_heightSpin->setSingleStep(2);
    auto* xLabel = new QLabel(QStringLiteral("\u00D7"), videoGroup); // Ã—
    xLabel->setAlignment(Qt::AlignCenter);
    xLabel->setFixedWidth(16);
    resLayout->addWidget(m_widthSpin, 1);
    resLayout->addWidget(xLabel);
    resLayout->addWidget(m_heightSpin, 1);
    videoForm->addRow(tr("Resolution"), resLayout);

    // FPS
    m_fpsCombo = new QComboBox(videoGroup);
    m_fpsCombo->setToolTip(tr("Output frame rate"));
    m_fpsCombo->addItem(QStringLiteral("23.976"), 24);
    m_fpsCombo->addItem(QStringLiteral("24"), 24);
    m_fpsCombo->addItem(QStringLiteral("25"), 25);
    m_fpsCombo->addItem(QStringLiteral("29.97"), 30);
    m_fpsCombo->addItem(QStringLiteral("30"), 30);
    m_fpsCombo->addItem(QStringLiteral("50"), 50);
    m_fpsCombo->addItem(QStringLiteral("59.94"), 60);
    m_fpsCombo->addItem(QStringLiteral("60"), 60);
    m_fpsCombo->setCurrentIndex(4); // 30 fps
    videoForm->addRow(tr("Frame Rate"), m_fpsCombo);

    // Codec
    m_codecCombo = new QComboBox(videoGroup);
    m_codecCombo->setToolTip(tr("Video codec for encoding"));
    populateCodecs();
    connect(m_codecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onCodecChanged);
    videoForm->addRow(tr("Codec"), m_codecCombo);

    // Hardware acceleration
    m_accelCombo = new QComboBox(videoGroup);
    m_accelCombo->setToolTip(tr("Hardware acceleration mode for encoding"));
    populateAccel();
    videoForm->addRow(tr("Acceleration"), m_accelCombo);

    // Quality (user-friendly slider â†’ internal CRF)
    auto* crfLayout = new QHBoxLayout();
    crfLayout->setSpacing(8);
    m_crfSlider = new QSlider(Qt::Horizontal, videoGroup);
    m_crfSlider->setToolTip(tr("Quality level (higher = better quality, larger file)"));
    m_crfSlider->setRange(0, 100);
    m_crfSlider->setValue(75);  // Default = High quality
    m_crfSlider->setTickPosition(QSlider::TicksBelow);
    m_crfSlider->setTickInterval(25);
    m_crfLabel = new QLabel(QStringLiteral("High"), videoGroup);
    m_crfLabel->setMinimumWidth(60);
    m_crfLabel->setAlignment(Qt::AlignCenter);
    connect(m_crfSlider, &QSlider::valueChanged, this, &ExportPanel::onCrfChanged);
    crfLayout->addWidget(m_crfSlider, 1);
    crfLayout->addWidget(m_crfLabel);
    videoForm->addRow(tr("Quality"), crfLayout);

    // Container
    m_containerCombo = new QComboBox(videoGroup);
    m_containerCombo->setToolTip(tr("Output container format"));
    populateContainers();
    videoForm->addRow(tr("Container"), m_containerCombo);

    settingsLayout->addWidget(videoGroup);

    // â”€â”€ Audio â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* audioGroup = new QGroupBox(tr("AUDIO"), settingsWidget);
    auto* audioLayout = new QHBoxLayout(audioGroup);
    m_audioCheck = new QCheckBox(tr("Include Audio"), audioGroup);
    m_audioCheck->setToolTip(tr("Include audio tracks in the exported file"));
    m_audioCheck->setChecked(true);
    audioLayout->addWidget(m_audioCheck);
    settingsLayout->addWidget(audioGroup);

    // â”€â”€ Range â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* rangeGroup = new QGroupBox(tr("RANGE"), settingsWidget);
    auto* rangeLayout = new QHBoxLayout(rangeGroup);
    m_rangeCombo = new QComboBox(rangeGroup);
    m_rangeCombo->setToolTip(tr("Export the entire sequence or only the In-to-Out range"));
    m_rangeCombo->addItem(tr("Entire Sequence"), 0);
    m_rangeCombo->addItem(tr("In to Out"), 1);
    connect(m_rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onRangeChanged);
    rangeLayout->addWidget(m_rangeCombo);
    settingsLayout->addWidget(rangeGroup);

    settingsLayout->addStretch();

    settingsScroll->setWidget(settingsWidget);
    splitter->addWidget(settingsScroll);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // RIGHT: Preview panel
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* previewWidget = new QWidget;
    auto* previewLayout = new QVBoxLayout(previewWidget);
    previewLayout->setContentsMargins(10, 0, 0, 0);
    previewLayout->setSpacing(8);

    auto* previewLabel = new QLabel(tr("Preview"));
    previewLabel->setObjectName(QStringLiteral("SectionTitle"));
    previewLayout->addWidget(previewLabel);

    // Live preview area
    m_previewImageLabel = new QLabel;
    m_previewImageLabel->setMinimumSize(640, 360);
    m_previewImageLabel->setAlignment(Qt::AlignCenter);
    m_previewImageLabel->setObjectName("PreviewLabel");
    m_previewImageLabel->setText(tr("Export Preview"));
    m_previewImageLabel->setScaledContents(false);
    previewLayout->addWidget(m_previewImageLabel, 1);

    // Mini timeline scrub bar (Premiere Proâ€“style)
    m_miniTimeline = new ExportMiniTimeline(previewWidget);
    connect(m_miniTimeline, &ExportMiniTimeline::scrubbed, this, [this](int64_t tick) {
        // Stop playback if scrubbing manually
        if (m_playing) {
            m_playing = false;
            m_playbackTimer->stop();
            m_playPauseBtn->setText(QStringLiteral("\u25B6")); // ▶
            if (m_playbackController) {
                m_playbackController->pause();
                // Restore the main timeline's saved playhead so scrubbing
                // in the export preview doesn't move the main timeline.
                if (m_savedMainPlayhead >= 0) {
                    m_playbackController->seekTo(m_savedMainPlayhead);
                    m_savedMainPlayhead = -1;
                }
            }
        }

        // Do NOT seek the shared PlaybackController here — the export
        // preview has its own independent playhead.  Audio scrub still
        // works via m_audioEngine->scrub() below.

        // Play a short audio burst at the scrub position
        if (m_audioEngine) {
            int64_t frame = tick; // ticks == frames at 48 kHz
            m_audioEngine->scrub(frame);
        }

        if (!m_previewCallback || !m_previewImageLabel) return;

        uint32_t renderW = m_widthSpin  ? static_cast<uint32_t>(m_widthSpin->value())  : 1920;
        uint32_t renderH = m_heightSpin ? static_cast<uint32_t>(m_heightSpin->value()) : 1080;

        auto frame = m_previewCallback(tick, renderW, renderH, true);
        if (frame && frame->ensurePixels() && frame->width > 0 && frame->height > 0) {
            uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
            QImage img(frame->pixels.data(), static_cast<int>(frame->width),
                       static_cast<int>(frame->height), static_cast<int>(stride),
                       QImage::Format_ARGB32);
            QPixmap pix = QPixmap::fromImage(img).scaled(
                m_previewImageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_previewImageLabel->setPixmap(pix);
        }
    });
    previewLayout->addWidget(m_miniTimeline);

    // Transport controls bar
    {
        auto* transportBar = new QHBoxLayout();
        transportBar->setContentsMargins(0, m.spacingXs, 0, m.spacingXs);
        transportBar->setSpacing(m.spacingSm);

        m_skipToStartBtn  = new QPushButton(QStringLiteral("\u23EE"), previewWidget); // â®
        m_stepBackBtn     = new QPushButton(QStringLiteral("\u23F4"), previewWidget); // â´
        m_playPauseBtn    = new QPushButton(QStringLiteral("\u25B6"), previewWidget); // â–¶
        m_stepForwardBtn  = new QPushButton(QStringLiteral("\u23F5"), previewWidget); // âµ
        m_skipToEndBtn    = new QPushButton(QStringLiteral("\u23ED"), previewWidget); // â­

        m_skipToStartBtn->setToolTip(tr("Skip to Start (Home)"));
        m_stepBackBtn->setToolTip(tr("Step Back (Left)"));
        m_playPauseBtn->setToolTip(tr("Play / Pause (Space)"));
        m_stepForwardBtn->setToolTip(tr("Step Forward (Right)"));
        m_skipToEndBtn->setToolTip(tr("Skip to End (End)"));

        for (auto* btn : {m_skipToStartBtn, m_stepBackBtn, m_playPauseBtn,
                          m_stepForwardBtn, m_skipToEndBtn}) {
            btn->setObjectName(QStringLiteral("TransportBtn"));
            btn->setFocusPolicy(Qt::NoFocus);
            btn->setCursor(Qt::PointingHandCursor);
        }

        transportBar->addStretch();
        transportBar->addWidget(m_skipToStartBtn);
        transportBar->addWidget(m_stepBackBtn);
        transportBar->addWidget(m_playPauseBtn);
        transportBar->addWidget(m_stepForwardBtn);
        transportBar->addWidget(m_skipToEndBtn);

        // ── In/Out point controls ──────────────────────────────────────
        transportBar->addSpacing(12);
        m_inPointBtn = new QPushButton(QStringLiteral("\u25B6|"), previewWidget);
        m_inPointBtn->setToolTip(tr("Set In Point (I)"));
        m_inPointBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_inPointBtn->setFocusPolicy(Qt::NoFocus);
        m_inPointBtn->setCursor(Qt::PointingHandCursor);
        connect(m_inPointBtn, &QPushButton::clicked, this, &ExportPanel::onSetInPoint);

        m_outPointBtn = new QPushButton(QStringLiteral("|\u25C0"), previewWidget);
        m_outPointBtn->setToolTip(tr("Set Out Point (O)"));
        m_outPointBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_outPointBtn->setFocusPolicy(Qt::NoFocus);
        m_outPointBtn->setCursor(Qt::PointingHandCursor);
        connect(m_outPointBtn, &QPushButton::clicked, this, &ExportPanel::onSetOutPoint);

        m_clearInOutBtn = new QPushButton(QStringLiteral("\u2716"), previewWidget);
        m_clearInOutBtn->setToolTip(tr("Clear In/Out Points (Ctrl+Shift+X / Delete)"));
        m_clearInOutBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_clearInOutBtn->setFocusPolicy(Qt::NoFocus);
        m_clearInOutBtn->setCursor(Qt::PointingHandCursor);
        connect(m_clearInOutBtn, &QPushButton::clicked, this, &ExportPanel::onClearInOut);

        transportBar->addWidget(m_inPointBtn);
        transportBar->addWidget(m_outPointBtn);
        transportBar->addWidget(m_clearInOutBtn);

        transportBar->addStretch();

        previewLayout->addLayout(transportBar);

        // Playback timer
        m_playbackTimer = new QTimer(this);
        m_playbackTimer->setTimerType(Qt::PreciseTimer);
        connect(m_playbackTimer, &QTimer::timeout, this, &ExportPanel::onPlaybackTick);

        // Connect transport buttons
        connect(m_skipToStartBtn,  &QPushButton::clicked, this, &ExportPanel::onSkipToStart);
        connect(m_stepBackBtn,     &QPushButton::clicked, this, &ExportPanel::onStepBack);
        connect(m_playPauseBtn,    &QPushButton::clicked, this, &ExportPanel::onPlayPause);
        connect(m_stepForwardBtn,  &QPushButton::clicked, this, &ExportPanel::onStepForward);
        connect(m_skipToEndBtn,    &QPushButton::clicked, this, &ExportPanel::onSkipToEnd);
    }

    // Info label
    m_previewInfoLabel = new QLabel(tr("No sequence loaded"));
    m_previewInfoLabel->setObjectName(QStringLiteral("EstimateLbl"));
    previewLayout->addWidget(m_previewInfoLabel);

    splitter->addWidget(previewWidget);

    splitter->setSizes({380, 620});
    mainLayout->addWidget(splitter, 1);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // BOTTOM: Progress + actions
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    // Progress bar
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(QStringLiteral("%p%"));
    mainLayout->addWidget(m_progressBar);

    // Status + render stats
    m_statusLabel = new QLabel(tr("Ready"), this);
    mainLayout->addWidget(m_statusLabel);

    // Action buttons row
    auto* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setToolTip(tr("Cancel the in-progress export"));
    m_cancelButton->setObjectName(QStringLiteral("CancelBtn"));
    m_cancelButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    connect(m_cancelButton, &QPushButton::clicked, this, &ExportPanel::onCancelExport);
    actionLayout->addWidget(m_cancelButton);

    actionLayout->addStretch();

    m_addQueueButton = new QPushButton(tr("Add to Queue"), this);
    m_addQueueButton->setToolTip(tr("Add current settings to the export queue"));
    m_addQueueButton->setObjectName(QStringLiteral("AddQueueBtn"));
    connect(m_addQueueButton, &QPushButton::clicked, this, &ExportPanel::onAddToQueue);
    actionLayout->addWidget(m_addQueueButton);

    m_startButton = new QPushButton(tr("Export"), this);
    m_startButton->setToolTip(tr("Start exporting"));
    m_startButton->setObjectName(QStringLiteral("ExportBtn"));
    connect(m_startButton, &QPushButton::clicked, this, &ExportPanel::onStartExport);
    actionLayout->addWidget(m_startButton);

    mainLayout->addLayout(actionLayout);

    // â”€â”€ Job Queue â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_jobList = new QListWidget(this);
    m_jobList->setToolTip(tr("Export job queue (right-click for options)"));
    m_jobList->setObjectName(QStringLiteral("JobList"));
    m_jobList->setMaximumHeight(120);
    m_jobList->setVisible(false); // Show when jobs are added
    m_jobList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_jobList, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = m_jobList->itemAt(pos);
        if (!item) return;
        QMenu menu(this);
        auto* removeAction = menu.addAction(tr("Remove from Queue"));

        // Reveal in Explorer — only shown if the job has a valid output path
        QAction* revealAction = nullptr;
        bool ok = false;
        uint32_t jobId = static_cast<uint32_t>(item->data(Qt::UserRole).toULongLong(&ok));
        if (ok && m_renderQueue) {
            const auto* job = m_renderQueue->job(jobId);
            if (job && QFileInfo(QString::fromStdString(pathToUtf8(job->config.outputPath))).exists()) {
                revealAction = menu.addAction(tr("Reveal in Explorer"));
            }
        }

        auto* chosen = menu.exec(m_jobList->mapToGlobal(pos));
        if (chosen == removeAction) {
            int row = m_jobList->row(item);
            delete m_jobList->takeItem(row);
            if (m_jobList->count() == 0)
                m_jobList->setVisible(false);
        } else if (revealAction && chosen == revealAction) {
            const auto* job = m_renderQueue->job(jobId);
            if (job) {
                QString path = QString::fromStdString(pathToUtf8(job->config.outputPath));
                QFileInfo fi(path);
                if (fi.exists()) {
                    QProcess::startDetached("explorer.exe",
                        {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
                }
            }
        }
    });
    mainLayout->addWidget(m_jobList);

    // â”€â”€ Wire up estimate updates â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto updateEstimate = [this]() { updateFileEstimate(); };
    connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateEstimate);
    connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateEstimate);
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateEstimate);
    connect(m_codecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateEstimate);
    connect(m_crfSlider, &QSlider::valueChanged, this, updateEstimate);
}


} // namespace rt
