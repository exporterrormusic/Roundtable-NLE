/*
 * ExportPanelUI.cpp - UI setup for ExportPanel.
 * Split from ExportPanel.cpp for maintainability.
 *
 * Premiere-style layout: a header block (File Name / Location / Preset /
 * Format) over collapsible VIDEO / AUDIO / RANGE sections. VIDEO and AUDIO
 * each carry a blue enable switch; turning VIDEO off (or just leaving AUDIO on)
 * makes the export audio-only.
 */
#include "ExportPanel.h"
#include "ExportMiniTimeline.h"
#include "Theme.h"
#include "PathUtils.h"

#include "Encoder.h"
#include "Muxer.h"

#include "widgets/CollapsibleSection.h"
#include "widgets/ToggleSwitch.h"

#include "audio/AudioEngine.h"
#include "cache/FrameCache.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSplitter>
#include <QToolButton>

#include <cmath>

#include <spdlog/spdlog.h>

namespace rt {

void ExportPanel::setupUI()
{
    const auto& m = Theme::metrics();

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(m.spacingXl, m.spacingXl, m.spacingXl, m.spacingXl);
    mainLayout->setSpacing(m.spacingLg);

    // ── Title ───────────────────────────────────────────────────────────
    auto* title = new QLabel(tr("Export"), this);
    title->setObjectName(QStringLiteral("PanelTitle"));
    mainLayout->addWidget(title);

    // ── Horizontal splitter: Settings (left) | Preview (right) ──────────
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    // ════════════════════════════════════════════════════════════════════
    // LEFT: Settings panel (scrollable)
    // ════════════════════════════════════════════════════════════════════
    auto* settingsScroll = new QScrollArea;
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    settingsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* settingsWidget = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsWidget);
    settingsLayout->setContentsMargins(0, 0, 10, 0);
    settingsLayout->setSpacing(m.spacingMd);

    // ── Header: File Name / Location / Preset / Format ──────────────────
    // m_outputPath holds the authoritative full path (hidden); File Name and
    // Location are editable views onto it.
    m_outputPath = new QLineEdit(settingsWidget);
    m_outputPath->setVisible(false);

    auto* headerForm = new QFormLayout();
    headerForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    headerForm->setVerticalSpacing(m.spacingMd);
    headerForm->setHorizontalSpacing(m.spacingLg);

    // File Name
    m_fileNameEdit = new QLineEdit(settingsWidget);
    m_fileNameEdit->setPlaceholderText(tr("export.mp4"));
    m_fileNameEdit->setToolTip(tr("Output file name"));
    connect(m_fileNameEdit, &QLineEdit::editingFinished, this,
            [this] { syncOutputPathFromParts(); updateFileEstimate(); });
    headerForm->addRow(tr("File Name"), m_fileNameEdit);

    // Location (clickable link to change the folder) + Browse for a full path
    auto* locRow = new QHBoxLayout();
    locRow->setSpacing(m.spacingSm);
    m_locationLabel = new QLabel(settingsWidget);
    m_locationLabel->setTextFormat(Qt::RichText);
    m_locationLabel->setToolTip(tr("Output folder — click to change"));
    m_locationLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_locationLabel->setOpenExternalLinks(false);
    connect(m_locationLabel, &QLabel::linkActivated, this, [this](const QString&) {
        const QString start = QFileInfo(m_outputPath->text()).absolutePath();
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Choose Output Folder"), start);
        if (dir.isEmpty()) return;
        QString name = m_fileNameEdit->text().trimmed();
        if (name.isEmpty()) name = QStringLiteral("export");
        m_outputPath->setText(dir + QStringLiteral("/") + name);
        syncPartsFromOutputPath();
        updateFileEstimate();
    });
    m_browseButton = new QPushButton(tr("Browse"), settingsWidget);
    m_browseButton->setObjectName(QStringLiteral("BrowseBtn"));
    m_browseButton->setToolTip(tr("Browse for the output file location"));
    connect(m_browseButton, &QPushButton::clicked, this, &ExportPanel::onBrowseOutput);
    locRow->addWidget(m_locationLabel, 1);
    locRow->addWidget(m_browseButton);
    headerForm->addRow(tr("Location"), locRow);

