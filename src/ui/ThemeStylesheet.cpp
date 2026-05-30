/*
 * ThemeStylesheet.cpp -- QSS stylesheet builder (base widgets).
 * Button/list controls -> ThemeStylesheetControls.cpp;
 * scroll/table views   -> ThemeStylesheetViews.cpp.
 */

#include "Theme.h"

namespace rt {

extern ThemeColors     s_colors;
extern ThemeTypography s_typography;
extern ThemeMetrics    s_metrics;

// Stylesheet parts defined in sibling files:
QString themeStyleControls();
QString themeStyleViews();

QString themeStyleBase()
{
    const auto& c = s_colors;
    const auto& t = s_typography;
    const auto& m = s_metrics;
    const auto rgb = [](const QColor& col) { return Theme::rgb(col); };
    QString qss;
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  BASE WIDGET DEFAULTS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(

/* â•â•â• Universal Font â•â•â• */
* {
    font-family: "%1";
    font-size: %2px;
}

)").arg(t.fontFamily).arg(t.sizeBody);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  DOCK WIDGETS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QDockWidget {
    font-size: %1px;
    font-weight: bold;
    titlebar-close-icon: none;
    titlebar-normal-icon: none;
    margin: 3px;
    padding: 4px;
    border: 2px solid %4;
}
QDockWidget::title {
    background: %2;
    color: %3;
    padding: 0px;
    margin: 0px;
    border: none;
    height: 0px;
    min-height: 0px;
    max-height: 0px;
}
QDockWidget::close-button, QDockWidget::float-button {
    background: transparent;
    border: none;
    padding: 0px;
    width: 0px;
    height: 0px;
    icon-size: 0px;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover {
    background: transparent;
}

)").arg(t.sizeCaption)
   .arg(rgb(c.dockTitleBg))
   .arg(rgb(c.dockTitleText))
   .arg(rgb(c.border));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  MENU BAR & MENUS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QMenuBar {
    background: %1;
    border-bottom: 1px solid %2;
    spacing: 0px;
}
QMenuBar::item {
    padding: 5px 10px;
    background: transparent;
    color: %3;
}
QMenuBar::item:selected {
    background: %4;
    border-radius: 3px;
}
QMenuBar::item:pressed {
    background: %5;
    color: %6;
}
QMenu {
    background: %7;
    border: 1px solid %8;
    border-radius: 4px;
    padding: 4px 0px;
}
QMenu::item {
    padding: 6px 30px 6px 20px;
    color: %3;
}
QMenu::item:selected {
    background: %5;
    color: %6;
    border-radius: 3px;
}
QMenu::item:disabled {
    color: %9;
}
QMenu::separator {
    height: 1px;
    background: %10;
    margin: 4px 8px;
}
)").arg(rgb(c.surface2))   // 1: menubar bg
   .arg(rgb(c.border))     // 2: menubar border
   .arg(rgb(c.text))       // 3: text
   .arg(rgb(c.controlBgHover)) // 4: item hover
   .arg(rgb(c.accent))     // 5: pressed/selected bg
   .arg(rgb(c.highlightedText)) // 6: selected text
   .arg(rgb(c.surface3))   // 7: menu bg
   .arg(rgb(c.border))     // 8: menu border
   .arg(rgb(c.textTertiary)) // 9: disabled text
   .arg(rgb(c.separator)); // 10: separator

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  TOOLBAR
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QToolBar {
    background: %1;
    border: none;
    spacing: 2px;
    padding: 2px;
}
QToolBar::separator {
    width: 1px;
    background: %2;
    margin: 4px 4px;
}
QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: %3px;
    padding: 4px;
    color: %4;
}
QToolButton:hover {
    background: %5;
    border-color: %6;
}
QToolButton:checked {
    background: %7;
    color: %8;
}
QToolButton:pressed {
    background: %9;
}
)").arg(rgb(c.surface2))
   .arg(rgb(c.separator))
   .arg(m.radiusSm)
   .arg(rgb(c.text))
   .arg(rgb(c.controlBgHover))
   .arg(rgb(c.borderLight))
   .arg(rgb(c.accent))
   .arg(rgb(c.highlightedText))
   .arg(rgb(c.controlBgActive));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SCROLLBARS (thin, Apple-style)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QScrollBar:vertical {
    background: %1;
    width: 16px;
    margin: 0;
    border: none;
}
QScrollBar::handle:vertical {
    background: %2;
    min-height: 40px;
    border-radius: 7px;
    margin: 1px;
}
QScrollBar::handle:vertical:hover {
    background: %3;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    height: 0px;
    background: transparent;
}
QScrollBar:horizontal {
    background: %1;
    height: 16px;
    margin: 0;
    border: none;
}
QScrollBar::handle:horizontal {
    background: %2;
    min-width: 40px;
    border-radius: 7px;
    margin: 1px;
}
QScrollBar::handle:horizontal:hover {
    background: %3;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    width: 0px;
    background: transparent;
}
)").arg(rgb(c.scrollbarTrack))
   .arg(rgb(c.scrollbarThumb))
   .arg(rgb(c.scrollbarThumbHover));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  STATUS BAR
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QStatusBar {
    background: %1;
    border-top: 1px solid %2;
    color: %3;
    font-size: %4px;
}
QStatusBar::item { border: none; }
)").arg(rgb(c.surface2))
   .arg(rgb(c.border))
   .arg(rgb(c.textSecondary))
   .arg(t.sizeCaption);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  TAB BAR (Premiere-style top-accent tabs)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QTabBar {
    qproperty-elideMode: 3;
}
QTabBar::tab {
    background: %1;
    color: %2;
    padding: 6px 14px;
    border-top: 2px solid transparent;
    border-bottom: none;
    border-left: none;
    border-right: none;
    margin-right: 1px;
    font-size: %5px;
}
QTabBar::tab:selected {
    background: %3;
    color: %4;
    border-top-color: %6;
}
QTabBar::tab:hover:!selected {
    background: %7;
    color: %4;
}
QTabWidget::pane {
    border: none;
    background: %3;
}
)").arg(rgb(c.surface2))       // 1: tab bg
   .arg(rgb(c.textSecondary))  // 2: inactive text
   .arg(rgb(c.surface1))       // 3: selected bg
   .arg(rgb(c.textPrimary))    // 4: selected text
   .arg(t.sizeCaption)         // 5: font size
   .arg(rgb(c.accent))         // 6: top accent line
   .arg(rgb(c.controlBgHover)); // 7: hover bg

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SPLITTERS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QSplitter::handle {
    background: %1;
}
QSplitter::handle:horizontal { width: 6px; }
QSplitter::handle:vertical { height: 6px; }
QSplitter::handle:hover {
    background: %2;
}

