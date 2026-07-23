/*
 * ToolButton.h — Custom-painted timeline tool button (Premiere Pro style).
 *
 * Renders editing-tool icons using QPainter geometric shapes, matching
 * the look of Adobe Premiere Pro's tool column icons.
 *
 * Usage:
 *   auto* btn = new ToolButton(ToolButton::Selection, parent);
 *   btn->setFixedSize(36, 32);
 */

#pragma once

#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

namespace rt {

class ToolButton : public QToolButton
{
public:
    enum Tool { Selection, Razor, Ripple, Rolling, Slip, Slide, Text, Zoom, PenMask };

    explicit ToolButton(Tool tool, QWidget* parent = nullptr)
        : QToolButton(parent), m_tool(tool)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setCheckable(true);
        setAutoExclusive(true);
        setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; border: none; }"));
    }

    void setTool(Tool tool) { m_tool = tool; update(); }
    [[nodiscard]] Tool tool() const { return m_tool; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const auto& tc = Theme::colors();

        // Checked / hover / pressed background
        if (isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.accent);
            p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
        } else if (isDown()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.controlBgActive);
            p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
        } else if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(tc.surface3);
            p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
        }

        // Icon color
        QColor color = isChecked() ? tc.textBright
                     : underMouse() ? tc.textPrimary
                     : tc.textSecondary;

        qreal cx = width() * 0.5;
        qreal cy = height() * 0.5;
        qreal s  = qMin(width(), height()) * 0.32;

        switch (m_tool) {
        case Selection: drawSelection(p, cx, cy, s, color); break;
        case Razor:     drawRazor(p, cx, cy, s, color);     break;
        case Ripple:    drawRipple(p, cx, cy, s, color);    break;
        case Rolling:   drawRolling(p, cx, cy, s, color);   break;
        case Slip:      drawSlip(p, cx, cy, s, color);      break;
        case Slide:     drawSlide(p, cx, cy, s, color);     break;
        case Text:      drawText(p, cx, cy, s, color);      break;
        case Zoom:      drawZoom(p, cx, cy, s, color);      break;
        case PenMask:   drawPenMask(p, cx, cy, s, color);   break;
        }
    }

