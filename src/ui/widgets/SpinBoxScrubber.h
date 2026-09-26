/*
 * SpinBoxScrubber — app-wide drag-to-adjust for every plain QSpinBox /
 * QDoubleSpinBox (dialogs, Captions, Export, Tier List, …), so numeric
 * fields everywhere scrub like Premiere's hot text, not just the
 * ScrubbySpinBox ones in Effect Controls / Properties.
 *
 * Installed once on qApp. Behaviour on an UNFOCUSED field:
 *   - hover shows the ↔ cursor
 *   - press + drag horizontally scrubs (Shift fine, Ctrl coarse)
 *   - press + release without dragging is an ordinary click (caret / edit)
 * A field that already has focus behaves as a normal text box (drag
 * selects text). ScrubbySpinBox has its own handling and is skipped.
 *
 * Opt out per widget with setProperty(SpinBoxScrubber::kOptOutProperty, true).
 *
 * Each scrub fires ordinary valueChanged() per step and editingFinished()
 * once at the end. Panels that push an undo step per valueChanged should
 * group the gesture via scrubStarted / scrubFinished.
 */

#pragma once

#include "widgets/ScrubGesture.h"

#include <QObject>
#include <QPointer>

class QAbstractSpinBox;
class QApplication;
class QLineEdit;

namespace rt {

class SpinBoxScrubber : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* kOptOutProperty = "rtNoScrub";

    /// Create the singleton and install it on the application.
    static void install(QApplication* app);
    /// nullptr until install().
    static SpinBoxScrubber* instance();

signals:
    void scrubStarted(QAbstractSpinBox* box);
    void scrubFinished(QAbstractSpinBox* box);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    explicit SpinBoxScrubber(QObject* parent);

    /// The spin box whose internal line edit is `obj`, if it is one we scrub.
    static QAbstractSpinBox* scrubbableBoxFor(QObject* obj);
    static double valueOf(const QAbstractSpinBox* box);
    static void   setValueOf(QAbstractSpinBox* box, double v);
    /// Value change per pixel at normal speed.
    static double ratePerPixel(const QAbstractSpinBox* box);

    ScrubGesture               m_gesture;
    QPointer<QAbstractSpinBox> m_box;
    QPointer<QLineEdit>        m_edit;
    double                     m_startValue{0.0};
    double                     m_accumulator{0.0};
};

} // namespace rt
