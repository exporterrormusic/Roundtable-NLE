/*
 * ManualMatchDialog.cpp — full-screen waveform overlay for manual matching.
 */

#include "widgets/ManualMatchDialog.h"
#include "widgets/FullWaveformWidget.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QFileInfo>
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QShowEvent>
#include <QTimer>

#include <cmath>

#include "audio/AudioEngine.h"

#include <algorithm>
#include <filesystem>

namespace rt {

ManualMatchDialog::ManualMatchDialog(
    const std::string& character,
    const std::string& dialogue,
    int lineNumber,
    const std::vector<std::string>& audioFiles,
    const std::unordered_map<std::string, AudioData>& audioSamples,
    const std::vector<std::pair<double,double>>& confirmedRegions,
    const std::vector<std::pair<double,double>>& tentativeRegions,
    const std::string& preselectedFile,
    AudioEngine* engine,
    QWidget* parent)
    : QDialog(parent)
    , m_character(character)
    , m_dialogue(dialogue)
    , m_lineNumber(lineNumber)
    , m_audioFiles(audioFiles)
    , m_audioSamples(audioSamples)
    , m_audioEngine(engine)
{
    // Store confirmed/tentative regions (currently global — could be per-file)
    // For now we store them globally and filter per file once we figure out
    // which clips belong to which file. The caller should pass per-file data.
    // We'll just use them as-is for the selected file.
    (void)confirmedRegions;
    (void)tentativeRegions;

    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, false);

    // Size and center on parent's top-level window
    if (parent) {
        QRect pr = parent->window()->geometry();
        int dlgW = pr.width() * 80 / 100;
        int dlgH = pr.height() * 78 / 100;
        int dlgX = pr.x() + (pr.width() - dlgW) / 2;
        int dlgY = pr.y() + (pr.height() - dlgH) / 2;
        setGeometry(dlgX, dlgY, dlgW, dlgH);
    } else if (auto* screen = QApplication::primaryScreen()) {
        QRect avail = screen->availableGeometry();
        int margin = 80;
        setGeometry(avail.x() + margin, avail.y() + margin,
                    avail.width() - margin * 2, avail.height() - margin * 2);
    }

    const auto& tc = Theme::colors();
    const auto& tm = Theme::metrics();
    const auto s0  = Theme::hex(tc.surface0);
    const auto s1  = Theme::hex(tc.surface1);
    const auto s2  = Theme::hex(tc.surface2);
    const auto s3  = Theme::hex(tc.surface3);
    const auto brd = Theme::hex(tc.border);
    const auto brl = Theme::hex(tc.borderLight);
    const auto t1  = Theme::hex(tc.textPrimary);
    const auto t2  = Theme::hex(tc.textSecondary);
    const auto t3  = Theme::hex(tc.textTertiary);
    const auto td  = Theme::hex(tc.textDisabled);
    const auto tb  = Theme::hex(tc.textBright);
    const auto acc = Theme::hex(tc.accent);
    const auto adm = Theme::hex(tc.accentDim);
    const auto inp = Theme::hex(tc.inputBg);
    const auto inb = Theme::hex(tc.inputBorder);
    const auto err = Theme::hex(tc.error);
    const auto sbg = Theme::hex(tc.successBtnBg);
    const auto sbh = Theme::hex(tc.successBtnHover);
    const auto dbg = Theme::hex(tc.dangerBg);
    const auto dtx = Theme::hex(tc.dangerText);
    const int  rad = tm.radiusMd;
    const int  rl  = tm.radiusLg;
    const auto& tt = Theme::typography();
    const auto fsXs      = QString::number(tt.sizeXs);
    const auto fsSmall   = QString::number(tt.sizeSmall);
    const auto fsCaption = QString::number(tt.sizeCaption);
    const auto fsBody    = QString::number(tt.sizeBody);

