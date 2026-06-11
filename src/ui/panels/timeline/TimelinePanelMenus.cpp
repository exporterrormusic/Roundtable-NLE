/*
 * TimelinePanelMenus.cpp - Context menu implementations for TimelinePanel.
 * Split from TimelinePanel.cpp for maintainability.
 */

#include "panels/timeline/TimelinePanel.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "timeline/SequenceClip.h"
#include "timeline/EditOperations.h"
#include "command/Command.h"
#include "command/LambdaCommand.h"
#include "command/CompoundCommand.h"
#include "command/CommandStack.h"
#include "effects/EffectStack.h"
#include "audio/AudioFile.h"
#include "analysis/OmniShotDetector.h"
#include "decode/VideoDecoder.h"
#include "Constants.h"

#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QColorDialog>
#include <QLabel>
#include <QPixmap>
#include <QIcon>
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QProgressDialog>
#include <QMessageBox>
#include <QTimer>

#include <spdlog/spdlog.h>
#include <cmath>

namespace rt {
// ═════════════════════════════════════════════════════════════════════════════
//  Clip context menu (Premiere Pro-style right-click menu)
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::showClipContextMenu(const QPointF& globalPos, const ClipRef& ref)
{
    if (!m_timeline) return;

    Track* track = m_timeline->track(ref.trackIndex);
    if (!track) return;
    size_t idx = track->findClipIndexById(ref.clipId);
    if (idx >= track->clipCount()) return;
    Clip* clip = track->clip(idx);
    if (!clip) return;

    // Ensure this clip is selected
    if (!m_selection.isSelected(ref)) {
        m_selection.clear();
        m_selection.selectClip(ref, false);
        emit selectionChanged();
    }

    QMenu menu(this);

    // ── Cut / Copy / Paste ──────────────────────────────────────────────
    QAction* cutAction   = menu.addAction("Cut                  Ctrl+X");
    QAction* copyAction  = menu.addAction("Copy                 Ctrl+C");
    QAction* pasteAction = menu.addAction("Paste                Ctrl+V");
    pasteAction->setEnabled(!m_clipboard.empty());

    menu.addSeparator();

    // ── Delete / Ripple Delete ──────────────────────────────────────────
    QAction* deleteAction       = menu.addAction("Delete (Lift)        Del");
    QAction* rippleDeleteAction = menu.addAction("Ripple Delete        Shift+Del");

    menu.addSeparator();

    // ── Split at Playhead ───────────────────────────────────────────────
    QAction* splitAction = menu.addAction("Split at Playhead    F");

    // ── Enable / Disable ────────────────────────────────────────────────
    QAction* enableAction = menu.addAction(clip->isEnabled() ? "Disable Clip" : "Enable Clip");

    // ── Nest ────────────────────────────────────────────────────────────
    QAction* nestAction = menu.addAction("Nest...");
    nestAction->setEnabled(m_selection.clips().size() >= 1);

    // ── Open Nested Sequence ────────────────────────────────────────────
    QAction* openNestedAction = nullptr;
    if (auto* seqClip = dynamic_cast<SequenceClip*>(clip)) {
        openNestedAction = menu.addAction("Open Nested Sequence");
    }

    menu.addSeparator();

    // ── Speed / Duration ────────────────────────────────────────────────
    QAction* speedAction = menu.addAction("Speed/Duration...");

    // ── Freeze Frame (video clips only) ─────────────────────────────────
    QAction* freezeFrameAction = nullptr;
    QAction* sceneDetectAction = nullptr;
    if (dynamic_cast<VideoClip*>(clip)) {
        freezeFrameAction = menu.addAction("Insert Freeze Frame");
        sceneDetectAction = menu.addAction("Detect Scene Cuts (AI)...");
        sceneDetectAction->setToolTip("OmniShotCut AI — requires Python + checkpoint installed");
        if (!OmniShotDetector::isAvailable())
            sceneDetectAction->setEnabled(false);
    }

    // ── Normalize Audio (AudioClip only) ────────────────────────────────
    QAction* normalizeAction = nullptr;
    QAction* audioGainAction = nullptr;
    if (dynamic_cast<AudioClip*>(clip)) {
        normalizeAction = menu.addAction("Normalize Audio...");
        audioGainAction = menu.addAction("Audio Gain...");
    }

    // ── Rename ──────────────────────────────────────────────────────────
    QAction* renameAction = menu.addAction("Rename...");

    menu.addSeparator();

    // ── Label Color ─────────────────────────────────────────────────────
    QMenu* colorMenu = menu.addMenu("Label Color");
    struct LabelColor { const char* name; uint32_t rgba; };
    static const LabelColor kLabelColors[] = {
        {"Default",     0xFF888888},
        {"Violet",      0xFF9966CC},
        {"Cerulean",    0xFF4A90D9},
        {"Forest",      0xFF4A9B4A},
        {"Rose",        0xFFCC6699},
        {"Mango",       0xFFCC9933},
        {"Lavender",    0xFFBB88DD},
        {"Caribbean",   0xFF33CCAA},
        {"Iris",        0xFF6666CC},
        {"Custom...",   0x00000000},
    };
    std::vector<QAction*> colorActions;
    for (const auto& lc : kLabelColors) {
        QAction* a = colorMenu->addAction(lc.name);
        if (lc.rgba != 0x00000000) {
            QPixmap px(12, 12);
            px.fill(QColor::fromRgba(lc.rgba));
            a->setIcon(QIcon(px));
        }
        colorActions.push_back(a);
    }

    menu.addSeparator();

    // ── Effect Copy/Paste ───────────────────────────────────────────────
    QAction* copyEffectsAction = menu.addAction("Copy Effects");
    copyEffectsAction->setEnabled(clip->effects().effectCount() > 0);
    QAction* pasteEffectsAction = menu.addAction("Paste Effects");
    pasteEffectsAction->setEnabled(m_effectClipboard && m_effectClipboard->effectCount() > 0);

    // ── Attributes Copy/Paste ───────────────────────────────────────────
    QAction* copyAttrsAction = menu.addAction("Copy Attributes");
    QAction* pasteAttrsAction = menu.addAction("Paste Attributes     Ctrl+Shift+V");
    pasteAttrsAction->setEnabled(m_attrClipboard.has_value());

    menu.addSeparator();

    // ── Reveal in Explorer / Project ────────────────────────────────────
    std::string clipMediaPath;
    if (auto* vc = dynamic_cast<VideoClip*>(clip))
        clipMediaPath = vc->mediaPath();
    else if (auto* ac = dynamic_cast<AudioClip*>(clip))
        clipMediaPath = ac->mediaPath();
    else if (auto* ic = dynamic_cast<ImageClip*>(clip))
        clipMediaPath = ic->mediaPath();

    QAction* revealExplorerAction = nullptr;
    QAction* revealProjectAction  = nullptr;
    if (!clipMediaPath.empty()) {
        revealExplorerAction = menu.addAction("Reveal in Explorer");
        revealProjectAction  = menu.addAction("Reveal in Project");
        menu.addSeparator();
    }

    // ── Properties ──────────────────────────────────────────────────────
    QAction* propsAction = menu.addAction("Properties...");

    // ── Execute ─────────────────────────────────────────────────────────
    QAction* chosen = menu.exec(globalPos.toPoint());
    if (!chosen) return;

    if (chosen == cutAction) {
        auto cmd = EditOperations::cutSelection(*m_timeline, m_selection, m_clipboard);
        if (cmd) {
            executeCommand(std::move(cmd));
            m_selection.clear();
            refreshTrackContents();
            emit selectionChanged();
        }
    }
    else if (chosen == copyAction) {
        EditOperations::copySelection(*m_timeline, m_selection, m_clipboard);
        copyAttributesFromSelection();
    }
    else if (chosen == pasteAction) {
        auto cmd = EditOperations::paste(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            executeCommand(std::move(cmd));
            refreshTrackContents();
            // Sync playhead from the model (the command moves it to end
            // of pasted content as part of its undoable state).
            setPlayheadPosition(m_timeline->playheadPosition());
        }
    }
    else if (chosen == deleteAction) {
        auto cmd = EditOperations::deleteSelection(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            executeCommand(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
        }
    }
    else if (chosen == rippleDeleteAction) {
        auto cmd = EditOperations::rippleDelete(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            executeCommand(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
        }
    }
    else if (chosen == splitAction) {
        auto cmd = EditOperations::splitClip(*m_timeline, ref.trackIndex, ref.clipId, m_playheadTick);
        if (cmd) { executeCommand(std::move(cmd)); refreshTrackContents(); }
    }
    else if (chosen == enableAction) {
        if (m_commandStack) {
            Clip* c = clip;
            bool oldEnabled = c->isEnabled();
            bool newEnabled = !oldEnabled;
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                newEnabled ? "Enable Clip" : "Disable Clip",
                [c, newEnabled, this]() {
                    c->setEnabled(newEnabled);
                    onScrollChanged();
                    emit contentChanged();
                },
                [c, oldEnabled, this]() {
                    c->setEnabled(oldEnabled);
                    onScrollChanged();
                    emit contentChanged();
                }));
        } else {
            clip->setEnabled(!clip->isEnabled());
        }
        onScrollChanged();
        emit contentChanged();
    }
    else if (chosen == nestAction) {
        bool ok = false;
        QString nestName = QInputDialog::getText(
            this, "Nest Clips", "Nested Sequence Name:",
            QLineEdit::Normal, "Nested Sequence", &ok);
        if (ok && !nestName.isEmpty()) {
            auto clips = m_selection.clips();
            emit nestSelectedClips(clips, nestName);
        }
    }
    else if (openNestedAction && chosen == openNestedAction) {
        auto* seqClip = dynamic_cast<SequenceClip*>(clip);
        if (seqClip) {
            emit openNestedSequence(seqClip->sequenceIndex());
        }
    }
    else if (chosen == speedAction) {
        QDialog dlg(this);
        dlg.setWindowTitle("Speed/Duration");
        dlg.setMinimumWidth(320);

        auto* layout = new QVBoxLayout(&dlg);
        auto* form = new QFormLayout;

        auto* speedSpin = new QDoubleSpinBox;
        speedSpin->setRange(1.0, 10000.0);
        speedSpin->setDecimals(0);
        speedSpin->setSingleStep(25);
        speedSpin->setSuffix(" %");
        speedSpin->setValue(clip->speed() * 100.0);
        form->addRow("Speed:", speedSpin);

        // Duration display (read-only)
        int64_t durTicks = clip->duration();

        // Auto-select the speed value so the user can type immediately
        speedSpin->setFocus();
        speedSpin->selectAll();
        QTimer::singleShot(0, &dlg, [speedSpin]() {
            speedSpin->setFocus();
            speedSpin->selectAll();
        });
        double durSecs = ticksToSeconds(durTicks);
        auto* durationLabel = new QLabel;
        durationLabel->setText(QString("%1:%2:%3")
            .arg(static_cast<int>(durSecs) / 3600, 2, 10, QChar('0'))
            .arg((static_cast<int>(durSecs) % 3600) / 60, 2, 10, QChar('0'))
            .arg(static_cast<int>(durSecs) % 60, 2, 10, QChar('0')));
        form->addRow("Duration:", durationLabel);

        // Update duration label when speed changes
        int64_t origDur = durTicks;
        double origSpeed = clip->speed();
        QObject::connect(speedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [&](double pct) {
                double newSpeed = pct / 100.0;
                if (newSpeed <= 0.0) newSpeed = 0.01;
                int64_t newDur = static_cast<int64_t>(std::llround(origDur * origSpeed / newSpeed));
                if (newDur < kMinClipDuration) newDur = kMinClipDuration;
                double newSecs = ticksToSeconds(newDur);
                durationLabel->setText(QString("%1:%2:%3")
                    .arg(static_cast<int>(newSecs) / 3600, 2, 10, QChar('0'))
                    .arg((static_cast<int>(newSecs) % 3600) / 60, 2, 10, QChar('0'))
                    .arg(static_cast<int>(newSecs) % 60, 2, 10, QChar('0')));
            });

        auto* pitchCheck = new QCheckBox("Maintain Audio Pitch");
        pitchCheck->setChecked(clip->maintainPitch());
        pitchCheck->setToolTip(
            "When checked, audio pitch is preserved at any speed (like Premiere Pro).\n"
            "When unchecked, pitch changes naturally with speed (chipmunk/slow effect).");
        form->addRow(pitchCheck);

        layout->addLayout(form);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() == QDialog::Accepted) {
            double pct = speedSpin->value();
            double newSpeed = pct / 100.0;
            if (newSpeed <= 0.0) newSpeed = 0.01;
            int64_t newDur = static_cast<int64_t>(std::llround(origDur * origSpeed / newSpeed));
            if (newDur < kMinClipDuration) newDur = kMinClipDuration;

            // Clamp to next clip's start so we don't overlap
            int64_t maxDur = INT64_MAX;
            if (m_timeline) {
                Track* trk = m_timeline->track(ref.trackIndex);
                if (trk) {
                    size_t ci = trk->findClipIndexById(ref.clipId);
                    for (size_t i = ci + 1; i < trk->clipCount(); ++i) {
                        const Clip* next = trk->clip(i);
                        if (next && next->timelineIn() > clip->timelineIn()) {
                            maxDur = next->timelineIn() - clip->timelineIn();
                            break;
                        }
                    }
                }
            }
            if (newDur > maxDur && maxDur >= kMinClipDuration) newDur = maxDur;

            if (m_commandStack) {
                Clip* c = clip;
                double oldSpeed = c->speed();
                int64_t oldDur = c->duration();
                bool oldPitch = c->maintainPitch();
                bool newPitch = pitchCheck->isChecked();
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Change Speed/Duration",
                    [c, newSpeed, newDur, newPitch, this]() {
                        c->setSpeed(newSpeed);
                        c->setDuration(newDur);
                        c->setMaintainPitch(newPitch);
                        onScrollChanged();
                        emit contentChanged();
                    },
                    [c, oldSpeed, oldDur, oldPitch, this]() {
                        c->setSpeed(oldSpeed);
                        c->setDuration(oldDur);
                        c->setMaintainPitch(oldPitch);
                        onScrollChanged();
                        emit contentChanged();
                    }));
            } else {
                clip->setSpeed(newSpeed);
                clip->setDuration(newDur);
                clip->setMaintainPitch(pitchCheck->isChecked());
            }
            onScrollChanged();
            emit contentChanged();
        }
    }
    else if (freezeFrameAction && chosen == freezeFrameAction) {
        // Split at playhead, then set the right half's speed to 0 (frame hold)
        if (m_playheadTick > clip->timelineIn() && m_playheadTick < clip->timelineOut()) {
            auto splitCmd = EditOperations::splitClip(
                *m_timeline, ref.trackIndex, ref.clipId, m_playheadTick);
            if (splitCmd) {
                executeCommand(std::move(splitCmd));
                // Find the new right-side clip (starts at playhead)
                auto* freezeTrack = m_timeline->track(ref.trackIndex);
                if (freezeTrack) {
                    for (size_t ci = 0; ci < freezeTrack->clipCount(); ++ci) {
                        auto* c = freezeTrack->clip(ci);
                        if (c && c->timelineIn() == m_playheadTick) {
                            c->setSpeed(0.0);
                            // Default freeze duration: 2 seconds
                            c->setDuration(96000);
                            break;
                        }
                    }
                }
                refreshTrackContents();
                emit contentChanged();
            }
        }
    }
    else if (sceneDetectAction && chosen == sceneDetectAction) {
        auto* vc = dynamic_cast<VideoClip*>(clip);
        if (vc && !vc->mediaPath().empty()) {
            // Determine source frame range to scan.
            // The clip may be trimmed, so only scan the visible portion.
            VideoDecoder probe;
            if (!probe.open(vc->mediaPath(), true)) {
                QMessageBox::warning(this, "Scene Detection",
                    QString("Cannot open media: %1").arg(QString::fromStdString(probe.lastError())));
            } else {
                const auto& vinfo = probe.info();
                double fps = vinfo.fps > 0 ? vinfo.fps : 24.0;
                double clipSpeed = std::max(clip->speed(), 0.01);

                // sourceIn/duration are in timeline ticks — convert to source seconds and then frames
                double srcInSec  = ticksToSeconds(clip->sourceIn()) * clipSpeed;
                double srcOutSec = ticksToSeconds(clip->sourceIn() + clip->duration()) * clipSpeed;
                int64_t startFrame = static_cast<int64_t>(srcInSec * fps);
                int64_t endFrame   = static_cast<int64_t>(srcOutSec * fps);
                probe.close();

                // Threshold dialog
                bool ok = false;
                double threshold = QInputDialog::getDouble(
                    this, "Detect Scene Cuts",
                    "Sensitivity threshold (lower = more cuts):",
                    27.0, 1.0, 200.0, 1, &ok);
                if (!ok) return;

                // Progress dialog
                auto* progress = new QProgressDialog("Detecting scene cuts...", "Cancel", 0, 100, this);
                progress->setWindowModality(Qt::WindowModal);
                progress->setMinimumDuration(0);
                progress->setValue(0);

                // Keep track/clip info for splitting after detection
                size_t   capturedTrack  = ref.trackIndex;
                uint64_t capturedClipId = ref.clipId;
                int64_t  clipTimelineIn = clip->timelineIn();
                int64_t  clipSourceIn   = clip->sourceIn();

                auto* detector = new OmniShotDetector();

                // Wire cancel button
                connect(progress, &QProgressDialog::canceled, this, [detector]() {
                    detector->cancel();
                });

                // Use a timer to poll progress from the worker thread
                auto* pollTimer = new QTimer(this);
                struct PollState {
                    std::atomic<float> percent{0.0f};
                    std::atomic<bool>  done{false};
                    std::atomic<bool>  errored{false};
                    std::string        errorMsg;
                    std::vector<DetectedCut> cuts;
                    std::mutex         mutex;
                };
                auto* state = new PollState();

                detector->detectAsync(
                    vc->mediaPath(), startFrame, endFrame,
                    static_cast<float>(threshold),
                    // onProgress (worker thread)
                    [state](float pct, int64_t /*frame*/, int64_t /*total*/) {
                        state->percent.store(pct);
                    },
                    // onComplete (worker thread)
                    [state](std::vector<DetectedCut> cuts) {
                        std::lock_guard lock(state->mutex);
                        state->cuts = std::move(cuts);
                        state->done.store(true);
                    },
                    // onError (worker thread)
                    [state](std::string msg) {
                        std::lock_guard lock(state->mutex);
                        state->errorMsg = std::move(msg);
                        state->errored.store(true);
                        state->done.store(true);
                    }
                );

                connect(pollTimer, &QTimer::timeout, this,
                    [this, progress, pollTimer, detector, state,
                     capturedTrack, capturedClipId, clipTimelineIn, clipSourceIn, fps]()
                {
                    if (m_destroying.load(std::memory_order_acquire)) { pollTimer->stop(); return; }
                    // Update progress bar
                    float pct = state->percent.load();
                    progress->setValue(static_cast<int>(pct * 100.0f));

                    if (!state->done.load()) return;

                    // Done — clean up timer
                    pollTimer->stop();
                    pollTimer->deleteLater();
                    progress->close();
                    progress->deleteLater();

                    if (state->errored.load()) {
                        std::string errMsg;
                        { std::lock_guard lock(state->mutex); errMsg = state->errorMsg; }
                        QMessageBox::warning(this, "Scene Detection",
                            QString("Detection failed: %1").arg(QString::fromStdString(errMsg)));
                        delete detector;
                        delete state;
                        return;
                    }

                    std::vector<DetectedCut> cuts;
                    { std::lock_guard lock(state->mutex); cuts = std::move(state->cuts); }

                    delete detector;
                    delete state;

                    if (cuts.empty()) {
                        QMessageBox::information(this, "Scene Detection",
                            "No scene cuts detected.");
                        return;
                    }

                    // Confirm with user
                    auto reply = QMessageBox::question(this, "Scene Detection",
                        QString("%1 scene cut(s) detected.\n\nSplit clip at all detected cuts?")
                            .arg(cuts.size()),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                    if (reply != QMessageBox::Yes) return;

                    // Build a compound command that splits the clip at each cut point.
                    // Split right-to-left so earlier split positions stay valid.
                    std::sort(cuts.begin(), cuts.end(), [](const DetectedCut& a, const DetectedCut& b) {
                        return a.frameNumber > b.frameNumber; // descending
                    });

                    // Convert cut frame numbers to timeline ticks
                    // cutSourceTick = sourceIn offset in ticks for this frame
                    // splitTimeline = clipTimelineIn + (cutSourceTick - clipSourceIn) / speed
                    double clipSpeed = 1.0;
                    if (m_timeline) {
                        Track* trk = m_timeline->track(capturedTrack);
                        if (trk) {
                            size_t ci = trk->findClipIndexById(capturedClipId);
                            if (ci < trk->clipCount()) {
                                clipSpeed = std::max(trk->clip(ci)->speed(), 0.01);
                            }
                        }
                    }

                    auto compound = std::make_unique<CompoundCommand>("Detect Scene Cuts");

                    // Find linked audio clip on the same track (Premiere-style: audio
                    // linked to video shares the same media path and timeline span).
                    size_t audioTrackIdx = SIZE_MAX;
                    uint64_t audioClipId = 0;
                    {
                        Track* vt = m_timeline->track(capturedTrack);
                        if (vt) {
                            std::string videoPath = static_cast<VideoClip*>(
                                vt->clip(vt->findClipIndexById(capturedClipId)))->mediaPath();
                            for (size_t ai = 0; ai < m_timeline->trackCount(); ++ai) {
                                Track* at = m_timeline->track(ai);
                                if (!at || at->type() != TrackType::Audio) continue;
                                for (size_t aci = 0; aci < at->clipCount(); ++aci) {
                                    auto* ac = dynamic_cast<AudioClip*>(at->clip(aci));
                                    if (ac && ac->mediaPath() == videoPath) {
                                        audioTrackIdx = ai;
                                        audioClipId = ac->id();
                                        goto foundAudio;
                                    }
                                }
                            }
                            foundAudio:;
                        }
                    }

                    // We need the current clip ID after each split.
                    // Since splitClip trims the original (left) and adds a new right clip,
                    // splitting right-to-left means we always split the same original clip.
                    uint64_t currentClipId = capturedClipId;

                    for (const auto& cut : cuts) {
                        // Frame → source seconds → source ticks
                        double cutSrcSec = static_cast<double>(cut.frameNumber) / fps;
                        int64_t cutSrcTick = secondsToTicks(cutSrcSec);

                        // Convert source tick to timeline position
                        int64_t splitTick = clipTimelineIn +
                            static_cast<int64_t>(
                                static_cast<double>(cutSrcTick - clipSourceIn) / clipSpeed);

                        auto splitCmd = EditOperations::splitClip(
                            *m_timeline, capturedTrack, currentClipId, splitTick);
                        if (splitCmd) {
                            splitCmd->execute();
                            compound->addExecuted(std::move(splitCmd));
                        }
                        // Also split linked audio at the same timeline position
                        if (audioTrackIdx != SIZE_MAX && audioClipId != 0) {
                            auto audioSplit = EditOperations::splitClip(
                                *m_timeline, audioTrackIdx, audioClipId, splitTick);
                            if (audioSplit) {
                                audioSplit->execute();
                                compound->addExecuted(std::move(audioSplit));
                            }
                        }
                    }

                    if (compound->size() > 0) {
                        // All sub-commands already executed; push compound to undo stack
                        m_commandStack->pushWithoutExecute(std::move(compound));
                        refreshTrackContents();
                        emit contentChanged();
                    }
                });

                pollTimer->start(50); // 20 fps progress updates
            }
        }
    }
    else if (normalizeAction && chosen == normalizeAction) {
        auto* ac = dynamic_cast<AudioClip*>(clip);
        if (ac) {
            AudioFile af;
            if (af.open(ac->mediaPath())) {
                auto samples = af.readAll();
                if (!samples.empty()) {
                    float peak = 0.0f;
                    for (float s : samples)
                        peak = std::max(peak, std::fabs(s));

                    if (peak > 1e-6f) {
                        float peakDb = 20.0f * std::log10(peak);
                        bool ok = false;
                        double targetDb = QInputDialog::getDouble(
                            this, "Normalize Audio",
                            QString("Current peak: %1 dB\n\nNormalize peak to (dB):")
                                .arg(static_cast<double>(peakDb), 0, 'f', 1),
                            0.0, -60.0, 0.0, 1, &ok);

                        if (ok) {
                            float targetLin = std::pow(10.0f, static_cast<float>(targetDb) / 20.0f);
                            float gain = targetLin / peak;
                            auto& vol = ac->volume();
                            for (size_t k = 0; k < vol.keyframeCount(); ++k)
                                vol.keyframe(k).value *= gain;
                            onScrollChanged();
                            emit contentChanged();
                        }
                    }
                }
            }
        }
    }
    else if (audioGainAction && chosen == audioGainAction) {
        auto* ac = dynamic_cast<AudioClip*>(clip);
        if (ac) {
            // Current gain: read the first keyframe value (or 1.0 if none)
            auto& vol = ac->volume();
            float currentLin = (vol.keyframeCount() > 0) ? vol.keyframe(0).value : 1.0f;
            float currentDb = (currentLin > 1e-6f)
                ? 20.0f * std::log10(currentLin)
                : -96.0f;

            QDialog dlg(this);
            dlg.setWindowTitle("Audio Gain");
            dlg.setMinimumWidth(300);

            auto* layout = new QVBoxLayout(&dlg);
            auto* form = new QFormLayout;

            auto* gainSpin = new QDoubleSpinBox;
            gainSpin->setRange(-96.0, 24.0);
            gainSpin->setDecimals(1);
            gainSpin->setSingleStep(0.5);
            gainSpin->setSuffix(" dB");
            gainSpin->setValue(static_cast<double>(currentDb));
            form->addRow("Set Gain to:", gainSpin);

            layout->addLayout(form);

            auto* buttons = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

            if (dlg.exec() == QDialog::Accepted) {
                float newLin = std::pow(10.0f, static_cast<float>(gainSpin->value()) / 20.0f);
                if (vol.keyframeCount() == 0) {
                    // No keyframes — set exactly one at frame 0
                    vol.keyframe(0).value = newLin;
                } else {
                    // Scale all keyframes relative to the change
                    float ratio = (currentLin > 1e-6f) ? (newLin / currentLin) : newLin;
                    for (size_t k = 0; k < vol.keyframeCount(); ++k)
                        vol.keyframe(k).value *= ratio;
                }
                onScrollChanged();
                emit contentChanged();
            }
        }
    }
    else if (chosen == renameAction) {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Clip", "Clip name:",
            QLineEdit::Normal,
            QString::fromStdString(clip->label()), &ok);
        if (ok) {
            if (m_commandStack) {
                Clip* c = clip;
                std::string oldLabel = c->label();
                std::string newNameStd = newName.toStdString();
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Rename Clip",
                    [c, newNameStd, this]() {
                        c->setLabel(newNameStd);
                        onScrollChanged();
                    },
                    [c, oldLabel, this]() {
                        c->setLabel(oldLabel);
                        onScrollChanged();
                    }));
            } else {
                clip->setLabel(newName.toStdString());
            }
            onScrollChanged();
        }
    }
    else if (chosen == propsAction) {
        // Emit signal so PropertiesPanel shows this clip
        emit clipSelected(ref.trackIndex, idx);
    }
    else if (revealExplorerAction && chosen == revealExplorerAction) {
        QString path = QFileInfo(QString::fromStdString(clipMediaPath)).absoluteFilePath();
        QProcess::startDetached("explorer", { "/select,", QDir::toNativeSeparators(path) });
    }
    else if (revealProjectAction && chosen == revealProjectAction) {
        emit revealInProjectBin(QString::fromStdString(clipMediaPath));
    }
    else if (chosen == copyEffectsAction) {
        m_effectClipboard = clip->effects().clone();
    }
    else if (chosen == pasteEffectsAction) {
        if (m_effectClipboard && m_effectClipboard->effectCount() > 0) {
            // Paste cloned effects onto all selected clips
            for (const auto& sel : m_selection.clips()) {
                Track* st = m_timeline->track(sel.trackIndex);
                if (!st) continue;
                size_t si = st->findClipIndexById(sel.clipId);
                if (si >= st->clipCount()) continue;
                Clip* target = st->clip(si);
                auto cloned = m_effectClipboard->clone();
                for (size_t ei = 0; ei < cloned->effectCount(); ++ei) {
                    target->effects().addEffect(cloned->removeEffect(0));
                }
            }
            onScrollChanged();
            emit contentChanged();
        }
    }
    else if (chosen == copyAttrsAction) {
        copyAttributesFromSelection();
    }
    else if (chosen == pasteAttrsAction) {
        showPasteAttributesDialog();
    }
    else {
        // Check label colors
        for (size_t ci = 0; ci < colorActions.size(); ++ci) {
            if (chosen == colorActions[ci]) {
                uint32_t rgba = kLabelColors[ci].rgba;
                if (rgba == 0x00000000) {
                    // Custom color picker
                    QColor current = QColor::fromRgba(clip->color());
                    QColor picked = QColorDialog::getColor(current, this, "Label Color");
                    if (picked.isValid()) {
                        rgba = picked.rgba();
                        clip->setColor(rgba);
                        // Apply to all selected clips
                        for (const auto& sel : m_selection.clips()) {
                            Track* st = m_timeline->track(sel.trackIndex);
                            if (!st) continue;
                            size_t si = st->findClipIndexById(sel.clipId);
                            if (si < st->clipCount())
                                st->clip(si)->setColor(rgba);
                        }
                        onScrollChanged();
                    }
                } else {
                    // Preset color — apply to all selected clips
                    for (const auto& sel : m_selection.clips()) {
                        Track* st = m_timeline->track(sel.trackIndex);
                        if (!st) continue;
                        size_t si = st->findClipIndexById(sel.clipId);
                        if (si < st->clipCount())
                            st->clip(si)->setColor(rgba);
                    }
                    onScrollChanged();
                }
                break;
            }
        }
    }
}

