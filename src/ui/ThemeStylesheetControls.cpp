/*
 * ThemeStylesheetControls.cpp -- QSS for button variants, lists, and form controls.
 * Extracted from ThemeStylesheet.cpp (behavior-preserving).
 */

#include "Theme.h"

namespace rt {

extern ThemeColors     s_colors;
extern ThemeTypography s_typography;
extern ThemeMetrics    s_metrics;

QString themeStyleControls()
{
    const auto& c = s_colors;
    const auto& t = s_typography;
    const auto& m = s_metrics;
    const auto rgb = [](const QColor& col) { return Theme::rgb(col); };
    const auto rgba = [](const QColor& col, int a) { return Theme::rgba(col, a); };
    QString qss;
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  BUTTON VARIANTS (by objectName)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(

/* â”€â”€ Primary Button (blue accent â€” main actions) â”€â”€ */
QPushButton#rt-btn-primary {
    background: %1;
    color: white;
    border: none;
    font-weight: bold;
}
QPushButton#rt-btn-primary:hover {
    background: %2;
}
QPushButton#rt-btn-primary:pressed {
    background: %3;
}

/* â”€â”€ Success Button (green â€” confirm, export) â”€â”€ */
QPushButton#rt-btn-success {
    background: %4;
    color: white;
    border: none;
    font-weight: bold;
}
QPushButton#rt-btn-success:hover {
    background: %5;
}

/* â”€â”€ Danger Button (red â€” delete, remove) â”€â”€ */
QPushButton#rt-btn-danger {
    background: %6;
    color: %7;
    border: none;
}
QPushButton#rt-btn-danger:hover {
    background: %8;
}

/* â”€â”€ Ghost Button (transparent â€” toolbar-style) â”€â”€ */
QPushButton#rt-btn-ghost {
    background: transparent;
    border: none;
    color: %9;
}
QPushButton#rt-btn-ghost:hover {
    background: %10;
    color: %11;
}

/* â”€â”€ Subtle Button (very faint bg â€” inline actions) â”€â”€ */
QPushButton#rt-btn-subtle {
    background: %12;
    border: 1px solid %13;
    color: %14;
}
QPushButton#rt-btn-subtle:hover {
    background: %15;
    border-color: %16;
}

)").arg(rgb(c.primaryBtnBg))      // 1
   .arg(rgb(c.primaryBtnHover))   // 2
   .arg(rgb(c.accentDim))         // 3
   .arg(rgb(c.successBtnBg))      // 4
   .arg(rgb(c.successBtnHover))   // 5
   .arg(rgb(c.dangerBg))          // 6
   .arg(rgb(c.dangerText))        // 7
   .arg(rgb(c.dangerBgHover))     // 8
   .arg(rgb(c.textSecondary))     // 9
   .arg(rgba(c.text, 20))         // 10
   .arg(rgb(c.textPrimary))       // 11
   .arg(rgb(c.surface2))          // 12
   .arg(rgb(c.border))            // 13
   .arg(rgb(c.textSecondary))     // 14
   .arg(rgb(c.surface3))          // 15
   .arg(rgb(c.borderLight));      // 16

    // â”€â”€ Aliases for panel-specific objectNames â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // Panels use PrimaryBtn/DangerBtn/GhostBtn etc. â€” same styles as rt-*
    qss += QStringLiteral(R"(

/* Panel button aliases (legacy objectNames â†’ same style as rt-btn-*) */
QPushButton#PrimaryBtn, QPushButton#CreateBtn, QPushButton#ExportBtn, QPushButton#AddQueueBtn {
    background: %1; color: white; border: none; font-weight: bold;
}
QPushButton#PrimaryBtn:hover, QPushButton#CreateBtn:hover, QPushButton#ExportBtn:hover, QPushButton#AddQueueBtn:hover {
    background: %2;
}
QPushButton#SaveBtn {
    background: %3; color: white; border: none; font-weight: bold;
}
QPushButton#SaveBtn:hover { background: %4; }
QPushButton#DangerBtn, QPushButton#CancelBtn {
    background: %5; color: %6; border: none;
}
QPushButton#DangerBtn:hover, QPushButton#CancelBtn:hover { background: %7; }
QPushButton#GhostBtn, QPushButton#SecondaryBtn, QPushButton#BrowseBtn, QPushButton#TransportBtn,
QPushButton#ResetViewBtn, QPushButton#LayerToolBtn {
    background: transparent; border: 1px solid %8; color: %9;
}
QPushButton#GhostBtn:hover, QPushButton#SecondaryBtn:hover, QPushButton#BrowseBtn:hover, QPushButton#TransportBtn:hover,
QPushButton#ResetViewBtn:hover, QPushButton#LayerToolBtn:hover {
    background: %10; border-color: %11; color: %12;
}
QPushButton#LayerToolBtnDanger {
    background: transparent; border: 1px solid %8; color: %6;
}
QPushButton#LayerToolBtnDanger:hover { background: %5; }