    setStyleSheet(
        "QDialog { background: " + s0 + "; border: 2px solid " + adm + "; border-radius: " + QString::number(rl) + "px; }"
        "QLabel { color: " + t2 + "; border: none; }"
        "QComboBox { background: " + inp + "; color: " + t2 + "; border: 1px solid " + inb + "; "
        "border-radius: " + QString::number(rad) + "px; padding: 6px 12px; font-size: 13px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: " + inp + "; color: " + t2 + "; "
        "selection-background-color: " + adm + "; }");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 20, 24, 20);
    mainLayout->setSpacing(12);

    // ── Custom title bar ─────────────────────────────────────────────────
    auto* titleBar = new QWidget;
    titleBar->setFixedHeight(32);
    titleBar->setStyleSheet("background: transparent; border: none;");
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(4, 0, 4, 0);
    titleLayout->setSpacing(8);

    auto* titleLabel = new QLabel("Manual Match");
    titleLabel->setStyleSheet("color: " + acc + "; font-size: " + fsSmall + "px; font-weight: bold; border: none;");
    titleLayout->addWidget(titleLabel);

    auto* shortcutHint = new QLabel("Space: Play/Pause    I: Set In    O: Set Out    Scroll: Zoom    Right-drag: Pan");
    shortcutHint->setStyleSheet("color: " + td + "; font-size: " + fsXs + "px; border: none;");
    titleLayout->addWidget(shortcutHint, 1);

    auto* closeTitleBtn = new QPushButton(QStringLiteral("\u2715"));
    closeTitleBtn->setFixedSize(28, 28);
    closeTitleBtn->setCursor(Qt::PointingHandCursor);
    closeTitleBtn->setStyleSheet(
        "QPushButton { background: transparent; color: " + t3 + "; font-size: " + fsBody + "px; border: none; border-radius: 4px; }"
        "QPushButton:hover { background: " + dbg + "; color: " + err + "; }");
    connect(closeTitleBtn, &QPushButton::clicked, this, [this]() {
        stopPreviewPlayback();
        reject();
    });
    titleLayout->addWidget(closeTitleBtn);
    mainLayout->addWidget(titleBar);

    // ── Script line header ───────────────────────────────────────────
    auto* headerFrame = new QFrame;
    headerFrame->setStyleSheet(
        "QFrame { background: " + s1 + "; border: 1px solid " + brl + "; border-radius: " + QString::number(rl) + "px; }");
    auto* headerLayout = new QHBoxLayout(headerFrame);
    headerLayout->setContentsMargins(16, 12, 16, 12);

    auto* lineNumLabel = new QLabel(QString("#%1").arg(lineNumber));
    lineNumLabel->setStyleSheet(
        "QLabel { color: " + tb + "; font-weight: bold; font-size: " + fsSmall + "px; "
        "background: " + s3 + "; border-radius: 4px; padding: 4px 10px; }");
    headerLayout->addWidget(lineNumLabel);

    auto* charLabel = new QLabel(QString::fromStdString(character));
    charLabel->setStyleSheet(
        "QLabel { color: " + acc + "; font-weight: bold; font-size: " + fsCaption + "px; }");
    headerLayout->addWidget(charLabel);

    m_scriptLabel = new QLabel(QString::fromStdString(dialogue));
    m_scriptLabel->setWordWrap(true);
    m_scriptLabel->setStyleSheet(
        "QLabel { color: " + t1 + "; font-size: " + fsSmall + "px; padding-left: 8px; }");
    headerLayout->addWidget(m_scriptLabel, 1);
    mainLayout->addWidget(headerFrame);

    // ── Audio file selector ──────────────────────────────────────────
    auto* fileRow = new QHBoxLayout;
    auto* fileLabel = new QLabel("Audio File:");
    fileLabel->setStyleSheet("QLabel { color: " + t3 + "; font-size: 13px; }");;
    fileRow->addWidget(fileLabel);

    m_fileCombo = new QComboBox;
    m_fileCombo->setMinimumWidth(300);
    int preselectedIdx = 0;
    // Build sorted entries for alphabetical dropdown
    struct FileEntry { QString displayName; QString fullPath; std::string originalPath; };
    std::vector<FileEntry> sortedFiles;
    sortedFiles.reserve(m_audioFiles.size());
    for (const auto& f : m_audioFiles) {
        FileEntry e;
        e.fullPath = QString::fromStdString(f);
        e.displayName = QFileInfo(e.fullPath).fileName();
        e.originalPath = f;
        sortedFiles.push_back(std::move(e));
    }
    std::sort(sortedFiles.begin(), sortedFiles.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.displayName.compare(b.displayName, Qt::CaseInsensitive) < 0;
              });
    for (int i = 0; i < static_cast<int>(sortedFiles.size()); ++i) {
        m_fileCombo->addItem(sortedFiles[i].displayName, sortedFiles[i].fullPath);
        if (sortedFiles[i].originalPath == preselectedFile)
            preselectedIdx = i;
    }
    connect(m_fileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ManualMatchDialog::onAudioFileChanged);
    fileRow->addWidget(m_fileCombo, 1);
    fileRow->addStretch();
    mainLayout->addLayout(fileRow);

    // ── Full waveform ────────────────────────────────────────────────
    m_waveform = new FullWaveformWidget;
    m_waveform->setMinimumHeight(200);
    connect(m_waveform, &FullWaveformWidget::selectionChanged,
            this, &ManualMatchDialog::onSelectionChanged);
    connect(m_waveform, &FullWaveformWidget::seekRequested,
            this, [this](double t) {
        // Reset shuttle speed so next L press starts at 1×
        m_jShuttleLevel = 0;
        m_lShuttleLevel = 0;
        if (m_audioEngine)
            m_audioEngine->setPlaybackSpeed(1.0);
        if (m_isPlaying)
            stopPreviewPlayback();

        m_waveform->setPlayhead(t);
        m_waveform->setPlayheadVisible(true);
        // Audio scrub — play a short burst at this position
        if (m_audioEngine && !m_isPlaying && !m_fullFileBuffer.empty()) {
            // Another shared-engine client must not be able to leave this
            // dialog playing with an empty source list.
            installFullFileSource();
            auto frame = static_cast<int64_t>(t * m_fullFileRate);
            frame = std::clamp(frame, int64_t(0),
                               static_cast<int64_t>(m_fullFileBuffer.size()) - 1);
            m_audioEngine->scrub(frame, 4096);
        }
    });
    mainLayout->addWidget(m_waveform, 1);

    // ── Selection info + I/O + play ───────────────────────────────
    auto* selRow = new QHBoxLayout;
    selRow->setSpacing(6);

    m_selectionLabel = new QLabel("Click to place playhead, then press I/O to set in/out points");
    m_selectionLabel->setStyleSheet(
        "QLabel { color: " + t3 + "; font-size: 13px; font-style: italic; }");
    selRow->addWidget(m_selectionLabel, 1);

    auto smallBtnStyle = QString(
        "QPushButton { background: " + adm + "; color: " + acc + "; font-size: " + fsXs + "px; font-weight: bold; "
        "border: 1px solid " + adm + "; border-radius: " + QString::number(rad) + "px; }"
        "QPushButton:hover { background: " + Theme::hex(tc.accentHover) + "; }");

    auto* inBtn = new QPushButton("I");
    inBtn->setFixedSize(32, 28);
    inBtn->setToolTip("Set In point at playhead (I key)");
    inBtn->setCursor(Qt::PointingHandCursor);
    inBtn->setStyleSheet(smallBtnStyle);
    connect(inBtn, &QPushButton::clicked, this, [this]() {
        if (m_waveform->playheadVisible()) {
            double t = m_waveform->playhead();
            double outPt = m_waveform->hasSelection() ? m_waveform->selectionEnd() : m_waveform->duration();
            if (t < outPt) {
                m_waveform->setSelection(t, outPt);
                onSelectionChanged(t, outPt);
            }
        }
    });
    selRow->addWidget(inBtn);

    auto* outBtn = new QPushButton("O");
    outBtn->setFixedSize(32, 28);
    outBtn->setToolTip("Set Out point at playhead (O key)");
    outBtn->setCursor(Qt::PointingHandCursor);
    outBtn->setStyleSheet(smallBtnStyle);
    connect(outBtn, &QPushButton::clicked, this, [this]() {
        if (m_waveform->playheadVisible()) {
            double t = m_waveform->playhead();
            double inPt = m_waveform->hasSelection() ? m_waveform->selectionStart() : 0.0;
            if (t > inPt) {
                m_waveform->setSelection(inPt, t);
                onSelectionChanged(inPt, t);
            }
        }
    });
    selRow->addWidget(outBtn);

    selRow->addSpacing(8);

    m_previewBtn = new QPushButton(QStringLiteral("\u25B6  Play"));
    m_previewBtn->setFixedHeight(32);
    m_previewBtn->setFixedWidth(100);
    m_previewBtn->setCursor(Qt::PointingHandCursor);
    m_previewBtn->setStyleSheet(
        "QPushButton { background: " + s2 + "; color: " + acc + "; font-size: 13px; "
        "border: 1px solid " + brd + "; border-radius: " + QString::number(rl) + "px; }"
        "QPushButton:hover { background: " + s3 + "; }");
    connect(m_previewBtn, &QPushButton::clicked, this, &ManualMatchDialog::onPreviewClicked);
    selRow->addWidget(m_previewBtn);
    mainLayout->addLayout(selRow);

    // ── Bottom buttons ───────────────────────────────────────────────
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch();

    m_cancelBtn = new QPushButton("Cancel");
    m_cancelBtn->setFixedHeight(40);
    m_cancelBtn->setFixedWidth(120);
    m_cancelBtn->setStyleSheet(
        "QPushButton { background: " + s2 + "; color: " + t3 + "; font-size: " + fsSmall + "px; "
        "border: 1px solid " + brl + "; border-radius: " + QString::number(rl) + "px; }"
        "QPushButton:hover { background: " + s3 + "; }");
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    bottomRow->addWidget(m_cancelBtn);

    bottomRow->addSpacing(12);

    m_confirmBtn = new QPushButton(QStringLiteral("\u2713  Confirm Match"));
    m_confirmBtn->setFixedHeight(44);
    m_confirmBtn->setFixedWidth(200);
    m_confirmBtn->setEnabled(false);
    m_confirmBtn->setStyleSheet(
        "QPushButton { background: " + sbg + "; color: " + tb + "; font-size: " + fsCaption + "px; font-weight: bold; "
        "border: 2px solid " + sbh + "; border-radius: " + QString::number(rl) + "px; }"
        "QPushButton:hover { background: " + sbh + "; }"
        "QPushButton:disabled { background: " + s1 + "; color: " + td + "; border-color: " + brl + "; }");
    connect(m_confirmBtn, &QPushButton::clicked, this, &ManualMatchDialog::onConfirmClicked);
    bottomRow->addWidget(m_confirmBtn);
    mainLayout->addLayout(bottomRow);
    // ── Playhead timer ─────────────────────────────────────────────
    m_playheadTimer = new QTimer(this);
    m_playheadTimer->setInterval(30);
    connect(m_playheadTimer, &QTimer::timeout, this, [this]() {
        if (!m_isPlaying || !m_audioEngine) { stopPreviewPlayback(); return; }
        auto frame = m_audioEngine->currentFrame();
        // Full file is loaded at m_fullFileRate, so frame/rate = absolute time
        double currentTime = static_cast<double>(frame) / m_fullFileRate;
        if (currentTime >= m_playbackEndTime ||
            m_audioEngine->transportState() != TransportState::Playing) {
            m_waveform->setPlayhead(m_playbackEndTime);
            stopPreviewPlayback();
            return;
        }
        m_waveform->setPlayhead(currentTime);
        m_waveform->setPlayheadVisible(true);
    });
    // ── Stop any in-progress audio from the main app ─────────────────
    if (m_audioEngine) {
        // Manual Match owns the shared engine for the lifetime of the modal
        // dialog.  In particular, do not let its preview move the timeline's
        // master clock.
        m_savedSyncClock = m_audioEngine->syncClock();
        m_audioEngine->setSyncClock(nullptr);
        m_audioEngine->stop();
        m_audioEngine->clearTrackSources();
    }

    // NOTE: Don't load file here — caller must set m_confirmedByFile/
    // m_tentativeByFile first, then call loadInitialFile().
    if (!m_audioFiles.empty())
        m_fileCombo->setCurrentIndex(preselectedIdx);
}

void ManualMatchDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    raise();
    activateWindow();
    m_waveform->setFocus();
}

void ManualMatchDialog::loadInitialFile()
{
    int idx = m_fileCombo->currentIndex();
    if (idx >= 0 && idx < m_fileCombo->count())
        loadFileIntoWaveform(m_fileCombo->itemData(idx).toString().toStdString());
}

void ManualMatchDialog::onAudioFileChanged(int index)
{
    if (index < 0 || index >= m_fileCombo->count()) return;
    loadFileIntoWaveform(m_fileCombo->itemData(index).toString().toStdString());
}

void ManualMatchDialog::loadFileIntoWaveform(const std::string& path)
{
    stopPreviewPlayback();
    m_currentFile = path;
    auto it = m_audioSamples.find(path);
    if (it != m_audioSamples.end()) {
        m_waveform->setAudio(it->second.samples, it->second.sampleRate);

        // Set confirmed/tentative regions for this file
        auto cIt = m_confirmedByFile.find(path);
        if (cIt != m_confirmedByFile.end())
            m_waveform->setConfirmedRegions(cIt->second);
        else
            m_waveform->setConfirmedRegions({});

        auto tIt = m_tentativeByFile.find(path);
        if (tIt != m_tentativeByFile.end())
            m_waveform->setTentativeRegions(tIt->second);
        else
            m_waveform->setTentativeRegions({});

        // Prepare full-file audio buffer resampled to engine rate for scrub/play
        setupFullFileSource();
    }
    m_waveform->clearSelection();
    m_confirmBtn->setEnabled(false);
    updateSelectionLabel();
}

