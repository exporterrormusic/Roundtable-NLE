#pragma once

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QWidget>

namespace rt {

// Global editor shortcuts must yield while a widget is using ordinary keys
// for text entry or keyboard selection. Editable combo boxes can report the
// combo itself (or their completer popup) as the focus widget instead of their
// internal QLineEdit, so checking only QLineEdit is not sufficient.
inline bool widgetConsumesTextKeys(const QWidget* focused)
{
    for (const QObject* object = focused; object; object = object->parent()) {
        if (qobject_cast<const QLineEdit*>(object)
            || qobject_cast<const QTextEdit*>(object)
            || qobject_cast<const QPlainTextEdit*>(object)
            || qobject_cast<const QAbstractSpinBox*>(object)
            || qobject_cast<const QComboBox*>(object)
            || qobject_cast<const QKeySequenceEdit*>(object)) {
            return true;
        }
    }
    return false;
}

inline bool keyboardFocusConsumesTextKeys()
{
    const QWidget* focused = QApplication::focusWidget();
    if (widgetConsumesTextKeys(focused)) return true;

    // QCompleter and combo-box popups may move focus to their item view. While
    // a popup owns that focus, keystrokes belong to the popup, not editor tools.
    const QWidget* popup = QApplication::activePopupWidget();
    return popup && (!focused || popup == focused || popup->isAncestorOf(focused));
}

} // namespace rt
