/*
 * ThemePresets.cpp u2014 Theme preset definitions (extracted from Theme.cpp).
 */

#include "Theme.h"

namespace rt {

extern ThemeColors     s_colors;
extern ThemeTypography s_typography;
extern ThemeMetrics    s_metrics;
void Theme::applyDarkPreset()
{
    auto& c = s_colors;
    auto& m = s_metrics;
    auto& t = s_typography;

    // Ã¢â€â‚¬Ã¢â€â‚¬ Surfaces: warm neutral dark grays (Premiere Pro reference) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    c.surface0 = QColor(23, 23, 23);
    c.surface1 = QColor(30, 30, 30);
    c.surface2 = QColor(37, 37, 37);
    c.surface3 = QColor(46, 46, 46);
    c.surface4 = QColor(56, 56, 56);

    c.window        = c.surface2;
    c.base          = c.surface0;
    c.panel         = c.surface1;
    c.alternateBase = QColor(33, 33, 33);
    c.toolTipBase   = c.surface4;

    c.textPrimary   = QColor(210, 210, 210);
    c.textSecondary = QColor(153, 153, 153);
    c.textTertiary  = QColor(118, 118, 118);
    c.textDisabled  = QColor(68, 68, 68);
    c.text          = c.textPrimary;
    c.textDim       = c.textSecondary;
    c.textBright    = QColor(240, 240, 240);
    c.toolTipText   = c.textPrimary;

    c.controlBg         = QColor(42, 42, 42);
    c.controlBgHover    = QColor(52, 52, 52);
    c.controlBgActive   = QColor(36, 36, 36);
    c.controlBorder     = QColor(56, 56, 56);
    c.controlBorderFocus= QColor(41, 121, 218);
    c.button       = c.controlBg;
    c.buttonText   = c.textPrimary;
    c.buttonHover  = c.controlBgHover;
    c.buttonPressed= c.controlBgActive;

    c.inputBg           = QColor(19, 19, 19);
    c.inputBorder       = QColor(52, 52, 52);
    c.inputBorderFocus  = QColor(41, 121, 218);
    c.inputSelection    = QColor(48, 78, 118);

    c.accent        = QColor(41, 121, 218);
    c.accentHover   = QColor(58, 140, 235);
    c.accentDim     = QColor(28, 82, 152);
    c.accentSubtle  = QColor(22, 42, 68);
    c.highlight     = c.accent;
    c.highlightedText = QColor(255, 255, 255);

    c.panelHeaderBg  = QColor(37, 37, 37);
    c.panelContentBg = c.surface1;
    c.panelBorder    = QColor(50, 50, 50);

    c.playhead    = QColor(41, 121, 218);
    c.trackBg     = QColor(25, 25, 25);
    c.trackBgAlt  = QColor(28, 28, 28);
    c.trackDivider= QColor(44, 44, 44);
    c.clipVideo   = QColor(52, 118, 200);
    c.clipImage   = QColor(150, 90, 200);   // purple — matches Project Bin image color
    c.clipCharacter = QColor(220, 130, 50); // orange — for video characters (e.g. Wells)
    c.clipAudio   = QColor(58, 162, 88);
    c.clipTitle   = QColor(172, 86, 205);
    c.clipSpine   = QColor(192, 136, 48);
    c.clipSpineCached = QColor(72, 165, 98);
    c.clipGraphic = QColor(64, 182, 250);
    c.clipSelected= QColor(82, 162, 240);
    c.clipHover   = QColor(62, 62, 68);
    c.transition  = QColor(96, 78, 155);

    c.error     = QColor(200, 55, 55);
    c.errorBg   = QColor(58, 24, 24);
    c.warning   = QColor(215, 175, 38);
    c.warningBg = QColor(52, 42, 18);
    c.success   = QColor(55, 175, 85);
    c.successBg = QColor(20, 48, 28);

    c.primaryBtnBg    = QColor(32, 96, 172);
    c.primaryBtnHover = QColor(42, 112, 192);
    c.successBtnBg    = QColor(38, 118, 56);
    c.successBtnHover = QColor(48, 138, 70);
    c.dangerBg        = QColor(72, 34, 34);
    c.dangerBgHover   = QColor(90, 42, 42);
    c.dangerText      = QColor(228, 138, 138);

    c.border      = QColor(46, 46, 46);
    c.borderLight = QColor(60, 60, 60);
    c.separator   = QColor(40, 40, 40);

    c.dockTitleBg     = QColor(37, 37, 37);
    c.dockTitleText   = QColor(180, 180, 180);
    c.dockTitleActive = c.accent;

    c.waveformBg  = c.surface0;
    c.waveformFg  = QColor(62, 116, 172);
    c.waveformGrid= QColor(38, 38, 38);
    c.waveformSelection = QColor(255, 200, 50);

    c.scrollbarTrack     = QColor(20, 20, 20);
    c.scrollbarThumb     = QColor(50, 50, 50);
    c.scrollbarThumbHover= QColor(64, 64, 64);

    m.radiusSm  = 2;
    m.radiusMd  = 3;
    m.radiusLg  = 5;
    m.radiusXl  = 8;

    m.spacingXs  = 3;
    m.spacingSm  = 4;
    m.spacingMd  = 6;
    m.spacingLg  = 8;
    m.spacingXl  = 12;
    m.spacingXxl = 18;

    // Premiere-style compact chrome shared by headers and standard controls.
    m.controlHeight      = 28;
    m.controlHeightSm    = 24;
    m.controlHeightLg    = 32;
    m.panelHeaderHeight  = 28;
    m.sectionHeaderHeight= 26;

    t.sizeBody    = 13;
    t.sizeCaption = 12;
    t.sizeH1      = 17;
    t.sizeH2      = 14;
    t.sizeSmall   = 11;
    t.sizeMono    = 13;
}

} // namespace rt
