/*
 * SpinBoxScrubber.cpp — see SpinBoxScrubber.h.
 */

#include "widgets/SpinBoxScrubber.h"
#include "widgets/ScrubbySpinBox.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSpinBox>

#include <algorithm>
#include <cmath>

namespace rt {

namespace {
SpinBoxScrubber* s_instance = nullptr;

// A field spanning fewer steps than this scrubs slower than one step per
// pixel (up to 10 px per step) so small ranges stay controllable.
constexpr double kComfortableTravelPx = 400.0;
constexpr double kFineMultiplier   = 0.1;
constexpr double kCoarseMultiplier = 10.0;
} // namespace

SpinBoxScrubber::SpinBoxScrubber(QObject* parent)
    : QObject(parent)
{
}

void SpinBoxScrubber::install(QApplication* app)
{
    if (s_instance || !app) return;
    s_instance = new SpinBoxScrubber(app);
    app->installEventFilter(s_instance);
}

SpinBoxScrubber* SpinBoxScrubber::instance()
{
    return s_instance;
}

QAbstractSpinBox* SpinBoxScrubber::scrubbableBoxFor(QObject* obj)
{
    auto* edit = qobject_cast<QLineEdit*>(obj);
    if (!edit) return nullptr;
    auto* box = qobject_cast<QAbstractSpinBox*>(edit->parentWidget());
    if (!box) return nullptr;
    if (!qobject_cast<QSpinBox*>(box) && !qobject_cast<QDoubleSpinBox*>(box))
        return nullptr;                       // date/time edits etc.
    if (qobject_cast<ScrubbySpinBox*>(box))
        return nullptr;                       // has its own scrubbing
    if (!box->isEnabled() || box->isReadOnly()
        || box->property(kOptOutProperty).toBool())
        return nullptr;
    return box;
}

double SpinBoxScrubber::valueOf(const QAbstractSpinBox* box)
{
    if (auto* s = qobject_cast<const QSpinBox*>(box))       return s->value();
    if (auto* d = qobject_cast<const QDoubleSpinBox*>(box)) return d->value();
    return 0.0;
}

void SpinBoxScrubber::setValueOf(QAbstractSpinBox* box, double v)
{
    if (auto* s = qobject_cast<QSpinBox*>(box))
        s->setValue(static_cast<int>(std::lround(v)));
    else if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        d->setValue(v);
}

double SpinBoxScrubber::ratePerPixel(const QAbstractSpinBox* box)
{
    double step = 1.0, range = 0.0;
    if (auto* s = qobject_cast<const QSpinBox*>(box)) {
        step  = s->singleStep();
        range = static_cast<double>(s->maximum()) - s->minimum();
    } else if (auto* d = qobject_cast<const QDoubleSpinBox*>(box)) {
        step  = d->singleStep();
        range = d->maximum() - d->minimum();
    }
    if (step <= 0.0) step = 1.0;
    const double steps     = std::max(range / step, 1.0);
    const double pxPerStep = std::clamp(kComfortableTravelPx / steps, 1.0, 10.0);
    return step / pxPerStep;
}

bool SpinBoxScrubber::eventFilter(QObject* obj, QEvent* event)
{
    // App-wide filter: bail on everything but the handful of types we use.
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove
        && type != QEvent::MouseButtonRelease && type != QEvent::Enter
        && type != QEvent::FocusIn && type != QEvent::FocusOut)
        return false;

    // The field died mid-gesture: never leave the cursor hidden.
    if (!m_edit && (m_gesture.isArmed() || m_gesture.isActive()))
        m_gesture.cancel();

    switch (type) {
    case QEvent::Enter:
    case QEvent::FocusIn:
    case QEvent::FocusOut: {
        if (auto* box = scrubbableBoxFor(obj)) {
            Q_UNUSED(box);
            auto* edit = static_cast<QLineEdit*>(obj);
            const bool editing = (type == QEvent::FocusIn)
                              || (type == QEvent::Enter && edit->hasFocus());
            edit->setCursor(editing ? Qt::IBeamCursor : Qt::SizeHorCursor);
        }
        return false;
    }

    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton) return false;
        auto* box = scrubbableBoxFor(obj);
        auto* edit = qobject_cast<QLineEdit*>(obj);
        // A focused field is being typed in — dragging selects text.
        if (!box || edit->hasFocus()) return false;
        m_box         = box;
        m_edit        = edit;
        m_startValue  = valueOf(box);
        m_accumulator = 0.0;
        m_gesture.press(me->globalPosition().toPoint());
        return false; // let the click place focus/caret as usual
    }

    case QEvent::MouseMove: {
        if (obj != m_edit || !m_box) return false;
        if (!m_gesture.isArmed() && !m_gesture.isActive()) return false;
        auto* me = static_cast<QMouseEvent*>(event);
        const bool wasActive = m_gesture.isActive();
        const int dx = m_gesture.move(me->globalPosition().toPoint());
        if (!m_gesture.isActive()) return false;
        if (!wasActive) {
            m_edit->deselect();
            emit scrubStarted(m_box);
        }

        double rate = ratePerPixel(m_box);
        if (me->modifiers() & Qt::ShiftModifier)        rate *= kFineMultiplier;
        else if (me->modifiers() & Qt::ControlModifier) rate *= kCoarseMultiplier;

        double lo = 0.0, hi = 0.0;
        if (auto* s = qobject_cast<QSpinBox*>(m_box.data())) {
            lo = s->minimum(); hi = s->maximum();
        } else if (auto* d = qobject_cast<QDoubleSpinBox*>(m_box.data())) {
            lo = d->minimum(); hi = d->maximum();
        }
        m_accumulator += dx * rate;
        const double target = std::clamp(m_startValue + m_accumulator, lo, hi);
        m_accumulator = target - m_startValue; // pin at limits
        setValueOf(m_box, target);
        return true;
    }

    case QEvent::MouseButtonRelease: {
        if (obj != m_edit) return false;
        if (static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton)
            return false;
        if (!m_gesture.isActive()) {
            m_gesture.cancel();
            m_box = nullptr;
            m_edit = nullptr;
            return false; // plain click → normal editing
        }
        m_gesture.release();
        QPointer<QAbstractSpinBox> box = m_box;
        m_box = nullptr;
        m_edit->deselect();
        m_edit = nullptr;
        if (box) {
            box->clearFocus(); // fires editingFinished once, like Return
            emit scrubFinished(box);
        }
        return true;
    }

    default:
        return false;
    }
}

} // namespace rt