void TimelinePanel::showEmptyAreaContextMenu(const QPointF& globalPos, size_t trackIndex)
{
    QMenu menu(this);

    QAction* pasteAction = menu.addAction("Paste");
    pasteAction->setEnabled(!m_clipboard.empty());

    menu.addSeparator();

    QAction* addVideoTrack = menu.addAction("Add Video Track");
    QAction* addAudioTrack = menu.addAction("Add Audio Track");
    QAction* addDividerTrack = menu.addAction("Add Divider Track");

    menu.addSeparator();

    QAction* selectAll = menu.addAction("Select All          Ctrl+A");

    QAction* chosen = menu.exec(globalPos.toPoint());
    if (!chosen) return;

    if (chosen == pasteAction) {
        auto cmd = EditOperations::paste(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            executeCommand(std::move(cmd));
            refreshTrackContents();
            // Sync playhead from the model (the command moves it to end
            // of pasted content as part of its undoable state).
            setPlayheadPosition(m_timeline->playheadPosition());
        }
    }
    else if (chosen == addVideoTrack) {
        emit addTrackAbove(trackIndex, true);
    }
    else if (chosen == addAudioTrack) {
        emit addTrackBelow(trackIndex < SIZE_MAX ? trackIndex + 1 : 0, false);
    }
    else if (chosen == addDividerTrack) {
        if (m_timeline) {
            size_t insertAt = (trackIndex < SIZE_MAX)
                                  ? trackIndex + 1
                                  : m_timeline->trackCount();
            addDividerUndoable(insertAt);
        }
    }
    else if (chosen == selectAll) {
        if (m_timeline) {
            m_selection.selectAll(*m_timeline);
            emit selectionChanged();
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Paste Attributes dialog (Premiere Pro-style with persistent checkbox state)
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::showPasteAttributesDialog()
{
    if (!m_attrClipboard || !m_timeline) return;

    QDialog dlg(this);
    dlg.setWindowTitle("Paste Attributes");
    dlg.setMinimumWidth(260);

    auto* layout = new QVBoxLayout(&dlg);
    auto* label = new QLabel("Select attributes to paste:", &dlg);
    layout->addWidget(label);


    // Determine the type of the first selected clip
    bool isAudio = false, isVideo = false;
    if (!m_selection.clips().empty() && m_timeline) {
        const auto& sel = m_selection.clips()[0];
        Track* track = m_timeline->track(sel.trackIndex);
        if (track) {
            size_t idx = track->findClipIndexById(sel.clipId);
            if (idx < track->clipCount()) {
                Clip* clip = track->clip(idx);
                if (dynamic_cast<AudioClip*>(clip)) isAudio = true;
                if (dynamic_cast<VideoClip*>(clip) || dynamic_cast<ImageClip*>(clip) || dynamic_cast<SequenceClip*>(clip)) isVideo = true;
            }
        }
    }

    std::vector<QCheckBox*> checks;
    // Audio clips: only show volume, pan, and speed if present
    if (isAudio && !isVideo) {
        auto* chkVolume = new QCheckBox("Volume", &dlg);
        auto* chkPan    = new QCheckBox("Pan", &dlg);
        auto* chkSpeed  = new QCheckBox("Speed", &dlg);
        checks = { chkVolume, chkPan, chkSpeed };
        // Clamp mask to number of checkboxes
        int numChecks = static_cast<int>(checks.size());
        unsigned int mask = m_pasteAttrMask & ((1u << numChecks) - 1);
        for (int i = 0; i < numChecks; ++i) {
            checks[i]->setChecked((mask >> i) & 1);
            layout->addWidget(checks[i]);
        }
    } else if (isVideo) {
        // Video/image/sequence: show transform, speed
        auto* chkOpacity   = new QCheckBox("Opacity", &dlg);
        auto* chkPosX      = new QCheckBox("Position X", &dlg);
        auto* chkPosY      = new QCheckBox("Position Y", &dlg);
        auto* chkScaleX    = new QCheckBox("Scale X", &dlg);
        auto* chkScaleY    = new QCheckBox("Scale Y", &dlg);
        auto* chkRotation  = new QCheckBox("Rotation", &dlg);
        auto* chkSpeed     = new QCheckBox("Speed", &dlg);
        auto* chkSpeedRamp = new QCheckBox("Speed Ramp", &dlg);
        checks = { chkOpacity, chkPosX, chkPosY, chkScaleX, chkScaleY, chkRotation, chkSpeed, chkSpeedRamp };
        int numChecks = static_cast<int>(checks.size());
        unsigned int mask = m_pasteAttrMask & ((1u << numChecks) - 1);
        for (int i = 0; i < numChecks; ++i) {
            checks[i]->setChecked((mask >> i) & 1);
            layout->addWidget(checks[i]);
        }
    } else {
        // Fallback: show all
        auto* chkOpacity   = new QCheckBox("Opacity", &dlg);
        auto* chkPosX      = new QCheckBox("Position X", &dlg);
        auto* chkPosY      = new QCheckBox("Position Y", &dlg);
        auto* chkScaleX    = new QCheckBox("Scale X", &dlg);
        auto* chkScaleY    = new QCheckBox("Scale Y", &dlg);
        auto* chkRotation  = new QCheckBox("Rotation", &dlg);
        auto* chkSpeed     = new QCheckBox("Speed", &dlg);
        auto* chkSpeedRamp = new QCheckBox("Speed Ramp", &dlg);
        checks = { chkOpacity, chkPosX, chkPosY, chkScaleX, chkScaleY, chkRotation, chkSpeed, chkSpeedRamp };
        int numChecks = static_cast<int>(checks.size());
        unsigned int mask = m_pasteAttrMask & ((1u << numChecks) - 1);
        for (int i = 0; i < numChecks; ++i) {
            checks[i]->setChecked((mask >> i) & 1);
            layout->addWidget(checks[i]);
        }
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        // Save checkbox state for next time (Premiere-style persistence)
        m_pasteAttrMask = 0;
        for (int i = 0; i < (int)checks.size(); ++i) {
            if (checks[i]->isChecked())
                m_pasteAttrMask |= (1 << i);
        }

        // Wrap paste attributes in an undoable command
        // Capture old property values for each modified clip
        struct AttrSnapshot {
            size_t trackIdx;
            uint64_t clipId;
            // Common
            float oldSpeed{1.0f};
            // Transform (video)
            float oldOpacity{1.0f};
            float oldPosX{0.0f}, oldPosY{0.0f};
            float oldScaleX{1.0f}, oldScaleY{1.0f};
            float oldRotation{0.0f};
            // Speed ramp
            KeyframeTrack<float> oldSpeedRamp{1.0f};
            // Audio
            float oldVolume{0.0f}, oldPan{0.0f};
        };
        std::vector<AttrSnapshot> snapshots;

        for (const auto& sel : m_selection.clips()) {
            Track* st = m_timeline->track(sel.trackIndex);
            if (!st) continue;
            size_t si = st->findClipIndexById(sel.clipId);
            if (si >= st->clipCount()) continue;
            Clip* target = st->clip(si);

            AttrSnapshot snap;
            snap.trackIdx = sel.trackIndex;
            snap.clipId = sel.clipId;

            if (isAudio && !isVideo) {
                if (auto* ac = dynamic_cast<AudioClip*>(target)) {
                    snap.oldVolume = ac->volume().defaultValue();
                    snap.oldPan = ac->pan().defaultValue();
                }
                snap.oldSpeed = target->speed();
            } else {
                snap.oldOpacity  = target->opacity().defaultValue();
                snap.oldPosX     = target->positionX().defaultValue();
                snap.oldPosY     = target->positionY().defaultValue();
                snap.oldScaleX   = target->scaleX().defaultValue();
                snap.oldScaleY   = target->scaleY().defaultValue();
                snap.oldRotation = target->rotation().defaultValue();
                snap.oldSpeed    = target->speed();
                snap.oldSpeedRamp = target->speedRamp();
            }
            snapshots.push_back(snap);

            // Apply new values
            if (isAudio && !isVideo) {
                if (checks.size() > 0 && checks[0]->isChecked()) {
                    if (auto* ac = dynamic_cast<AudioClip*>(target))
                        ac->volume() = m_attrClipboard->opacity;
                }
                if (checks.size() > 1 && checks[1]->isChecked()) {
                    if (auto* ac = dynamic_cast<AudioClip*>(target))
                        ac->pan() = m_attrClipboard->posX;
                }
                if (checks.size() > 2 && checks[2]->isChecked()) {
                    target->setSpeed(m_attrClipboard->speed);
                }
            } else {
                if (checks.size() > 0 && checks[0]->isChecked()) target->opacity()   = m_attrClipboard->opacity;
                if (checks.size() > 1 && checks[1]->isChecked()) target->positionX() = m_attrClipboard->posX;
                if (checks.size() > 2 && checks[2]->isChecked()) target->positionY() = m_attrClipboard->posY;
                if (checks.size() > 3 && checks[3]->isChecked()) target->scaleX()    = m_attrClipboard->scaleX;
                if (checks.size() > 4 && checks[4]->isChecked()) target->scaleY()    = m_attrClipboard->scaleY;
                if (checks.size() > 5 && checks[5]->isChecked()) target->rotation()  = m_attrClipboard->rotation;
                if (checks.size() > 6 && checks[6]->isChecked()) target->setSpeed(m_attrClipboard->speed);
                if (checks.size() > 7 && checks[7]->isChecked()) target->speedRamp() = m_attrClipboard->speedRamp;
            }
        }

        // Wrap the entire operation in a LambdaCommand for undo/redo
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Paste Attributes",
                [this, snapshots, checks_states = [&]() {
                    std::vector<bool> states;
                    for (auto* ck : checks) states.push_back(ck->isChecked());
                    return states;
                }(), isAudio, isVideo]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    // Redo: re-apply from clipboard
                    if (!m_timeline || !m_attrClipboard) return;
                    for (const auto& snap : snapshots) {
                        Track* st = m_timeline->track(snap.trackIdx);
                        if (!st) continue;
                        size_t si = st->findClipIndexById(snap.clipId);
                        if (si >= st->clipCount()) continue;
                        Clip* target = st->clip(si);
                        if (isAudio && !isVideo) {
                            if (checks_states.size() > 0 && checks_states[0]) {
                                if (auto* ac = dynamic_cast<AudioClip*>(target))
                                    ac->volume() = m_attrClipboard->opacity;
                            }
                            if (checks_states.size() > 1 && checks_states[1]) {
                                if (auto* ac = dynamic_cast<AudioClip*>(target))
                                    ac->pan() = m_attrClipboard->posX;
                            }
                            if (checks_states.size() > 2 && checks_states[2])
                                target->setSpeed(m_attrClipboard->speed);
                        } else {
                            if (checks_states.size() > 0 && checks_states[0]) target->opacity()   = m_attrClipboard->opacity;
                            if (checks_states.size() > 1 && checks_states[1]) target->positionX() = m_attrClipboard->posX;
                            if (checks_states.size() > 2 && checks_states[2]) target->positionY() = m_attrClipboard->posY;
                            if (checks_states.size() > 3 && checks_states[3]) target->scaleX()    = m_attrClipboard->scaleX;
                            if (checks_states.size() > 4 && checks_states[4]) target->scaleY()    = m_attrClipboard->scaleY;
                            if (checks_states.size() > 5 && checks_states[5]) target->rotation()  = m_attrClipboard->rotation;
                            if (checks_states.size() > 6 && checks_states[6]) target->setSpeed(m_attrClipboard->speed);
                            if (checks_states.size() > 7 && checks_states[7]) target->speedRamp() = m_attrClipboard->speedRamp;
                        }
                    }
                    onScrollChanged();
                    emit contentChanged();
                },
                [this, snapshots, isAudio, isVideo]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    // Undo: restore old values
                    if (!m_timeline) return;
                    for (const auto& snap : snapshots) {
                        Track* st = m_timeline->track(snap.trackIdx);
                        if (!st) continue;
                        size_t si = st->findClipIndexById(snap.clipId);
                        if (si >= st->clipCount()) continue;
                        Clip* target = st->clip(si);
                        if (isAudio && !isVideo) {
                            if (auto* ac = dynamic_cast<AudioClip*>(target)) {
                                ac->volume().setDefaultValue(snap.oldVolume);
                                ac->pan().setDefaultValue(snap.oldPan);
                            }
                            target->setSpeed(snap.oldSpeed);
                        } else {
                            target->opacity().setDefaultValue(snap.oldOpacity);
                            target->positionX().setDefaultValue(snap.oldPosX);
                            target->positionY().setDefaultValue(snap.oldPosY);
                            target->scaleX().setDefaultValue(snap.oldScaleX);
                            target->scaleY().setDefaultValue(snap.oldScaleY);
                            target->rotation().setDefaultValue(snap.oldRotation);
                            target->setSpeed(snap.oldSpeed);
                            target->speedRamp() = snap.oldSpeedRamp;
                        }
                    }
                    onScrollChanged();
                    emit contentChanged();
                }));
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Copy attributes from the first selected clip
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::copyAttributesFromSelection()
{
    if (!m_timeline) return;

    // Use the first selected clip
    auto sel = m_selection.clips();
    if (sel.empty()) return;

    Track* track = m_timeline->track(sel[0].trackIndex);
    if (!track) return;
    size_t idx = track->findClipIndexById(sel[0].clipId);
    if (idx >= track->clipCount()) return;
    Clip* clip = track->clip(idx);
    if (!clip) return;

    AttributesClipboard ac;
    ac.opacity   = clip->opacity();
    ac.posX      = clip->positionX();
    ac.posY      = clip->positionY();
    ac.scaleX    = clip->scaleX();
    ac.scaleY    = clip->scaleY();
    ac.rotation  = clip->rotation();
    ac.speed     = clip->speed();
    ac.speedRamp = clip->speedRamp();
    m_attrClipboard = std::move(ac);
}


} // namespace rt