/*
 * TransportButton.h — Custom-painted transport control button.
 *
 * Renders the monitor button-row icons (Premiere's set: Add Marker, Mark
 * In/Out, Go to In/Out, Step Back/Forward 1 Frame, Play-Stop, Lift, Extract,
 * Export Frame, Mute, the wrench) using QPainter geometric shapes instead of
 * Unicode symbols, ensuring reliable rendering regardless of installed fonts.
 * A checkable button draws in the accent colour while checked.
 *
 * Usage:
 *   auto* btn = new TransportButton(TransportButton::Play, parent);
 *   btn->setFixedSize(28, 28);
 */

#pragma once

#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPushButton>

namespace rt {

class TransportButton : public QPushButton
{
public:
    enum Icon { GoStart, StepBack, Play, Pause, Stop, StepForward, GoEnd, Screenshot,
                AddMarker, MarkIn, MarkOut, GoToIn, GoToOut, Lift, Extract,
                Speaker, Settings };

    explicit TransportButton(Icon icon, QWidget* parent = nullptr)
        : QPushButton(parent), m_icon(icon)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; }"));
    }

    void setTransportIcon(Icon icon) { if (m_icon == icon) return; m_icon = icon; update(); }
    [[nodiscard]] Icon transportIcon() const { return m_icon; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const auto& tc = Theme::colors();

        // Hover / pressed background
        if (isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.controlBgActive);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        } else if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.controlBgHover);
            p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        }

        // Icon color
        QColor color = (isCheckable() && isChecked()) ? tc.accent
                     : underMouse()                   ? tc.textBright
                     : !isEnabled()                   ? tc.textDisabled
                                                      : tc.textSecondary;
        m_color = color;
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        qreal cx = width() * 0.5;
        qreal cy = height() * 0.5;
        qreal s  = qMin(width(), height()) * 0.28;

        switch (m_icon) {
        case GoStart:   drawGoStart(p, cx, cy, s);   break;
        case StepBack:  drawStepBack(p, cx, cy, s);  break;
        case Play:      drawPlay(p, cx, cy, s);      break;
        case Pause:     drawPause(p, cx, cy, s);     break;
        case Stop:      drawStop(p, cx, cy, s);      break;
        case StepForward: drawStepFwd(p, cx, cy, s); break;
        case GoEnd:       drawGoEnd(p, cx, cy, s);       break;
        case Screenshot:  drawScreenshot(p, cx, cy, s);  break;
        case AddMarker:   drawAddMarker(p, cx, cy, s);   break;
        case MarkIn:      drawBrace(p, cx + s * 0.2, cy, s, +1); break;
        case MarkOut:     drawBrace(p, cx - s * 0.2, cy, s, -1); break;
        case GoToIn:      drawGoToIn(p, cx, cy, s);      break;
        case GoToOut:     drawGoToOut(p, cx, cy, s);     break;
        case Lift:        drawLiftExtract(p, cx, cy, s, /*gap=*/true);  break;
        case Extract:     drawLiftExtract(p, cx, cy, s, /*gap=*/false); break;
        case Speaker:     drawSpeaker(p, cx, cy, s);     break;
        case Settings:    drawWrench(p, cx, cy, s);      break;
        }
    }

