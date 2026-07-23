/*
 * GraphicLayer.cpp — GraphicLayer base + TextLayer + ShapeLayer implementation.
 */

#include "timeline/GraphicLayer.h"

#include <algorithm>
#include <limits>

namespace rt {

namespace {

std::vector<uint32_t> decodeUtf8(const std::string& text)
{
    std::vector<uint32_t> result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0xFFFD;
        size_t count = 1;
        if (first < 0x80) {
            cp = first;
        } else if ((first & 0xE0) == 0xC0 && i + 1 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xC0) == 0x80) {
                cp = ((first & 0x1F) << 6) | (b1 & 0x3F);
                if (cp >= 0x80) count = 2; else cp = 0xFFFD;
            }
        } else if ((first & 0xF0) == 0xE0 && i + 2 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                cp = ((first & 0x0F) << 12) | ((b1 & 0x3F) << 6)
                    | (b2 & 0x3F);
                if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF))
                    count = 3;
                else
                    cp = 0xFFFD;
            }
        } else if ((first & 0xF8) == 0xF0 && i + 3 < text.size()) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            const auto b2 = static_cast<unsigned char>(text[i + 2]);
            const auto b3 = static_cast<unsigned char>(text[i + 3]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80
                && (b3 & 0xC0) == 0x80) {
                cp = ((first & 0x07) << 18) | ((b1 & 0x3F) << 12)
                    | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                if (cp >= 0x10000 && cp <= 0x10FFFF)
                    count = 4;
                else
                    cp = 0xFFFD;
            }
        }
        result.push_back(cp);
        i += count;
    }
    return result;
}

uint32_t utf16Length(const std::vector<uint32_t>& codepoints,
                     size_t begin, size_t end)
{
    uint64_t length = 0;
    for (size_t i = begin; i < end; ++i)
        length += codepoints[i] > 0xFFFF ? 2u : 1u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        length, std::numeric_limits<uint32_t>::max()));
}

bool sameTextStyle(const TextStyleRun& a, const TextStyleRun& b)
{
    return a.fontFamily == b.fontFamily && a.fontStyle == b.fontStyle
        && a.fontSize == b.fontSize
        && a.fontWeight == b.fontWeight && a.italic == b.italic
        && a.allCaps == b.allCaps && a.smallCaps == b.smallCaps
        && a.tracking == b.tracking
        && a.baselineShift == b.baselineShift
        && a.leading == b.leading
        && a.kerning == b.kerning && a.tabWidth == b.tabWidth
        && a.tsume == b.tsume
        && a.fauxBold == b.fauxBold && a.fauxItalic == b.fauxItalic
        && a.underline == b.underline
        && a.superscript == b.superscript && a.subscript == b.subscript
        && a.appearance == b.appearance
        && a.overrideMask == b.overrideMask;
}

std::vector<TextStyleRun> normalizeStyleRuns(std::vector<TextStyleRun> runs)
{
    // Qt can discard the private QTextCharFormat properties when the
    // auto-selected placeholder is replaced. Older builds serialized the
    // missing properties as an explicit all-zero appearance override, making
    // the entire run transparent. This byte signature cannot be produced by
    // the appearance controls (disabled components retain their color/width),
    // so repair affected projects by restoring normal layer inheritance.
    constexpr uint32_t appearanceOverrides = TextOverrideFill
        | TextOverrideStroke | TextOverrideShadow | TextOverrideBackground;
    for (auto& run : runs) {
        const auto& appearance = run.appearance;
        const bool missingInlineAppearance =
            run.overrideMask == appearanceOverrides
            && !appearance.fillEnabled && appearance.fillColor == 0
            && !appearance.strokeEnabled && appearance.strokeColor == 0
            && appearance.strokeWidth == 0.0f
            && appearance.strokePosition == StrokePosition::Center
            && !appearance.shadowEnabled && appearance.shadowColor == 0
            && appearance.shadowDistance == 0.0f
            && appearance.shadowAngle == 0.0f
            && appearance.shadowSoftness == 0.0f
            && appearance.shadowOpacity == 0.0f
            && !appearance.backgroundEnabled
            && appearance.backgroundColor == 0
            && appearance.backgroundPadding == 0.0f;
        if (missingInlineAppearance)
            run.overrideMask &= ~appearanceOverrides;
    }

    runs.erase(std::remove_if(runs.begin(), runs.end(),
                              [](const TextStyleRun& run) {
                                  return run.length == 0;
                              }),
               runs.end());
    std::stable_sort(runs.begin(), runs.end(),
                     [](const TextStyleRun& a, const TextStyleRun& b) {
                         return a.start < b.start;
                     });

    std::vector<TextStyleRun> normalized;
    normalized.reserve(runs.size());
    for (auto run : runs) {
        if (!normalized.empty()) {
            const uint64_t previousEnd =
                static_cast<uint64_t>(normalized.back().start)
                + normalized.back().length;
            if (run.start < previousEnd) {
                const uint64_t overlap = previousEnd - run.start;
                if (overlap >= run.length) continue;
                run.start = static_cast<uint32_t>(previousEnd);
                run.length -= static_cast<uint32_t>(overlap);
            }
            if (sameTextStyle(normalized.back(), run)
                && previousEnd == run.start) {
                normalized.back().length += run.length;
                continue;
            }
        }
        normalized.push_back(std::move(run));
    }
    return normalized;
}

