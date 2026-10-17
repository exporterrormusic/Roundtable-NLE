/*
 * ScrubbySpinBox — Premiere Pro-style drag-to-adjust numeric input.
 *
 * Step 17: Properties Panel
 *
 * A QDoubleSpinBox subclass that supports:
 *   - Click and drag left/right to scrub the value (cursor: ↔ on hover)
 *   - Shift+drag for fine adjust (10x slower)
 *   - Ctrl+drag for coarse adjust (10x faster); modifiers change the rate
 *     from the current point, mid-drag, without jumping the value
 *   - Cursor hidden while scrubbing, unlimited travel (wraps at the screen
 *     edge), reappears where the drag started — see ScrubGesture
 *   - Normal text editing when clicked without dragging
 *   - Emits valueCommitted when the user finishes a drag (for undo grouping)
 *   - Configurable step, range, precision, and suffix
 *   - Premiere Pro blue value text styling with underline on hover
 *
 * Visual (Premiere Pro Effect Controls style):
 *   ┌──────────────────┐
 *   │  960.0            │  ← blue text, underline on hover, ↔ cursor
 *   └──────────────────┘
 */

#pragma once

#include "widgets/ScrubGesture.h"

#include <QDoubleSpinBox>

namespace rt {

class ScrubbySpinBox : public QDoubleSpinBox
{
    Q_OBJECT

public:
    explicit ScrubbySpinBox(QWidget* parent = nullptr);
    ~ScrubbySpinBox() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the base step size for scrubbing (per pixel of drag).
    void setScrubStep(double step) noexcept { m_scrubStep = step; }
    [[nodiscard]] double scrubStep() const noexcept { return m_scrubStep; }

    /// Set the multiplier for fine mode (shift+drag). Default 0.1.
    void setFineMultiplier(double m) noexcept { m_fineMultiplier = m; }
    [[nodiscard]] double fineMultiplier() const noexcept { return m_fineMultiplier; }

    /// Set the multiplier for coarse mode (ctrl+drag). Default 10.0.
    void setCoarseMultiplier(double m) noexcept { m_coarseMultiplier = m; }
    [[nodiscard]] double coarseMultiplier() const noexcept { return m_coarseMultiplier; }

    /// Whether the user is currently scrubbing.
    [[nodiscard]] bool isScrubbing() const noexcept { return m_scrubbing; }

    /// The value at the start of the current scrub gesture.
    [[nodiscard]] double scrubStartValue() const noexcept { return m_startValue; }

    // ── Integer mode convenience ────────────────────────────────────────

    /// Configure as an integer spinbox (decimals=0, step=1).
    void setIntegerMode();

    /// Get/set value as int for convenience.
    [[nodiscard]] int intValue() const noexcept { return static_cast<int>(value()); }
    void setIntValue(int v) { setValue(static_cast<double>(v)); }

signals:
    /// Emitted when a scrub gesture finishes (mouse release after drag).
    /// Contains the value before the scrub began, for command creation.
    void valueCommitted(double oldValue, double newValue);

    /// Emitted during a scrub drag (each mouse move).
    void valueScrubbed(double currentValue);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void applyPremiereStyle();
    void installLineEditFilter();
    void onEditingFinished();
    // Tell the hosting panel (EditBracketHost.h) where one edit — a scrub or
    // a typed entry — begins and ends, so it records as one undo step.
    void beginHostEdit();
    void endHostEdit();
    bool m_hostEditOpen{false};

    double m_scrubStep{0.01};
    double m_fineMultiplier{0.1};
    double m_coarseMultiplier{10.0};

    bool   m_scrubbing{false};
    bool   m_potentialScrub{false};
    ScrubGesture m_gesture;
    double m_startValue{0.0};
    double m_accumulator{0.0};
};

} // namespace rt
