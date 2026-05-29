/*
 * ScrubbySpinBox.cpp — Premiere Pro-style drag-to-adjust numeric input.
 * Step 17
 */

#include "widgets/ScrubbySpinBox.h"
#include "Theme.h"

#include <QApplication>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QCursor>
#include <QLineEdit>
#include <QTimer>

#include <cmath>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Premiere Pro Effect Controls color reference:
//    Background:  #232323 (dark charcoal)
//    Value text:  #4A9EDB (blue, scrubby)  — underlines on hover
//    Label text:  #999999 (gray)
//    Border:      none (frameless)
//    Hover:       value underlines, cursor changes to ↔
//    Editing:     white text on selection-blue background
// ═════════════════════════════════════════════════════════════════════════════

ScrubbySpinBox::ScrubbySpinBox(QWidget* parent)
    : QDoubleSpinBox(parent)
{
    setCursor(Qt::SizeHorCursor);
    setKeyboardTracking(false);
    setButtonSymbols(QAbstractSpinBox::NoButtons); // Premiere has no up/down arrows
    setMinimumHeight(34);
    applyPremiereStyle();
    installLineEditFilter();

    // When editing finishes (Return key or focus loss), commit the typed
    // value through the same valueScrubbed/valueCommitted path that scrub
    // uses (proven working for undo).  The scrub release path blocks
    // signals around clearFocus, so this won't double-fire for scrubs.
    connect(this, &QDoubleSpinBox::editingFinished,
            this, &ScrubbySpinBox::onEditingFinished);

    // Keep the scrub baseline in sync with PROGRAMMATIC value changes (e.g.
    // populateFromClip() rewriting the spinbox after a flip toggles the sign
    // of the scale track).  m_startValue is the "before" value reported to
    // valueCommitted for undo; if it goes stale, undoing one edit can also
    // revert an earlier unrelated change that lives in the same property
    // track (flip H/V is stored as the sign of scaleX/scaleY).  We only sync
    // when there is no active user gesture: scrubbing maintains its own
    // press-time baseline, and typed entry seeds m_startValue on FocusIn.
    connect(this, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_scrubbing && !hasFocus())
            m_startValue = v;
    });
}

ScrubbySpinBox::~ScrubbySpinBox() = default;

void ScrubbySpinBox::installLineEditFilter()
{
    // The internal QLineEdit consumes mouse events before they reach
    // QDoubleSpinBox overrides.  Install ourselves as event filter so
    // we can intercept press/move/release for scrubbing.
    if (auto* le = lineEdit())
    {
        le->installEventFilter(this);
        le->setCursor(Qt::SizeHorCursor);
        le->setReadOnly(false);
    }
}

void ScrubbySpinBox::applyPremiereStyle()
{
    // Premiere Pro Effect Controls styling:
    // - No border or frame, flat dark bg
    // - Blue value text with underline on hover
    // - White text when editing
    const auto bg   = Theme::hex(Theme::colors().inputBg);
    const auto val  = Theme::hex(Theme::colors().accent);
    const auto sel  = Theme::hex(Theme::colors().inputSelection);
    const auto selT = Theme::hex(Theme::colors().textBright);
    const auto focT = Theme::hex(Theme::colors().textBright);
    const auto focB = Theme::hex(Theme::colors().surface0);
    setStyleSheet(QString(
        "QDoubleSpinBox {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-bottom: 1px solid transparent;"
        "  padding: 0px 4px;"
        "  font-size: 12px;"
        "  font-family: 'Segoe UI', 'Arial', sans-serif;"
        "  selection-background-color: %3;"
        "  selection-color: %4;"
        "}"
        "QDoubleSpinBox:hover {"
        "  border-bottom: 1px solid %2;"
        "}"
        "QDoubleSpinBox:focus {"
        "  color: %5;"
        "  background: %6;"
        "  border-bottom: 1px solid %2;"
        "}"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {"
        "  width: 0; height: 0; border: none;"
        "}"
    ).arg(bg, val, sel, selT, focT, focB));
    setAlignment(Qt::AlignVCenter | Qt::AlignRight);
}

void ScrubbySpinBox::setIntegerMode()
{
    setDecimals(0);
    setScrubStep(1.0);
    setSingleStep(1.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Hover effects
// ═════════════════════════════════════════════════════════════════════════════

void ScrubbySpinBox::enterEvent(QEnterEvent* event)
{
    if (!m_scrubbing) {
        setCursor(Qt::SizeHorCursor);
        if (lineEdit()) lineEdit()->setCursor(Qt::SizeHorCursor);
    }
    QDoubleSpinBox::enterEvent(event);
}

void ScrubbySpinBox::leaveEvent(QEvent* event)
{
    if (!m_scrubbing) {
        setCursor(Qt::ArrowCursor);
        if (lineEdit()) lineEdit()->setCursor(Qt::ArrowCursor);
    }
    QDoubleSpinBox::leaveEvent(event);
}

void ScrubbySpinBox::wheelEvent(QWheelEvent* event)
{
    // Ignore mouse wheel — values should only change via click+type or click+drag.
    event->ignore();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Event filter — intercept mouse events on the internal QLineEdit
// ═════════════════════════════════════════════════════════════════════════════

bool ScrubbySpinBox::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == lineEdit())
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
            mousePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseMove:
            mouseMoveEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonRelease:
            mouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::FocusIn:
            // Capture the value when edit mode begins so we know the
            // "before" state for undo when editing finishes.
            m_startValue = value();
            break;
        case QEvent::ShortcutOverride: {
            // Prevent the QLineEdit from accepting Ctrl+Z / Ctrl+Y so
            // the application-level undo/redo QAction fires instead.
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->matches(QKeySequence::Undo) ||
                ke->matches(QKeySequence::Redo)) {
                event->ignore();
                return true; // block QLineEdit from seeing it
            }
            break;
        }
        case QEvent::KeyPress: {
            // Forward Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z to the top-level
            // window so the app's undo stack handles them, not the
            // QLineEdit's internal text undo.
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->matches(QKeySequence::Undo) ||
                ke->matches(QKeySequence::Redo)) {
                if (auto* w = window())
                    QApplication::sendEvent(w, ke);
                return true;
            }
            // Return/Enter: let it fall through to QDoubleSpinBox's own
            // event filter (QAbstractSpinBox installs one on the lineEdit).
            // That base filter calls interpretText() + emits editingFinished,
            // which our onEditingFinished() slot handles.
            break;
        }
        default:
            break;
        }
    }
    return QDoubleSpinBox::eventFilter(obj, event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Mouse events — scrub interaction (with cursor wrapping like Premiere Pro)
// ═════════════════════════════════════════════════════════════════════════════

void ScrubbySpinBox::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        m_pressPos       = event->globalPosition().toPoint();
        m_lastGlobalPos  = m_pressPos;
        m_startValue     = value();
        m_accumulator    = 0.0;
        m_potentialScrub = true;
        m_scrubbing      = false;
        event->accept();
        return;
    }
    QDoubleSpinBox::mousePressEvent(event);
}

