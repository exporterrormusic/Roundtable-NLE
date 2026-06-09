/*
 * TimelineClipWidget.cpp — Clip visual style and painting.
 * Step 12: Timeline Panel — Core UI
 */

#include "widgets/TimelineClipWidget.h"
#include "Theme.h"

#include "timeline/Clip.h"

#include <QPainter>
#include <QPen>

namespace rt {

ClipVisualStyle TimelineClipWidget::defaultStyle(ClipType type)
{
    ClipVisualStyle style;

    const auto& tc = Theme::colors();
    style.selectedBorderColor = tc.clipSelected;
    style.textColor = tc.textBright;

    switch (type)
    {
        case ClipType::Spine:
            style.fillColor   = tc.clipSpine;
            style.borderColor = tc.clipSpine.lighter(130);
            break;
        case ClipType::Video:
            style.fillColor   = tc.clipVideo;
            style.borderColor = tc.clipVideo.lighter(130);
            break;
        case ClipType::Audio:
            style.fillColor   = tc.clipAudio;
            style.borderColor = tc.clipAudio.lighter(130);
            break;
        case ClipType::Title:
            style.fillColor   = tc.clipTitle;
            style.borderColor = tc.clipTitle.lighter(130);
            break;
        case ClipType::Adjustment:
            style.fillColor   = tc.surface3;
            style.borderColor = tc.surface4;
            break;
        case ClipType::Image:
            style.fillColor   = tc.warning;
            style.borderColor = tc.warning.lighter(130);
            break;
        case ClipType::Graphic:
            style.fillColor   = tc.clipGraphic;
            style.borderColor = tc.clipGraphic.lighter(130);
            break;
        case ClipType::Sequence:
            style.fillColor   = QColor(123, 104, 238); // mediumslateblue
            style.borderColor = QColor(123, 104, 238).lighter(130);
            break;
        case ClipType::Caption:
            // Teal caption clip with a lighter border (matches other clip
            // types) so the gaps/borders between cues read clearly instead
            // of the default-constructed near-black.
            style.fillColor   = QColor(0x2E, 0x8B, 0x80); // muted teal
            style.borderColor = QColor(0x2E, 0x8B, 0x80).lighter(140);
            break;
        case ClipType::PngPuppet:
            style.fillColor   = QColor(0xC0, 0x5A, 0x8C); // pink puppet
            style.borderColor = QColor(0xC0, 0x5A, 0x8C).lighter(140);
            break;
    }

    return style;
}

QString TimelineClipWidget::typeName(ClipType type)
{
    switch (type)
    {
        case ClipType::Spine:      return QStringLiteral("Spine");
        case ClipType::Video:      return QStringLiteral("Video");
        case ClipType::Audio:      return QStringLiteral("Audio");
        case ClipType::Title:      return QStringLiteral("Title");
        case ClipType::Adjustment: return QStringLiteral("Adjust");
        case ClipType::Image:      return QStringLiteral("Image");
        case ClipType::Graphic:    return QStringLiteral("Graphic");
        case ClipType::Sequence:   return QStringLiteral("Sequence");
        case ClipType::Caption:    return QStringLiteral("Caption");
        case ClipType::PngPuppet:  return QStringLiteral("Puppet");
    }
    return QStringLiteral("Unknown");
}

QChar TimelineClipWidget::typeIcon(ClipType type)
{
    switch (type)
    {
        case ClipType::Spine:      return QChar(0x2666); // ♦
        case ClipType::Video:      return QChar(0x25B6); // ▶
        case ClipType::Audio:      return QChar(0x266B); // ♫
        case ClipType::Title:      return QChar(0x0054); // T
        case ClipType::Adjustment: return QChar(0x2699); // ⚙
        case ClipType::Image:      return QChar(0x25A3); // ▣
        case ClipType::Graphic:    return QChar(0x0047); // G
        case ClipType::Sequence:   return QChar(0x229E); // ⊞ nested
        case ClipType::Caption:    return QChar(0x0043); // C (caption)
        case ClipType::PngPuppet:  return QChar(0x263A); // ☺ puppet face
    }
    return QChar(0x25CF); // ●
}

QString TimelineClipWidget::displayLabel(ClipType type, const QString& label)
{
    // Caption clips show only their text — no leading type-icon letter (which
    // looked like a stray initial in front of each transcript line).
    if (type == ClipType::Caption)
        return label;
    QChar icon = typeIcon(type);
    if (label.isEmpty())
        return QString(icon);
    return QString("%1 %2").arg(icon).arg(label);
}

void TimelineClipWidget::paint(QPainter& painter,
                                const QRectF& rectIn,
                                const ClipVisualStyle& style,
                                const QString& label,
                                bool selected,
                                bool enabled)
{
    if (rectIn.width() < 1.0) return;

    // Qt's raster paint engine silently drops primitives whose coordinates
    // exceed its internal fixed-point range (~±32767). At deep zoom a clip can
    // be tens of thousands of pixels wide with one edge far outside the
    // viewport, which made the whole clip body vanish from the timeline even
    // though it still rendered in the program monitor. Clamp the off-screen
    // edges to a safe window — the painter already clips to the widget, so any
    // coordinate well beyond the viewport is invisible and clamping is exact.
    constexpr qreal kSafeCoord = 16384.0;
    QRectF rect = rectIn;
    if (rect.left()  < -kSafeCoord) rect.setLeft(-kSafeCoord);
    if (rect.right() >  kSafeCoord) rect.setRight(kSafeCoord);
    if (rect.width() < 1.0) return;

    painter.save();

    // Fill
    QColor fill = style.fillColor;
    if (!enabled)
        fill = fill.darker(150);

    // Premiere Pro-style selection: brighten fill + thick glowing border
    if (selected)
    {
        QColor selFill = fill.lighter(135);
        painter.setBrush(selFill);

        // Outer glow (semi-transparent, wider stroke)
        QColor glowColor = style.selectedBorderColor;
        glowColor.setAlpha(90);
        painter.setPen(QPen(glowColor, style.selectedBorderWidth + 3.0f));
        painter.drawRoundedRect(rect.adjusted(-1, -1, 1, 1),
                                style.borderRadius + 1, style.borderRadius + 1);

        // Inner solid border
        painter.setPen(QPen(style.selectedBorderColor, style.selectedBorderWidth + 0.5f));
    }
    else
    {
        painter.setBrush(fill);
        painter.setPen(QPen(style.borderColor, style.borderWidth));
    }

    painter.drawRoundedRect(rect, style.borderRadius, style.borderRadius);

    // Selected highlight bar at top (Premiere Pro white accent strip)
    if (selected)
    {
        QColor accent = style.selectedBorderColor;
        accent.setAlpha(200);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        QRectF strip(rect.left() + 2, rect.top() + 1,
                     rect.width() - 4, 3.0);
        painter.drawRoundedRect(strip, 1.5, 1.5);
    }

    // Label
    if (rect.width() >= style.minWidthForLabel && !label.isEmpty())
    {
        painter.setPen(enabled ? style.textColor : style.textColor.darker(130));
        static const QFont clipLabelFont("Segoe UI", 8);
        painter.setFont(clipLabelFont);

        QRectF textRect = rect.adjusted(style.labelPadding, 2, -style.labelPadding, -2);
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);
    }

    // Trim handles (thin lines at edges)
    if (rect.width() >= 8.0)
    {
        painter.setPen(QPen(QColor(Theme::colors().textBright.red(), Theme::colors().textBright.green(),
                                      Theme::colors().textBright.blue(), 40), 1.0));
        painter.drawLine(QPointF(rect.left() + 2, rect.top() + 4),
                         QPointF(rect.left() + 2, rect.bottom() - 4));
        painter.drawLine(QPointF(rect.right() - 2, rect.top() + 4),
                         QPointF(rect.right() - 2, rect.bottom() - 4));
    }

    painter.restore();
}
} // namespace rt