/* Panel label aliases */
QLabel#PanelTitle {
    font-size: %13px; font-weight: bold; color: %14; padding: 4px 0;
}
QLabel#SectionTitle, QLabel#SectionLabel {
    font-size: %15px; font-weight: 600; color: %12; padding: 2px 0;
}
QLabel#FieldLabel, QLabel#PropLabel, QLabel#ControlLabel, QLabel#DetailFieldLabel {
    color: %9; font-size: %15px;
}
QLabel#StatusLabel, QLabel#EstimateLbl, QLabel#DetailFieldValue {
    color: %9; font-size: %16px;
}
QLabel#EmptyStateLabel, QLabel#EmptyLabel, QLabel#PlaceholderLabel, QLabel#PreviewPlaceholder {
    color: %17; font-size: %16px;
}

/* Panel card containers */
QWidget#LeftCard {
    background: %18; border: 1px solid %8; border-radius: %19px;
}
QWidget#RightPanel, QWidget#DetailsSidebar {
    background: %18; border: 1px solid %8; border-radius: %19px;
}
QWidget#PreviewArea, QWidget#PreviewHeader, QWidget#PreviewToolbar {
    background: %20; border: none;
}
QWidget#ControlsBar, QWidget#TransformTabBg {
    background: %18; border-top: 1px solid %8;
}

/* Panel list widgets */
QListWidget#CharacterList, QListWidget#ProjectList, QListWidget#RecentList,
QListWidget#ShotList, QListWidget#LibraryList, QListWidget#LayerList, QListWidget#JobList {
    background: %20; border: 1px solid %8; border-radius: %19px;
    outline: none;
}

)").arg(rgb(c.primaryBtnBg))      // %1
   .arg(rgb(c.primaryBtnHover))   // %2
   .arg(rgb(c.successBtnBg))      // %3
   .arg(rgb(c.successBtnHover))   // %4
   .arg(rgb(c.dangerBg))          // %5
   .arg(rgb(c.dangerText))        // %6
   .arg(rgb(c.dangerBgHover))     // %7
   .arg(rgb(c.border))            // %8
   .arg(rgb(c.textSecondary))     // %9
   .arg(rgb(c.surface3))          // %10
   .arg(rgb(c.borderLight))       // %11
   .arg(rgb(c.textPrimary))       // %12
   .arg(t.sizeH1)                 // %13
   .arg(rgb(c.accent))            // %14
   .arg(t.sizeH2)                 // %15
   .arg(t.sizeCaption)            // %16
   .arg(rgb(c.textTertiary))      // %17
   .arg(rgb(c.surface2))          // %18
   .arg(m.radiusMd)               // %19
   .arg(rgb(c.surface0));         // %20

    // â”€â”€ Unique panel-specific objectName rules â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
/* Star button (ShotComposer favourites) */
QPushButton#StarBtn {
    font-size: 16px; padding: 0; border-radius: 4px;
    background: %1; color: %2; border: 1px solid %3;
}
QPushButton#StarBtn:hover { background: %3; }
QPushButton#StarBtn:checked { background: %4; border: 2px solid %2; }
QPushButton#StarBtn:disabled { background: %5; color: %6; border-color: %7; }

/* Axis labels */
QLabel#AxisLabel { color: %8; font-weight: bold; font-size: 15px; }

/* Project panel list-item labels */
QLabel#ProjectItemName { color: %9; font-weight: bold; }
QLabel#ProjectItemMeta { color: %8; }
QLabel#CurrentProject {
    color: %10; background: %5; border-radius: 10px; padding: 4px 14px;
}
QLabel#CurrentBadge { color: %11; font-weight: bold; }