    // Preset + "⋯" menu (Save / Delete)
    auto* presetRow = new QHBoxLayout();
    presetRow->setSpacing(m.spacingSm);
    m_presetCombo = new QComboBox(settingsWidget);
    m_presetCombo->setToolTip(tr("Select an export preset"));
    populatePresets();
    m_presetMenuBtn = new QToolButton(settingsWidget);
    m_presetMenuBtn->setText(QStringLiteral("⋯"));   // horizontal ellipsis
    m_presetMenuBtn->setToolTip(tr("Save or delete export presets"));
    m_presetMenuBtn->setPopupMode(QToolButton::InstantPopup);
    {
        auto* presetMenu = new QMenu(m_presetMenuBtn);
        auto* saveAct = presetMenu->addAction(tr("Save Preset..."));
        m_deletePresetAction = presetMenu->addAction(tr("Delete Preset"));
        m_deletePresetAction->setEnabled(false);
        connect(saveAct, &QAction::triggered, this, &ExportPanel::onSavePreset);
        connect(m_deletePresetAction, &QAction::triggered, this, &ExportPanel::onDeletePreset);
        m_presetMenuBtn->setMenu(presetMenu);
    }
    connect(m_presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onPresetChanged);
    presetRow->addWidget(m_presetCombo, 1);
    presetRow->addWidget(m_presetMenuBtn);
    headerForm->addRow(tr("Preset"), presetRow);

    // Format = the top-level output format / video codec.
    m_codecCombo = new QComboBox(settingsWidget);
    m_codecCombo->setToolTip(tr("Output format / video codec"));
    populateCodecs();
    connect(m_codecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onCodecChanged);
    headerForm->addRow(tr("Format"), m_codecCombo);

    settingsLayout->addLayout(headerForm);

    // Estimated file size
    m_estimateLabel = new QLabel(QString(), settingsWidget);
    m_estimateLabel->setObjectName(QStringLiteral("EstimateLbl"));
    settingsLayout->addWidget(m_estimateLabel);

    // ── VIDEO section (collapsible + blue enable switch) ────────────────
    m_videoSection = new CollapsibleSection(tr("VIDEO"), /*withToggle=*/true, settingsWidget);

