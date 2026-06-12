/*
 * TimelineWorkspacePanelsShortcuts.cpp — Keyboard shortcut registration
 * extracted from TimelineWorkspacePanels.cpp::buildPanels().
 */

#include "panels/timeline/ShortcutController.h"
#include "panels/timeline/TimelineWorkspace.h"

#include "command/CommandStack.h"
#include "command/commands/TransitionCmds.h"
#include "MainWindow.h"
#include "playback/PlaybackController.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/timeline/TimelinePanel.h"
#include "timeline/EditOperations.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"

#include <QApplication>
#include <QKeySequence>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSpinBox>
#include <QTextEdit>
#include <QWidget>

namespace rt {

void ShortcutController::registerKeyboardShortcuts()
{
    m_ws->setFocusPolicy(Qt::StrongFocus);
    for (auto* btn : m_ws->m_toolButtons)
        if (btn) btn->setFocusPolicy(Qt::NoFocus);

    auto addShortcut = [this](const QKeySequence& key, auto&& fn) {
        auto* sc = new QShortcut(key, m_ws);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, std::forward<decltype(fn)>(fn));
    };

    // Home / End: go to start / end of timeline
    addShortcut(Qt::Key_Home, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;
        if (m_ws->m_playbackController) m_ws->m_playbackController->goToStart();
    });
    addShortcut(Qt::Key_End, [this]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw)) return;
        if (m_ws->m_playbackController) m_ws->m_playbackController->goToEnd();
    });

    // Arrow keys — window-level so they work regardless of which panel has
    // focus (text inputs are still guarded).  Left/Right step frame-by-frame
    // with auto-repeat; Up/Down jump between edit points.
    auto addGlobalShortcut = [this](const QKeySequence& key, auto&& fn) {
        auto* sc = new QShortcut(key, m_ws);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, std::forward<decltype(fn)>(fn));
    };
    // Arrow keys nudge the playhead.  If playback is active, stop it first
    // (Premiere Pro behaviour: pressing Left/Right/Up/Down to navigate halts
    // playback rather than letting it run away from where the user is
    // looking).
    // Mirror keyPressEvent's routing: when the Source Monitor owns focus,
    // arrow keys drive its controller; otherwise the timeline controller.
    // Without this, Left/Right in the Source Monitor did nothing because
    // the global shortcut always poked the timeline playhead.
    // NOTE: isAncestorOf(self) returns false in Qt, so the focus check
    // must also handle the case where focus is on the SourceMonitor widget
    // itself (its eventFilter does setFocus() on the monitor on click).
    auto activeArrowController = [this]() -> PlaybackController* {
        QWidget* fw = QApplication::focusWidget();
        if (m_ws->m_sourceMonitor && m_ws->m_sourceMonitor->controller() &&
            m_ws->m_sourceMonitor->hasClip() && fw &&
            (fw == m_ws->m_sourceMonitor || m_ws->m_sourceMonitor->isAncestorOf(fw)))
            return m_ws->m_sourceMonitor->controller();
        return m_ws->m_playbackController;
    };
    auto stopPlaybackIfRunning = [activeArrowController]() {
        if (auto* ctl = activeArrowController(); ctl && ctl->isPlaying())
            ctl->pause();
    };
    addGlobalShortcut(Qt::Key_Left, [activeArrowController, stopPlaybackIfRunning]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw) ||
            qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QSpinBox*>(fw))
            return;
        stopPlaybackIfRunning();
        if (auto* ctl = activeArrowController()) ctl->stepBackward();
    });
    addGlobalShortcut(Qt::Key_Right, [activeArrowController, stopPlaybackIfRunning]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw) ||
            qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QSpinBox*>(fw))
            return;
        stopPlaybackIfRunning();
        if (auto* ctl = activeArrowController()) ctl->stepForward();
    });
    addGlobalShortcut(Qt::Key_Up, [activeArrowController, stopPlaybackIfRunning]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw) ||
            qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QSpinBox*>(fw))
            return;
        stopPlaybackIfRunning();
        if (auto* ctl = activeArrowController()) ctl->goToPrevEditPoint();
    });
    addGlobalShortcut(Qt::Key_Down, [activeArrowController, stopPlaybackIfRunning]() {
        auto* fw = QApplication::focusWidget();
        if (qobject_cast<QLineEdit*>(fw) || qobject_cast<QTextEdit*>(fw) ||
            qobject_cast<QPlainTextEdit*>(fw) || qobject_cast<QSpinBox*>(fw))
            return;
        stopPlaybackIfRunning();
        if (auto* ctl = activeArrowController()) ctl->goToNextEditPoint();
    });

    // Shift+I / Shift+O: go to in/out point
    addShortcut(Qt::SHIFT | Qt::Key_I, [this]() {
        if (m_ws->m_playbackController) m_ws->m_playbackController->goToInPoint();
    });
    addShortcut(Qt::SHIFT | Qt::Key_O, [this]() {
        if (m_ws->m_playbackController) m_ws->m_playbackController->goToOutPoint();
    });
    // Alt+X: clear in/out
    addShortcut(Qt::ALT | Qt::Key_X, [this]() {
        if (m_ws->m_timeline) {
            EditOperations::clearInOutPoints(*m_ws->m_timeline);
            if (m_ws->m_timelinePanel) m_ws->m_timelinePanel->updateInOutRange();
            m_ws->syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+X: clear in/out points
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_X, [this]() {
        if (m_ws->m_timeline) {
            EditOperations::clearInOutPoints(*m_ws->m_timeline);
            if (m_ws->m_timelinePanel) m_ws->m_timelinePanel->updateInOutRange();
            m_ws->syncProgramMonitorInOut();
        }
    });
    // Ctrl+Shift+V: Paste Attributes dialog
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_V, [this]() {
        if (m_ws->m_timelinePanel) m_ws->m_timelinePanel->showPasteAttributesDialog();
    });
    // Ctrl+Shift+C: Paste Insert
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_C, [this]() {
        if (m_ws->m_timeline && m_ws->m_timelinePanel && m_ws->m_commandStack && !m_ws->m_timelinePanel->clipboard().empty()) {
            const int64_t pasteTick = m_ws->m_playbackController ? m_ws->m_playbackController->currentTick() : 0;
            auto cmd = EditOperations::pasteInsert(
                *m_ws->m_timeline, m_ws->m_timelinePanel->clipboard(), pasteTick);
            if (cmd) {
                m_ws->m_commandStack->execute(std::move(cmd));
                if (m_ws->m_timelinePanel) m_ws->m_timelinePanel->refreshTrackContents();
                // Sync playhead from the model (the command moves it to end
                // of inserted content as part of its undoable state).
                int64_t modelTick = m_ws->m_timeline->playheadPosition();
                m_ws->m_timelinePanel->setPlayheadPosition(modelTick);
                if (m_ws->m_playbackController) m_ws->m_playbackController->seekTo(modelTick);
                m_ws->invalidateAudioSources();
                m_ws->invalidateCompositeCache();
                m_ws->updateTransformOverlay();
                if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
                m_ws->schedulePostEditWork();
            }
        }
    });
    // Ctrl+V: paste at playhead (or paste keyframes if Effect Controls has
    //          keyframe clipboard, paste layer if Essential Graphics focused,
    //          or paste effect if one was copied from Effect Controls)
    addShortcut(Qt::CTRL | Qt::Key_V, [this]() {
        auto* fw = QApplication::focusWidget();
        // ── Keyframe clipboard takes highest priority ──────────────────
        // If the user copied keyframes in Effect Controls, Ctrl+V pastes
        // them regardless of which panel currently has focus (matching
        // Premiere Pro behavior).
        if (m_ws->m_effectControlsPanel && m_ws->m_effectControlsPanel->hasKfClipboardData()
            && m_ws->m_effectControlsPanel->clip()) {
            m_ws->m_effectControlsPanel->pasteKeyframes();
            m_ws->invalidateCompositeCache();
            m_ws->updateTransformOverlay();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
            m_ws->schedulePostEditWork();
            return;
        }
        // Project Bin focused → paste the clipboard as an independent
        // duplicate (sequence, footage, or color matte).
        if (m_ws->m_projectBin && m_ws->m_projectBin->isAncestorOf(fw)) {
            m_ws->m_projectBin->pasteClipboard();
            return;
        }
        bool egFocused = m_ws->m_GraphicsEditorPanel && m_ws->m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_ws->m_programMonitor && m_ws->m_programMonitor->isAncestorOf(fw);
        if (m_ws->m_GraphicsEditorPanel && (egFocused || (pmFocused && m_ws->m_selection.graphicLayerIdx >= 0))) {
            m_ws->m_GraphicsEditorPanel->pasteLayer();
            m_ws->invalidateCompositeCache();
            m_ws->scheduleOverlayRefresh();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
            return;
        }
        // Effect Controls has a copied effect → paste it onto the current
        // clip. Does NOT require EC focus — the user may have clicked a
        // different clip on the timeline after copying the effect.
        // (Ctrl+C on the timeline clears the effect clipboard, so this
        //  only fires when the last copy was an effect.)
        if (m_ws->m_effectControlsPanel && m_ws->m_effectControlsPanel->hasCopiedEffect()
            && m_ws->m_effectControlsPanel->clip()) {
            m_ws->m_effectControlsPanel->pasteEffect();
            m_ws->invalidateCompositeCache();
            m_ws->updateTransformOverlay();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
            m_ws->schedulePostEditWork();
            return;
        }
        if (m_ws->m_timeline && m_ws->m_timelinePanel && m_ws->m_commandStack && !m_ws->m_timelinePanel->clipboard().empty()) {
            const int64_t pasteTick = m_ws->m_playbackController ? m_ws->m_playbackController->currentTick() : 0;
            auto cmd = EditOperations::paste(
                *m_ws->m_timeline, m_ws->m_timelinePanel->clipboard(), pasteTick);
            if (cmd) {
                m_ws->m_commandStack->execute(std::move(cmd));
                if (m_ws->m_timelinePanel) m_ws->m_timelinePanel->refreshTrackContents();
                // Sync playhead from the model (the command moves it to end
                // of pasted content as part of its undoable state).
                int64_t modelTick = m_ws->m_timeline->playheadPosition();
                m_ws->m_timelinePanel->setPlayheadPosition(modelTick);
                if (m_ws->m_playbackController) m_ws->m_playbackController->seekTo(modelTick);
                m_ws->invalidateAudioSources();
                m_ws->invalidateCompositeCache();
                m_ws->updateTransformOverlay();
                if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
                m_ws->schedulePostEditWork();
            }
        }
    });
    // Ctrl+X: cut (or cut keyframes if Effect Controls has selection)
    addShortcut(Qt::CTRL | Qt::Key_X, [this]() {
        // ── Keyframe selection in Effect Controls → cut keyframes ──────
        if (m_ws->m_effectControlsPanel && m_ws->m_effectControlsPanel->hasSelectedKeyframes()) {
            m_ws->m_effectControlsPanel->cutSelectedKeyframes();
            return;
        }
        if (!m_ws->m_timeline || !m_ws->m_timelinePanel || !m_ws->m_commandStack) return;
        auto& cb = m_ws->m_timelinePanel->mutableClipboard();
        auto cmd = EditOperations::cutSelection(*m_ws->m_timeline,
            m_ws->m_timelinePanel->selection(), cb);
        if (cmd) {
            m_ws->m_timelinePanel->selection().clear();
            m_ws->m_commandStack->execute(std::move(cmd));
            m_ws->m_timelinePanel->refreshTrackContents();
            m_ws->invalidateAudioSources();
            m_ws->invalidateCompositeCache();
            m_ws->updateTransformOverlay();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
        }
    });
    // Ctrl+C: copy (or copy keyframes if Effect Controls has selection,
    //          or copy layer if Essential Graphics focused,
    //          or copy effect if Effect Controls focused)
    addShortcut(Qt::CTRL | Qt::Key_C, [this]() {
        auto* fw = QApplication::focusWidget();
        // ── Keyframe selection in Effect Controls → copy keyframes ─────
        if (m_ws->m_effectControlsPanel && m_ws->m_effectControlsPanel->hasSelectedKeyframes()) {
            m_ws->m_effectControlsPanel->copySelectedKeyframes();
            return;
        }
        // Project Bin focused → copy the current selection (sequence,
        // footage, or color matte) to the bin clipboard.
        if (m_ws->m_projectBin && m_ws->m_projectBin->isAncestorOf(fw)) {
            m_ws->m_projectBin->copySelection();
            if (m_ws->m_effectControlsPanel) {
                m_ws->m_effectControlsPanel->clearCopiedEffect();
                m_ws->m_effectControlsPanel->clearKfClipboard();
            }
            return;
        }
        bool egFocused = m_ws->m_GraphicsEditorPanel && m_ws->m_GraphicsEditorPanel->isAncestorOf(fw);
        bool pmFocused = m_ws->m_programMonitor && m_ws->m_programMonitor->isAncestorOf(fw);
        if (m_ws->m_GraphicsEditorPanel && (egFocused || (pmFocused && m_ws->m_selection.graphicLayerIdx >= 0))) {
            m_ws->m_GraphicsEditorPanel->copySelectedLayer();
            if (m_ws->m_effectControlsPanel) {
                m_ws->m_effectControlsPanel->clearCopiedEffect();
                m_ws->m_effectControlsPanel->clearKfClipboard();
            }
            return;
        }
        // Effect Controls focused with a selected effect → copy the effect
        if (m_ws->m_effectControlsPanel && m_ws->m_effectControlsPanel->isAncestorOf(fw)
            && m_ws->m_effectControlsPanel->hasSelectedEffect()) {
            m_ws->m_effectControlsPanel->copySelectedEffect();
            return;
        }
        if (!m_ws->m_timeline || !m_ws->m_timelinePanel) return;
        // Copying a clip on the timeline → clear any stale effect
        // and keyframe clipboards so Ctrl+V pastes the clip.
        if (m_ws->m_effectControlsPanel) {
            m_ws->m_effectControlsPanel->clearCopiedEffect();
            m_ws->m_effectControlsPanel->clearKfClipboard();
        }
        EditOperations::copySelection(*m_ws->m_timeline,
            m_ws->m_timelinePanel->selection(),
            m_ws->m_timelinePanel->mutableClipboard());
        m_ws->m_timelinePanel->copyAttributesFromSelection();
    });
    // Shift+Delete / Shift+Backspace: extract (ripple delete)
    addShortcut(Qt::SHIFT | Qt::Key_Delete, [this]() {
        if (!m_ws->m_timeline || !m_ws->m_timelinePanel || !m_ws->m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_ws->m_timeline,
            m_ws->m_timelinePanel->selection());
        if (cmd) {
            m_ws->m_timelinePanel->selection().clear();
            m_ws->m_commandStack->execute(std::move(cmd));
            m_ws->m_timelinePanel->refreshTrackContents();
            m_ws->invalidateAudioSources();
            m_ws->invalidateCompositeCache();
            m_ws->updateTransformOverlay();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
            m_ws->schedulePostEditWork();
        }
    });
    addShortcut(Qt::SHIFT | Qt::Key_Backspace, [this]() {
        if (!m_ws->m_timeline || !m_ws->m_timelinePanel || !m_ws->m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_ws->m_timeline,
            m_ws->m_timelinePanel->selection());
        if (cmd) {
            m_ws->m_timelinePanel->selection().clear();
            m_ws->m_commandStack->execute(std::move(cmd));
            m_ws->m_timelinePanel->refreshTrackContents();
            m_ws->invalidateAudioSources();
            m_ws->invalidateCompositeCache();
            m_ws->updateTransformOverlay();
            if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
            m_ws->schedulePostEditWork();
        }
    });
    // Ctrl+A: select all
    addShortcut(Qt::CTRL | Qt::Key_A, [this]() {
        if (m_ws->m_projectBin && m_ws->m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_ws->m_projectBin->selectAllItems();
            return;
        }
        if (!m_ws->m_timeline || !m_ws->m_timelinePanel) return;
        m_ws->m_timelinePanel->selection().selectAll(*m_ws->m_timeline);
        emit m_ws->m_timelinePanel->selectionChanged();
    });
    // Ctrl+Shift+A: deselect all
    addShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_A, [this]() {
        if (!m_ws->m_timelinePanel) return;
        m_ws->m_timelinePanel->selection().clear();
        emit m_ws->m_timelinePanel->selectionChanged();
    });

    // Ctrl+B: New Bin when Project Bin is focused
    addShortcut(Qt::CTRL | Qt::Key_B, [this]() {
        if (m_ws->m_projectBin && m_ws->m_projectBin->isAncestorOf(
                QApplication::focusWidget())) {
            m_ws->m_projectBin->createNewBin();
            return;
        }
    });

    // Ctrl+T: add default transition
    addShortcut(Qt::CTRL | Qt::Key_T, [this]() {
        if (!m_ws->m_timeline || !m_ws->m_commandStack || !m_ws->m_timelinePanel) return;
        auto edge = m_ws->m_timelinePanel->lastClickedEdge();
        if (!edge.valid) return;

        Track* track = m_ws->m_timeline->track(edge.clipRef.trackIndex);
        if (!track) return;

        size_t clipIdx = track->findClipIndexById(edge.clipRef.clipId);
        if (clipIdx >= track->clipCount()) return;

        const Clip* clip = track->clip(clipIdx);
        Transition trans;
        trans.duration = kDefaultTransitionDuration;

        if (edge.edge == ClipEdge::Head) {
            const Clip* leftClip = nullptr;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineOut() - clip->timelineIn());
                if (gap <= 1600) {
                    leftClip = c;
                    break;
                }
            }
            if (leftClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = leftClip->id();
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            } else {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = 0;
                trans.rightClipId = clip->id();
                trans.editPointTick = clip->timelineIn();
            }
        } else {
            const Clip* rightClip = nullptr;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                if (ci == clipIdx) continue;
                const Clip* c = track->clip(ci);
                int64_t gap = std::abs(c->timelineIn() - clip->timelineOut());
                if (gap <= 1600) {
                    rightClip = c;
                    break;
                }
            }
            if (rightClip) {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = rightClip->id();
                trans.editPointTick = clip->timelineOut();
            } else {
                trans.type = TransitionType::CrossDissolve;
                trans.leftClipId = clip->id();
                trans.rightClipId = 0;
                trans.editPointTick = clip->timelineOut();
            }
        }

        // Reject if the new transition would overlap any existing
        // transition's range (not just the same edit point — a long
        // fade-in can overlap a fade-out on the same clip).
        {
            int64_t newStart, newEnd;
            trans.getRange(newStart, newEnd);
            bool wouldOverlap = false;
            for (size_t ti2 = 0; ti2 < track->transitionCount(); ++ti2) {
                const Transition* existing = track->transition(ti2);
                if (!existing) continue;
                int64_t exStart, exEnd;
                existing->getRange(exStart, exEnd);
                // Two ranges [a,b) and [c,d) overlap if a<d && c<b
                if (newStart < exEnd && exStart < newEnd) {
                    wouldOverlap = true;
                    break;
                }
            }
            if (wouldOverlap) return;
        }

        auto cmd = std::make_unique<AddTransitionCommand>(
            track, clipIdx, clipIdx, trans);
        m_ws->m_commandStack->execute(std::move(cmd));

        m_ws->invalidateCompositeCache();
        // A cross-dissolve on an audio track bakes its crossfade into the
        // mixed audio source; rebuild it now so the transition is audible
        // immediately instead of only after its duration is adjusted.
        m_ws->invalidateAudioSources();
        if (m_ws->m_timelinePanel) {
            m_ws->m_timelinePanel->rebuildTracks();
            // The edge click that primed this transition left an edit-point
            // bracket painted at the cut.  rebuildTracks() reuses widgets
            // in place and doesn't touch it, so clear it explicitly — the
            // transition is now applied and the bracket would otherwise
            // sit there looking like a stuck selection until the next click.
            m_ws->m_timelinePanel->clearEditPointSelection();
        }
        if (m_ws->m_programMonitor) m_ws->m_programMonitor->requestRefresh();
    });

    // Ctrl+=: zoom in
    addShortcut(Qt::CTRL | Qt::Key_Equal, [this]() {
        if (m_ws->m_timelinePanel) {
            auto& engine = m_ws->m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_ws->m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_ws->m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.3);
            m_ws->m_timelinePanel->notifyZoomChanged();
        }
    });
    // Ctrl+-: zoom out
    addShortcut(Qt::CTRL | Qt::Key_Minus, [this]() {
        if (m_ws->m_timelinePanel) {
            auto& engine = m_ws->m_timelinePanel->layoutEngine();
            double anchorPx = engine.viewportWidth() * 0.5;
            if (m_ws->m_playbackController) {
                double playheadPx = engine.timeToPixelX(m_ws->m_playbackController->currentTick());
                if (playheadPx >= 0.0 && playheadPx <= engine.viewportWidth())
                    anchorPx = playheadPx;
            }
            engine.zoomAt(anchorPx, 1.0 / 1.3);
            m_ws->m_timelinePanel->notifyZoomChanged();
        }
    });

    // Ctrl+E: switch to Export tab
    addShortcut(Qt::CTRL | Qt::Key_E, [this]() {
        for (QWidget* w = m_ws->parentWidget(); w; w = w->parentWidget()) {
            if (auto* mw = qobject_cast<MainWindow*>(w)) {
                mw->setCurrentPage(Page::Export);
                break;
            }
        }
    });

    // Give this widget focus so shortcuts work immediately
    m_ws->setFocus();
}

} // namespace rt