std::vector<TextParagraphStyle> normalizeParagraphStyles(
    std::vector<TextParagraphStyle> styles)
{
    styles.erase(std::remove_if(styles.begin(), styles.end(),
                                [](const TextParagraphStyle& style) {
                                    return style.length == 0;
                                }),
                 styles.end());
    std::stable_sort(styles.begin(), styles.end(),
                     [](const TextParagraphStyle& a,
                        const TextParagraphStyle& b) {
                         return a.start < b.start;
                     });
    std::vector<TextParagraphStyle> normalized;
    for (auto style : styles) {
        if (!normalized.empty()) {
            const uint64_t previousEnd = static_cast<uint64_t>(
                normalized.back().start) + normalized.back().length;
            if (style.start < previousEnd) {
                const uint64_t overlap = previousEnd - style.start;
                if (overlap >= style.length) continue;
                style.start = static_cast<uint32_t>(previousEnd);
                style.length -= static_cast<uint32_t>(overlap);
            }
            if (normalized.back().alignment == style.alignment
                && normalized.back().rightToLeft == style.rightToLeft
                && previousEnd == style.start) {
                normalized.back().length += style.length;
                continue;
            }
        }
        normalized.push_back(style);
    }
    return normalized;
}

} // namespace

std::vector<TextStyleRun> remapTextStyleRuns(
    const std::string& oldText,
    const std::string& newText,
    const std::vector<TextStyleRun>& runs)
{
    if (oldText == newText || runs.empty()) return runs;

    const auto oldChars = decodeUtf8(oldText);
    const auto newChars = decodeUtf8(newText);
    size_t prefix = 0;
    while (prefix < oldChars.size() && prefix < newChars.size()
           && oldChars[prefix] == newChars[prefix])
        ++prefix;

    size_t suffix = 0;
    while (suffix < oldChars.size() - prefix
           && suffix < newChars.size() - prefix
           && oldChars[oldChars.size() - 1 - suffix]
              == newChars[newChars.size() - 1 - suffix])
        ++suffix;

    const uint32_t editStart = utf16Length(oldChars, 0, prefix);
    const uint32_t oldEnd = utf16Length(oldChars, 0,
                                         oldChars.size() - suffix);
    const uint32_t newEnd = utf16Length(newChars, 0,
                                         newChars.size() - suffix);
    const int64_t delta = static_cast<int64_t>(newEnd)
        - static_cast<int64_t>(oldEnd);

    const TextStyleRun* inherited = nullptr;
    auto findContaining = [&runs](uint32_t position) -> const TextStyleRun* {
        for (const auto& run : runs) {
            const uint64_t end = static_cast<uint64_t>(run.start) + run.length;
            if (run.start <= position && position < end) return &run;
        }
        return nullptr;
    };
    inherited = findContaining(editStart);
    if (!inherited && editStart > 0) inherited = findContaining(editStart - 1);

    std::vector<TextStyleRun> mapped;
    mapped.reserve(runs.size() * 2 + 1);
    for (const auto& run : runs) {
        const uint64_t runEnd64 = static_cast<uint64_t>(run.start) + run.length;
        const uint32_t runEnd = static_cast<uint32_t>(std::min<uint64_t>(
            runEnd64, std::numeric_limits<uint32_t>::max()));
        if (run.start < editStart) {
            auto left = run;
            left.length = std::min(runEnd, editStart) - run.start;
            if (left.length > 0) mapped.push_back(std::move(left));
        }
        if (runEnd > oldEnd) {
            auto right = run;
            const uint32_t preservedStart = std::max(run.start, oldEnd);
            right.start = static_cast<uint32_t>(
                static_cast<int64_t>(preservedStart) + delta);
            right.length = runEnd - preservedStart;
            mapped.push_back(std::move(right));
        }
    }

    if (inherited && newEnd > editStart) {
        auto inserted = *inherited;
        inserted.start = editStart;
        inserted.length = newEnd - editStart;
        mapped.push_back(std::move(inserted));
    }
    return normalizeStyleRuns(std::move(mapped));
}