    // Match Sequence Settings (Premiere Pro style) — top of the VIDEO section.
    m_matchSequenceCheck = new QCheckBox(tr("Match Sequence Settings"));
    m_matchSequenceCheck->setToolTip(
        tr("When checked, resolution and frame rate are locked to the active sequence settings."));
    connect(m_matchSequenceCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            syncMatchSequenceSettings();
        if (m_widthSpin)  m_widthSpin->setEnabled(!checked);
        if (m_heightSpin) m_heightSpin->setEnabled(!checked);
        if (m_fpsCombo)   m_fpsCombo->setEnabled(!checked);
    });
    m_videoSection->addContentWidget(m_matchSequenceCheck);

    auto* videoForm = new QFormLayout();
    videoForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    videoForm->setVerticalSpacing(8);
    videoForm->setHorizontalSpacing(10);

    // Frame size
    auto* resLayout = new QHBoxLayout();
    resLayout->setSpacing(m.spacingSm);
    m_widthSpin = new QSpinBox();
    m_widthSpin->setToolTip(tr("Output video width in pixels"));
    m_widthSpin->setRange(128, 7680);
    m_widthSpin->setValue(1920);
    m_widthSpin->setSingleStep(2);
    m_heightSpin = new QSpinBox();
    m_heightSpin->setToolTip(tr("Output video height in pixels"));
    m_heightSpin->setRange(128, 4320);
    m_heightSpin->setValue(1080);
    m_heightSpin->setSingleStep(2);
    auto* xLabel = new QLabel(QStringLiteral("×"));   // ×
    xLabel->setAlignment(Qt::AlignCenter);
    xLabel->setFixedWidth(16);
    resLayout->addWidget(m_widthSpin, 1);
    resLayout->addWidget(xLabel);
    resLayout->addWidget(m_heightSpin, 1);
    videoForm->addRow(tr("Frame Size"), resLayout);

    // Frame rate
    m_fpsCombo = new QComboBox();
    m_fpsCombo->setToolTip(tr("Output frame rate"));
    // Data = exact rate as double; NTSC rates become 24000/1001 etc. at encode.
    m_fpsCombo->addItem(QStringLiteral("23.976"), 23.976);
    m_fpsCombo->addItem(QStringLiteral("24"), 24.0);
    m_fpsCombo->addItem(QStringLiteral("25"), 25.0);
    m_fpsCombo->addItem(QStringLiteral("29.97"), 29.97);
    m_fpsCombo->addItem(QStringLiteral("30"), 30.0);
    m_fpsCombo->addItem(QStringLiteral("50"), 50.0);
    m_fpsCombo->addItem(QStringLiteral("59.94"), 59.94);
    m_fpsCombo->addItem(QStringLiteral("60"), 60.0);
    m_fpsCombo->setCurrentIndex(4); // 30 fps
    videoForm->addRow(tr("Frame Rate"), m_fpsCombo);

    // Codec profile (ProRes / DNxHR) — shown/hidden by onCodecChanged.
    m_profileCombo = new QComboBox();
    m_profileCombo->setToolTip(tr("Codec quality profile (e.g. ProRes 422 HQ / 4444, DNxHR HQX)"));
    m_profileLabel = new QLabel(tr("Profile"));
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateAlphaAvailability(); updateFileEstimate(); });
    videoForm->addRow(m_profileLabel, m_profileCombo);
    m_profileLabel->setVisible(false);
    m_profileCombo->setVisible(false);

    // Hardware acceleration
    m_accelCombo = new QComboBox();
    m_accelCombo->setToolTip(tr("Hardware acceleration mode for encoding"));
    populateAccel();
    videoForm->addRow(tr("Acceleration"), m_accelCombo);

    // Quality (friendly slider -> internal CRF)
    auto* crfLayout = new QHBoxLayout();
    crfLayout->setSpacing(8);
    m_crfSlider = new QSlider(Qt::Horizontal);
    m_crfSlider->setToolTip(tr("Quality level (higher = better quality, larger file)"));
    m_crfSlider->setRange(0, 100);
    m_crfSlider->setValue(75);  // Default = High quality
    m_crfSlider->setTickPosition(QSlider::TicksBelow);
    m_crfSlider->setTickInterval(25);
    m_crfLabel = new QLabel(QStringLiteral("High"));
    m_crfLabel->setMinimumWidth(60);
    m_crfLabel->setAlignment(Qt::AlignCenter);
    connect(m_crfSlider, &QSlider::valueChanged, this, &ExportPanel::onCrfChanged);
    crfLayout->addWidget(m_crfSlider, 1);
    crfLayout->addWidget(m_crfLabel);
    videoForm->addRow(tr("Quality"), crfLayout);

    // Container
    m_containerCombo = new QComboBox();
    m_containerCombo->setToolTip(tr("Output container format"));
    populateContainers();
    videoForm->addRow(tr("Container"), m_containerCombo);

    // Alpha channel — enabled by onCodecChanged only for alpha-capable targets.
    m_alphaCheck = new QCheckBox(tr("Export with alpha (transparent background)"));
    m_alphaCheck->setToolTip(tr("Preserve transparency — requires ProRes 4444 / 4444 XQ or a PNG image sequence"));
    m_alphaCheck->setEnabled(false);
    connect(m_alphaCheck, &QCheckBox::toggled, this, [this](bool) { updateFileEstimate(); });
    videoForm->addRow(QString(), m_alphaCheck);

    m_videoSection->addContentLayout(videoForm);
    settingsLayout->addWidget(m_videoSection);
    connect(m_videoSection->toggle(), &QAbstractButton::toggled,
            this, &ExportPanel::onVideoEnabledChanged);

    // ── AUDIO section (collapsible + blue enable switch) ────────────────
    m_audioSection = new CollapsibleSection(tr("AUDIO"), /*withToggle=*/true, settingsWidget);
    auto* audioForm = new QFormLayout();
    audioForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    audioForm->setVerticalSpacing(8);
    audioForm->setHorizontalSpacing(10);

    m_audioFormatCombo = new QComboBox();
    m_audioFormatCombo->setToolTip(
        tr("Audio file format. Used when exporting audio only; video exports always mux AAC."));
    populateAudioFormats();
    connect(m_audioFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onAudioFormatChanged);
    m_audioFormatLabel = new QLabel(tr("Audio Format"));
    audioForm->addRow(m_audioFormatLabel, m_audioFormatCombo);

    m_audioBitrateCombo = new QComboBox();
    m_audioBitrateCombo->setToolTip(tr("Audio bitrate for the exported MP3 / AAC file"));
    m_audioBitrateCombo->addItem(tr("128 kbps"), 128000);
    m_audioBitrateCombo->addItem(tr("192 kbps"), 192000);
    m_audioBitrateCombo->addItem(tr("256 kbps"), 256000);
    m_audioBitrateCombo->addItem(tr("320 kbps"), 320000);
    m_audioBitrateCombo->setCurrentIndex(1); // 192 kbps
    connect(m_audioBitrateCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateFileEstimate(); });
    m_audioBitrateLabel = new QLabel(tr("Bitrate"));
    audioForm->addRow(m_audioBitrateLabel, m_audioBitrateCombo);

    // Sample rate / channels are deliberately un-exposed (mockup 04 flag):
    // one line documents the fixed pipeline instead of dead controls.
    {
        auto* audioNote = new QLabel(
            tr("48 kHz stereo. Video exports mux AAC; the format unlocks "
               "for audio-only exports."));
        audioNote->setObjectName(QStringLiteral("EstimateLbl"));
        audioNote->setWordWrap(true);
        audioForm->addRow(QString(), audioNote);
    }

    m_audioSection->addContentLayout(audioForm);
    settingsLayout->addWidget(m_audioSection);
    connect(m_audioSection->toggle(), &QAbstractButton::toggled,
            this, &ExportPanel::onAudioEnabledChanged);

    // ── Range state (hidden combo — mockup 04 dropped the RANGE section) ─
    // The combo stays the authoritative value: workspace save/restore
    // persists its index and every consumer (buildJobConfig, estimate,
    // in/out auto-switch) reads it. The visible control is the "Export
    // in → out only" checkbox in the transport row, synced two-way below.
    m_rangeCombo = new QComboBox(settingsWidget);
    m_rangeCombo->addItem(tr("Entire Sequence"), 0);
    m_rangeCombo->addItem(tr("In to Out"), 1);
    m_rangeCombo->setVisible(false);
    connect(m_rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExportPanel::onRangeChanged);
    connect(m_rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_rangeCheck && m_rangeCheck->isChecked() != (idx == 1)) {
            const QSignalBlocker block(m_rangeCheck);
            m_rangeCheck->setChecked(idx == 1);
        }
    });

    // Default: VIDEO expanded, AUDIO collapsed.
    m_videoSection->setExpanded(true);
    m_audioSection->setExpanded(false);

    // Apply initial audio-section state, then default Match Sequence on (this
    // fires the handler now that all video widgets exist).
    updateAudioSectionState();
    m_matchSequenceCheck->setChecked(true);

    settingsLayout->addStretch();

    settingsScroll->setWidget(settingsWidget);
    splitter->addWidget(settingsScroll);

    // ════════════════════════════════════════════════════════════════════
    // RIGHT: Preview panel
    // ════════════════════════════════════════════════════════════════════
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

    // "RENDERING FRAME n / m" pip — floats top-right over the preview while
    // a run is active (the preview already live-updates with rendered
    // frames; this labels it). A QLabel can host a layout, so the pip is a
    // child pinned by stretches — no manual positioning on resize.
    {
        auto* overlay = new QHBoxLayout(m_previewImageLabel);
        overlay->setContentsMargins(10, 8, 10, 8);
        m_renderPip = new QLabel(m_previewImageLabel);
        m_renderPip->setObjectName(QStringLiteral("RenderPip"));
        const QColor warn = Theme::colors().warning;
        m_renderPip->setStyleSheet(QStringLiteral(
            "QLabel#RenderPip { color: %1; background: rgba(13,16,20,160);"
            " border: 1px solid rgba(%2,%3,%4,90); border-radius: 9px;"
            " padding: 2px 9px; font-size: 10px; font-weight: 600; }")
            .arg(warn.name())
            .arg(warn.red()).arg(warn.green()).arg(warn.blue()));
        m_renderPip->setVisible(false);
        auto* pipCol = new QVBoxLayout();
        pipCol->addWidget(m_renderPip);
        pipCol->addStretch();
        overlay->addStretch();
        overlay->addLayout(pipCol);
    }

    // Mini timeline scrub bar (Premiere-style)
    m_miniTimeline = new ExportMiniTimeline(previewWidget);
    connect(m_miniTimeline, &ExportMiniTimeline::scrubbed, this, [this](int64_t tick) {
        // Stop playback if scrubbing manually
        if (m_playing) {
            m_playing = false;
            m_playbackTimer->stop();
            m_playPauseBtn->setText(QStringLiteral("▶")); // ▶
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

        m_skipToStartBtn  = new QPushButton(QStringLiteral("⏮"), previewWidget);
        m_stepBackBtn     = new QPushButton(QStringLiteral("⏴"), previewWidget);
        m_playPauseBtn    = new QPushButton(QStringLiteral("▶"), previewWidget);
        m_stepForwardBtn  = new QPushButton(QStringLiteral("⏵"), previewWidget);
        m_skipToEndBtn    = new QPushButton(QStringLiteral("⏭"), previewWidget);

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

        // In/Out point controls
        transportBar->addSpacing(12);
        m_inPointBtn = new QPushButton(QStringLiteral("▶|"), previewWidget);
        m_inPointBtn->setToolTip(tr("Set In Point (I)"));
        m_inPointBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_inPointBtn->setFocusPolicy(Qt::NoFocus);
        m_inPointBtn->setCursor(Qt::PointingHandCursor);
        connect(m_inPointBtn, &QPushButton::clicked, this, &ExportPanel::onSetInPoint);

        m_outPointBtn = new QPushButton(QStringLiteral("|◀"), previewWidget);
        m_outPointBtn->setToolTip(tr("Set Out Point (O)"));
        m_outPointBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_outPointBtn->setFocusPolicy(Qt::NoFocus);
        m_outPointBtn->setCursor(Qt::PointingHandCursor);
        connect(m_outPointBtn, &QPushButton::clicked, this, &ExportPanel::onSetOutPoint);

        m_clearInOutBtn = new QPushButton(QStringLiteral("✖"), previewWidget);
        m_clearInOutBtn->setToolTip(tr("Clear In/Out Points (Ctrl+Shift+X / Delete)"));
        m_clearInOutBtn->setObjectName(QStringLiteral("TransportBtn"));
        m_clearInOutBtn->setFocusPolicy(Qt::NoFocus);
        m_clearInOutBtn->setCursor(Qt::PointingHandCursor);
        connect(m_clearInOutBtn, &QPushButton::clicked, this, &ExportPanel::onClearInOut);

        transportBar->addWidget(m_inPointBtn);
        transportBar->addWidget(m_outPointBtn);
        transportBar->addWidget(m_clearInOutBtn);

        // Range lives here, next to where in/out points are actually set
        // (replaces the old one-control RANGE collapsible).
        transportBar->addSpacing(10);
        m_rangeCheck = new QCheckBox(tr("Export in → out only"), previewWidget);
        m_rangeCheck->setToolTip(
            tr("Export only the In-to-Out range. Unchecked = entire sequence."));
        connect(m_rangeCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_rangeCombo && m_rangeCombo->currentIndex() != (on ? 1 : 0))
                m_rangeCombo->setCurrentIndex(on ? 1 : 0);
        });
        transportBar->addWidget(m_rangeCheck);

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

    // ════════════════════════════════════════════════════════════════════
    // BOTTOM: run row (bar · stats · Cancel, shown while running) + queue
    // ════════════════════════════════════════════════════════════════════

    m_runRow = new QWidget(this);
    {
        auto* runLay = new QHBoxLayout(m_runRow);
        runLay->setContentsMargins(0, 0, 0, 0);
        runLay->setSpacing(12);

        m_progressBar = new QProgressBar(m_runRow);
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(0);
        m_progressBar->setTextVisible(true);
        m_progressBar->setFormat(QStringLiteral("%p%"));
        runLay->addWidget(m_progressBar, 1);

        m_statusLabel = new QLabel(tr("Ready"), m_runRow);
        runLay->addWidget(m_statusLabel);

        m_cancelButton = new QPushButton(tr("Cancel"), m_runRow);
        m_cancelButton->setToolTip(tr("Cancel the in-progress export"));
        m_cancelButton->setObjectName(QStringLiteral("CancelBtn"));
        m_cancelButton->setEnabled(false);
        connect(m_cancelButton, &QPushButton::clicked, this, &ExportPanel::onCancelExport);
        runLay->addWidget(m_cancelButton);
    }
    m_runRow->setVisible(false);
    mainLayout->addWidget(m_runRow);

    // Action buttons row
    auto* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);
    actionLayout->addStretch();

    m_addQueueButton = new QPushButton(tr("Add to Queue"), this);
    m_addQueueButton->setToolTip(tr("Add current settings to the export queue"));
    m_addQueueButton->setObjectName(QStringLiteral("AddQueueBtn"));
    connect(m_addQueueButton, &QPushButton::clicked, this, &ExportPanel::onAddToQueue);
    actionLayout->addWidget(m_addQueueButton);

    m_startQueueButton = new QPushButton(tr("Start Queue"), this);
    m_startQueueButton->setToolTip(
        tr("Render the queued jobs in order (Export renders only the current settings)"));
    m_startQueueButton->setObjectName(QStringLiteral("StartQueueBtn"));
    m_startQueueButton->setEnabled(false);
    connect(m_startQueueButton, &QPushButton::clicked, this, &ExportPanel::onStartQueue);
    actionLayout->addWidget(m_startQueueButton);

    m_startButton = new QPushButton(tr("Export"), this);
    m_startButton->setToolTip(tr("Start exporting"));
    m_startButton->setObjectName(QStringLiteral("ExportBtn"));
    connect(m_startButton, &QPushButton::clicked, this, &ExportPanel::onStartExport);
    actionLayout->addWidget(m_startButton);

    mainLayout->addLayout(actionLayout);

    // ── Job Queue ───────────────────────────────────────────────────────
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
            const auto job = m_renderQueue->job(jobId);
            if (job && QFileInfo(QString::fromStdString(pathToUtf8(job->config.outputPath))).exists()) {
                revealAction = menu.addAction(tr("Reveal in Explorer"));
            }
        }

        auto* chosen = menu.exec(m_jobList->mapToGlobal(pos));
        if (chosen == removeAction) {
            // Remove the job itself, not just the list row. Running jobs can't
            // be erased — cancel them and let the status update mark the row.
            bool erased = true;
            if (ok && m_renderQueue) {
                m_renderQueue->cancelJob(jobId);
                erased = m_renderQueue->removeJob(jobId);
            }
            if (erased) {
                int row = m_jobList->row(item);
                delete m_jobList->takeItem(row);
                if (m_jobList->count() == 0)
                    m_jobList->setVisible(false);
            } else if (ok) {
                setJobRowState(jobId, JobRowState::Cancelling);
            }
            updateStartQueueEnabled();
        } else if (revealAction && chosen == revealAction) {
            const auto job = m_renderQueue->job(jobId);
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

    // ── Wire up estimate updates ──────────────────────────────────────────
    auto updateEstimate = [this]() { updateFileEstimate(); };
    connect(m_widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateEstimate);
    connect(m_heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, updateEstimate);
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateEstimate);
    connect(m_codecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateEstimate);
    connect(m_crfSlider, &QSlider::valueChanged, this, updateEstimate);
}