void ManualMatchDialog::setupFullFileSource()
{
    if (!m_audioEngine) return;
    auto it = m_audioSamples.find(m_currentFile);
    if (it == m_audioSamples.end()) return;
    const auto& audioData = it->second;

    double srcRate = static_cast<double>(audioData.sampleRate);
    m_fullFileRate = static_cast<double>(m_audioEngine->config().sampleRate);

    if (std::abs(srcRate - m_fullFileRate) > 1.0 && !audioData.samples.empty()) {
        // Resample to engine rate
        double ratio = m_fullFileRate / srcRate;
        auto newSize = static_cast<size_t>(
            static_cast<double>(audioData.samples.size()) * ratio) + 1;
        m_fullFileBuffer.resize(newSize);
        for (size_t j = 0; j < newSize; ++j) {
            double srcPos = static_cast<double>(j) / ratio;
            auto idx = static_cast<size_t>(srcPos);
            float frac = static_cast<float>(srcPos - static_cast<double>(idx));
            if (idx + 1 < audioData.samples.size())
                m_fullFileBuffer[j] = audioData.samples[idx] * (1.0f - frac)
                                    + audioData.samples[idx + 1] * frac;
            else if (idx < audioData.samples.size())
                m_fullFileBuffer[j] = audioData.samples[idx];
            else
                m_fullFileBuffer[j] = 0.0f;
        }
    } else {
        m_fullFileBuffer = audioData.samples;
        m_fullFileRate = srcRate;
    }

    installFullFileSource();
}