std::vector<TextParagraphStyle> remapTextParagraphStyles(
    const std::string& oldText,
    const std::string& newText,
    const std::vector<TextParagraphStyle>& styles)
{
    if (oldText == newText || styles.empty()) return styles;

    const auto oldChars = decodeUtf8(oldText);
    const auto newChars = decodeUtf8(newText);
    size_t prefix = 0;
    while (prefix < oldChars.size() && prefix < newChars.size()
           && oldChars[prefix] == newChars[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < oldChars.size() - prefix
           && suffix < newChars.size() - prefix
           && oldChars[oldChars.size() - 1 - suffix]
              == newChars[newChars.size() - 1 - suffix]) ++suffix;

    const uint32_t editStart = utf16Length(oldChars, 0, prefix);
    const uint32_t oldEnd = utf16Length(oldChars, 0,
                                         oldChars.size() - suffix);
    const uint32_t newEnd = utf16Length(newChars, 0,
                                         newChars.size() - suffix);
    const int64_t delta = static_cast<int64_t>(newEnd)
        - static_cast<int64_t>(oldEnd);

    const TextParagraphStyle* inherited = nullptr;
    for (const auto& style : styles) {
        const uint64_t end = static_cast<uint64_t>(style.start) + style.length;
        if (style.start <= editStart && editStart < end) {
            inherited = &style;
            break;
        }
    }
    std::vector<TextParagraphStyle> mapped;
    for (const auto& style : styles) {
        const uint32_t styleEnd = static_cast<uint32_t>(std::min<uint64_t>(
            static_cast<uint64_t>(style.start) + style.length,
            std::numeric_limits<uint32_t>::max()));
        if (style.start < editStart) {
            auto left = style;
            left.length = std::min(styleEnd, editStart) - style.start;
            if (left.length) mapped.push_back(left);
        }
        if (styleEnd > oldEnd) {
            auto right = style;
            const uint32_t preservedStart = std::max(style.start, oldEnd);
            right.start = static_cast<uint32_t>(
                static_cast<int64_t>(preservedStart) + delta);
            right.length = styleEnd - preservedStart;
            mapped.push_back(right);
        }
    }
    if (inherited && newEnd > editStart) {
        auto inserted = *inherited;
        inserted.start = editStart;
        inserted.length = newEnd - editStart;
        mapped.push_back(inserted);
    }
    return normalizeParagraphStyles(std::move(mapped));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GraphicLayer base
// ═════════════════════════════════════════════════════════════════════════════

std::atomic<uint64_t> GraphicLayer::s_nextLayerId{1};

GraphicLayer::GraphicLayer(GraphicLayerType type)
    : m_type(type)
    , m_id(s_nextLayerId.fetch_add(1, std::memory_order_relaxed))
    , m_name("Layer")
{
    // Default appearance: one white fill, no stroke, no shadow
    m_appearance.fills.push_back(FillEntry{0xFFFFFFFF, 1.0f, true});
}

GraphicLayer::~GraphicLayer() = default;

void GraphicLayer::assignStateFrom(const GraphicLayer& other)
{
    // Copies shared (non-identity) state. Subclasses chain to this then copy
    // their own fields. m_id and m_type are deliberately NOT copied.
    m_name       = other.m_name;
    m_visible    = other.m_visible;
    m_locked     = other.m_locked;
    m_transform  = other.m_transform;   // LayerTransform is copy-assignable
    m_appearance = other.m_appearance;
}

// ═════════════════════════════════════════════════════════════════════════════
//  TextLayer
// ═════════════════════════════════════════════════════════════════════════════

TextLayer::TextLayer()
    : GraphicLayer(GraphicLayerType::Text)
{
    m_name = "Text";
}

void TextLayer::setStyleRuns(std::vector<TextStyleRun> runs)
{
    m_styleRuns = normalizeStyleRuns(std::move(runs));
}

void TextLayer::setParagraphStyles(std::vector<TextParagraphStyle> styles)
{
    m_paragraphStyles = normalizeParagraphStyles(std::move(styles));
}

void TextLayer::replaceTextPreservingStyles(const std::string& text)
{
    if (m_text == text) return;
    auto mapped = remapTextStyleRuns(m_text, text, m_styleRuns);
    auto mappedParagraphs = remapTextParagraphStyles(
        m_text, text, m_paragraphStyles);
    m_text = text;
    m_styleRuns = std::move(mapped);
    m_paragraphStyles = std::move(mappedParagraphs);
}

void TextLayer::setFillForAll(bool enabled, uint32_t color)
{
    if (m_appearance.fills.empty())
        m_appearance.fills.push_back({color, 1.0f, enabled});
    else {
        m_appearance.fills[0].enabled = enabled;
        m_appearance.fills[0].color = color;
    }
    for (auto& run : m_styleRuns) {
        run.appearance.fillEnabled = enabled;
        run.appearance.fillColor = color;
        run.overrideMask &= ~TextOverrideFill;
    }
}

void TextLayer::setStrokeForAll(bool enabled, uint32_t color, float width,
                                StrokePosition position)
{
    if (m_appearance.strokes.empty())
        m_appearance.strokes.push_back({color, width, position, 1.0f, enabled});
    else {
        auto& stroke = m_appearance.strokes[0];
        stroke.enabled = enabled;
        stroke.color = color;
        stroke.width = width;
        stroke.position = position;
    }
    for (auto& run : m_styleRuns) {
        run.appearance.strokeEnabled = enabled;
        run.appearance.strokeColor = color;
        run.appearance.strokeWidth = width;
        run.appearance.strokePosition = position;
        run.overrideMask &= ~TextOverrideStroke;
    }
}

void TextLayer::setShadowForAll(bool enabled, uint32_t color, float distance,
                                float angle, float softness, float opacity)
{
    if (m_appearance.shadows.empty())
        m_appearance.shadows.push_back(
            {color, distance, angle, softness, opacity, enabled});
    else {
        auto& shadow = m_appearance.shadows[0];
        shadow.enabled = enabled;
        shadow.color = color;
        shadow.distance = distance;
        shadow.angle = angle;
        shadow.softness = softness;
        shadow.opacity = opacity;
    }
    for (auto& run : m_styleRuns) {
        auto& appearance = run.appearance;
        appearance.shadowEnabled = enabled;
        appearance.shadowColor = color;
        appearance.shadowDistance = distance;
        appearance.shadowAngle = angle;
        appearance.shadowSoftness = softness;
        appearance.shadowOpacity = opacity;
        run.overrideMask &= ~TextOverrideShadow;
    }
}

void TextLayer::setBackgroundForAll(bool enabled, uint32_t color,
                                    float padding)
{
    m_backgroundEnabled = enabled;
    m_backgroundColor = color;
    m_backgroundPadding = padding;
    for (auto& run : m_styleRuns) {
        run.appearance.backgroundEnabled = enabled;
        run.appearance.backgroundColor = color;
        run.appearance.backgroundPadding = padding;
        run.overrideMask &= ~TextOverrideBackground;
    }
}

std::unique_ptr<GraphicLayer> TextLayer::clone() const
{
    auto copy = std::make_unique<TextLayer>();

    // Base
    copy->m_name    = m_name;
    copy->m_visible = m_visible;
    copy->m_locked  = m_locked;
    copy->m_transform.posX     = m_transform.posX;
    copy->m_transform.posY     = m_transform.posY;
    copy->m_transform.scaleX   = m_transform.scaleX;
    copy->m_transform.scaleY   = m_transform.scaleY;
    copy->m_transform.rotation = m_transform.rotation;
    copy->m_transform.anchorX  = m_transform.anchorX;
    copy->m_transform.anchorY  = m_transform.anchorY;
    copy->m_transform.opacity  = m_transform.opacity;
    copy->m_appearance = m_appearance;

    // Text-specific
    copy->m_text        = m_text;
    copy->m_fontFamily  = m_fontFamily;
    copy->m_fontStyle   = m_fontStyle;
    copy->m_fontSize    = m_fontSize;
    copy->m_fontWeight  = m_fontWeight;
    copy->m_italic      = m_italic;
    copy->m_allCaps     = m_allCaps;
    copy->m_smallCaps   = m_smallCaps;
    copy->m_align       = m_align;
    copy->m_valign      = m_valign;
    copy->m_styleRuns   = m_styleRuns;
    copy->m_paragraphStyles = m_paragraphStyles;
    copy->m_kerning     = m_kerning;
    copy->m_tabWidth    = m_tabWidth;
    copy->m_tsume       = m_tsume;
    copy->m_fauxBold    = m_fauxBold;
    copy->m_fauxItalic  = m_fauxItalic;
    copy->m_underline   = m_underline;
    copy->m_superscript = m_superscript;
    copy->m_subscript   = m_subscript;
    copy->m_rightToLeft = m_rightToLeft;
    copy->m_backgroundEnabled = m_backgroundEnabled;
    copy->m_backgroundColor = m_backgroundColor;
    copy->m_backgroundPadding = m_backgroundPadding;
    copy->m_maskWithText = m_maskWithText;
    copy->m_linkedStyleName = m_linkedStyleName;
    copy->m_tracking    = m_tracking;
    copy->m_leading     = m_leading;
    copy->m_baselineShift = m_baselineShift;
    copy->m_boxWidth    = m_boxWidth;
    copy->m_boxHeight   = m_boxHeight;
    copy->m_useParagraphBox = m_useParagraphBox;

    return copy;
}

void TextLayer::assignStateFrom(const GraphicLayer& other)
{
    GraphicLayer::assignStateFrom(other);
    const auto* o = dynamic_cast<const TextLayer*>(&other);
    if (!o) return;
    m_text            = o->m_text;
    m_fontFamily      = o->m_fontFamily;
    m_fontStyle       = o->m_fontStyle;
    m_fontSize        = o->m_fontSize;
    m_fontWeight      = o->m_fontWeight;
    m_italic          = o->m_italic;
    m_allCaps         = o->m_allCaps;
    m_smallCaps       = o->m_smallCaps;
    m_align           = o->m_align;
    m_valign          = o->m_valign;
    m_styleRuns       = o->m_styleRuns;
    m_paragraphStyles = o->m_paragraphStyles;
    m_kerning         = o->m_kerning;
    m_tabWidth        = o->m_tabWidth;
    m_tsume           = o->m_tsume;
    m_fauxBold        = o->m_fauxBold;
    m_fauxItalic      = o->m_fauxItalic;
    m_underline       = o->m_underline;
    m_superscript     = o->m_superscript;
    m_subscript       = o->m_subscript;
    m_rightToLeft     = o->m_rightToLeft;
    m_backgroundEnabled = o->m_backgroundEnabled;
    m_backgroundColor = o->m_backgroundColor;
    m_backgroundPadding = o->m_backgroundPadding;
    m_maskWithText    = o->m_maskWithText;
    m_linkedStyleName = o->m_linkedStyleName;
    m_tracking        = o->m_tracking;
    m_leading         = o->m_leading;
    m_baselineShift   = o->m_baselineShift;
    m_boxWidth        = o->m_boxWidth;
    m_boxHeight       = o->m_boxHeight;
    m_useParagraphBox = o->m_useParagraphBox;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ShapeLayer
// ═════════════════════════════════════════════════════════════════════════════

ShapeLayer::ShapeLayer()
    : GraphicLayer(GraphicLayerType::Shape)
{
    m_name = "Shape";
}

std::unique_ptr<GraphicLayer> ShapeLayer::clone() const
{
    auto copy = std::make_unique<ShapeLayer>();

    // Base
    copy->m_name    = m_name;
    copy->m_visible = m_visible;
    copy->m_locked  = m_locked;
    copy->m_transform.posX     = m_transform.posX;
    copy->m_transform.posY     = m_transform.posY;
    copy->m_transform.scaleX   = m_transform.scaleX;
    copy->m_transform.scaleY   = m_transform.scaleY;
    copy->m_transform.rotation = m_transform.rotation;
    copy->m_transform.anchorX  = m_transform.anchorX;
    copy->m_transform.anchorY  = m_transform.anchorY;
    copy->m_transform.opacity  = m_transform.opacity;
    copy->m_appearance = m_appearance;

    // Shape-specific
    copy->m_shape        = m_shape;
    copy->m_width        = m_width;
    copy->m_height       = m_height;
    copy->m_cornerRadius = m_cornerRadius;
    copy->m_fillColor    = m_fillColor;

    return copy;
}

void ShapeLayer::assignStateFrom(const GraphicLayer& other)
{
    GraphicLayer::assignStateFrom(other);
    const auto* o = dynamic_cast<const ShapeLayer*>(&other);
    if (!o) return;
    m_shape        = o->m_shape;
    m_width        = o->m_width;
    m_height       = o->m_height;
    m_cornerRadius = o->m_cornerRadius;
    m_fillColor    = o->m_fillColor;
}

} // namespace rt
