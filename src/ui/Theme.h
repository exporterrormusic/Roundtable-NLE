/*
 * Theme — Universal design-token theme for ROUNDTABLE NLE.
 *
 * Centralizes ALL visual styling into one place:
 *   - ThemeColors:       Named color tokens (surfaces, text, accents, status)
 *   - ThemeTypography:   Font families, sizes, weights
 *   - ThemeMetrics:      Spacing, radii, control dimensions
 *   - QPalette:          Qt palette for Fusion style
 *   - Global stylesheet: Component-class QSS for buttons, inputs, sections, etc.
 *
 * Inspired by Adobe Spectrum + Apple HIG dark mode:
 *   - Blue-tinted deep grays (not pure black)
 *   - Layered surfaces for depth without hard borders
 *   - Muted accent blue (#4a90d9) for interactive elements
 *   - 4-tier text hierarchy for readability
 *   - Generous spacing on an 8px grid
 *
 * Applied once at startup from App::init().
 * All panels and widgets should use Theme::colors() / Theme::typography() /
 * Theme::metrics() instead of hardcoding values.
 */

#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Color Tokens
// ═════════════════════════════════════════════════════════════════════════════

/// Named color tokens used across the entire application.
/// Use Theme::colors() to access the singleton instance.
struct ThemeColors
{
    // ── Surface layers (darkest → lightest) ─────────────────────────
    // Layered depth model (Apple vibrancy-like):
    //   surface0 = deepest background (viewport, timeline canvas)
    //   surface1 = panel content area
    //   surface2 = section / card background
    //   surface3 = raised element (header bars, floating controls)
    //   surface4 = highest layer (tooltips, popups)
    QColor surface0{14, 14, 20};       ///< Deepest background (viewport)
    QColor surface1{22, 22, 30};       ///< Panel content area
    QColor surface2{30, 30, 37};       ///< Section / card background
    QColor surface3{38, 38, 46};       ///< Header bars, raised controls
    QColor surface4{48, 48, 56};       ///< Tooltips, popups

    // ── Legacy background aliases (mapped to surfaces) ──────────────
    QColor window{30, 30, 37};         ///< = surface2 (main window bg)
    QColor base{14, 14, 20};           ///< = surface0 (inputs, lists)
    QColor panel{22, 22, 30};          ///< = surface1 (panel bg)
    QColor alternateBase{26, 26, 34};  ///< Alternate row
    QColor toolTipBase{48, 48, 56};    ///< = surface4

    // ── Text hierarchy (4-tier Apple-style) ─────────────────────────
    QColor textPrimary{220, 220, 225};   ///< Main content text
    QColor textSecondary{160, 160, 170}; ///< Labels, metadata
    QColor textTertiary{110, 110, 120};  ///< Placeholders, hints
    QColor textDisabled{70, 70, 78};     ///< Disabled controls
    // Legacy aliases
    QColor text{220, 220, 225};          ///< = textPrimary
    QColor textDim{160, 160, 170};       ///< = textSecondary
    QColor textBright{255, 255, 255};    ///< Headings, active items
    QColor toolTipText{220, 220, 225};

    // ── Controls ────────────────────────────────────────────────────
    QColor controlBg{38, 38, 46};          ///< Button/combo background
    QColor controlBgHover{48, 48, 58};     ///< Hover state
    QColor controlBgActive{32, 32, 40};    ///< Pressed state
    QColor controlBorder{50, 50, 62};      ///< Default border
    QColor controlBorderFocus{74, 144, 217}; ///< Focus ring
    // Legacy aliases
    QColor button{38, 38, 46};
    QColor buttonText{220, 220, 225};
    QColor buttonHover{48, 48, 58};
    QColor buttonPressed{32, 32, 40};

    // ── Input fields ────────────────────────────────────────────────
    QColor inputBg{18, 18, 26};            ///< Text input background
    QColor inputBorder{50, 50, 62};        ///< Default input border
    QColor inputBorderFocus{74, 144, 217}; ///< Focused input border
    QColor inputSelection{58, 85, 120};    ///< Selection highlight in inputs

    // ── Accents ─────────────────────────────────────────────────────
    QColor accent{74, 144, 217};       ///< Primary accent (#4a90d9)
    QColor accentHover{90, 158, 233};  ///< Accent hover
    QColor accentDim{42, 90, 155};     ///< Accent subdued
    QColor accentSubtle{30, 50, 75};   ///< Very subtle accent tint (bg)
    QColor highlight{74, 144, 217};    ///< Selection highlight
    QColor highlightedText{255, 255, 255};