void ManualMatchDialog::installFullFileSource()
{
    if (!m_audioEngine || m_fullFileBuffer.empty()) return;

    AudioTrackSource src;
    src.trackId     = 9999;
    src.samples     = m_fullFileBuffer.data();
    src.totalFrames = static_cast<int64_t>(m_fullFileBuffer.size());
    src.startFrame  = 0;
    src.channels    = 1;
    src.sampleRate  = static_cast<uint32_t>(m_fullFileRate);
    src.volume      = 1.0f;
    src.pan         = 0.0f;
    src.muted       = false;
    src.solo        = false;

    m_audioEngine->setTrackSources({src});
}

void ManualMatchDialog::onSelectionChanged(double start, double end)
{
    m_result.audioFile = m_currentFile;
    m_result.start     = start;
    m_result.end       = end;
    m_confirmBtn->setEnabled(true);
    updateSelectionLabel();
}

void ManualMatchDialog::updateSelectionLabel()
{
    if (m_waveform->hasSelection()) {
        double dur = m_waveform->selectionEnd() - m_waveform->selectionStart();
        auto formatTime = [](double t) -> QString {
            int mins = static_cast<int>(t) / 60;
            double secs = t - mins * 60;
            return QString("%1:%2").arg(mins, 2, 10, QChar('0'))
                                  .arg(secs, 05, 'f', 1, QChar('0'));
        };
        m_selectionLabel->setText(
            QString("Selection: %1 \u2013 %2  (%3s)")
                .arg(formatTime(m_waveform->selectionStart()))
                .arg(formatTime(m_waveform->selectionEnd()))
                .arg(dur, 0, 'f', 1));
        m_selectionLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-style: normal; }")
                .arg(Theme::hex(Theme::colors().accent)));
    } else {
        m_selectionLabel->setText("Click to place playhead, then press I/O to set in/out points");
        m_selectionLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; font-style: italic; }")
                .arg(Theme::hex(Theme::colors().textTertiary)));
    }
}