/* Preview label (ExportPanel) */
QLabel#PreviewLabel {
    background: %12; border: 1px solid %7; border-radius: 6px; color: %6;
}

/* Accent-tinted combo boxes */
QComboBox#OutfitCombo { color: %2; font-weight: bold; }
QComboBox#StanceCombo { color: %11; font-weight: bold; }
QComboBox#OutfitCombo::down-arrow, QComboBox#StanceCombo::down-arrow,
QComboBox#AnimCombo::down-arrow { border-top: none; border-bottom: 5px solid %8; }

/* Download progress bar */
QProgressBar#DownloadProgress {
    background: %13; border: 1px solid %14; border-radius: 4px;
    text-align: center; color: %9;
}
QProgressBar#DownloadProgress::chunk { background: %15; border-radius: 3px; }

/* Export button gradient override */
QPushButton#ExportBtn {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 %16, stop:1 %17);
    color: white; padding: 12px 36px; border-radius: 6px;
}
QPushButton#ExportBtn:hover {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 %16, stop:1 %16);
}
QPushButton#ExportBtn:pressed { background: %17; }
QPushButton#ExportBtn:disabled { background: %5; color: %6; }

)")
    .arg(rgb(c.warning.darker(250)))   // %1
    .arg(rgb(c.warning))               // %2
    .arg(rgb(c.warning.darker(200)))   // %3
    .arg(rgb(c.warning.darker(150)))   // %4
    .arg(rgb(c.surface2))              // %5
    .arg(rgb(c.textDisabled))          // %6
    .arg(rgb(c.border))                // %7
    .arg(rgb(c.textTertiary))          // %8
    .arg(rgb(c.textPrimary))           // %9
    .arg(rgb(c.accentHover))           // %10
    .arg(rgb(c.accent))                // %11
    .arg(rgb(c.surface0))              // %12
    .arg(rgb(c.inputBg))               // %13
    .arg(rgb(c.inputBorder))           // %14
    .arg(rgb(c.primaryBtnBg))          // %15
    .arg(rgb(c.successBtnHover))       // %16
    .arg(rgb(c.successBtnBg));         // %17

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  GROUPBOX (polished section cards â€” unified across all panels)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QGroupBox {
    background: %1;
    border: 1px solid %2;
    border-radius: %3px;
    margin-top: 22px;
    padding: 14px 14px 12px 14px;
    font-weight: bold;
    font-size: %4px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 2px 8px;
    color: %5;
    font-size: %6px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.5px;
}
)").arg(rgb(c.surface2))
   .arg(rgb(c.border))
   .arg(m.radiusLg)
   .arg(t.sizeBody)
   .arg(rgb(c.textSecondary))
   .arg(t.sizeCaption);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  PANEL HEADER LABEL  (set objectName to "rt-panel-header")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLabel#rt-panel-header {
    font-size: %1px;
    font-weight: bold;
    color: %2;
    padding: 4px 0px;
}
)").arg(t.sizeH1)
   .arg(rgb(c.accent));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SECTION HEADER LABEL  (set objectName to "rt-section-header")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLabel#rt-section-header {
    font-size: %1px;
    font-weight: 600;
    color: %2;
    padding: 2px 0px;
}
)").arg(t.sizeH2)
   .arg(rgb(c.textPrimary));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  CAPTION LABEL  (set objectName to "rt-caption")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLabel#rt-caption {
    font-size: %1px;
    color: %2;
}
)").arg(t.sizeCaption)
   .arg(rgb(c.textTertiary));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  BADGE  (small status pill â€” set objectName to "rt-badge")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLabel#rt-badge {
    background: %1;
    color: %2;
    font-size: %3px;
    font-weight: bold;
    border-radius: %4px;
    padding: 2px 8px;
    min-height: 16px;
}
)").arg(rgb(c.surface3))
   .arg(rgb(c.textPrimary))
   .arg(t.sizeSmall)
   .arg(m.radiusSm);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  TIMECODE DISPLAY  (set objectName to "rt-timecode")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QLabel#rt-timecode {
    font-family: "%1";
    font-size: %2px;
    font-weight: bold;
    color: %3;
    background: %4;
    border-radius: %5px;
    padding: 3px 8px;
}
)").arg(t.monoFamily)
   .arg(t.sizeMono)
   .arg(rgb(c.textPrimary))
   .arg(rgb(c.surface0))
   .arg(m.radiusSm);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  CARD FRAME  (set objectName to "rt-card")
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QFrame#rt-card {
    background: %1;
    border: 1px solid %2;
    border-radius: %3px;
}
)").arg(rgb(c.surface1))
   .arg(rgb(c.border))
   .arg(m.radiusLg);

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  LIST/TREE WIDGETS
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView, QTableView {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: %4px;
    outline: none;
    alternate-background-color: %5;
}
QListWidget::item, QTreeWidget::item, QListView::item, QTreeView::item {
    padding: 3px 6px;
    border-radius: 3px;
    margin: 1px 2px;
}
QListWidget::item:selected, QTreeWidget::item:selected,
QListView::item:selected, QTreeView::item:selected {
    background: %6;
    color: %7;
}
QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected,
QListView::item:hover:!selected, QTreeView::item:hover:!selected {
    background: %8;
}
QHeaderView::section {
    background: %9;
    color: %10;
    border: none;
    border-right: 1px solid %3;
    border-bottom: 1px solid %3;
    padding: 4px 6px;
    font-weight: 600;
    font-size: %11px;
}
)").arg(rgb(c.surface0))          // 1: bg
   .arg(rgb(c.textPrimary))       // 2: text
   .arg(rgb(c.border))            // 3: border
   .arg(m.radiusMd)               // 4: radius
   .arg(rgb(c.alternateBase))     // 5: alt row
   .arg(rgb(c.accent))            // 6: selected bg
   .arg(rgb(c.highlightedText))   // 7: selected text
   .arg(rgba(c.text, 15))         // 8: hover
   .arg(rgb(c.surface2))          // 9: header bg
   .arg(rgb(c.textSecondary))     // 10: header text
   .arg(t.sizeCaption);           // 11: header font size

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  PROGRESS BAR
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QProgressBar {
    background: %1;
    border: 1px solid %2;
    border-radius: %3px;
    text-align: center;
    color: %4;
    font-size: %5px;
    min-height: 18px;
}
QProgressBar::chunk {
    background: %6;
    border-radius: %3px;
}
)").arg(rgb(c.surface0))
   .arg(rgb(c.border))
   .arg(m.radiusSm)
   .arg(rgb(c.textPrimary))
   .arg(t.sizeSmall)
   .arg(rgb(c.accent));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  CHECKBOX & RADIO  (Apple-clean style)
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QCheckBox, QRadioButton {
    color: %1;
    spacing: 10px;
    font-size: %2px;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 20px;
    height: 20px;
    background: %3;
    border: 1px solid %4;
}
QCheckBox::indicator {
    border-radius: 3px;
}
QRadioButton::indicator {
    border-radius: 8px;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %5;
    border-color: %5;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border-color: %6;
}
)").arg(rgb(c.textPrimary))
   .arg(t.sizeBody)
   .arg(rgb(c.surface0))
   .arg(rgb(c.controlBorder))
   .arg(rgb(c.accent))
   .arg(rgb(c.accentHover));

    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    //  SLIDER
    // â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    qss += QStringLiteral(R"(
QSlider::groove:horizontal {
    background: %1;
    height: 4px;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: %2;
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
    border: 1px solid %3;
}
QSlider::handle:horizontal:hover {
    background: %4;
    border-color: %5;
}
QSlider::sub-page:horizontal {
    background: %6;
    border-radius: 2px;
}
QSlider::groove:vertical {
    background: %1;
    width: 4px;
    border-radius: 2px;
}
QSlider::handle:vertical {
    background: %2;
    width: 14px;
    height: 14px;
    margin: 0 -5px;
    border-radius: 7px;
    border: 1px solid %3;
}
QSlider::handle:vertical:hover {
    background: %4;
    border-color: %5;
}
)").arg(rgb(c.surface0))       // 1: groove
   .arg(rgb(c.surface4))      // 2: handle
   .arg(rgb(c.border))        // 3: handle border
   .arg(rgb(c.textSecondary)) // 4: handle hover
   .arg(rgb(c.accent))        // 5: handle hover border
   .arg(rgb(c.accentDim));    // 6: filled portion

    return qss;
}

} // namespace rt
