/*
 * TimelinePanelMouseDragRelease.cpp — mouseReleaseEvent extracted from
 * TimelinePanelMouseDrag.cpp.
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/EditOperations.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"

#include <QToolTip>

#include <spdlog/spdlog.h>

#include <unordered_map>
#include <unordered_set>

namespace rt {

void TimelinePanel::mouseReleaseEvent(QMouseEvent* event)
{
    // ── PendingClipClick: user clicked an already-selected clip without dragging ──
    if (m_dragMode == DragMode::PendingClipClick) {
        // If this was an edge click (m_lastClickedEdge.valid is true),
        // the edit-point bracket is already showing and the user wants
        // to keep it for Ctrl+T — don't select the whole clip.
        if (!m_lastClickedEdge.valid) {
            // Body click: select just the clicked clip (Premiere Pro
            // behaviour) — but carry its link partner along unless Alt
            // was held, so clicking a linked video doesn't silently drop
            // its companion audio.
            const bool isAlt = event->modifiers() & Qt::AltModifier;
            m_selection.clear();
            m_selection.selectClip(m_dragClipRef, false);
            if (!isAlt)
                setLinkPartnersSelected(m_dragClipRef, true);
            emit selectionChanged();
            // Emit clipSelected so panels update
            Track* trk = m_timeline ? m_timeline->track(m_dragClipRef.trackIndex) : nullptr;
            if (trk) {
                size_t clipIdx = trk->findClipIndexById(m_dragClipRef.clipId);
                if (clipIdx < trk->clipCount())
                    emit clipSelected(m_dragClipRef.trackIndex, clipIdx);
            }
        }
    }

    // Drags converted to tools finish their own release (a marquee that never
    // moved clears the selection; a move or roll records itself).
    if (m_dragTool)
        m_dragTool->release(*event);

    endDrag(DragEnd::Committed);

    // NOTE: the edit-point bracket is deliberately NOT cleared on release.
    // A plain edge/seam click (no drag) primes a ClipTrim* drag and
    // paints the bracket; that bracket IS the persistent "this edit
    // point is selected — Ctrl+T will add a transition here" visual.
    // It is already cleared (a) at press-start on the next click and
    // (b) the moment a trim actually moves (in the drag-move handler),
    // so clearing it on release would only kill the indicator while
    // the user is reading it.
    //
    // Gap selection is preserved on release so it remains visible.
    // It will be cleared on the next mouse press (when clicking on a
    // clip or non-gap empty space), matching Premiere Pro behavior.

    QWidget::mouseReleaseEvent(event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag restore / cancel
// ═════════════════════════════════════════════════════════════════════════════

bool TimelinePanel::cancelActiveDrag()
{
    if (!m_timeline || m_dragMode == DragMode::None) return false;
    endDrag(DragEnd::Cancelled);
    return true;
}

bool TimelinePanel::dragRecordsIntoMacro(DragMode mode) noexcept
{
    switch (mode) {
    case DragMode::ClipTrimHead:
    case DragMode::ClipTrimTail:
    case DragMode::SlipTool:
    case DragMode::SlideTool:
    case DragMode::TransitionTrim:
    case DragMode::TransitionHandle:
        return true;
    default:
        return false;
    }
}

void TimelinePanel::endDrag(DragEnd how)
{
    switch (how) {
    case DragEnd::Committed:
        // The whole drag — live changes and the release's final commands —
        // is one undo step (nothing is recorded when it changed nothing).
        commitDragEditGesture();
        if (m_dragMode == DragMode::None) return;
        // Trim/slip/slide/transition drags record into an open macro: close
        // it so the buffered mouse-move commands collapse into one entry.
        if (m_commandStack && m_commandStack->isMacroActive()
            && dragRecordsIntoMacro(m_dragMode))
            m_commandStack->endMacro();
        break;

    case DragEnd::Cancelled:
        // Premiere: Esc during a drag puts everything back as it was and
        // records nothing.
        if (dragRecordsIntoMacro(m_dragMode)) {
            // These execute real commands into an open macro: close it and
            // undo it — but only if the drag actually recorded something.
            if (m_commandStack && m_commandStack->isMacroActive()) {
                const size_t before = m_commandStack->undoCount();
                m_commandStack->endMacro();
                if (m_commandStack->undoCount() > before) m_commandStack->undo();
            }
        }   // marquee / pending click: nothing applied yet
        if (m_dragTool) m_dragTool->cancel();
        // Restore anything the drag changed that the code above didn't (and
        // record nothing).
        cancelDragEditGesture();
        break;

    case DragEnd::Interrupted:
        // A press arrived before the release (lost mouse capture). Moves and
        // rolls are applied live and only RECORDED on release: put them back
        // (as Esc does) rather than leave clips moved with no undo step.
        if (m_dragTool) m_dragTool->interrupt();
        // Close any dangling undo macro so future commands don't accumulate
        // into it, and keep what the drag applied as one step.
        if (m_commandStack && m_commandStack->isMacroActive())
            m_commandStack->endMacro();
        commitDragEditGesture();
        break;
    }

    resetDragState();
    if (how == DragEnd::Cancelled) {
        rebuildTracks();
        emit contentChanged();
    }
}

void TimelinePanel::resetDragState()
{
    QToolTip::hideText();
    if (m_dragTool) {
        m_dragTool->finish();
        m_dragTool.reset();
    }
    m_dragMode = DragMode::None;
    m_dragClipRef = {};
    m_ghostTrackVisible = false;
    if (m_ghostOverlay) m_ghostOverlay->hide();
    if (m_clipDragScrollTimer) m_clipDragScrollTimer->stop();
    m_clipDragLastMovePos = QPointF();
    setCursor(Qt::ArrowCursor);
    for (auto tw : m_trackWidgets)
        tw->setHoverEdgeTick(-1);
    m_snapEngine.resetHysteresis();
    // The snap-indicator (white dashed line) is a transient drag affordance:
    // it must disappear the moment the drag/roll/trim ends, not linger until
    // the next press (a snapped roller/trim looked like a stuck selection).
    setSnapIndicator(-1);
}

} // namespace rt
