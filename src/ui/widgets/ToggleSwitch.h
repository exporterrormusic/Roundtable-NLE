/*
 * ToggleSwitch.h — Premiere-style sliding on/off switch.
 *
 * A checkable QAbstractButton painted as a rounded pill track with a sliding
 * knob (blue accent when ON, grey when OFF). Used for the per-section
 * enable/disable switches in the Export panel (VIDEO / AUDIO).
 *
 * No Q_OBJECT of its own — connect to the inherited
 * QAbstractButton::toggled(bool) signal:
 *   auto* sw = new ToggleSwitch(parent);
 *   connect(sw, &QAbstractButton::toggled, this, [](bool on){ ... });
 */

#pragma once

#include "Theme.h"

#include <QAbstractButton>
#include <QPainter>

namespace rt {

class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget* parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setChecked(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setFixedSize(38, 20);
    }

    [[nodiscard]] QSize sizeHint() const override { return {38, 20}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto& tc = Theme::colors();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const qreal w = width();
        const qreal h = height();
        const qreal r = h / 2.0;
        const bool on = isChecked();
        const bool en = isEnabled();

        // ── Track ───────────────────────────────────────────────────────
        QColor track = on ? tc.accent : QColor(72, 72, 84);
        if (en && underMouse())
            track = on ? tc.accentHover : QColor(90, 90, 104);
        if (!en)
            track.setAlpha(90);
        p.setPen(Qt::NoPen);
        p.setBrush(track);
        p.drawRoundedRect(QRectF(0, 0, w, h), r, r);

        // ── Knob ────────────────────────────────────────────────────────
        const qreal margin = 2.0;
        const qreal kd = h - margin * 2.0;          // knob diameter
        const qreal kx = on ? (w - kd - margin) : margin;
        QColor knob = en ? tc.textBright : tc.textSecondary;
        p.setBrush(knob);
        p.drawEllipse(QRectF(kx, margin, kd, kd));
    }
};

} // namespace rt