/* Dock area separators (nested QMainWindow) â€” Premiere Pro style */
QMainWindow::separator {
    background: %1;
    width: 6px;
    height: 6px;
}
QMainWindow::separator:hover {
    background: %2;
}
)").arg(rgb(c.border))
   .arg(rgb(c.accent));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  INPUTS: QLineEdit, QSpinBox, QDoubleSpinBox
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLineEdit, QSpinBox, QDoubleSpinBox {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 4px 8px;
    min-height: %5px;
    selection-background-color: %6;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: %7;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: %8;
    background: %9;
}
QLineEdit[readOnly="true"] {
    background: %10;
    color: %11;
}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    background: transparent;
    border: none;
    width: 16px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-bottom: 5px solid %11;
    width: 0px; height: 0px;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid %11;
    width: 0px; height: 0px;
}
)").arg(rgb(c.inputBg))          // 1
   .arg(rgb(c.textPrimary))      // 2
   .arg(rgb(c.inputBorder))      // 3
   .arg(m.radiusMd)              // 4
   .arg(m.controlHeightSm - 8)   // 5
   .arg(rgb(c.inputSelection))   // 6
   .arg(rgb(c.inputBorderFocus)) // 7
   .arg(rgb(c.textDisabled))     // 8
   .arg(rgb(c.surface0))         // 9
   .arg(rgb(c.surface1))         // 10
   .arg(rgb(c.textSecondary));   // 11

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  COMBOBOX
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QComboBox {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 0px 24px 0px 10px;
    min-height: %5px;
    font-size: 15px;
}
QComboBox:hover {
    border-color: %6;
}
QComboBox:focus {
    border-color: %7;
}
QComboBox::drop-down {
    border: none;
    width: 28px;
    subcontrol-position: right center;
}
QComboBox::down-arrow {
    image: none;
    border-left: 6px solid transparent;
    border-right: 6px solid transparent;
    border-top: 6px solid %8;
    width: 0px; height: 0px;
}
QComboBox QAbstractItemView {
    background: %9;
    color: %2;
    border: 1px solid %3;
    border-radius: 4px;
    selection-background-color: %10;
    selection-color: %11;
    outline: none;
    padding: 4px;
}
QComboBox QAbstractItemView::item {
    padding: 6px 10px;
    min-height: 28px;
    border-radius: 3px;
}
QComboBox QAbstractItemView::item:hover {
    background: %12;
}
)").arg(rgb(c.inputBg))          // 1: bg
   .arg(rgb(c.textPrimary))      // 2: text
   .arg(rgb(c.inputBorder))      // 3: border
   .arg(m.radiusMd)              // 4: radius
   .arg(m.controlHeightSm - 8)   // 5: min-height
   .arg(rgb(c.borderLight))      // 6: hover border
   .arg(rgb(c.inputBorderFocus)) // 7: focus border
   .arg(rgb(c.textSecondary))    // 8: arrow color
   .arg(rgb(c.surface3))         // 9: dropdown bg
   .arg(rgb(c.accent))           // 10: selection bg
   .arg(rgb(c.highlightedText))  // 11: selection text
   .arg(rgb(c.controlBgHover));  // 12: item hover

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  PUSH BUTTONS (default style)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QPushButton {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    padding: 5px 14px;
    font-weight: 600;
    min-height: %5px;
}
QPushButton:hover {
    background: %6;
    border-color: %7;
}
QPushButton:pressed {
    background: %8;
}
QPushButton:disabled {
    color: %9;
    background: %10;
    border-color: %11;
}
QPushButton:checked {
    background: %12;
    color: %13;
    border-color: %12;
}
)").arg(rgb(c.controlBg))       // 1: bg
   .arg(rgb(c.textPrimary))     // 2: text
   .arg(rgb(c.controlBorder))   // 3: border
   .arg(m.radiusMd)             // 4: radius
   .arg(m.controlHeightSm - 10) // 5: min-height
   .arg(rgb(c.controlBgHover))  // 6: hover bg
   .arg(rgb(c.borderLight))     // 7: hover border
   .arg(rgb(c.controlBgActive)) // 8: pressed bg
   .arg(rgb(c.textDisabled))    // 9: disabled text
   .arg(rgb(c.surface1))        // 10: disabled bg
   .arg(rgb(c.border))          // 11: disabled border
   .arg(rgb(c.accent))          // 12: checked bg
   .arg(rgb(c.highlightedText)); // 13: checked text

    return qss;
}

QString Theme::stylesheet()
{
    QString qss;
    qss.reserve(12000);
    qss += themeStyleBase();
    qss += themeStyleControls();
    qss += themeStyleViews();
    return qss;
}

} // namespace rt