    // ── Panel chrome ────────────────────────────────────────────────
    QColor panelHeaderBg{30, 30, 40};    ///< Panel header bar
    QColor panelContentBg{22, 22, 30};   ///< Panel content area
    QColor panelBorder{42, 42, 52};      ///< Panel frame border

    // ── Timeline-specific ───────────────────────────────────────────
    QColor playhead{74, 144, 217};     ///< Playhead (accent blue)
    QColor trackBg{20, 20, 26};        ///< Track background
    QColor trackBgAlt{24, 24, 30};     ///< Alternate track background
    QColor trackDivider{40, 40, 50};   ///< Line between tracks
    QColor clipVideo{58, 120, 200};    ///< Video clip color
    QColor clipImage{150, 90, 200};    ///< Still-image clip color (purple, matches Project Bin)
    QColor clipCharacter{220, 130, 50};///< Video-character clip color (orange)
    QColor clipAudio{64, 168, 96};     ///< Audio clip color
    QColor clipTitle{180, 90, 210};    ///< Title clip color
    QColor clipSpine{200, 140, 50};    ///< Spine clip color (live eval)
    QColor clipSpineCached{80, 170, 105}; ///< Spine clip pre-rendered
    QColor clipGraphic{68, 187, 255};    ///< Graphic (text/shape) clip color
    QColor clipSelected{90, 165, 240}; ///< Selected clip outline
    QColor clipHover{70, 70, 80};      ///< Clip hover overlay
    QColor transition{100, 80, 160};   ///< Transition overlay

    // ── Status ──────────────────────────────────────────────────────
    QColor error{210, 60, 60};
    QColor errorBg{60, 25, 25};
    QColor warning{220, 180, 40};
    QColor warningBg{55, 45, 20};
    QColor success{58, 180, 90};
    QColor successBg{22, 50, 30};

    // ── Semantic button tints ───────────────────────────────────────
    QColor dangerBg{74, 34, 34};          ///< Danger button bg
    QColor dangerBgHover{90, 42, 42};
    QColor dangerText{230, 140, 140};
    QColor primaryBtnBg{42, 100, 170};    ///< Primary action button
    QColor primaryBtnHover{52, 115, 190};
    QColor successBtnBg{42, 120, 58};     ///< Confirm/export button
    QColor successBtnHover{52, 140, 72};

    // ── Borders & separators ────────────────────────────────────────
    QColor border{42, 42, 52};
    QColor borderLight{58, 58, 70};
    QColor separator{35, 35, 45};

    // ── Dock widget ─────────────────────────────────────────────────
    QColor dockTitleBg{30, 30, 40};
    QColor dockTitleText{170, 170, 180};
    QColor dockTitleActive{74, 144, 217};

    // ── Waveform / audio ────────────────────────────────────────────
    QColor waveformBg{14, 14, 20};
    QColor waveformFg{70, 120, 180};
    QColor waveformGrid{35, 35, 45};
    QColor waveformSelection{255, 200, 50};

    // ── Scrollbar (specific) ────────────────────────────────────────
    QColor scrollbarTrack{14, 14, 20};
    QColor scrollbarThumb{45, 45, 55};
    QColor scrollbarThumbHover{58, 58, 68};
};

// ═════════════════════════════════════════════════════════════════════════════
//  Typography Tokens
// ═════════════════════════════════════════════════════════════════════════════

/// Font families, sizes, and weights for consistent text hierarchy.
struct ThemeTypography
{
    // Families
    QString fontFamily{"Segoe UI"};       ///< Primary UI font (Windows)
    QString monoFamily{"Consolas"};       ///< Monospace for timecodes

    // Size hierarchy (logical px — scaled by OS DPI factor)
    int sizeH1{21};        ///< Panel titles
    int sizeH2{19};        ///< Section headers
    int sizeBody{16};      ///< Normal labels, controls
    int sizeCaption{15};   ///< Metadata, secondary info
    int sizeSmall{14};     ///< Fine print, shortcut hints
    int sizeXs{12};        ///< Compact controls, table cells, chips
    int sizeXxs{11};       ///< Badges, count bubbles, dense toolbars
    int sizeMono{15};      ///< Timecodes

    // Weights (QFont::Weight values)
    int weightBold{700};
    int weightSemiBold{600};
    int weightMedium{500};
    int weightRegular{400};
    int weightLight{300};

