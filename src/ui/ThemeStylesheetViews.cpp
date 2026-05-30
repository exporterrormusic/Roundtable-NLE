/*
 * ThemeStylesheetViews.cpp -- QSS for scroll areas, tables, and view widgets.
 * Extracted from ThemeStylesheet.cpp (behavior-preserving).
 */

#include "Theme.h"

namespace rt {

extern ThemeColors     s_colors;
extern ThemeTypography s_typography;
extern ThemeMetrics    s_metrics;

QString themeStyleViews()
{
    const auto& c = s_colors;
    const auto& t = s_typography;
    const auto& m = s_metrics;
    const auto rgb = [](const QColor& col) { return Theme::rgb(col); };
    QString qss;
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SCROLL AREA
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QScrollArea {
    background: transparent;
    border: none;
}
)");

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  TOOLTIP
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QToolTip {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 4px 8px;
    font-size: %5px;
}
)").arg(rgb(c.surface4))
   .arg(rgb(c.textPrimary))
   .arg(rgb(c.borderLight))
   .arg(m.radiusSm)
   .arg(t.sizeCaption);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SEPARATOR FRAME  (set objectName to "rt-separator")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QFrame#rt-separator {
    background: %1;
    max-height: 1px;
    min-height: 1px;
    border: none;
}
)").arg(rgb(c.border));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  AUDIO MIXER COMPONENTS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(

/* Mixer background */
AudioMixer { background: %1; }
QWidget#StripContainer { background: %1; }
QScrollArea#MixerScroll { background: transparent; border: none; }

/* Channel strips */
QWidget#ChannelStrip {
    background: %2;
    border: 1px solid %3;
    border-radius: %4px;
}
QWidget#ChannelStrip:hover {
    border-color: %5;
    background: %6;
}
QWidget#MasterStrip {
    background: %7;
    border: 1px solid %8;
    border-radius: %4px;
}
QWidget#MasterStrip:hover { border-color: %5; }

/* Strip labels */
QLabel#TrackName {
    color: %9;
    font-weight: bold;
    padding: 2px 0;
    border: none;
    background: transparent;
}
QLabel#MasterName {
    color: %10;
    font-weight: bold;
    letter-spacing: 1px;
    padding: 2px 0;
    border: none;
    background: transparent;
}
QLabel#DbLabel, QLabel#PanLabel {
    color: %9;
    font-family: "%11";
    border: none;
    background: transparent;
    padding: 1px 0;
}

/* Mixer divider / separator */
QWidget#StripDivider { background: %6; border: none; }
QWidget#MixerSeparator {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
        stop:0 transparent, stop:0.15 %6, stop:0.5 %12,
        stop:0.85 %6, stop:1 transparent);
    border: none;
}

/* Mute button */
QPushButton#MuteBtn {
    background: %2;
    color: %13;
    border: 1px solid %14;
    border-radius: %15px;
    font-weight: bold;
}
QPushButton#MuteBtn:hover {
    background: %6;
    color: %9;
    border-color: %12;
}
QPushButton#MuteBtn:checked {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
        stop:0 %16, stop:1 %17);
    color: %18;
    border-color: %16;
}

/* Solo button */
QPushButton#SoloBtn {
    background: %2;
    color: %13;
    border: 1px solid %14;
    border-radius: %15px;
    font-weight: bold;
}
QPushButton#SoloBtn:hover {
    background: %6;
    color: %9;
    border-color: %12;
}
QPushButton#SoloBtn:checked {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
        stop:0 %19, stop:1 %20);
    color: %18;
    border-color: %19;
}

)").arg(rgb(c.surface0))        // %1 - mixer bg
   .arg(rgb(c.surface2))        // %2 - strip bg
   .arg(rgb(c.inputBorder))     // %3 - strip border
   .arg(m.radiusMd)             // %4 - radius
   .arg(rgb(c.accent))          // %5 - accent
   .arg(rgb(c.surface3))        // %6 - hover bg
   .arg(rgb(c.surface1))        // %7 - master bg
   .arg(rgb(c.accentDim))       // %8 - master border
   .arg(rgb(c.textSecondary))   // %9 - label color
   .arg(rgb(c.accentHover))     // %10 - master label
   .arg(t.monoFamily)           // %11 - mono font
   .arg(rgb(c.surface4))        // %12 - separator mid
   .arg(rgb(c.textTertiary))    // %13 - mute/solo text
   .arg(rgb(c.borderLight))     // %14 - mute/solo border
   .arg(m.radiusSm)             // %15 - btn radius
   .arg(rgb(c.error))           // %16 - mute checked
   .arg(rgb(c.dangerBg))        // %17 - mute gradient end
   .arg(rgb(c.textBright))      // %18 - checked text
   .arg(rgb(c.warning))         // %19 - solo checked
   .arg(rgb(c.warning.darker(130))); // %20 - solo gradient end

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  PAGE TAB BAR  (objectName "PageTabBar")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(

QTabBar#PageTabBar {
    background: %1;
    border: none;
}
QTabBar#PageTabBar::tab {
    background: %1;
    color: %2;
    border: none;
    border-right: 1px solid %3;
    padding: 12px 24px;
    font-size: %4px;
    font-weight: bold;
    letter-spacing: 2px;
    min-width: 80px;
}
QTabBar#PageTabBar::tab:selected {
    background: %5;
    color: %6;
    border-bottom: 3px solid %7;
}
QTabBar#PageTabBar::tab:hover {
    background: %8;
    color: %9;
}

)").arg(rgb(c.surface0))        // %1 - bg
   .arg(rgb(c.textTertiary))    // %2 - normal text
   .arg(rgb(c.border))          // %3 - border
   .arg(t.sizeBody)             // %4 - font size
   .arg(rgb(c.surface1))        // %5 - selected bg
   .arg(rgb(c.textBright))      // %6 - selected text
   .arg(rgb(c.accent))          // %7 - accent bottom
   .arg(rgb(c.surface2))        // %8 - hover bg
   .arg(rgb(c.textSecondary));  // %9 - hover text

    return qss;
}

} // namespace rt
