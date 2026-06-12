/*
 * ShortcutController.h — keyboard routing binder for the timeline workspace.
 *
 * Owns:
 *   - the workspace-level key handling (Space/JKL transport with the
 *     Premiere-style K+J / K+L slow shuttle, edit-point navigation,
 *     tool keys, Delete/ripple, tilde maximize, ...) — the bodies of the
 *     former TimelineWorkspace::keyPressEvent/keyReleaseEvent
 *   - QShortcut registration (Ctrl+X/C/V, I/O, Home/End, ...)
 *
 * The QWidget overrides stay on TimelineWorkspace as thin wrappers:
 * they call handleKeyPress/handleKeyRelease and forward to
 * QWidget::key*Event only when the controller left the event ignored
 * (the controller marks unhandled fallthrough with event->ignore()).
 *
 * Extracted from TimelineWorkspace (god-class decomposition,
 * fable_cleanup.txt §3.1).  TRANSITIONAL DESIGN like OverlayController /
 * DropController: friend + back-pointer (m_X → m_ws->m_X).
 *
 * Definitions: ShortcutControllerKeys.cpp (key handlers),
 * ShortcutControllerRegister.cpp (registerKeyboardShortcuts).
 */
#pragma once

#include <QObject>

class QKeyEvent;

namespace rt {

class TimelineWorkspace;

class ShortcutController : public QObject {
public:
    explicit ShortcutController(TimelineWorkspace* ws) : m_ws(ws) {}

    /// Body of the former keyPressEvent override.  Accepts the event when
    /// handled; leaves it ignored for the workspace wrapper to forward.
    void handleKeyPress(QKeyEvent* event);
    /// Body of the former keyReleaseEvent override (K+J/K+L slow-shuttle
    /// stop on release).
    void handleKeyRelease(QKeyEvent* event);

    /// Register keyboard shortcuts (Home/End, I/O, Ctrl+X/C/V, etc.).
    /// QShortcuts are parented to the workspace widget so their context
    /// (WidgetWithChildrenShortcut) is unchanged.
    void registerKeyboardShortcuts();

private:
    TimelineWorkspace* m_ws{nullptr};

    // ── JKL key state ──────────────────────────────────────────────────
    // Tracks held-key state for Premiere-style K+J / K+L slow shuttle.
    // When K is held and J/L is pressed, we engage a 0.5× scrub that
    // stops the moment J/L is released.
    bool m_kHeld{false};
    bool m_kSlowShuttleActive{false};
};

} // namespace rt