private:
    // |◀  — vertical bar + left-pointing triangle
    void drawGoStart(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        p.drawRect(QRectF(cx - s - 1, cy - s, 2, s * 2));
        QPainterPath tri;
        tri.moveTo(cx + s,     cy - s);
        tri.lineTo(cx - s + 2, cy);
        tri.lineTo(cx + s,     cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // ◀| — Premiere's Step Back 1 Frame: left-pointing triangle + bar
    void drawStepBack(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath tri;
        tri.moveTo(cx + s * 0.3,  cy - s);
        tri.lineTo(cx - s * 0.85, cy);
        tri.lineTo(cx + s * 0.3,  cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawRect(QRectF(cx + s * 0.55, cy - s, 2, s * 2));
    }

    // ▶  — right-pointing triangle
    void drawPlay(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath tri;
        tri.moveTo(cx - s * 0.65, cy - s);
        tri.lineTo(cx + s,        cy);
        tri.lineTo(cx - s * 0.65, cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // ⏸ — two vertical bars
    void drawPause(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        qreal bw = s * 0.35;
        qreal gap = s * 0.2;
        p.drawRect(QRectF(cx - gap - bw, cy - s, bw, s * 2));
        p.drawRect(QRectF(cx + gap,      cy - s, bw, s * 2));
    }

    // ■ — filled square
    void drawStop(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        qreal half = s * 0.8;
        p.drawRect(QRectF(cx - half, cy - half, half * 2, half * 2));
    }

    // |▶ — Premiere's Step Forward 1 Frame: bar + right-pointing triangle
    void drawStepFwd(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        p.drawRect(QRectF(cx - s * 0.55 - 2, cy - s, 2, s * 2));
        QPainterPath tri;
        tri.moveTo(cx - s * 0.3,  cy - s);
        tri.lineTo(cx + s * 0.85, cy);
        tri.lineTo(cx - s * 0.3,  cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
    }

    // ▶| — right-pointing triangle + vertical bar
    void drawGoEnd(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath tri;
        tri.moveTo(cx - s,     cy - s);
        tri.lineTo(cx + s - 2, cy);
        tri.lineTo(cx - s,     cy + s);
        tri.closeSubpath();
        p.drawPath(tri);
        p.drawRect(QRectF(cx + s - 1, cy - s, 2, s * 2));
    }

    // 📷 — camera icon (rounded rect body + circle lens)
    void drawScreenshot(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        // Camera body — rounded rectangle
        qreal bw = s * 1.8;
        qreal bh = s * 1.3;
        p.drawRoundedRect(QRectF(cx - bw / 2, cy - bh / 2 + s * 0.15, bw, bh), 2, 2);
        // Lens — circle in center
        qreal r = s * 0.42;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Theme::colors().surface0, 1.5));
        p.drawEllipse(QPointF(cx, cy + s * 0.15), r, r);
        p.setPen(Qt::NoPen);
        p.setBrush(m_color);
        // Viewfinder bump on top
        qreal vw = s * 0.6;
        qreal vh = s * 0.3;
        p.drawRect(QRectF(cx - vw / 2, cy - bh / 2 + s * 0.15 - vh, vw, vh));
    }

    // Outline pen for the stroked icons (braces, arrows, waves, wrench).
    QPen strokePen(qreal width = 1.5) const
    {
        return QPen(m_color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    }

    // Premiere's marker: a filled pentagon pointing down.
    void drawAddMarker(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath m;
        m.moveTo(cx - s * 0.7, cy - s * 0.85);
        m.lineTo(cx + s * 0.7, cy - s * 0.85);
        m.lineTo(cx + s * 0.7, cy + s * 0.3);
        m.lineTo(cx,           cy + s * 0.9);
        m.lineTo(cx - s * 0.7, cy + s * 0.3);
        m.closeSubpath();
        p.drawPath(m);
    }

    // "{" (dir +1, Mark In) or "}" (dir -1, Mark Out) centred on x.
    void drawBrace(QPainter& p, qreal x, qreal cy, qreal s, int dir)
    {
        const qreal w = s * 0.45 * dir;
        QPainterPath b;
        b.moveTo(x + w, cy - s);
        b.quadTo(x, cy - s, x, cy - s * 0.6);
        b.lineTo(x, cy - s * 0.25);
        b.quadTo(x, cy, x - w, cy);
        b.quadTo(x, cy, x, cy + s * 0.25);
        b.lineTo(x, cy + s * 0.6);
        b.quadTo(x, cy + s, x + w, cy + s);
        p.save();
        p.setPen(strokePen());
        p.setBrush(Qt::NoBrush);
        p.drawPath(b);
        p.restore();
    }

    // Horizontal arrow from x0 to x1; the head sits at x1.
    void drawArrow(QPainter& p, qreal x0, qreal x1, qreal cy, qreal s)
    {
        const qreal dir = (x1 > x0) ? 1.0 : -1.0;
        p.save();
        p.setPen(strokePen());
        p.drawLine(QPointF(x0, cy), QPointF(x1 - dir * s * 0.3, cy));
        p.restore();
        QPainterPath head;
        head.moveTo(x1, cy);
        head.lineTo(x1 - dir * s * 0.6, cy - s * 0.5);
        head.lineTo(x1 - dir * s * 0.6, cy + s * 0.5);
        head.closeSubpath();
        p.drawPath(head);
    }

    // {← — Premiere's Go to In
    void drawGoToIn(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        drawBrace(p, cx - s * 0.75, cy, s, +1);
        drawArrow(p, cx + s * 1.05, cx - s * 0.2, cy, s);
    }

    // →} — Premiere's Go to Out
    void drawGoToOut(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        drawArrow(p, cx - s * 1.05, cx + s * 0.2, cy, s);
        drawBrace(p, cx + s * 0.75, cy, s, -1);
    }

    // A clip lifted up out of the track. Lift leaves the gap in the track;
    // Extract closes it.
    void drawLiftExtract(QPainter& p, qreal cx, qreal cy, qreal s, bool gap)
    {
        const qreal y = cy + s * 0.45, h = s * 0.5;
        if (gap) {
            p.drawRect(QRectF(cx - s * 1.1, y, s * 0.65, h));
            p.drawRect(QRectF(cx + s * 0.45, y, s * 0.65, h));
        } else {
            p.drawRect(QRectF(cx - s * 1.1, y, s * 1.05, h));
            p.drawRect(QRectF(cx + s * 0.05, y, s * 1.05, h));
        }
        p.save();
        p.setPen(strokePen());
        p.drawLine(QPointF(cx, cy + s * 0.2), QPointF(cx, cy - s * 0.5));
        p.restore();
        QPainterPath head;
        head.moveTo(cx, cy - s);
        head.lineTo(cx - s * 0.5, cy - s * 0.4);
        head.lineTo(cx + s * 0.5, cy - s * 0.4);
        head.closeSubpath();
        p.drawPath(head);
    }

    // Speaker with sound waves; crossed out while checked (muted).
    void drawSpeaker(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        QPainterPath body;
        body.moveTo(cx - s,        cy - s * 0.35);
        body.lineTo(cx - s * 0.5,  cy - s * 0.35);
        body.lineTo(cx + s * 0.05, cy - s * 0.85);
        body.lineTo(cx + s * 0.05, cy + s * 0.85);
        body.lineTo(cx - s * 0.5,  cy + s * 0.35);
        body.lineTo(cx - s,        cy + s * 0.35);
        body.closeSubpath();
        p.drawPath(body);
        p.save();
        p.setPen(strokePen(1.3));
        p.setBrush(Qt::NoBrush);
        if (isCheckable() && isChecked()) {
            p.drawLine(QPointF(cx + s * 0.4, cy - s * 0.4), QPointF(cx + s * 1.1, cy + s * 0.4));
            p.drawLine(QPointF(cx + s * 0.4, cy + s * 0.4), QPointF(cx + s * 1.1, cy - s * 0.4));
        } else {
            p.drawArc(QRectF(cx - s * 0.2, cy - s * 0.45, s * 0.8, s * 0.9), -50 * 16, 100 * 16);
            p.drawArc(QRectF(cx - s * 0.35, cy - s * 0.85, s * 1.4, s * 1.7), -50 * 16, 100 * 16);
        }
        p.restore();
    }

    // Premiere's Settings wrench: open jaw at the top right, handle down-left.
    void drawWrench(QPainter& p, qreal cx, qreal cy, qreal s)
    {
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(strokePen(2.2));
        p.drawLine(QPointF(cx - s * 0.8, cy + s * 0.8), QPointF(cx + s * 0.1, cy - s * 0.1));
        p.setPen(strokePen(1.8));
        p.drawArc(QRectF(cx - s * 0.05, cy - s * 0.95, s * 0.9, s * 0.9), 90 * 16, 270 * 16);
        p.restore();
    }

    Icon   m_icon{Play};
    QColor m_color;   ///< Icon colour of the paint in progress
};

} // namespace rt