// ═════════════════════════════════════════════════════════════════════════════
// Queue rows — one status-pill row widget per job (mockup 04)
// ═════════════════════════════════════════════════════════════════════════════

void ExportPanel::updateStartQueueEnabled()
{
    if (!m_startQueueButton) return;
    const bool canStart = m_renderQueue && !m_renderQueue->isRunning()
                       && m_renderQueue->pendingCount() > 0;
    m_startQueueButton->setEnabled(canStart);
}

void ExportPanel::setJobRowState(uint32_t jobId, JobRowState st, int pct,
                                 const QString& detail)
{
    if (!m_jobList) return;

    // Find (or create) the row for this job.
    QListWidgetItem* item = nullptr;
    for (int i = 0; i < m_jobList->count(); ++i) {
        auto* it = m_jobList->item(i);
        if (it && it->data(Qt::UserRole).toULongLong()
                      == static_cast<qulonglong>(jobId)) { item = it; break; }
    }
    if (!item) {
        item = new QListWidgetItem();
        item->setData(Qt::UserRole, static_cast<qulonglong>(jobId));
        m_jobList->addItem(item);
        m_jobList->setVisible(true);
    }

    // Pill text + palette per state.
    const auto& tc = Theme::colors();
    QString pillText;
    QColor  pillColor;
    switch (st) {
        case JobRowState::Queued:     pillText = tr("○ Queued");     pillColor = tc.textSecondary; break;
        case JobRowState::Running:    pillText = pct >= 0
                                          ? tr("↻ Running %1%").arg(pct)
                                          : tr("↻ Running");         pillColor = tc.accent;  break;
        case JobRowState::Done:       pillText = tr("✓ Done");       pillColor = tc.success; break;
        case JobRowState::Cancelled:  pillText = tr("⊘ Cancelled");  pillColor = tc.textSecondary; break;
        case JobRowState::Failed:     pillText = tr("✗ Failed");     pillColor = tc.error;   break;
        case JobRowState::Cancelling: pillText = tr("⊘ Cancelling…"); pillColor = tc.textSecondary; break;
    }

    // Fast path: the row widget exists — update pill / detail in place
    // (the poll timer calls this every tick while running).
    if (auto* w = m_jobList->itemWidget(item)) {
        if (auto* pill = w->findChild<QLabel*>(QStringLiteral("JobPill"))) {
            pill->setText(pillText);
            pill->setStyleSheet(QStringLiteral(
                "QLabel#JobPill { color: %1; background: rgba(%2,%3,%4,36);"
                " border-radius: 8px; padding: 1px 8px;"
                " font-size: 10px; font-weight: 600; }")
                .arg(pillColor.name())
                .arg(pillColor.red()).arg(pillColor.green()).arg(pillColor.blue()));
        }
        if (auto* extra = w->findChild<QLabel*>(QStringLiteral("JobExtra")))
            if (!detail.isEmpty()) extra->setText(detail);
        if (auto* reveal = w->findChild<QPushButton*>(QStringLiteral("JobReveal")))
            reveal->setVisible(st == JobRowState::Done);
        return;
    }

    // Build the row widget: [pill] [name………] [Reveal] [detail]
    QString name = tr("Job %1").arg(jobId);
    QString fullPath;
    if (m_renderQueue) {
        if (const auto job = m_renderQueue->job(jobId)) {
            fullPath = QString::fromStdString(pathToUtf8(job->config.outputPath));
            name = tr("Job %1 — %2").arg(jobId).arg(fullPath);
        }
    }

    auto* w = new QWidget(m_jobList);
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(8, 3, 8, 3);
    lay->setSpacing(10);

    auto* pill = new QLabel(pillText, w);
    pill->setObjectName(QStringLiteral("JobPill"));
    pill->setStyleSheet(QStringLiteral(
        "QLabel#JobPill { color: %1; background: rgba(%2,%3,%4,36);"
        " border-radius: 8px; padding: 1px 8px;"
        " font-size: 10px; font-weight: 600; }")
        .arg(pillColor.name())
        .arg(pillColor.red()).arg(pillColor.green()).arg(pillColor.blue()));
    lay->addWidget(pill);

    auto* nameLbl = new QLabel(name, w);
    nameLbl->setObjectName(QStringLiteral("JobName"));
    nameLbl->setToolTip(name);
    // Let the label shrink below its text width; paint elides via stylesheet-
    // free default (Qt elides nothing on QLabel, so cap with size policy and
    // rely on the tooltip for the full path).
    nameLbl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    lay->addWidget(nameLbl, 1);

    auto* reveal = new QPushButton(tr("Reveal"), w);
    reveal->setObjectName(QStringLiteral("JobReveal"));
    reveal->setFlat(true);
    reveal->setCursor(Qt::PointingHandCursor);
    reveal->setToolTip(tr("Reveal the exported file in Explorer"));
    reveal->setStyleSheet(QStringLiteral(
        "QPushButton#JobReveal { color: %1; border: none; padding: 1px 6px;"
        " font-size: 11px; }"
        "QPushButton#JobReveal:hover { text-decoration: underline; }")
        .arg(Theme::colors().accent.name()));
    reveal->setVisible(st == JobRowState::Done);
    connect(reveal, &QPushButton::clicked, this, [this, jobId]() {
        if (!m_renderQueue) return;
        const auto job = m_renderQueue->job(jobId);
        if (!job) return;
        QFileInfo fi(QString::fromStdString(pathToUtf8(job->config.outputPath)));
        if (fi.exists())
            QProcess::startDetached("explorer.exe",
                {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
    });
    lay->addWidget(reveal);

    auto* extra = new QLabel(detail, w);
    extra->setObjectName(QStringLiteral("JobExtra"));
    extra->setStyleSheet(QStringLiteral("color: %1; font-size: 10.5px;")
        .arg(Theme::colors().textSecondary.name()));
    lay->addWidget(extra);

    item->setSizeHint(QSize(0, w->sizeHint().height()));
    m_jobList->setItemWidget(item, w);
}

} // namespace rt
