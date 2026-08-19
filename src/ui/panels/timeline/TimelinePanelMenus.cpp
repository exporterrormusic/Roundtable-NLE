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
#include "AudioStreamLabels.h"
#include "PathUtils.h"
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
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QColorDialog>
#include <QLabel>
#include <QPixmap>
#include <QIcon>
#include <QProcess>
#include <QFileInfo>
#include <QFileDialog>
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
    QMenu*   audioStreamMenu = nullptr;   // "Audio Stream ▸" — multi-track sources
    if (auto* ac = dynamic_cast<AudioClip*>(clip)) {
        normalizeAction = menu.addAction("Normalize Audio...");
        audioGainAction = menu.addAction("Audio Gain...");

        // Offer a stream picker only when the source actually has more than
        // one audio stream (multicam / camera scratch+lav / OBS multi-track).
        const auto streams = AudioFile::enumerateAudioStreams(utf8ToPath(ac->mediaPath()));
        if (streams.size() > 1) {
            audioStreamMenu = menu.addMenu("Audio Stream");
            const int current = ac->audioStreamIndex();
            QAction* autoAct = audioStreamMenu->addAction(tr("Auto (best)"));
            autoAct->setCheckable(true);
            autoAct->setChecked(current < 0);
            autoAct->setData(-1);
            for (const auto& s : streams) {
                QAction* a = audioStreamMenu->addAction(audioStreamLabel(s));
                a->setCheckable(true);
                a->setChecked(current == s.ordinal);
                a->setData(s.ordinal);
            }
        }
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
    QAction* relinkMediaAction    = nullptr;
    if (!clipMediaPath.empty()) {
        relinkMediaAction    = menu.addAction("Re-link Media...");
        revealExplorerAction = menu.addAction("Reveal in Explorer");
        revealProjectAction  = menu.addAction("Reveal in Project");
        menu.addSeparator();
    }

    // ── Properties ──────────────────────────────────────────────────────
    QAction* propsAction = menu.addAction("Properties...");

    // A track lock makes every clip-level mutation read-only. Copy, reveal,
    // open, and properties remain available for inspection. Mixed selections
    // containing a locked track do not silently mutate only their unlocked
    // subset.
    bool selectionTouchesLockedTrack = false;
    for (const auto& selected : m_selection.clips()) {
        const Track* selectedTrack = m_timeline->track(selected.trackIndex);
        if (selectedTrack && selectedTrack->isLocked()) {
            selectionTouchesLockedTrack = true;
            break;
        }
    }
    if (selectionTouchesLockedTrack) {
        cutAction->setEnabled(false);
        deleteAction->setEnabled(false);
        rippleDeleteAction->setEnabled(false);
        nestAction->setEnabled(false);
        colorMenu->setEnabled(false);
        pasteEffectsAction->setEnabled(false);
        pasteAttrsAction->setEnabled(false);
    }
    if (track->isLocked()) {
        splitAction->setEnabled(false);
        enableAction->setEnabled(false);
        speedAction->setEnabled(false);
        renameAction->setEnabled(false);
        colorMenu->setEnabled(false);
        pasteEffectsAction->setEnabled(false);
        pasteAttrsAction->setEnabled(false);
        if (freezeFrameAction) freezeFrameAction->setEnabled(false);
        if (sceneDetectAction) sceneDetectAction->setEnabled(false);
        if (normalizeAction) normalizeAction->setEnabled(false);
        if (audioGainAction) audioGainAction->setEnabled(false);
        if (audioStreamMenu) audioStreamMenu->setEnabled(false);
        if (relinkMediaAction) relinkMediaAction->setEnabled(false);
    }

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

        auto* interpolationCombo = new QComboBox;
        interpolationCombo->setObjectName(
            QStringLiteral("speedTimeInterpolationCombo"));
        interpolationCombo->addItem("Frame Sampling");
        interpolationCombo->addItem("Frame Blending");
        interpolationCombo->addItem("Optical Flow");
        interpolationCombo->setCurrentIndex(
            static_cast<int>(clip->timeInterpolation()));
        form->addRow("Time Interpolation:", interpolationCombo);

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
            struct SpeedChange {
                Clip*             clip{nullptr};
                double            oldSpeed{1.0};
                int64_t           oldDuration{0};
                int64_t           newDuration{0};
                bool              oldMaintainPitch{true};
                TimeInterpolation oldInterpolation{TimeInterpolation::FrameSampling};
            };

            // The context menu preserves an existing multi-selection. Resolve
            // every selected clip and calculate its duration from its own
            // current speed/duration pair, just as the single-clip path does.
            std::vector<SpeedChange> changes;
            changes.reserve(m_selection.count());
            for (const auto& selectedRef : m_selection.clips()) {
                Track* selectedTrack = m_timeline->track(selectedRef.trackIndex);
                if (!selectedTrack) continue;
                const size_t selectedIndex =
                    selectedTrack->findClipIndexById(selectedRef.clipId);
                if (selectedIndex >= selectedTrack->clipCount()) continue;

                Clip* selectedClip = selectedTrack->clip(selectedIndex);
                if (!selectedClip) continue;

                SpeedChange change;
                change.clip = selectedClip;
                change.oldSpeed = selectedClip->speed();
                change.oldDuration = selectedClip->duration();
                change.oldMaintainPitch = selectedClip->maintainPitch();
                change.oldInterpolation = selectedClip->timeInterpolation();
                change.newDuration = static_cast<int64_t>(std::llround(
                    change.oldDuration * change.oldSpeed / newSpeed));
                change.newDuration = std::max(change.newDuration, kMinClipDuration);

                // Clamp each changed clip independently to the next clip on
                // its track so a slower speed cannot introduce an overlap.
                for (size_t i = selectedIndex + 1;
                     i < selectedTrack->clipCount(); ++i) {
                    const Clip* next = selectedTrack->clip(i);
                    if (next && next->timelineIn() > selectedClip->timelineIn()) {
                        const int64_t maxDuration =
                            next->timelineIn() - selectedClip->timelineIn();
                        if (change.newDuration > maxDuration &&
                            maxDuration >= kMinClipDuration) {
                            change.newDuration = maxDuration;
                        }
                        break;
                    }
                }

                changes.push_back(change);
            }

            if (changes.empty()) return;

            const bool newMaintainPitch = pitchCheck->isChecked();
            const TimeInterpolation newInterpolation =
                static_cast<TimeInterpolation>(interpolationCombo->currentIndex());

            auto applyChanges = [changes, newSpeed, newMaintainPitch,
                                 newInterpolation, this]() {
                for (const auto& change : changes) {
                    change.clip->setSpeed(newSpeed);
                    change.clip->setDuration(change.newDuration);
                    change.clip->setMaintainPitch(newMaintainPitch);
                    change.clip->setTimeInterpolation(newInterpolation);
                }
                onScrollChanged();
                emit contentChanged();
            };
            auto restoreChanges = [changes, this]() {
                for (const auto& change : changes) {
                    change.clip->setSpeed(change.oldSpeed);
                    change.clip->setDuration(change.oldDuration);
                    change.clip->setMaintainPitch(change.oldMaintainPitch);
                    change.clip->setTimeInterpolation(change.oldInterpolation);
                }
                onScrollChanged();
                emit contentChanged();
            };

            if (m_commandStack) {
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    changes.size() > 1 ? "Change Clip Speeds"
                                       : "Change Speed/Duration",
                    std::move(applyChanges), std::move(restoreChanges)));
            } else {
                applyChanges();
            }
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
            // Current gain: read the first keyframe value, or the static
            // track value when animation is disabled.
            auto& vol = ac->volume();
            float currentLin = (vol.keyframeCount() > 0)
                ? vol.keyframe(0).value
                : vol.defaultValue();
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
                    // A static track deliberately has no keyframes. Keep it
                    // static and update its default instead of indexing an
                    // empty keyframe vector.
                    vol.writeValue(0, newLin);
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
    else if (audioStreamMenu && audioStreamMenu->actions().contains(chosen)) {
        auto* ac = dynamic_cast<AudioClip*>(clip);
        if (ac && chosen->data().isValid()) {
            const int newVal = chosen->data().toInt();
            const int oldVal = ac->audioStreamIndex();
            if (newVal != oldVal) {
                const uint64_t clipId = ac->id();
                // contentChanged() rebuilds the audio sources (via the
                // workspace's invalidateAudioSources); invalidateClipWaveform
                // re-decodes the waveform for the newly chosen stream.
                auto applyStream = [this, ac, clipId](int ord) {
                    ac->setAudioStreamIndex(ord);
                    invalidateClipWaveform(clipId);
                    onScrollChanged();
                    emit contentChanged();
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Change audio stream",
                        [applyStream, newVal]() { applyStream(newVal); },
                        [applyStream, oldVal]() { applyStream(oldVal); }));
                } else {
                    applyStream(newVal);
                }
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
    else if (relinkMediaAction && chosen == relinkMediaAction) {
        const QString oldPath = QString::fromStdString(clipMediaPath);
        QFileInfo oldInfo(oldPath);
        QString startDir = oldInfo.absolutePath();
        if (startDir.isEmpty() || !QDir(startDir).exists())
            startDir = QDir::homePath();

        const QString newPath = QFileDialog::getOpenFileName(
            this, tr("Re-link Media - %1").arg(oldInfo.fileName()), startDir,
            QStringLiteral("Media Files (*.mp4 *.m4v *.mkv *.avi *.mov *.webm "
                           "*.ts *.mts *.m2ts *.mpg *.mpeg *.wmv *.flv *.mxf "
                           "*.gif *.png *.jpg *.jpeg *.bmp *.tga *.webp *.tif "
                           "*.tiff *.avif *.jxl *.wav *.mp3 *.flac *.ogg *.aac "
                           "*.m4a *.opus *.wma *.aif *.aiff);;All Files (*.*)"));
        if (!newPath.isEmpty() && newPath != oldPath)
            emit mediaRelinkRequested(oldPath, newPath);
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
        chkVolume->setEnabled(m_attrClipboard->audioVolume.has_value());
        chkPan->setEnabled(m_attrClipboard->audioPan.has_value());
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
        auto* chkTimeInterpolation = new QCheckBox("Time Interpolation", &dlg);
        checks = { chkOpacity, chkPosX, chkPosY, chkScaleX, chkScaleY,
                   chkRotation, chkSpeed, chkSpeedRamp, chkTimeInterpolation };
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
        auto* chkTimeInterpolation = new QCheckBox("Time Interpolation", &dlg);
        checks = { chkOpacity, chkPosX, chkPosY, chkScaleX, chkScaleY,
                   chkRotation, chkSpeed, chkSpeedRamp, chkTimeInterpolation };
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
        // Freeze the copied values into the command. Redo must not depend on
        // whatever the user may copy after this paste operation.
        const AttributesClipboard pastedAttributes = *m_attrClipboard;

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
            TimeInterpolation oldTimeInterpolation{TimeInterpolation::FrameSampling};
            // Audio
            KeyframeTrack<float> oldVolume{1.0f};
            KeyframeTrack<float> oldPan{0.0f};
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
                    snap.oldVolume = ac->volume();
                    snap.oldPan = ac->pan();
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
                snap.oldTimeInterpolation = target->timeInterpolation();
            }
            snapshots.push_back(snap);

            // Apply new values
            if (isAudio && !isVideo) {
                if (checks.size() > 0 && checks[0]->isChecked()) {
                    if (auto* ac = dynamic_cast<AudioClip*>(target);
                        ac && pastedAttributes.audioVolume)
                        ac->volume() = *pastedAttributes.audioVolume;
                }
                if (checks.size() > 1 && checks[1]->isChecked()) {
                    if (auto* ac = dynamic_cast<AudioClip*>(target);
                        ac && pastedAttributes.audioPan)
                        ac->pan() = *pastedAttributes.audioPan;
                }
                if (checks.size() > 2 && checks[2]->isChecked()) {
                    target->setSpeed(pastedAttributes.speed);
                }
            } else {
                if (checks.size() > 0 && checks[0]->isChecked()) target->opacity()   = pastedAttributes.opacity;
                if (checks.size() > 1 && checks[1]->isChecked()) target->positionX() = pastedAttributes.posX;
                if (checks.size() > 2 && checks[2]->isChecked()) target->positionY() = pastedAttributes.posY;
                if (checks.size() > 3 && checks[3]->isChecked()) target->scaleX()    = pastedAttributes.scaleX;
                if (checks.size() > 4 && checks[4]->isChecked()) target->scaleY()    = pastedAttributes.scaleY;
                if (checks.size() > 5 && checks[5]->isChecked()) target->rotation()  = pastedAttributes.rotation;
                if (checks.size() > 6 && checks[6]->isChecked()) target->setSpeed(pastedAttributes.speed);
                if (checks.size() > 7 && checks[7]->isChecked()) target->speedRamp() = pastedAttributes.speedRamp;
                if (checks.size() > 8 && checks[8]->isChecked()) target->setTimeInterpolation(pastedAttributes.timeInterpolation);
            }
        }

        // Wrap the entire operation in a LambdaCommand for undo/redo
        if (m_commandStack) {
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Paste Attributes",
                [this, snapshots, pastedAttributes, checks_states = [&]() {
                    std::vector<bool> states;
                    for (auto* ck : checks) states.push_back(ck->isChecked());
                    return states;
                }(), isAudio, isVideo]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    // Redo: re-apply the values captured by this command.
                    if (!m_timeline) return;
                    for (const auto& snap : snapshots) {
                        Track* st = m_timeline->track(snap.trackIdx);
                        if (!st) continue;
                        size_t si = st->findClipIndexById(snap.clipId);
                        if (si >= st->clipCount()) continue;
                        Clip* target = st->clip(si);
                        if (isAudio && !isVideo) {
                            if (checks_states.size() > 0 && checks_states[0]) {
                                if (auto* ac = dynamic_cast<AudioClip*>(target);
                                    ac && pastedAttributes.audioVolume)
                                    ac->volume() = *pastedAttributes.audioVolume;
                            }
                            if (checks_states.size() > 1 && checks_states[1]) {
                                if (auto* ac = dynamic_cast<AudioClip*>(target);
                                    ac && pastedAttributes.audioPan)
                                    ac->pan() = *pastedAttributes.audioPan;
                            }
                            if (checks_states.size() > 2 && checks_states[2])
                                target->setSpeed(pastedAttributes.speed);
                        } else {
                            if (checks_states.size() > 0 && checks_states[0]) target->opacity()   = pastedAttributes.opacity;
                            if (checks_states.size() > 1 && checks_states[1]) target->positionX() = pastedAttributes.posX;
                            if (checks_states.size() > 2 && checks_states[2]) target->positionY() = pastedAttributes.posY;
                            if (checks_states.size() > 3 && checks_states[3]) target->scaleX()    = pastedAttributes.scaleX;
                            if (checks_states.size() > 4 && checks_states[4]) target->scaleY()    = pastedAttributes.scaleY;
                            if (checks_states.size() > 5 && checks_states[5]) target->rotation()  = pastedAttributes.rotation;
                            if (checks_states.size() > 6 && checks_states[6]) target->setSpeed(pastedAttributes.speed);
                            if (checks_states.size() > 7 && checks_states[7]) target->speedRamp() = pastedAttributes.speedRamp;
                            if (checks_states.size() > 8 && checks_states[8]) target->setTimeInterpolation(pastedAttributes.timeInterpolation);
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
                                ac->volume() = snap.oldVolume;
                                ac->pan() = snap.oldPan;
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
                            target->setTimeInterpolation(snap.oldTimeInterpolation);
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
    if (auto* audio = dynamic_cast<AudioClip*>(clip)) {
        ac.audioVolume = audio->volume();
        ac.audioPan = audio->pan();
    }
    ac.speed     = clip->speed();
    ac.speedRamp = clip->speedRamp();
    ac.timeInterpolation = clip->timeInterpolation();
    m_attrClipboard = std::move(ac);
}


} // namespace rt
