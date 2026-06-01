/*
 * TimelinePanelCommands.cpp — Editing setup, command execution, tool management.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: setCommandStack(), setShortcutManager(), setActiveTool(),
 *           setSnappingEnabled(), setCaptionTrackVisible(), executeCommand().
 */

#include "panels/timeline/TimelinePanel.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "command/CommandStack.h"
#include "command/Command.h"
#include "command/CompoundCommand.h"
#include "ShortcutManager.h"

#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QCursor>

#include <spdlog/spdlog.h>

namespace rt {

void TimelinePanel::setCommandStack(CommandStack* stack)
{
    m_commandStack = stack;
}

void TimelinePanel::setShortcutManager(ShortcutManager* shortcuts)
{
    m_shortcuts = shortcuts;
    if (m_shortcuts)
        wireShortcuts();
}

void TimelinePanel::setActiveTool(EditTool tool)
{
    m_activeTool = tool;
    updateCursorForTool();

    // Clear razor hover line when switching away from Razor
    if (tool != EditTool::Razor) {
        for (auto tw : m_trackWidgets)
            tw->setRazorTick(-1);
    }

    emit toolChanged(tool);
}

void TimelinePanel::setSnappingEnabled(bool enabled)
{
    m_snapEngine.setEnabled(enabled);
}

void TimelinePanel::setCaptionTrackVisible(bool visible)
{
    if (!m_timeline) return;
    // "Hidden" == caption track muted: the compositor skips muted video
    // tracks (so the burned-in overlay disappears), the state persists with
    // the project, and applyCaptionTrackVisibility() collapses the row.
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* trk = m_timeline->track(i);
        if (trk && trk->isCaptionTrack()) {
            trk->setMuted(!visible);
            break;
        }
    }
    applyCaptionTrackVisibility();
    update();
}

void TimelinePanel::applyCaptionTrackVisibility()
{
    if (!m_timeline) return;
    // Hide BOTH the header and the content widget for the caption track so the
    // two parallel QVBoxLayouts stay aligned (hiding only one desyncs them and
    // shoves the rest of the timeline out of place).
    for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
        Track* trk = m_timeline->track(i);
        if (!trk || !trk->isCaptionTrack()) continue;
        const bool show = !trk->isMuted();
        if (i < m_trackWidgets.size() && m_trackWidgets[i])
            m_trackWidgets[i]->setVisible(show);
        if (i < m_trackHeaders.size() && m_trackHeaders[i])
            m_trackHeaders[i]->setVisible(show);
        break;
    }
}