void ManualMatchDialog::onPreviewClicked()
{
    if (m_isPlaying)
        stopPreviewPlayback();
    else
        startPreviewPlayback();
}

void ManualMatchDialog::onConfirmClicked()
{
    if (!m_waveform->hasSelection()) return;
    stopPreviewPlayback();
    m_result.audioFile = m_currentFile;
    m_result.start     = m_waveform->selectionStart();
    m_result.end       = m_waveform->selectionEnd();
    accept();
}

ManualMatchDialog::~ManualMatchDialog()
{
    stopPreviewPlayback();
    if (m_audioEngine) {
        // The installed source contains a raw pointer into
        // m_fullFileBuffer, so remove it before that buffer is destroyed.
        m_audioEngine->clearTrackSources();
        m_audioEngine->setSyncClock(m_savedSyncClock);
    }
}

void ManualMatchDialog::startPreviewPlayback()
{
    if (!m_audioEngine || m_fullFileBuffer.empty()) return;
    stopPreviewPlayback();

    // Determine play range
    double playFrom, playTo;
    if (m_waveform->hasSelection()) {
        playFrom = m_waveform->selectionStart();
        playTo   = m_waveform->selectionEnd();
        // If playhead is within selection, play from playhead
        if (m_waveform->playheadVisible()) {
            double ph = m_waveform->playhead();
            if (ph >= playFrom && ph < playTo - 0.05)
                playFrom = ph;
        }
    } else if (m_waveform->playheadVisible()) {
        playFrom = m_waveform->playhead();
        playTo   = m_waveform->duration();
    } else {
        playFrom = 0.0;
        playTo   = m_waveform->duration();
    }
    if (playTo <= playFrom + 0.01) return;

    m_playbackEndTime = playTo;

    // Full file is already loaded as track source via setupFullFileSource().
    // Just seek to the right frame and play.
    auto seekFrame = static_cast<int64_t>(playFrom * m_fullFileRate);
    seekFrame = std::clamp(seekFrame, int64_t(0),
                           static_cast<int64_t>(m_fullFileBuffer.size()) - 1);

    m_audioEngine->stop();
    // Reinstall our source on every start.  AudioEngine is shared across
    // panels, and a stale owner may have cleared its source list.
    installFullFileSource();
    m_audioEngine->seekToFrame(seekFrame);
    m_audioEngine->play();

    m_isPlaying = true;
    m_waveform->setPlayhead(playFrom);
    m_waveform->setPlayheadVisible(true);
    m_previewBtn->setText(QStringLiteral("\u23F9  Stop"));
    m_playheadTimer->start();
}

