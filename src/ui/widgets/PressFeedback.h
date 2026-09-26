/*
 * PressFeedback — app-wide "pressed" state for push/tool buttons whose
 * local stylesheet defines a look (background, :hover) but no :pressed.
 *
 * About 100 inline panel stylesheets restyle buttons with a hover state
 * only; a widget's own / ancestor stylesheet overrides the theme's
 * QPushButton:pressed / QToolButton:pressed, so those buttons lit up on
 * hover but never visibly went down when clicked. This filter lays a
 * translucent dark rounded overlay on such a button while it isDown(),
 * which is the Premiere "the button physically depressed" cue.
 *
 * Only EXACT QPushButton / QToolButton instances are handled — subclasses
 * (rt::ToolButton, TransportButton, …) paint their own pressed state.
 * Buttons covered by a stylesheet that already has :pressed are skipped.
 */

#pragma once

#include <QObject>
#include <QPointer>

class QAbstractButton;
class QApplication;
class QTimer;
class QWidget;

namespace rt {

class PressFeedback : public QObject
{
    Q_OBJECT

public:
    static void install(QApplication* app);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    explicit PressFeedback(QObject* parent);

    static bool wantsOverlay(QAbstractButton* button);
    void sync();

    QPointer<QAbstractButton> m_button;
    QPointer<QWidget>         m_overlay;
    QTimer*                   m_timer{nullptr};
};

} // namespace rt
