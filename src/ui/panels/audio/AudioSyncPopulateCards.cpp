/*
 * AudioSyncPopulateCards.cpp - Right-pane waveform card population.
 * Extracted from AudioSyncCards.cpp for modularity.
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "media/AudioEngine.h"
#include "widgets/MiniWaveformWidget.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>
#include <algorithm>

namespace rt {

void AudioSync::populateCards()
{
    // Save scroll position to restore after rebuild
    int savedScrollPos = 0;
    if (m_rightScrollArea && m_rightScrollArea->verticalScrollBar())
        savedScrollPos = m_rightScrollArea->verticalScrollBar()->value();

    // Take focus off any card widget before tearing down. Otherwise Qt
    // re-targets focus to whichever button survives — typically a button on
    // the LAST card added — and QScrollArea auto-scrolls to make that
    // newly-focused widget visible, slamming the view to the bottom right
    // after a manual match.
    if (auto* fw = QApplication::focusWidget()) {
        if (m_rightScrollContent && m_rightScrollContent->isAncestorOf(fw))
            m_rightScrollArea->setFocus(Qt::OtherFocusReason);
    }
    // Suppress scroll-bar value-change side effects (syncLeftListFromScroll)
    // while the layout churns through clear → repopulate.
    QScrollBar* rightSB = m_rightScrollArea ? m_rightScrollArea->verticalScrollBar() : nullptr;
    if (rightSB) rightSB->blockSignals(true);

    // ── Card widget pool (Phase 5.A) ──────────────────────────────────
    // Hide existing cards for reuse instead of destroy+recreate.
    m_cardPool.releaseAll();
    m_cardWaveforms.clear();
    m_cardWidgets.clear();
    m_cardClipIndices.clear();
    m_highlightedCard = nullptr;
    m_selectedLeftCard = nullptr;

    // Remove all widget items from layout (without deleting widgets)
    if (m_rightLayout) {
        while (m_rightLayout->count() > 0) {
            auto* item = m_rightLayout->takeAt(0);
            if (auto* w = item->widget())
                w->setParent(nullptr);  // detach but don't delete
            delete item;
        }
    }

    // Suppress repaints during bulk widget creation
    if (m_rightScrollContent)
        m_rightScrollContent->setUpdatesEnabled(false);

    if (!m_script) {
        // No script loaded â€” show placeholder
        if (m_rightLayout) {
            auto* placeholder = new QLabel("Load a script and import audio to begin.");
            placeholder->setAlignment(Qt::AlignCenter);
            placeholder->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; padding: 40px; border: none; }").arg(Theme::hex(Theme::colors().textDisabled)));
            m_rightLayout->addWidget(placeholder);
            m_rightLayout->addStretch();
        }
        populateLeftList();
        updateSmartBar();
        if (rightSB) rightSB->blockSignals(false);
        return;
    }

    // Character filter from MATCH side panel list
    QString filterChar;
    bool filterUnmatched = false;
    if (m_charFilterList && m_charFilterList->currentRow() > 0) {
        auto* item = m_charFilterList->currentItem();
        if (item) {
            if (item->text() == "UNMATCHED")
                filterUnmatched = true;
            else
                filterChar = item->text();
        }
    }

    // Build maps: scriptLineNumber â†’ clip indices
    std::unordered_map<int, std::vector<size_t>> lineToClips;
    for (size_t i = 0; i < m_clips.size(); ++i) {
        if (m_clips[i].scriptLineNumber > 0)
            lineToClips[m_clips[i].scriptLineNumber].push_back(i);
    }

    // Build display-index â†’ script line mapping for dropdown
    struct ScriptLineDisplay {
        int displayNum;
        int lineNumber;
        std::string character;
        std::string dialogue;
    };
    std::vector<ScriptLineDisplay> displayLines;
    {
        int idx = 0;
        for (const auto& line : m_script->lines) {
            ++idx;
            displayLines.push_back({idx, line.lineNumber, line.character, line.dialogue});
        }
    }

    int cardIdx = 0;
    const auto& _tc = Theme::colors();
    const auto _radM = QString::number(Theme::metrics().radiusMd);
    const auto _rad  = QString::number(Theme::metrics().radiusSm);

    for (const auto& line : m_script->lines) {
        // Apply character filter
        if (!filterChar.isEmpty()) {
            if (QString::fromStdString(line.character) != filterChar)
                continue;
        }

        // Apply unmatched filter â€” show only lines without a matched clip
        if (filterUnmatched) {
            bool hasMatch = lineToClips.count(line.lineNumber) > 0;
            if (hasMatch) continue;
        }

        ++cardIdx;

        // Find matched clip(s) for this line
        auto clipIt = lineToClips.find(line.lineNumber);
        bool hasClip = (clipIt != lineToClips.end() && !clipIt->second.empty());
        size_t primaryClipIdx = SIZE_MAX;
        int matchState = 0;
        float confidence = 0.0f;
        if (hasClip) {
            // Pick the clip with the highest match state (confirmed > tentative > unmatched)
            // so the card visual matches the aggregate counters which count a line as
            // confirmed if ANY clip for that line is confirmed.
            for (size_t ci : clipIt->second) {
                const auto& c = m_clips[ci];
                if (c.matchState > matchState ||
                    (c.matchState == matchState && c.confidence > confidence)) {
                    primaryClipIdx = ci;
                    matchState = c.matchState;
                    confidence = c.confidence;
                }
            }
        }

        // â”€â”€ Card frame â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        QString borderColor, bgColor;
        switch (matchState) {
        case 2:  bgColor = Theme::hex(_tc.successBg); borderColor = Theme::hex(_tc.success); break;
        case 1:  bgColor = Theme::hex(_tc.warningBg); borderColor = Theme::hex(_tc.warning); break;
        default: bgColor = Theme::hex(_tc.surface2); borderColor = Theme::hex(_tc.borderLight); break;
        }

        QString hoverBg, hoverBorder;
        switch (matchState) {
        case 2:  hoverBg = Theme::hex(_tc.successBg.lighter(120)); hoverBorder = Theme::hex(_tc.success.lighter(120)); break;
        case 1:  hoverBg = Theme::hex(_tc.warningBg.lighter(120)); hoverBorder = Theme::hex(_tc.warning.lighter(120)); break;
        default: hoverBg = Theme::hex(_tc.surface3); hoverBorder = Theme::hex(_tc.border); break;
        }

        // Acquire card from pool or create new
        auto* card = m_cardPool.acquire();
        if (!card) {
            card = new QFrame;
            m_cardPool.pool.push_back(card);
        } else {
            // ── Clean up old layout and child widgets when reusing ─────
            // Qt will NOT replace a non-empty layout on an existing widget
            // (setLayout() returns early with a warning), so the new
            // QHBoxLayout created below would be orphaned and the card
            // would still show stale content from the previous populateCards
            // call — causing "No audio assigned" to persist even after
            // auto-sync has matched clips.
            QLayout* oldLayout = card->layout();
            if (oldLayout) {
                while (auto* item = oldLayout->takeAt(0)) {
                    if (auto* w = item->widget()) {
                        w->setParent(nullptr);
                        delete w;
                    }
                    delete item;
                }
                delete oldLayout;  // removes layout from card
            }
        }
        card->setObjectName(QString("scriptCard_%1").arg(line.lineNumber));
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(
            QString("QFrame#scriptCard_%3 { background: %1; border: 1px solid %2; "
                    "border-radius: %6px; margin-bottom: 2px; }"
                    "QFrame#scriptCard_%3:hover { background: %4; border: 1px solid %5; "
                    "border-radius: %6px; margin-bottom: 2px; }")
                .arg(bgColor, borderColor).arg(line.lineNumber)
                .arg(hoverBg, hoverBorder).arg(_radM));

        // â”€â”€ Outer: left (script + waveform) | separator | right (controls) â”€â”€
        auto* cardOuterLayout = new QHBoxLayout(card);
        cardOuterLayout->setContentsMargins(14, 10, 10, 10);
        cardOuterLayout->setSpacing(0);

        auto* leftWidget = new QWidget;
        leftWidget->setStyleSheet("background: transparent; border: none;");
        auto* leftLayout = new QVBoxLayout(leftWidget);
        leftLayout->setContentsMargins(0, 0, 8, 0);
        leftLayout->setSpacing(6);
        cardOuterLayout->addWidget(leftWidget, 1);

        auto* sepLine = new QFrame;
        sepLine->setFrameShape(QFrame::VLine);
        sepLine->setStyleSheet(QString("QFrame { color: %1; }").arg(Theme::hex(_tc.border)));
        sepLine->setFixedWidth(1);
        cardOuterLayout->addWidget(sepLine);

        auto* rightWidget = new QWidget;
        rightWidget->setFixedWidth(130);
        rightWidget->setStyleSheet("background: transparent; border: none;");
        auto* rightLayout = new QVBoxLayout(rightWidget);
        rightLayout->setContentsMargins(8, 0, 0, 0);
        rightLayout->setSpacing(4);
        cardOuterLayout->addWidget(rightWidget);

        // â”€â”€ Header (left panel): # | Character | Script Text â”€â”€â”€â”€â”€â”€
        auto* headerRow = new QHBoxLayout;
        headerRow->setSpacing(8);

        auto* numLabel = new QLabel(QString("#%1").arg(cardIdx));
        numLabel->setFixedSize(36, 24);
        numLabel->setAlignment(Qt::AlignCenter);
        QString badgeColor = (matchState == 2) ? Theme::hex(_tc.successBtnBg)
                           : (matchState == 1) ? Theme::hex(_tc.warning) : Theme::hex(_tc.surface3);
        numLabel->setStyleSheet(
            QString("QLabel { color: %2; font-weight: bold; font-size: 12px; "
                    "background: %1; border-radius: %3px; border: none; }").arg(badgeColor, Theme::hex(_tc.textBright), _rad));
        headerRow->addWidget(numLabel);

        QColor charColor = characterColor(line.character);
        auto* charLabel = new QLabel(QString::fromStdString(line.character));
        charLabel->setFixedWidth(90);
        charLabel->setAlignment(Qt::AlignCenter);
        charLabel->setStyleSheet(
            QString("QLabel { color: %1; font-weight: bold; font-size: 13px; border: none; }")
                .arg(charColor.name()));
        headerRow->addWidget(charLabel);

        auto* dialogueLabel = new QLabel(QString::fromStdString(line.dialogue));
        dialogueLabel->setWordWrap(true);
        dialogueLabel->setStyleSheet(
            QString("QLabel { color: %1; font-size: 13px; border: none; }").arg(Theme::hex(_tc.textPrimary)));
        dialogueLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        headerRow->addWidget(dialogueLabel, 1);
        leftLayout->addLayout(headerRow);

        // â”€â”€ Status icon (right panel top) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        auto* statusIcon = new QLabel;
        statusIcon->setFixedHeight(20);
        statusIcon->setAlignment(Qt::AlignCenter);
        switch (matchState) {
        case 2:
            statusIcon->setText(QStringLiteral("\u2713"));
            statusIcon->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 15px; border: none; }").arg(Theme::hex(_tc.success)));
            break;
        case 1:
            statusIcon->setText("?");
            statusIcon->setStyleSheet(QString("QLabel { color: %1; font-weight: bold; font-size: 15px; border: none; }").arg(Theme::hex(_tc.warning)));
            break;
        default:
            statusIcon->setText(QStringLiteral("\u25CB"));
            statusIcon->setStyleSheet(QString("QLabel { color: %1; font-size: 15px; border: none; }").arg(Theme::hex(_tc.textDisabled)));
            break;
        }
        rightLayout->addWidget(statusIcon);

        // â”€â”€ Audio content â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        MiniWaveformWidget* waveform = nullptr;

        if (hasClip) {
            const auto& clip = m_clips[primaryClipIdx];

            // Confidence bar
            if (confidence > 0.001f) {
                auto* confBar = new QProgressBar;
                confBar->setFixedHeight(4);
                confBar->setRange(0, 100);
                confBar->setValue(static_cast<int>(confidence * 100.0f));
                confBar->setTextVisible(false);
                QString barColor = (confidence >= 0.8f) ? Theme::hex(_tc.success)
                                 : (confidence >= 0.5f) ? Theme::hex(_tc.warning)
                                                         : Theme::hex(_tc.error);
                confBar->setStyleSheet(
                    QString("QProgressBar { background: %2; border: none; border-radius: 2px; }"
                            "QProgressBar::chunk { background: %1; border-radius: 2px; }")
                        .arg(barColor, Theme::hex(_tc.inputBg)));
                confBar->setToolTip(QString("Match confidence: %1%")
                    .arg(static_cast<int>(confidence * 100.0f)));
                leftLayout->addWidget(confBar);
            }

            // Waveform
            waveform = new MiniWaveformWidget;
            auto sampleIt = m_audioSamples.find(clip.sourceFile);
            if (sampleIt != m_audioSamples.end()) {
                waveform->setAudioShared(&sampleIt->second.samples, sampleIt->second.sampleRate,
                                   clip.start, clip.end);
            }

            // Connect waveform signals
            size_t clipIdx = primaryClipIdx;
            connect(waveform, &MiniWaveformWidget::trimChanged,
                    this, [this, clipIdx, waveform](double inPt, double outPt) {
                if (clipIdx < m_clips.size()) {
                    // Capture old state on first drag move
                    if (!m_trimDebounceTimer->isActive()) {
                        m_preTrimStart = m_clips[clipIdx].start;
                        m_preTrimEnd   = m_clips[clipIdx].end;
                        m_preTrimMatchState = m_clips[clipIdx].matchState;
                        m_trimDebounceClipIdx = clipIdx;
                    }
                    m_clips[clipIdx].start = inPt;
                    m_clips[clipIdx].end   = outPt;
                    if (m_clips[clipIdx].matchState == 2)
                        m_clips[clipIdx].matchState = 1;
                    if (waveform->isPlayheadVisible()) {
                        double ph = waveform->playhead();
                        if (ph < inPt) waveform->setPlayhead(inPt);
                        else if (ph >= outPt) waveform->setPlayhead(outPt - 0.001);
                    }
                    m_trimDebounceTimer->start();
                    // Rebuild playback buffer so trim changes are heard immediately
                    if (m_playingClipIdx == static_cast<int>(clipIdx))
                        playClip(clipIdx);
                }
            });
            connect(waveform, &MiniWaveformWidget::seekRequested,
                    this, [this, clipIdx, waveform](double t) {
                if (clipIdx < m_clips.size())
                    t = std::clamp(t, m_clips[clipIdx].start, m_clips[clipIdx].end - 0.001);
                waveform->setPlayhead(t);
                waveform->setPlayheadVisible(true);
                m_selectedClipIdx = static_cast<int>(clipIdx);
                if (m_playingClipIdx == static_cast<int>(clipIdx))
                    seekPlayingClip(t);
                else
                    scrubClipAt(clipIdx, t);
            });
            connect(waveform, &MiniWaveformWidget::playToggleRequested,
                    this, [this, clipIdx]() {
                m_selectedClipIdx = static_cast<int>(clipIdx);
                togglePlayClip(clipIdx);
            });
            connect(waveform, &MiniWaveformWidget::shuttleSpeedChanged,
                    this, [this, clipIdx](double speed) {
                if (clipIdx >= m_clips.size()) {
                    spdlog::warn("AudioSync shuttle: clipIdx {} >= clips.size() {}", clipIdx, m_clips.size());
                    return;
                }
                if (!m_audioEngine) {
                    spdlog::warn("AudioSync shuttle: no audio engine");
                    return;
                }
                spdlog::info("AudioSync shuttle: clip={} speed={:.2f} playing={} engineState={}",
                             clipIdx, speed, m_playingClipIdx,
                             static_cast<int>(m_audioEngine->transportState()));
                m_selectedClipIdx = static_cast<int>(clipIdx);
                if (speed == 0.0) {
                    pausePlayback();
                } else {
                    if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                        playClip(clipIdx);
                        // playClip may fail silently (e.g. no audio data).
                        // If it didn't start, don't try to shuttle.
                        if (m_playingClipIdx != static_cast<int>(clipIdx)) {
                            spdlog::warn("AudioSync shuttle: playClip failed for clip {}", clipIdx);
                            return;
                        }
                    }
                    m_audioEngine->setPlaybackSpeed(speed);
                    if (m_audioEngine->transportState() != TransportState::Playing)
                        m_audioEngine->play();
                    if (m_playheadTimer) m_playheadTimer->start();
                }
            });
            connect(waveform, &MiniWaveformWidget::inPointSet,
                    this, [this, clipIdx, waveform](double t) {
                if (clipIdx < m_clips.size()) {
                    // trimChanged already fired from the same I-key press and
                    // modified start/end/matchState.  Use saved pre-trim state.
                    double oldStart;
                    int oldMS;
                    if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                        m_trimDebounceTimer->stop();
                        oldStart = m_preTrimStart;
                        oldMS    = m_preTrimMatchState;
                    } else {
                        oldStart = m_clips[clipIdx].start;
                        oldMS    = m_clips[clipIdx].matchState;
                        m_clips[clipIdx].start = t;
                        if (m_clips[clipIdx].matchState == 2)
                            m_clips[clipIdx].matchState = 1;
                    }
                    int newMS = m_clips[clipIdx].matchState;
                    if (m_commandStack) {
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Set in point",
                            [this, clipIdx, t, newMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].start = t;
                                    m_clips[clipIdx].matchState = newMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setTrimRange(t, m_clips[clipIdx].end);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            },
                            [this, clipIdx, oldStart, oldMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].start = oldStart;
                                    m_clips[clipIdx].matchState = oldMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setTrimRange(oldStart, m_clips[clipIdx].end);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            }
                        ));
                    }
                    populateLeftList();
                    updateCardMatchStyle(clipIdx);
                    updateSmartBar();

                    // Restart playback from the new in-point so the user
                    // always hears the result of their trim.
                    waveform->setPlayhead(t);
                    waveform->setPlayheadVisible(true);
                    playClip(clipIdx);
                }
            });
            connect(waveform, &MiniWaveformWidget::outPointSet,
                    this, [this, clipIdx, waveform](double t) {
                if (clipIdx < m_clips.size()) {
                    // trimChanged already fired from the same O-key press.
                    double oldEnd;
                    int oldMS;
                    if (m_trimDebounceTimer->isActive() && m_trimDebounceClipIdx == clipIdx) {
                        m_trimDebounceTimer->stop();
                        oldEnd = m_preTrimEnd;
                        oldMS  = m_preTrimMatchState;
                    } else {
                        oldEnd = m_clips[clipIdx].end;
                        oldMS  = m_clips[clipIdx].matchState;
                        m_clips[clipIdx].end = t;
                        if (m_clips[clipIdx].matchState == 2)
                            m_clips[clipIdx].matchState = 1;
                    }
                    int newMS = m_clips[clipIdx].matchState;
                    if (m_commandStack) {
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Set out point",
                            [this, clipIdx, t, newMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].end = t;
                                    m_clips[clipIdx].matchState = newMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setTrimRange(m_clips[clipIdx].start, t);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            },
                            [this, clipIdx, oldEnd, oldMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].end = oldEnd;
                                    m_clips[clipIdx].matchState = oldMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setTrimRange(m_clips[clipIdx].start, oldEnd);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            }
                        ));
                    }
                    populateLeftList();
                    updateCardMatchStyle(clipIdx);
                    updateSmartBar();

                    // Rebuild playback buffer so out-point change is heard immediately
                    if (m_playingClipIdx == static_cast<int>(clipIdx))
                        playClip(clipIdx);
                }
            });
            connect(waveform, &MiniWaveformWidget::deleteRegionRequested,
                    this, [this, clipIdx](double s, double e) {
                if (clipIdx < m_clips.size()) {
                    int oldMS = m_clips[clipIdx].matchState;
                    m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                    if (m_clips[clipIdx].matchState == 2)
                        m_clips[clipIdx].matchState = 1;
                    int newMS = m_clips[clipIdx].matchState;
                    if (m_commandStack) {
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Delete audio region",
                            [this, clipIdx, s, e, newMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].deletedRegions.emplace_back(s, e);
                                    m_clips[clipIdx].matchState = newMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            },
                            [this, clipIdx, oldMS]() {
                                if (clipIdx < m_clips.size() && !m_clips[clipIdx].deletedRegions.empty()) {
                                    m_clips[clipIdx].deletedRegions.pop_back();
                                    m_clips[clipIdx].matchState = oldMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setDeletedRegions(m_clips[clipIdx].deletedRegions);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            }
                        ));
                    }
                }
            });
            connect(waveform, &MiniWaveformWidget::deletedRegionsChanged,
                    this, [this, clipIdx, waveform]() {
                if (clipIdx < m_clips.size()) {
                    auto oldRegions = m_clips[clipIdx].deletedRegions;
                    int oldMS = m_clips[clipIdx].matchState;
                    m_clips[clipIdx].deletedRegions = waveform->deletedRegions();
                    if (m_clips[clipIdx].matchState == 2)
                        m_clips[clipIdx].matchState = 1;
                    int newMS = m_clips[clipIdx].matchState;
                    if (m_playingClipIdx == static_cast<int>(clipIdx))
                        playClip(clipIdx);
                    populateLeftList();
                    updateCardMatchStyle(clipIdx);
                    updateSmartBar();
                    if (m_commandStack) {
                        auto newRegions = m_clips[clipIdx].deletedRegions;
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Edit deleted regions",
                            [this, clipIdx, newRegions, newMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].deletedRegions = newRegions;
                                    m_clips[clipIdx].matchState = newMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setDeletedRegions(newRegions);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            },
                            [this, clipIdx, oldRegions, oldMS]() {
                                if (clipIdx < m_clips.size()) {
                                    m_clips[clipIdx].deletedRegions = oldRegions;
                                    m_clips[clipIdx].matchState = oldMS;
                                    if (auto* wf = waveformForClip(static_cast<int>(clipIdx)))
                                        wf->setDeletedRegions(oldRegions);
                                    populateLeftList();
                                    updateCardMatchStyle(clipIdx);
                                    updateSmartBar();
                                }
                            }
                        ));
                    }
                }
            });
            if (!clip.deletedRegions.empty())
                waveform->setDeletedRegions(clip.deletedRegions);

            leftLayout->addWidget(waveform);

            // â”€â”€ Right-panel controls â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            // CONFIRM / CONFIRMED is a toggle: clicking when tentative
            // (matchState=1) sets matchState=2; clicking when confirmed
            // sets it back to 1.  We update the button in place rather
            // than rebuilding all cards (a populateCards() here would
            // tear down every widget and the scroll position jumps).
            // Both directions push a LambdaCommand so Ctrl+Z reverses
            // either step.
            {
                auto* confirmBtn = new QPushButton;
                confirmBtn->setFixedHeight(36);
                confirmBtn->setCursor(Qt::PointingHandCursor);

                auto applyButtonStyle = [confirmBtn, _tc, _radM](int ms) {
                    const bool isConfirmed = (ms == 2);
                    confirmBtn->setText(isConfirmed
                        ? QStringLiteral("\u2713  CONFIRMED")
                        : QStringLiteral("\u2713  CONFIRM"));
                    confirmBtn->setToolTip(isConfirmed
                        ? "Click to unconfirm match"
                        : "Confirm match");
                    confirmBtn->setStyleSheet(
                        QString("QPushButton { background: %1; color: %2; font-weight: bold; "
                        "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
                        "QPushButton:hover { background: %5; }").arg(
                            isConfirmed ? Theme::hex(_tc.success)
                                        : Theme::hex(_tc.successBtnBg),
                            Theme::hex(_tc.textBright),
                            Theme::hex(_tc.success), _radM,
                            Theme::hex(_tc.successBtnHover)));
                };
                // Size the button using the LONGER "CONFIRMED" label first
                // and lock that as the minimum width.  Otherwise the button
                // sized for "CONFIRM" (shorter) is too narrow to fit
                // "CONFIRMED" on the first paint, clipping the trailing "D"
                // until a manual relayout (e.g. clicking unconfirm then
                // confirm again).  Computing this from sizeHint after the
                // stylesheet is applied lets the actual font / padding /
                // border be measured rather than estimated.
                applyButtonStyle(2);
                confirmBtn->ensurePolished();
                confirmBtn->setMinimumWidth(confirmBtn->sizeHint().width());
                applyButtonStyle(matchState);

                // Captured by QPointer so undo lambdas can safely no-op the
                // visual update if the card has since been torn down by
                // another populateCards() pass \u2014 the underlying m_clips
                // state still gets reverted; only the in-place visual
                // refresh is skipped.
                QPointer<QPushButton> btnGuard(confirmBtn);
                connect(confirmBtn, &QPushButton::clicked, this,
                        [this, clipIdx, btnGuard, applyButtonStyle]() {
                    if (clipIdx >= m_clips.size()) return;
                    const int oldMS = m_clips[clipIdx].matchState;
                    const int newMS = (oldMS == 2) ? 1 : 2;
                    if (oldMS == newMS) return;
                    auto apply = [this, clipIdx, btnGuard, applyButtonStyle](int ms) {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        if (clipIdx >= m_clips.size()) return;
                        m_clips[clipIdx].matchState = ms;
                        if (btnGuard) applyButtonStyle(ms);
                        updateCardMatchStyle(clipIdx);
                        populateLeftList();
                        updateWorkflowState();
                    };
                    if (m_commandStack) {
                        m_commandStack->execute(std::make_unique<LambdaCommand>(
                            newMS == 2 ? "Confirm audio match"
                                       : "Unconfirm audio match",
                            [apply, newMS]() { apply(newMS); },
                            [apply, oldMS]() { apply(oldMS); }));
                    } else {
                        apply(newMS);
                    }
                });
                rightLayout->addWidget(confirmBtn);
            }

            auto* smallBtnRow = new QHBoxLayout;
            smallBtnRow->setSpacing(3);
            auto makeSmallBtn = [&_tc, &_rad](const QString& text, const QString& tip, const QString& bg) {
                auto* btn = new QPushButton(text);
                btn->setFixedHeight(26);
                btn->setToolTip(tip);
                btn->setCursor(Qt::PointingHandCursor);
                btn->setStyleSheet(
                    QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                            "border-radius: %4px; font-size: 12px; padding: 2px 6px; }"
                            "QPushButton:hover { border-color: %5; }")
                        .arg(bg, Theme::hex(_tc.textSecondary), Theme::hex(_tc.controlBorder), _rad, Theme::hex(_tc.accent)));
                return btn;
            };

            auto* playBtn = makeSmallBtn(QStringLiteral("\u25B6"), "Play clip", Theme::hex(_tc.successBg));
            auto* trimBtn = makeSmallBtn(QStringLiteral("\u26A1"), "Auto-trim", Theme::hex(_tc.surface3));
            connect(playBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                m_selectedClipIdx = static_cast<int>(clipIdx);
                togglePlayClip(clipIdx);
            });
            connect(trimBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                autoTrimClip(clipIdx);
            });
            smallBtnRow->addWidget(playBtn);
            smallBtnRow->addWidget(trimBtn);
            rightLayout->addLayout(smallBtnRow);

            auto* rejectBtn = new QPushButton(QStringLiteral("\u2715  Reject"));
            rejectBtn->setFixedHeight(26);
            rejectBtn->setToolTip("Reject match");
            rejectBtn->setCursor(Qt::PointingHandCursor);
            rejectBtn->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; font-size: 12px; padding: 2px 6px; }"
                "QPushButton:hover { border-color: %5; }").arg(
                    Theme::hex(_tc.dangerBg), Theme::hex(_tc.textSecondary),
                    Theme::hex(_tc.controlBorder), _rad, Theme::hex(_tc.error)));
            connect(rejectBtn, &QPushButton::clicked, this, [this, clipIdx]() {
                if (clipIdx >= m_clips.size()) return;
                struct Snap {
                    int         matchState;
                    int         scriptLineNumber;
                    std::string scriptSegment;
                    float       confidence;
                };
                Snap oldSnap{
                    m_clips[clipIdx].matchState,
                    m_clips[clipIdx].scriptLineNumber,
                    m_clips[clipIdx].scriptSegment,
                    m_clips[clipIdx].confidence};
                Snap newSnap{0, -1, std::string{}, 0.0f};
                auto apply = [this, clipIdx](Snap s) {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (clipIdx >= m_clips.size()) return;
                    m_clips[clipIdx].matchState       = s.matchState;
                    m_clips[clipIdx].scriptLineNumber = s.scriptLineNumber;
                    m_clips[clipIdx].scriptSegment    = std::move(s.scriptSegment);
                    m_clips[clipIdx].confidence       = s.confidence;
                    // Defer rebuild so the clicked widget isn't deleted during its own signal
                    QTimer::singleShot(0, this, [this]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        populateCards();
                    });
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Reject audio match",
                        [apply, newSnap]() mutable { apply(std::move(newSnap)); },
                        [apply, oldSnap]() mutable { apply(std::move(oldSnap)); }));
                } else {
                    apply(std::move(newSnap));
                }
            });
            rightLayout->addWidget(rejectBtn);

            rightLayout->addStretch();

            // Reassign dropdown
            auto* lineCombo = new QComboBox;
            lineCombo->setFixedHeight(24);
            lineCombo->setStyleSheet(
                QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                    Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                    Theme::hex(_tc.inputBorder), _rad));
            lineCombo->addItem("Reassign...");
            for (size_t j = 0; j < displayLines.size(); ++j) {
                const auto& dl = displayLines[j];
                QString entry = QString("%1. %2: %3")
                    .arg(dl.displayNum)
                    .arg(QString::fromStdString(dl.character))
                    .arg(QString::fromStdString(dl.dialogue).left(20));
                lineCombo->addItem(entry);
            }
            lineCombo->setCurrentIndex(0);
            auto displayLinesCopy = displayLines;
            connect(lineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, clipIdx, displayLinesCopy](int comboIdx) {
                if (clipIdx >= m_clips.size() || comboIdx <= 0) return;
                size_t lineIdx = static_cast<size_t>(comboIdx - 1);
                if (lineIdx >= displayLinesCopy.size()) return;
                const auto& dl = displayLinesCopy[lineIdx];
                struct Snap {
                    int         matchState;
                    int         scriptLineNumber;
                    std::string character;
                    std::string scriptSegment;
                    float       confidence;
                };
                Snap oldSnap{
                    m_clips[clipIdx].matchState,
                    m_clips[clipIdx].scriptLineNumber,
                    m_clips[clipIdx].character,
                    m_clips[clipIdx].scriptSegment,
                    m_clips[clipIdx].confidence};
                Snap newSnap{1, dl.lineNumber, dl.character,
                             dl.character + ": " + dl.dialogue, 1.0f};
                auto apply = [this, clipIdx](Snap s) {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (clipIdx >= m_clips.size()) return;
                    m_clips[clipIdx].matchState       = s.matchState;
                    m_clips[clipIdx].scriptLineNumber = s.scriptLineNumber;
                    m_clips[clipIdx].character        = std::move(s.character);
                    m_clips[clipIdx].scriptSegment    = std::move(s.scriptSegment);
                    m_clips[clipIdx].confidence       = s.confidence;
                    // Defer rebuild so the combo isn't deleted during its own signal
                    QTimer::singleShot(0, this, [this]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        populateCards();
                    });
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Reassign audio match",
                        [apply, newSnap]() mutable { apply(std::move(newSnap)); },
                        [apply, oldSnap]() mutable { apply(std::move(oldSnap)); }));
                } else {
                    apply(std::move(newSnap));
                }
            });
            rightLayout->addWidget(lineCombo);

        } else {
            // â”€â”€ Unmatched: left (text) | right (manual match) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            auto* noAudioLabel = new QLabel("No audio assigned");
            noAudioLabel->setAlignment(Qt::AlignCenter);
            noAudioLabel->setStyleSheet(
                QString("QLabel { color: %1; font-size: 13px; font-style: italic; border: none; "
                "padding: 20px; }").arg(Theme::hex(_tc.textDisabled)));
            leftLayout->addWidget(noAudioLabel, 1);

            int lineNum = line.lineNumber;

            // Audio file selector â€” lets the user pick which imported
            // audio file this line should match against.
            if (m_audioPaths.size() > 1) {
                auto* audioFileCombo = new QComboBox;
                audioFileCombo->setFixedHeight(24);
                audioFileCombo->setStyleSheet(
                    QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                    "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                        Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                        Theme::hex(_tc.inputBorder), _rad));
                audioFileCombo->addItem("Audio file: auto");
                // Build sorted list of (filename, full path) pairs
                std::vector<std::pair<QString, QString>> sortedAudio;
                sortedAudio.reserve(m_audioPaths.size());
                for (const auto& ap : m_audioPaths) {
                    QString fname = QString::fromStdString(
                        std::filesystem::path(ap).filename().string());
                    sortedAudio.emplace_back(fname, QString::fromStdString(ap));
                }
                std::sort(sortedAudio.begin(), sortedAudio.end(),
                    [](const auto& a, const auto& b) {
                        return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
                    });
                for (const auto& [fname, fullPath] : sortedAudio)
                    audioFileCombo->addItem(fname, fullPath);
                // Pre-select if already assigned
                auto ait = m_lineAudioFile.find(lineNum);
                if (ait != m_lineAudioFile.end()) {
                    for (int ci = 1; ci < audioFileCombo->count(); ++ci) {
                        if (audioFileCombo->itemData(ci).toString().toStdString() == ait->second) {
                            audioFileCombo->setCurrentIndex(ci);
                            break;
                        }
                    }
                }
                connect(audioFileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, lineNum](int idx) {
                    if (idx <= 0) {
                        m_lineAudioFile.erase(lineNum);
                    } else {
                        auto* combo = qobject_cast<QComboBox*>(sender());
                        if (combo) {
                            m_lineAudioFile[lineNum] = combo->itemData(idx).toString().toStdString();
                        }
                    }
                });
                rightLayout->addWidget(audioFileCombo);
            }

            auto* manualBtn = new QPushButton("MANUAL");
            manualBtn->setFixedHeight(40);
            manualBtn->setToolTip("Open manual match dialog");
            manualBtn->setCursor(Qt::PointingHandCursor);
            manualBtn->setStyleSheet(
                QString("QPushButton { background: %1; color: %2; font-weight: bold; "
                "font-size: 13px; border: 2px solid %3; border-radius: %4px; }"
                "QPushButton:hover { background: %3; }").arg(
                    Theme::hex(_tc.accentDim), Theme::hex(_tc.textBright),
                    Theme::hex(_tc.accent), _radM));
            connect(manualBtn, &QPushButton::clicked, this, [this, lineNum]() {
                openManualMatch(lineNum);
            });
            rightLayout->addWidget(manualBtn);

            rightLayout->addStretch();

            // Assign from orphan dropdown
            auto* assignCombo = new QComboBox;
            assignCombo->setFixedHeight(24);
            assignCombo->setStyleSheet(
                QString("QComboBox { background: %1; color: %2; border: 1px solid %3; "
                "padding: 2px 6px; border-radius: %4px; font-size: 12px; }").arg(
                    Theme::hex(_tc.inputBg), Theme::hex(_tc.textPrimary),
                    Theme::hex(_tc.inputBorder), _rad));
            assignCombo->addItem("Assign clip...");
            for (size_t i = 0; i < m_clips.size(); ++i) {
                if (m_clips[i].scriptLineNumber < 0) {
                    const auto& oc = m_clips[i];
                    QString label = QString("Clip #%1 (%2) %3s")
                        .arg(i + 1)
                        .arg(oc.character.empty() ? "?" : QString::fromStdString(oc.character))
                        .arg(oc.end - oc.start, 0, 'f', 1);
                    assignCombo->addItem(label, QVariant(static_cast<int>(i)));
                }
            }
            std::string lineChar = line.character;
            std::string lineDial = line.dialogue;
            connect(assignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this, lineNum, lineChar, lineDial](int idx) {
                if (idx <= 0) return;
                auto* combo = qobject_cast<QComboBox*>(sender());
                if (!combo) return;
                int clipIdxRaw = combo->itemData(idx).toInt();
                if (clipIdxRaw < 0) return;
                const size_t clipIdx = static_cast<size_t>(clipIdxRaw);
                if (clipIdx >= m_clips.size()) return;
                struct Snap {
                    int         matchState;
                    int         scriptLineNumber;
                    std::string character;
                    std::string scriptSegment;
                    float       confidence;
                };
                Snap oldSnap{
                    m_clips[clipIdx].matchState,
                    m_clips[clipIdx].scriptLineNumber,
                    m_clips[clipIdx].character,
                    m_clips[clipIdx].scriptSegment,
                    m_clips[clipIdx].confidence};
                Snap newSnap{1, lineNum, lineChar,
                             lineChar + ": " + lineDial, 1.0f};
                auto apply = [this, clipIdx](Snap s) {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (clipIdx >= m_clips.size()) return;
                    m_clips[clipIdx].matchState       = s.matchState;
                    m_clips[clipIdx].scriptLineNumber = s.scriptLineNumber;
                    m_clips[clipIdx].character        = std::move(s.character);
                    m_clips[clipIdx].scriptSegment    = std::move(s.scriptSegment);
                    m_clips[clipIdx].confidence       = s.confidence;
                    // Defer rebuild so the combo isn't deleted during its own signal
                    QTimer::singleShot(0, this, [this]() {
                        if (m_destroying.load(std::memory_order_acquire)) return;
                        populateCards();
                    });
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Assign audio clip to script line",
                        [apply, newSnap]() mutable { apply(std::move(newSnap)); },
                        [apply, oldSnap]() mutable { apply(std::move(oldSnap)); }));
                } else {
                    apply(std::move(newSnap));
                }
            });
            rightLayout->addWidget(assignCombo);
        }

        // Store mapping
        m_cardWidgets.push_back(card);
        m_cardWaveforms.push_back(waveform);
        m_cardClipIndices.push_back(hasClip ? static_cast<int>(primaryClipIdx) : -1);

        m_rightLayout->addWidget(card);

        // Install event filter on card and focusable children for keyboard routing + click
        card->installEventFilter(this);
        for (auto* child : card->findChildren<QWidget*>()) {
            if (qobject_cast<QLineEdit*>(child) || qobject_cast<QComboBox*>(child))
                continue;
            child->installEventFilter(this);
        }

        // Show the card (pooled cards are hidden after releaseAll)
        card->setVisible(true);
    }

    m_rightLayout->addStretch();

    // Shrink pool to active count + headroom (Phase 5.A)
    m_cardPool.shrink(m_cardPool.activeCount + 16);

    // Re-enable repaints after bulk creation
    if (m_rightScrollContent)
        m_rightScrollContent->setUpdatesEnabled(true);

    // Also update left list to stay in sync
    populateLeftList();
    updateSmartBar();
    updateWorkflowState();

    // Restore scroll position after rebuild. Two passes: one immediate
    // (queued for after the current event-loop iteration so layout settles)
    // and one slightly delayed to overwrite any focus-driven auto-scroll
    // that QScrollArea performs when widgets are re-parented in.
    auto restoreScroll = [this, savedScrollPos]() {
        if (!m_rightScrollArea) return;
        auto* sb = m_rightScrollArea->verticalScrollBar();
        if (!sb) return;
        sb->blockSignals(true);
        sb->setValue(std::min(savedScrollPos, sb->maximum()));
        sb->blockSignals(false);
    };
    if (rightSB) rightSB->blockSignals(false);
    if (m_rightScrollArea && m_rightScrollArea->verticalScrollBar() && savedScrollPos > 0) {
        QTimer::singleShot(0,  this, restoreScroll);
        QTimer::singleShot(30, this, restoreScroll);
    }
}

} // namespace rt