private:
    // ── Selection tool: Arrow cursor pointing up-left ────────────────
    void drawSelection(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        // Arrow cursor shape (Premiere-style pointer)
        QPainterPath arrow;
        qreal ax = cx - s * 0.5;
        qreal ay = cy - s * 1.1;
        arrow.moveTo(ax, ay);                           // top point
        arrow.lineTo(ax, ay + s * 2.2);                 // down left
        arrow.lineTo(ax + s * 0.7, ay + s * 1.6);       // notch right
        arrow.lineTo(ax + s * 1.2, ay + s * 2.2);       // handle bottom
        arrow.lineTo(ax + s * 1.5, ay + s * 2.0);       // handle right
        arrow.lineTo(ax + s * 1.0, ay + s * 1.4);       // handle top
        arrow.lineTo(ax + s * 1.6, ay + s * 1.4);       // notch far right
        arrow.closeSubpath();
        p.drawPath(arrow);
    }

    // ── Razor tool: Blade / cutting tool ────────────────────────────
    void drawRazor(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.8));
        p.setBrush(Qt::NoBrush);

        // Blade shape — tilted rectangle with pointed bottom
        QPainterPath blade;
        qreal bx = cx;
        qreal by = cy - s * 0.9;
        blade.moveTo(bx - s * 0.35, by);
        blade.lineTo(bx + s * 0.35, by);
        blade.lineTo(bx + s * 0.25, by + s * 1.5);
        blade.lineTo(bx, by + s * 2.0);   // point
        blade.lineTo(bx - s * 0.25, by + s * 1.5);
        blade.closeSubpath();
        p.setBrush(color);
        p.drawPath(blade);

        // Handle line
        p.setPen(QPen(color, 1.5));
        p.drawLine(QPointF(bx - s * 0.2, by + s * 0.4),
                   QPointF(bx + s * 0.2, by + s * 0.4));
    }

    // ── Ripple Edit: Two arrows pointing inward with gap ────────────
    void drawRipple(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.8));
        p.setBrush(Qt::NoBrush);

        qreal gap = s * 0.15;

        // Left block
        p.drawRect(QRectF(cx - s * 1.3, cy - s * 0.7, s * 1.0, s * 1.4));
        // Right block
        p.drawRect(QRectF(cx + s * 0.3, cy - s * 0.7, s * 1.0, s * 1.4));

        // Center divider
        p.setPen(QPen(color, 2.0));
        p.drawLine(QPointF(cx, cy - s * 0.8), QPointF(cx, cy + s * 0.8));

        // Left arrow pointing right (toward center)
        p.setBrush(color);
        QPainterPath la;
        la.moveTo(cx - s * 0.5, cy - s * 0.25);
        la.lineTo(cx - gap,     cy);
        la.lineTo(cx - s * 0.5, cy + s * 0.25);
        la.closeSubpath();
        p.drawPath(la);

        // Right arrow pointing left (toward center)
        QPainterPath ra;
        ra.moveTo(cx + s * 0.5, cy - s * 0.25);
        ra.lineTo(cx + gap,     cy);
        ra.lineTo(cx + s * 0.5, cy + s * 0.25);
        ra.closeSubpath();
        p.drawPath(ra);
    }

    // ── Rolling Edit: Two blocks meeting with double arrows ─────────
    void drawRolling(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.8));
        p.setBrush(Qt::NoBrush);

        // Left block
        p.drawRect(QRectF(cx - s * 1.3, cy - s * 0.7, s * 1.15, s * 1.4));
        // Right block
        p.drawRect(QRectF(cx + s * 0.15, cy - s * 0.7, s * 1.15, s * 1.4));

        // Center bracket/bar where they meet
        p.setPen(QPen(color, 2.5));
        p.drawLine(QPointF(cx, cy - s * 0.9), QPointF(cx, cy + s * 0.9));

        // Double-headed horizontal arrow across the edit point
        p.setPen(QPen(color, 1.5));
        p.drawLine(QPointF(cx - s * 0.8, cy), QPointF(cx + s * 0.8, cy));
        // Left arrowhead
        p.drawLine(QPointF(cx - s * 0.8, cy), QPointF(cx - s * 0.5, cy - s * 0.2));
        p.drawLine(QPointF(cx - s * 0.8, cy), QPointF(cx - s * 0.5, cy + s * 0.2));
        // Right arrowhead
        p.drawLine(QPointF(cx + s * 0.8, cy), QPointF(cx + s * 0.5, cy - s * 0.2));
        p.drawLine(QPointF(cx + s * 0.8, cy), QPointF(cx + s * 0.5, cy + s * 0.2));
    }

    // ── Slip tool: Film strip with left-right arrows ────────────────
    void drawSlip(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);

        // Film frame rectangle
        QRectF frame(cx - s * 0.8, cy - s * 0.6, s * 1.6, s * 1.2);
        p.drawRect(frame);

        // Sprocket holes (top and bottom)
        p.setBrush(color);
        for (int i = 0; i < 3; ++i) {
            qreal hx = cx - s * 0.5 + i * s * 0.5;
            p.drawRect(QRectF(hx, cy - s * 0.6, s * 0.2, s * 0.15));
            p.drawRect(QRectF(hx, cy + s * 0.45, s * 0.2, s * 0.15));
        }
        p.setBrush(Qt::NoBrush);

        // Horizontal arrow below (content sliding inside)
        qreal ay = cy + s * 1.1;
        p.drawLine(QPointF(cx - s * 0.7, ay), QPointF(cx + s * 0.7, ay));
        // Left arrowhead
        p.drawLine(QPointF(cx - s * 0.7, ay), QPointF(cx - s * 0.4, ay - s * 0.2));
        p.drawLine(QPointF(cx - s * 0.7, ay), QPointF(cx - s * 0.4, ay + s * 0.2));
        // Right arrowhead
        p.drawLine(QPointF(cx + s * 0.7, ay), QPointF(cx + s * 0.4, ay - s * 0.2));
        p.drawLine(QPointF(cx + s * 0.7, ay), QPointF(cx + s * 0.4, ay + s * 0.2));
    }

    // ── Slide tool: Center block with outer arrows ──────────────────
    void drawSlide(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);

        // Center block (the clip being slid)
        QRectF center(cx - s * 0.45, cy - s * 0.6, s * 0.9, s * 1.2);
        p.drawRect(center);

        // Left neighbor (faded)
        p.setPen(QPen(color.darker(140), 1.2));
        p.drawRect(QRectF(cx - s * 1.3, cy - s * 0.6, s * 0.7, s * 1.2));

        // Right neighbor (faded)
        p.drawRect(QRectF(cx + s * 0.6, cy - s * 0.6, s * 0.7, s * 1.2));

        // Arrows pointing outward from center block
        p.setPen(QPen(color, 1.5));
        // Left arrow
        qreal al = cx - s * 0.55;
        p.drawLine(QPointF(al, cy), QPointF(al - s * 0.6, cy));
        p.drawLine(QPointF(al - s * 0.6, cy), QPointF(al - s * 0.35, cy - s * 0.2));
        p.drawLine(QPointF(al - s * 0.6, cy), QPointF(al - s * 0.35, cy + s * 0.2));
        // Right arrow
        qreal ar = cx + s * 0.55;
        p.drawLine(QPointF(ar, cy), QPointF(ar + s * 0.6, cy));
        p.drawLine(QPointF(ar + s * 0.6, cy), QPointF(ar + s * 0.35, cy - s * 0.2));
        p.drawLine(QPointF(ar + s * 0.6, cy), QPointF(ar + s * 0.35, cy + s * 0.2));
    }

    // ── Text tool: Large "T" icon ─────────────────────────────────
    void drawText(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 2.2));
        p.setBrush(Qt::NoBrush);

        // Horizontal bar of T
        qreal barY = cy - s * 0.8;
        p.drawLine(QPointF(cx - s * 0.9, barY), QPointF(cx + s * 0.9, barY));

        // Serifs at top ends
        p.drawLine(QPointF(cx - s * 0.9, barY - s * 0.15),
                   QPointF(cx - s * 0.9, barY + s * 0.15));
        p.drawLine(QPointF(cx + s * 0.9, barY - s * 0.15),
                   QPointF(cx + s * 0.9, barY + s * 0.15));

        // Vertical stem
        p.drawLine(QPointF(cx, barY), QPointF(cx, cy + s * 1.0));

        // Serif at bottom
        p.drawLine(QPointF(cx - s * 0.35, cy + s * 1.0),
                   QPointF(cx + s * 0.35, cy + s * 1.0));
    }

    // ── Zoom tool: Magnifying glass icon ──────────────────────────────
    void drawZoom(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.8));
        p.setBrush(Qt::NoBrush);

        // Circle (lens)
        qreal r = s * 0.7;
        p.drawEllipse(QPointF(cx - s * 0.15, cy - s * 0.2), r, r);

        // Handle (bottom-right diagonal)
        qreal hx = cx - s * 0.15 + r * 0.707;
        qreal hy = cy - s * 0.2  + r * 0.707;
        p.setPen(QPen(color, 2.5));
        p.drawLine(QPointF(hx, hy), QPointF(hx + s * 0.6, hy + s * 0.6));

        // Plus sign inside lens
        p.setPen(QPen(color, 1.5));
        qreal pcx = cx - s * 0.15;
        qreal pcy = cy - s * 0.2;
        p.drawLine(QPointF(pcx - s * 0.3, pcy), QPointF(pcx + s * 0.3, pcy));
        p.drawLine(QPointF(pcx, pcy - s * 0.3), QPointF(pcx, pcy + s * 0.3));
    }

    // Pen Mask tool: nib plus a small Bezier path/anchor cue.
    void drawPenMask(QPainter& p, qreal cx, qreal cy, qreal s, const QColor& color)
    {
        p.setPen(QPen(color, 1.7));
        p.setBrush(Qt::NoBrush);

        QPainterPath nib;
        nib.moveTo(cx - s * 0.85, cy + s * 0.85);
        nib.lineTo(cx - s * 0.35, cy - s * 0.55);
        nib.lineTo(cx + s * 0.75, cy - s * 1.0);
        nib.lineTo(cx + s * 1.0,  cy - s * 0.75);
        nib.lineTo(cx + s * 0.55, cy + s * 0.35);
        nib.closeSubpath();
        p.drawPath(nib);
        p.drawLine(QPointF(cx - s * 0.85, cy + s * 0.85),
                   QPointF(cx + s * 0.06, cy - s * 0.08));

        p.setBrush(color);
        p.drawEllipse(QPointF(cx - s * 0.85, cy + s * 0.85), s * 0.13, s * 0.13);
    }

    Tool m_tool;
};

} // namespace rt
