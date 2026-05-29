/*
 * Theme.cpp â€” ROUNDTABLE universal dark theme implementation.
 *
 * Premiere Pro + Apple HIG-inspired dark mode with:
 *   - Blue-tinted deep gray surfaces
 *   - Layered depth without harsh borders
 *   - Muted accent blue for interactive elements
 *   - Comprehensive component QSS classes
 */

#include "Theme.h"

#include <QApplication>
#include <QStyleFactory>

namespace rt {

// â”€â”€ Singleton design tokens (external linkage â€” shared across ThemeStylesheet.cpp, ThemePresets.cpp)
ThemeColors      s_colors;
ThemeTypography  s_typography;
ThemeMetrics     s_metrics;

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Apply
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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

    // â”€â”€ Font â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    QFont defaultFont(s_typography.fontFamily);
    defaultFont.setPixelSize(s_typography.sizeBody);
    app->setFont(defaultFont);

    // â”€â”€ Palette â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    app->setPalette(palette());

    // â”€â”€ Stylesheet â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    app->setStyleSheet(stylesheet());
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Palette
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Token Accessors
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

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