void ScrubbySpinBox::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_potentialScrub && !m_scrubbing)
    {
        QDoubleSpinBox::mouseMoveEvent(event);
        return;
    }

    QPoint globalPos = event->globalPosition().toPoint();

    // Detect if we've started scrubbing (drag threshold)
    if (m_potentialScrub && !m_scrubbing)
    {
        if (std::abs(globalPos.x() - m_pressPos.x()) >= kDragThreshold)
        {
            m_scrubbing = true;
            m_potentialScrub = false;
        }
        else
        {
            return; // Haven't moved far enough yet
        }
    }

    if (m_scrubbing)
    {
        // Calculate step with modifiers
        double step = m_scrubStep;

        if (event->modifiers() & Qt::ShiftModifier)
            step *= m_fineMultiplier;
        else if (event->modifiers() & Qt::ControlModifier)
            step *= m_coarseMultiplier;

        // Use total horizontal displacement from press position rather than
        // per-event deltas — avoids the inherent jitter of warping the cursor
        // back on every mouse move (which causes OS cursor-position races).
        int dx = globalPos.x() - m_pressPos.x();
        m_accumulator = dx * step;
        double newVal = std::clamp(m_startValue + m_accumulator, minimum(), maximum());

        if (newVal != value())
        {
            setValue(newVal);
            emit valueScrubbed(newVal);
        }

        event->accept();
        return;
    }

    m_lastGlobalPos = globalPos;
}

void ScrubbySpinBox::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (m_scrubbing)
        {
            // End scrub — restore cursor
            setCursor(Qt::SizeHorCursor);
            if (lineEdit()) lineEdit()->setCursor(Qt::SizeHorCursor);
            double finalValue = value();
            m_scrubbing = false;
            m_potentialScrub = false;

            if (finalValue != m_startValue)
                emit valueCommitted(m_startValue, finalValue);

            // Block signals around clearFocus to prevent editingFinished
            // from firing a duplicate undo push.
            blockSignals(true);
            clearFocus();
            blockSignals(false);

            event->accept();
            return;
        }

        if (m_potentialScrub)
        {
            // User clicked without dragging — enter edit mode
            m_potentialScrub = false;
            setCursor(Qt::IBeamCursor);
            if (lineEdit()) {
                lineEdit()->setCursor(Qt::IBeamCursor);
                lineEdit()->setFocus();
                lineEdit()->selectAll();
            }
            event->accept();
            return;
        }
    }

    QDoubleSpinBox::mouseReleaseEvent(event);
}

void ScrubbySpinBox::onEditingFinished()
{
    // Called when the user presses Return or the spinbox loses focus.
    // Scrub release blocks signals around clearFocus, so this only
    // fires for typed entry / focus-loss commits — never for scrub.
    if (m_scrubbing) return;

    double newVal = value();
    if (newVal != m_startValue) {
        emit valueScrubbed(newVal);
        emit valueCommitted(m_startValue, newVal);
        m_startValue = newVal; // prevent duplicate on subsequent focus loss
    }

    // Defer focus transfer so we don't re-enter during signal processing.
    // Block signals to prevent a second editingFinished from the focus change.
    QTimer::singleShot(0, this, [this]() {
        blockSignals(true);
        if (auto* p = parentWidget())
            p->setFocus(Qt::OtherFocusReason);
        blockSignals(false);

        setCursor(Qt::SizeHorCursor);
        if (lineEdit()) lineEdit()->setCursor(Qt::SizeHorCursor);
    });
}

} // namespace rt