void ManualMatchDialog::stopPreviewPlayback()
{
    if (m_audioEngine) {
        m_audioEngine->stop();
        m_audioEngine->setPlaybackSpeed(1.0);
    }
    if (m_playheadTimer)
        m_playheadTimer->stop();
    m_isPlaying = false;
    m_jShuttleLevel = 0;
    m_lShuttleLevel = 0;
    if (m_previewBtn)
        m_previewBtn->setText(QStringLiteral("\u25B6  Play"));
}

void ManualMatchDialog::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        onPreviewClicked();
        event->accept();
        return;
    case Qt::Key_I:
        if (m_waveform->playheadVisible()) {
            double t = m_waveform->playhead();
            double outPt = m_waveform->hasSelection()
                ? m_waveform->selectionEnd() : m_waveform->duration();
            if (t < outPt) {
                m_waveform->setSelection(t, outPt);
                onSelectionChanged(t, outPt);
            }
        }
        event->accept();
        return;
    case Qt::Key_O:
        if (m_waveform->playheadVisible()) {
            double t = m_waveform->playhead();
            double inPt = m_waveform->hasSelection()
                ? m_waveform->selectionStart() : 0.0;
            if (t > inPt) {
                m_waveform->setSelection(inPt, t);
                onSelectionChanged(inPt, t);
            }
        }
        event->accept();
        return;
    case Qt::Key_J: {
        // Shuttle reverse
        if (m_lShuttleLevel > 0) { m_lShuttleLevel = 0; m_jShuttleLevel = 0; }
        m_jShuttleLevel = std::min(m_jShuttleLevel + 1, 3);
        m_lShuttleLevel = 0;
        double speed = -std::pow(2.0, m_jShuttleLevel - 1);
        if (!m_isPlaying) startPreviewPlayback();
        if (m_audioEngine) {
            m_audioEngine->setPlaybackSpeed(speed);
            m_audioEngine->play();
        }
        event->accept();
        return;
    }
    case Qt::Key_K:
        // Shuttle pause
        m_jShuttleLevel = 0;
        m_lShuttleLevel = 0;
        stopPreviewPlayback();
        event->accept();
        return;
    case Qt::Key_L: {
        // Shuttle forward
        if (m_jShuttleLevel > 0) { m_jShuttleLevel = 0; m_lShuttleLevel = 0; }
        m_lShuttleLevel = std::min(m_lShuttleLevel + 1, 3);
        m_jShuttleLevel = 0;
        double speed = std::pow(2.0, m_lShuttleLevel - 1);
        if (!m_isPlaying) startPreviewPlayback();
        if (m_audioEngine) {
            m_audioEngine->setPlaybackSpeed(speed);
            m_audioEngine->play();
        }
        event->accept();
        return;
    }
    case Qt::Key_Escape:
        stopPreviewPlayback();
        reject();
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_waveform->hasSelection()) {
            onConfirmClicked();
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QDialog::keyPressEvent(event);
}

} // namespace rt
