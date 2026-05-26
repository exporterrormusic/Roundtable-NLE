/*
 * TimelinePanelPaintEvents.cpp — Painting and event interception.
 * Split from TimelinePanel.cpp for maintainability.
 *
 * Contains: paintEvent(), eventFilter(), keyPressEvent().
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "ShortcutManager.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QApplication>
#include <QMenu>
#include <QAction>
#include <QPainter>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  paintEvent — Draws marquee selection box and in/out overlays on top
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::paintEvent(QPaintEvent* event)
{
    // ── Paint recursion guard ─────────────────────────────────────────
    // Detect paint → layout → repaint loops caused by QSplitter + dock
    // widget interactions during startup.  If we re-enter paintEvent
    // more than 5 times without returning, bail out.
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        return;
    }

    QWidget::paintEvent(event);

    --s_paintDepth;
}

// ═════════════════════════════════════════════════════════════════════════════
//  eventFilter — Intercepts mouse events on TimelineTrackWidgets so we can
//  handle drag-move, trim, and marquee selection centrally.
// ═════════════════════════════════════════════════════════════════════════════

bool TimelinePanel::eventFilter(QObject* watched, QEvent* event)
{
    // Forward wheel events from the vertical scroll area or any child
    // back to TimelinePanel so Ctrl+wheel zoom, Shift+wheel track resize,
    // and regular-wheel horizontal scroll always work regardless of which
    // child widget the cursor is hovering over.
    if (event->type() == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        wheelEvent(we);
        return true;  // consumed — don't let QScrollArea scroll vertically
    }

    // Right-click context menu on empty space in the track header area
    if (event->type() == QEvent::ContextMenu && watched == m_trackHeaderArea) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        QMenu menu(this);
        QAction* addVideo = menu.addAction(QStringLiteral("Add Video Track"));
        QAction* addAudio = menu.addAction(QStringLiteral("Add Audio Track"));
        QAction* chosen = menu.exec(ce->globalPos());
        if (chosen == addVideo)
            emit addTrackAbove(0, true);
        else if (chosen == addAudio)
            emit addTrackBelow(m_timeline ? m_timeline->trackCount() : 0, false);
        event->accept();
        return true;
    }

    // Intercept mouse events on track widgets AND the track content area /
    // scroll-area viewport so that drag-select (marquee) works even when
    // the drag starts in empty space outside any track widget.
    auto* tw = qobject_cast<TimelineTrackWidget*>(watched);
    bool isTrackChild = (tw != nullptr);
    bool isTrackArea = (watched == m_trackContentArea
                     || watched == m_verticalScroll
                     || watched == m_verticalScroll->viewport());
    if (!isTrackChild && !isTrackArea)
        return QWidget::eventFilter(watched, event);

    // ── Right-click context menu on clips ────────────────────────────────
    if (event->type() == QEvent::ContextMenu && isTrackChild) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        QPointF panelPos = tw->mapTo(this, ce->pos());
        auto hitRef = hitTestClip(panelPos);
        if (hitRef) {
            showClipContextMenu(ce->globalPos(), *hitRef);
        } else {
            size_t ti = hitTestTrack(panelPos.y());
            showEmptyAreaContextMenu(ce->globalPos(), ti);
        }
        event->accept();
        return true;
    }

    // Right-click context menu on empty area below/between tracks
    if (event->type() == QEvent::ContextMenu && isTrackArea) {
        auto* ce = static_cast<QContextMenuEvent*>(event);
        size_t lastTrack = m_timeline ? m_timeline->trackCount() : 0;
        showEmptyAreaContextMenu(ce->globalPos(), lastTrack > 0 ? lastTrack - 1 : 0);
        event->accept();
        return true;
    }

    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonDblClick)
    {
        auto* me = static_cast<QMouseEvent*>(event);

        // Map the position from the child widget's local coords to
        // TimelinePanel's local coords so our hit-test and drag logic
        // works with the same coordinate space as mousePressEvent etc.
        auto* sourceWidget = qobject_cast<QWidget*>(watched);
        QPointF panelPos = sourceWidget->mapTo(this, me->pos());

        // Create a translated mouse event in panel coords
        QMouseEvent mapped(me->type(), panelPos, me->globalPosition(),
                           me->button(), me->buttons(), me->modifiers());

        switch (event->type())
        {
        case QEvent::MouseButtonDblClick:
            mouseDoubleClickEvent(&mapped);
            break;
        case QEvent::MouseButtonPress:
            // Give keyboard focus to the enclosing TimelineWorkspace so that
            // single-key shortcuts (Left/Right arrows, I, O, Delete, Space,
            // etc.) fire from its keyPressEvent. parentWidget() here is the
            // intermediate centerContainer the layout sits in — focusing it
            // didn't propagate keys reliably (every other click left arrows
            // dead until the user re-clicked).  Walk up explicitly until we
            // hit the TimelineWorkspace.
            {
                QWidget* w = parentWidget();
                while (w && !qobject_cast<TimelineWorkspace*>(w))
                    w = w->parentWidget();
                if (w) w->setFocus(Qt::MouseFocusReason);
            }
            mousePressEvent(&mapped);
            break;
        case QEvent::MouseMove:
            mouseMoveEvent(&mapped);
            break;
        case QEvent::MouseButtonRelease:
            mouseReleaseEvent(&mapped);
            break;
        default:
            break;
        }

        // Accept the original event so the child widget doesn't also
        // process it (prevents double-handling).
        event->accept();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  keyPressEvent
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::keyPressEvent(QKeyEvent* event)
{
    if (m_shortcuts && m_shortcuts->handleKeyPress(event->key(), event->modifiers()))
    {
        event->accept();
        return;
    }
    // Forward unhandled keys to the enclosing TimelineWorkspace so its
    // keyPressEvent can handle transport (Left/Right arrows with auto-repeat)
    // and other workspace-level shortcuts.  parentWidget() is an
    // intermediate container widget here (the layout's host), not the
    // workspace itself — sending to it sat the event in a chain whose top
    // sometimes swallowed arrow keys.  Walk up explicitly until we find the
    // workspace and dispatch there.  QApplication::sendEvent is used
    // instead of calling keyPressEvent directly because QWidget::keyPressEvent
    // is protected.
    QWidget* w = parentWidget();
    while (w && !qobject_cast<TimelineWorkspace*>(w))
        w = w->parentWidget();
    if (w) {
        QApplication::sendEvent(w, event);
        if (event->isAccepted())
            return;
    }
    QWidget::keyPressEvent(event);
}

// Forward releases to the workspace too. Required for Premiere-style K+J/K+L
// slow scrub: releasing J or L (while K is still held) must reach
// TimelineWorkspace::keyReleaseEvent to stop playback. Default propagation
// gets eaten by an intermediate container widget in our dock layout.
void TimelinePanel::keyReleaseEvent(QKeyEvent* event)
{
    QWidget* w = parentWidget();
    while (w && !qobject_cast<TimelineWorkspace*>(w))
        w = w->parentWidget();
    if (w) {
        QApplication::sendEvent(w, event);
        if (event->isAccepted())
            return;
    }
    QWidget::keyReleaseEvent(event);
}

} // namespace rt
