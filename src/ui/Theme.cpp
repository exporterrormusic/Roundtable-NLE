/*
 * Theme.cpp — ROUNDTABLE universal dark theme implementation.
 *
 * Premiere Pro + Apple HIG-inspired dark mode with:
 *   - Blue-tinted deep gray surfaces
 *   - Layered depth without harsh borders
 *   - Muted accent blue for interactive elements
 *   - Comprehensive component QSS classes
 */

#include "Theme.h"

#include <QApplication>
#include <QSplitter>
#include <QStyleFactory>

#include <algorithm>

namespace rt {

// ── Singleton design tokens (external linkage — shared across ThemeStylesheet.cpp, ThemePresets.cpp)
ThemeColors      s_colors;
ThemeTypography  s_typography;
ThemeMetrics     s_metrics;

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

QString Theme::rgb(const QColor& color)
{
    return QString("rgb(%1,%2,%3)")
        .arg(color.red()).arg(color.green()).arg(color.blue());
}

QString Theme::rgba(const QColor& color, int alpha)
{
    return QString("rgba(%1,%2,%3,%4)")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha);
}

QString Theme::hex(const QColor& color)
{
    return color.name(QColor::HexRgb);
}

void Theme::styleThinSplitter(QSplitter* splitter, int visiblePx,
                              const QColor& line, const QColor& hover,
                              const QString& extraQss)
{
    if (!splitter) return;
    visiblePx = std::max(1, visiblePx);
    // Same parity as the line so it centres on whole pixels.
    int grab = std::max(kSplitterGrabPx, visiblePx);
    if ((grab - visiblePx) % 2) ++grab;
    splitter->setHandleWidth(grab);

    // Hard-stop gradient = a centred solid line on a transparent handle.
    const double a  = (grab - visiblePx) / 2.0 / grab;
    const double b  = 1.0 - a;
    const double e  = 0.0001;
    const QString stops = QStringLiteral(
        "stop:0 transparent, stop:%1 transparent, stop:%2 %5, "
        "stop:%3 %5, stop:%4 transparent, stop:1 transparent")
        .arg(a).arg(a + e).arg(b).arg(b + e).arg(hex(line));

    // Scoped by property so nested splitters keep the global handle style.
    splitter->setProperty("rtThinSplitter", true);
    splitter->setStyleSheet(QStringLiteral(
        "QSplitter[rtThinSplitter=\"true\"]::handle:horizontal {"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:0, %1); }"
        "QSplitter[rtThinSplitter=\"true\"]::handle:vertical {"
        " background: qlineargradient(x1:0, y1:0, x2:0, y2:1, %1); }"
        "QSplitter[rtThinSplitter=\"true\"]::handle:hover { background: %2; }")
        .arg(stops, hex(hover)) + extraQss);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Apply
// ═════════════════════════════════════════════════════════════════════════════

void Theme::apply()
{
    auto* app = qApp;
    if (!app) return;

    // Apply the single dark theme.
    applyDarkPreset();

    // Fusion base style (only set once to avoid expensive re-init)
    static bool fusionSet = false;
    if (!fusionSet) {
        app->setStyle(QStyleFactory::create("Fusion"));
        fusionSet = true;
    }

    // ── Font ────────────────────────────────────────────────────────────
    QFont defaultFont(s_typography.fontFamily);
    defaultFont.setPixelSize(s_typography.sizeBody);
    app->setFont(defaultFont);

    // ── Palette ─────────────────────────────────────────────────────────
    app->setPalette(palette());

    // ── Stylesheet ──────────────────────────────────────────────────────
    app->setStyleSheet(stylesheet());
}

// ═════════════════════════════════════════════════════════════════════════════
//  Palette
// ═════════════════════════════════════════════════════════════════════════════

QPalette Theme::palette()
{
    const auto& c = s_colors;
    QPalette p;

    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.alternateBase);
    p.setColor(QPalette::ToolTipBase, c.toolTipBase);
    p.setColor(QPalette::ToolTipText, c.toolTipText);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.buttonText);
    p.setColor(QPalette::BrightText, c.textBright);
    p.setColor(QPalette::Highlight, c.highlight);
    p.setColor(QPalette::HighlightedText, c.highlightedText);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::LinkVisited, c.accentDim);

    // Disabled
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.accentDim);

    return p;
}


// ═════════════════════════════════════════════════════════════════════════════
//  Token Accessors
// ═════════════════════════════════════════════════════════════════════════════

const ThemeColors& Theme::colors()
{
    return s_colors;
}

const ThemeTypography& Theme::typography()
{
    return s_typography;
}

const ThemeMetrics& Theme::metrics()
{
    return s_metrics;
}

} // namespace rt