    // Convenience font creation — uses setPixelSize() so sizes match
    // stylesheet "font-size: Xpx" values and scale uniformly with DPI.
    [[nodiscard]] QFont h1() const      { QFont f(fontFamily); f.setPixelSize(sizeH1); f.setWeight(static_cast<QFont::Weight>(weightBold)); return f; }
    [[nodiscard]] QFont h2() const      { QFont f(fontFamily); f.setPixelSize(sizeH2); f.setWeight(static_cast<QFont::Weight>(weightSemiBold)); return f; }
    [[nodiscard]] QFont body() const    { QFont f(fontFamily); f.setPixelSize(sizeBody); f.setWeight(static_cast<QFont::Weight>(weightRegular)); return f; }
    [[nodiscard]] QFont caption() const { QFont f(fontFamily); f.setPixelSize(sizeCaption); f.setWeight(static_cast<QFont::Weight>(weightRegular)); return f; }
    [[nodiscard]] QFont smallFont() const { QFont f(fontFamily); f.setPixelSize(sizeSmall); f.setWeight(static_cast<QFont::Weight>(weightLight)); return f; }
    [[nodiscard]] QFont mono() const    { QFont f(monoFamily); f.setPixelSize(sizeMono); f.setWeight(static_cast<QFont::Weight>(weightRegular)); return f; }
    [[nodiscard]] QFont monoBold() const { QFont f(monoFamily); f.setPixelSize(sizeMono); f.setWeight(static_cast<QFont::Weight>(weightBold)); return f; }
};

// ═════════════════════════════════════════════════════════════════════════════
//  Metrics Tokens
// ═════════════════════════════════════════════════════════════════════════════

/// Spacing, radii, and dimension tokens for layout consistency.
struct ThemeMetrics
{
    // Spacing (8px grid system)
    int spacingXxs{2};
    int spacingXs{4};
    int spacingSm{6};
    int spacingMd{8};
    int spacingLg{12};
    int spacingXl{16};
    int spacingXxl{24};

    // Border radii
    int radiusNone{0};
    int radiusSm{3};       ///< Small buttons, badges
    int radiusMd{5};       ///< Inputs, combos, cards
    int radiusLg{8};       ///< Dialogs, sections
    int radiusXl{12};      ///< Large modals, popovers
    int radiusPill{9999};  ///< Fully rounded pill buttons

    // Border widths
    int borderThin{1};
    int borderMedium{2};

    // Control dimensions
    int controlHeight{36};        ///< Standard button/input height
    int controlHeightSm{32};      ///< Small buttons
    int controlHeightLg{42};      ///< Large buttons (confirm, etc.)
    int panelHeaderHeight{32};    ///< Panel header bar height
    int sectionHeaderHeight{28};  ///< Section title height

    // Icon sizes
    int iconSm{14};
    int iconMd{16};
    int iconLg{20};
};

// ═════════════════════════════════════════════════════════════════════════════
//  Theme API
// ═════════════════════════════════════════════════════════════════════════════

/// Application theming utilities.
/// Access the singleton design tokens from anywhere via static methods.
/// Roundtable ships a single dark theme; there is no theme selection.
class Theme
{
public:
    /// Apply the dark theme to the application. Call once from App::init().
    static void apply();

    /// Get the current themed palette.
    [[nodiscard]] static QPalette palette();

    /// Get the global stylesheet (for fine-tuned widget styling).
    [[nodiscard]] static QString stylesheet();

    /// Get the named color constants.
    [[nodiscard]] static const ThemeColors& colors();

    /// Get typography tokens.
    [[nodiscard]] static const ThemeTypography& typography();

    /// Get metrics/spacing tokens.
    [[nodiscard]] static const ThemeMetrics& metrics();

    /// Return QSS for an Apple-style section card (use on QGroupBox).
    // Card style is now part of global stylesheet — no per-widget helper needed

    // ── Convenience helpers for QSS ─────────────────────────────────

    /// Format a QColor as "rgb(r,g,b)" for use in QSS strings.
    [[nodiscard]] static QString rgb(const QColor& color);

    /// Format a QColor as "rgba(r,g,b,a)" for use in QSS strings.
    [[nodiscard]] static QString rgba(const QColor& color, int alpha);

    /// Format a QColor as "#rrggbb" hex for use in QSS strings.
    [[nodiscard]] static QString hex(const QColor& color);

private:
    static void applyDarkPreset();
};

} // namespace rt