void TimelinePanel::executeCommand(std::unique_ptr<Command> cmd)
{
    if (cmd && m_commandStack) {
        m_commandStack->execute(std::move(cmd));
        // Refresh all track widgets to reflect the change immediately
        onScrollChanged();
        // Notify workspace so ProgramMonitor re-composites
        emit contentChanged();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Cursor and shortcut wiring
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::updateCursorForTool()
{
    // Remove any global override from previous tool
    while (QApplication::overrideCursor()) QApplication::restoreOverrideCursor();

    switch (m_activeTool)
    {
    case EditTool::Selection:
        setCursor(Qt::ArrowCursor);
        break;
    case EditTool::Razor:
        setCursor(Qt::CrossCursor);
        break;
    case EditTool::Rolling:
    case EditTool::Ripple:
        setCursor(Qt::SplitHCursor);
        break;
    case EditTool::Slip:
    case EditTool::Slide:
        setCursor(Qt::SizeHorCursor);
        break;
    case EditTool::Text:
        setCursor(Qt::IBeamCursor);
        break;
    case EditTool::Zoom:
    {
        // Custom magnifying glass cursor — use global override so all
        // child widgets (track headers, content area, scroll viewport)
        // show it regardless of their own cursor settings.
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::white, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(9, 9), 7, 7);
        p.setPen(QPen(Qt::white, 2.5));
        p.drawLine(QPointF(14.5, 14.5), QPointF(21, 21));
        p.setPen(QPen(Qt::white, 1.4));
        p.drawLine(QPointF(6, 9), QPointF(12, 9));
        p.drawLine(QPointF(9, 6), QPointF(9, 12));
        p.end();
        setCursor(QCursor(pm, 9, 9));
        QApplication::setOverrideCursor(QCursor(pm, 9, 9));
        break;
    }
    }
}

void TimelinePanel::wireShortcuts()
{
    if (!m_shortcuts) return;

    // ── Copy: copy selected clips to clipboard + copy attributes ──────
    m_shortcuts->setActionCallback(ShortcutManager::kCopy, [this]() {
        if (!m_timeline) return;
        EditOperations::copySelection(*m_timeline, m_selection, m_clipboard);
        copyAttributesFromSelection();
    });

    // ── Cut: copy + delete selection ──────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kCut, [this]() {
        if (!m_timeline || !m_commandStack) return;
        ClipboardContents cb;
        auto cmd = EditOperations::cutSelection(*m_timeline, m_selection, cb);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Paste: overwrite at playhead ──────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kPaste, [this]() {
        if (!m_timeline || !m_commandStack || m_clipboard.empty()) return;
        auto cmd = EditOperations::paste(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            // Sync playhead from the model (the command moves it to end
            // of pasted content as part of its undoable state).
            setPlayheadPosition(m_timeline->playheadPosition());
            emit contentChanged();
        }
    });

    // ── Paste Insert: push clips right, insert at playhead ────────────
    m_shortcuts->setActionCallback(ShortcutManager::kPasteInsert, [this]() {
        if (!m_timeline || !m_commandStack || m_clipboard.empty()) return;
        auto cmd = EditOperations::pasteInsert(*m_timeline, m_clipboard, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            // Sync playhead from the model (the command moves it to end
            // of inserted content as part of its undoable state).
            setPlayheadPosition(m_timeline->playheadPosition());
            emit contentChanged();
        }
    });

    // ── Paste Attributes: open dialog to choose which attributes ──────
    m_shortcuts->setActionCallback(ShortcutManager::kPasteAttributes, [this]() {
        showPasteAttributesDialog();
    });

    // ── Delete (Lift): remove selected clips ──────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kDelete, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::deleteSelection(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Ripple Delete: remove clips and close gaps ────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kRippleDel, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::rippleDelete(*m_timeline, m_selection);
        if (cmd) {
            m_selection.clear();
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit selectionChanged();
            emit contentChanged();
        }
    });

    // ── Duplicate: paste in-place offset ──────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kDuplicate, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::duplicateSelection(*m_timeline, m_selection);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            // Sync playhead from the model (the command moves it to end
            // of duplicated content as part of its undoable state).
            setPlayheadPosition(m_timeline->playheadPosition());
            emit contentChanged();
        }
    });

    // ── Select All ────────────────────────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSelectAll, [this]() {
        if (!m_timeline) return;
        m_selection.selectAll(*m_timeline);
        refreshTrackContents();
        emit selectionChanged();
    });

    // ── Split Selected Clips (F) ────────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSplitAt, [this]() {
        if (!m_timeline || !m_commandStack || m_selection.empty()) return;
        // Build a compound command that splits each selected clip
        auto compound = std::make_unique<CompoundCommand>("Split Selected Clips");
        for (const auto& ref : m_selection.clips()) {
            if (ref.trackIndex < m_timeline->trackCount()) {
                auto cmd = EditOperations::splitClip(
                    *m_timeline, ref.trackIndex, ref.clipId, m_playheadTick);
                if (cmd)
                    compound->addCommand(std::move(cmd));
            }
        }
        if (compound->size() > 0) {
            m_commandStack->execute(std::move(compound));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Split All Tracks (Shift+F) ───────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kSplitAll, [this]() {
        if (!m_timeline || !m_commandStack) return;
        auto cmd = EditOperations::splitAllAtPlayhead(*m_timeline, m_playheadTick);
        if (cmd) {
            m_commandStack->execute(std::move(cmd));
            refreshTrackContents();
            emit contentChanged();
        }
    });

    // ── Tool shortcuts (A/B/R/N/S/U) ─────────────────────────────────
    m_shortcuts->setActionCallback(ShortcutManager::kToolSelection, [this]() {
        setActiveTool(EditTool::Selection);
    });
    m_shortcuts->setActionCallback(ShortcutManager::kToolRazor, [this]() {
        setActiveTool(EditTool::Razor);
    });
    m_shortcuts->setActionCallback(ShortcutManager::kToolRolling, [this]() {
        setActiveTool(EditTool::Rolling);
    });
    m_shortcuts->setActionCallback(ShortcutManager::kToolRipple, [this]() {
        setActiveTool(EditTool::Ripple);
    });
    m_shortcuts->setActionCallback(ShortcutManager::kToolSlip, [this]() {
        setActiveTool(EditTool::Slip);
    });
    m_shortcuts->setActionCallback(ShortcutManager::kToolSlide, [this]() {
        setActiveTool(EditTool::Slide);
    });
}

} // namespace rt
