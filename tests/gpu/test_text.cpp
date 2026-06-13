/*
 * test_text.cpp — Unit tests for Step 20: GPU Text Renderer.
 *
 * Tests:
 *   GlyphAtlasTest — Atlas management with manual glyphs
 *   TextLayoutTest — Text measurement and positioning
 *   TextVertexTest — Vertex generation for GPU rendering
 *   TextRendererTest — Top-level API and utilities
 */

#include "TextRenderer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

// ── Helper: build a test atlas with simple monospace-like glyphs ─────────

void populateTestAtlas(rt::GlyphAtlas& atlas)
{
    // Create font 0 with fixed metrics
    atlas.ensureFont(0);
    rt::FontMetrics metrics;
    metrics.ascender   = 40.0f;
    metrics.descender  = -10.0f;
    metrics.lineHeight = 55.0f;     // ascender - descender + 5 gap
    metrics.emSize     = 48.0f;
    atlas.setManualMetrics(0, metrics);

    // Add ASCII printable glyphs (32–126) with uniform advance
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        rt::GlyphInfo g;
        g.codepoint = cp;
        g.atlasX    = static_cast<float>((cp - 32) % 16) * 32.0f;
        g.atlasY    = static_cast<float>((cp - 32) / 16) * 48.0f;
        g.atlasW    = 24.0f;
        g.atlasH    = 40.0f;
        g.bearingX  = 2.0f;
        g.bearingY  = 36.0f;        // baseline to top
        g.advance   = 28.0f;        // monospace width
        g.sdfPadding = 4.0f;
        g.valid     = true;
        atlas.addManualGlyph(0, cp, g);
    }

    // Space (cp 32) should have no visual (atlasW = 0)
    rt::GlyphInfo space;
    space.codepoint = 32;
    space.advance   = 14.0f;
    space.atlasW    = 0;
    space.atlasH    = 0;
    space.valid     = false;
    atlas.addManualGlyph(0, 32, space);
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
// GlyphAtlasTest
// ═════════════════════════════════════════════════════════════════════════════

class GlyphAtlasTest : public ::testing::Test
{
protected:
    rt::GlyphAtlas atlas;
};

TEST_F(GlyphAtlasTest, DefaultConfig)
{
    EXPECT_EQ(atlas.atlasWidth(), 1024u);
    EXPECT_EQ(atlas.atlasHeight(), 1024u);
    EXPECT_NE(atlas.atlasPixels(), nullptr);
}

TEST_F(GlyphAtlasTest, SetConfig)
{
    rt::GlyphAtlasConfig cfg;
    cfg.atlasWidth  = 512;
    cfg.atlasHeight = 512;
    atlas.setConfig(cfg);

    EXPECT_EQ(atlas.atlasWidth(), 512u);
    EXPECT_EQ(atlas.atlasHeight(), 512u);
}

TEST_F(GlyphAtlasTest, EnsureFont)
{
    EXPECT_EQ(atlas.fontCount(), 0u);
    atlas.ensureFont(0);
    EXPECT_EQ(atlas.fontCount(), 1u);
    atlas.ensureFont(3);
    EXPECT_EQ(atlas.fontCount(), 4u);
}

TEST_F(GlyphAtlasTest, ManualGlyph_addAndGet)
{
    atlas.ensureFont(0);

    rt::GlyphInfo g;
    g.codepoint = 'A';
    g.atlasX = 10;
    g.atlasY = 20;
    g.atlasW = 24;
    g.atlasH = 32;
    g.advance = 28;
    g.valid = true;
    atlas.addManualGlyph(0, 'A', g);

    const auto& result = atlas.getGlyph(0, 'A');
    EXPECT_EQ(result.codepoint, static_cast<uint32_t>('A'));
    EXPECT_FLOAT_EQ(result.atlasX, 10.0f);
    EXPECT_FLOAT_EQ(result.atlasW, 24.0f);
    EXPECT_FLOAT_EQ(result.advance, 28.0f);
    EXPECT_TRUE(result.valid);
}

TEST_F(GlyphAtlasTest, ManualMetrics)
{
    atlas.ensureFont(0);

    rt::FontMetrics m;
    m.ascender   = 40.0f;
    m.descender  = -10.0f;
    m.lineHeight = 55.0f;
    m.emSize     = 48.0f;
    atlas.setManualMetrics(0, m);

    auto result = atlas.getMetrics(0);
    EXPECT_FLOAT_EQ(result.ascender, 40.0f);
    EXPECT_FLOAT_EQ(result.descender, -10.0f);
    EXPECT_FLOAT_EQ(result.lineHeight, 55.0f);
    EXPECT_FLOAT_EQ(result.emSize, 48.0f);
}

TEST_F(GlyphAtlasTest, MissingFont_returnsFallback)
{
    // Font 99 doesn't exist → fallback glyph
    const auto& g = atlas.getGlyph(99, 'X');
    EXPECT_FALSE(g.valid);
}

TEST_F(GlyphAtlasTest, MissingGlyph_returnsFallback)
{
    atlas.ensureFont(0);
    // Codepoint not added → fallback
    const auto& g = atlas.getGlyph(0, 0x1F600);  // emoji
    EXPECT_FALSE(g.valid);
}

TEST_F(GlyphAtlasTest, DirtyFlag)
{
    EXPECT_TRUE(atlas.isDirty());   // Dirty after construction
    atlas.clearDirty();
    EXPECT_FALSE(atlas.isDirty());
}

TEST_F(GlyphAtlasTest, MultipleGlyphs_sameFont)
{
    atlas.ensureFont(0);

    for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) {
        rt::GlyphInfo g;
        g.codepoint = cp;
        g.advance = 20.0f;
        g.valid = true;
        atlas.addManualGlyph(0, cp, g);
    }

    EXPECT_TRUE(atlas.getGlyph(0, 'A').valid);
    EXPECT_TRUE(atlas.getGlyph(0, 'Z').valid);
    EXPECT_FLOAT_EQ(atlas.getGlyph(0, 'M').advance, 20.0f);
}

TEST_F(GlyphAtlasTest, MultipleFonts)
{
    atlas.ensureFont(0);
    atlas.ensureFont(1);

    rt::GlyphInfo g0;
    g0.codepoint = 'A';
    g0.advance = 20.0f;
    g0.valid = true;

    rt::GlyphInfo g1;
    g1.codepoint = 'A';
    g1.advance = 30.0f;
    g1.valid = true;

    atlas.addManualGlyph(0, 'A', g0);
    atlas.addManualGlyph(1, 'A', g1);

    EXPECT_FLOAT_EQ(atlas.getGlyph(0, 'A').advance, 20.0f);
    EXPECT_FLOAT_EQ(atlas.getGlyph(1, 'A').advance, 30.0f);
}

TEST_F(GlyphAtlasTest, MetricsForMissingFont)
{
    auto m = atlas.getMetrics(42);
    EXPECT_FLOAT_EQ(m.ascender, 0.0f);
    EXPECT_FLOAT_EQ(m.lineHeight, 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// TextLayoutTest
// ═════════════════════════════════════════════════════════════════════════════

class TextLayoutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        populateTestAtlas(atlas);
    }

    rt::GlyphAtlas atlas;
    rt::TextLayoutOptions opts;
};

TEST_F(TextLayoutTest, EmptyText)
{
    auto layout = rt::TextRenderer::layoutText("", atlas, opts);
    EXPECT_TRUE(layout.glyphs.empty());
    EXPECT_FLOAT_EQ(layout.width, 0.0f);
}

TEST_F(TextLayoutTest, SingleChar)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    ASSERT_EQ(layout.glyphs.size(), 1u);
    EXPECT_EQ(layout.glyphs[0].glyph.codepoint, static_cast<uint32_t>('A'));
    EXPECT_GT(layout.width, 0.0f);
    EXPECT_EQ(layout.lineCount, 1);
}

TEST_F(TextLayoutTest, SimpleWord)
{
    auto layout = rt::TextRenderer::layoutText("Hello", atlas, opts);
    ASSERT_EQ(layout.glyphs.size(), 5u);
    EXPECT_EQ(layout.lineCount, 1);

    // Each glyph should be further right than the previous
    for (size_t i = 1; i < layout.glyphs.size(); ++i) {
        EXPECT_GT(layout.glyphs[i].x, layout.glyphs[i - 1].x);
    }
}

TEST_F(TextLayoutTest, MeasureText_width)
{
    auto size = rt::TextRenderer::measureText("ABC", atlas, opts);
    // 3 glyphs × 28px advance = 84 at font size = emSize (48)
    EXPECT_NEAR(size.x, 84.0f, 5.0f);
    EXPECT_GT(size.y, 0.0f);
}

TEST_F(TextLayoutTest, MeasureText_height)
{
    auto size1 = rt::TextRenderer::measureText("A", atlas, opts);
    auto size2 = rt::TextRenderer::measureText("A\nB", atlas, opts);
    EXPECT_GT(size2.y, size1.y);
}

TEST_F(TextLayoutTest, Alignment_left)
{
    opts.hAlign = rt::TextAlign::Left;
    auto layout = rt::TextRenderer::layoutText("AB", atlas, opts);
    // Left-aligned: first glyph near x=0
    EXPECT_NEAR(layout.glyphs[0].x, 2.0f, 5.0f);  // bearingX ≈ 2
}

TEST_F(TextLayoutTest, Alignment_center)
{
    opts.hAlign = rt::TextAlign::Center;
    auto layout = rt::TextRenderer::layoutText("AB", atlas, opts);
    // Center-aligned: first glyph shifted right compared to width
    float lineWidth = 2 * 28.0f;  // 2 glyphs × 28 advance
    float expectedOffset = (layout.width - lineWidth) * 0.5f;
    EXPECT_NEAR(layout.glyphs[0].x, expectedOffset + 2.0f, 2.0f);
}

TEST_F(TextLayoutTest, Alignment_right)
{
    opts.hAlign = rt::TextAlign::Right;
    auto layout = rt::TextRenderer::layoutText("AB", atlas, opts);
    // Right-aligned: last glyph should end near layout.width
    float lastGlyphRight = layout.glyphs.back().x + layout.glyphs.back().glyph.advance;
    EXPECT_NEAR(lastGlyphRight, layout.width, 5.0f);
}

TEST_F(TextLayoutTest, Tracking)
{
    auto layout1 = rt::TextRenderer::measureText("AB", atlas, opts);

    opts.tracking = 10.0f;
    auto layout2 = rt::TextRenderer::measureText("AB", atlas, opts);

    // Extra tracking → wider
    EXPECT_GT(layout2.x, layout1.x);
    EXPECT_NEAR(layout2.x - layout1.x, 20.0f, 5.0f);  // 2 chars × 10px
}

TEST_F(TextLayoutTest, MultiLine_newline)
{
    auto layout = rt::TextRenderer::layoutText("AB\nCD", atlas, opts);
    EXPECT_EQ(layout.lineCount, 2);
    EXPECT_EQ(layout.glyphs.size(), 4u);

    // Second line glyphs should have higher Y than first line
    EXPECT_GT(layout.glyphs[2].y, layout.glyphs[0].y);
}

TEST_F(TextLayoutTest, MultiLine_wrap)
{
    // Max width = 60, each glyph advance = 28, so ~2 glyphs per line
    opts.maxWidth = 60.0f;
    auto layout = rt::TextRenderer::layoutText("ABCD", atlas, opts);
    EXPECT_GE(layout.lineCount, 2);
}

TEST_F(TextLayoutTest, ScaleFromFontSize)
{
    // Default fontSize = 48 = emSize → scale = 1.0
    auto layout1 = rt::TextRenderer::measureText("A", atlas, opts);

    // fontSize = 24 → scale = 0.5 → half width
    opts.fontSize = 24.0f;
    auto layout2 = rt::TextRenderer::measureText("A", atlas, opts);

    EXPECT_NEAR(layout2.x, layout1.x * 0.5f, 2.0f);
}

TEST_F(TextLayoutTest, Baseline)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    // Baseline should be at ascender (40 at scale=1)
    EXPECT_NEAR(layout.baseline, 40.0f, 2.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// TextVertexTest
// ═════════════════════════════════════════════════════════════════════════════

class TextVertexTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        populateTestAtlas(atlas);
    }

    rt::GlyphAtlas atlas;
    rt::TextLayoutOptions opts;
};

TEST_F(TextVertexTest, EmptyLayout)
{
    rt::TextLayout layout;
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);
    EXPECT_TRUE(verts.empty());
}

TEST_F(TextVertexTest, SingleGlyph_produces6Vertices)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);
    EXPECT_EQ(verts.size(), 6u);
}

TEST_F(TextVertexTest, MultiGlyph_produces6PerGlyph)
{
    auto layout = rt::TextRenderer::layoutText("Hello", atlas, opts);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);
    // 5 glyphs × 6 vertices = 30
    EXPECT_EQ(verts.size(), 30u);
}

TEST_F(TextVertexTest, VertexColor)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    uint32_t color = rt::TextRenderer::packColor(255, 0, 128, 200);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, color);

    for (const auto& v : verts) {
        EXPECT_EQ(v.color, color);
    }
}

TEST_F(TextVertexTest, VertexUVs_normalized)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);

    for (const auto& v : verts) {
        EXPECT_GE(v.u, 0.0f);
        EXPECT_LE(v.u, 1.0f);
        EXPECT_GE(v.v, 0.0f);
        EXPECT_LE(v.v, 1.0f);
    }
}

TEST_F(TextVertexTest, OriginOffset)
{
    auto layout = rt::TextRenderer::layoutText("A", atlas, opts);
    auto verts0 = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF, 0, 0);
    auto verts1 = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF, 100, 50);

    ASSERT_EQ(verts0.size(), verts1.size());
    for (size_t i = 0; i < verts0.size(); ++i) {
        EXPECT_NEAR(verts1[i].x - verts0[i].x, 100.0f, 0.01f);
        EXPECT_NEAR(verts1[i].y - verts0[i].y, 50.0f, 0.01f);
    }
}

TEST_F(TextVertexTest, SpaceProducesNoVertices)
{
    // Space has atlasW=0, should be skipped in vertex generation
    auto layout = rt::TextRenderer::layoutText(" ", atlas, opts);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);
    EXPECT_EQ(verts.size(), 0u);
}

TEST_F(TextVertexTest, MixedText_spacesSkipped)
{
    // "A B" → 3 glyphs laid out, but space produces no vertices
    auto layout = rt::TextRenderer::layoutText("A B", atlas, opts);
    auto verts = rt::TextRenderer::layoutToVertices(layout, atlas, 0xFFFFFFFF);
    // 2 visible glyphs × 6 = 12
    EXPECT_EQ(verts.size(), 12u);
}

// ═════════════════════════════════════════════════════════════════════════════
// TextRendererTest
// ═════════════════════════════════════════════════════════════════════════════

class TextRendererTest : public ::testing::Test {};

TEST_F(TextRendererTest, Construction)
{
    rt::TextRenderer renderer;
    EXPECT_EQ(renderer.atlas().fontCount(), 0u);
}

TEST_F(TextRendererTest, AtlasAccess)
{
    rt::TextRenderer renderer;
    renderer.atlas().ensureFont(0);
    EXPECT_EQ(renderer.atlas().fontCount(), 1u);
}

TEST_F(TextRendererTest, PushConstantsSize)
{
    EXPECT_LE(sizeof(rt::TextPushConstants), 128u);
}

TEST_F(TextRendererTest, PackColor_bytes)
{
    uint32_t c = rt::TextRenderer::packColor(255, 128, 64, 200);
    float r, g, b, a;
    rt::TextRenderer::unpackColor(c, r, g, b, a);
    EXPECT_NEAR(r, 1.0f,   0.01f);
    EXPECT_NEAR(g, 0.502f, 0.01f);
    EXPECT_NEAR(b, 0.251f, 0.01f);
    EXPECT_NEAR(a, 0.784f, 0.01f);
}

TEST_F(TextRendererTest, PackColor_floats)
{
    uint32_t c = rt::TextRenderer::packColorF(1.0f, 0.5f, 0.25f, 0.75f);
    float r, g, b, a;
    rt::TextRenderer::unpackColor(c, r, g, b, a);
    EXPECT_NEAR(r, 1.0f,  0.01f);
    EXPECT_NEAR(g, 0.5f,  0.02f);
    EXPECT_NEAR(b, 0.25f, 0.02f);
    EXPECT_NEAR(a, 0.75f, 0.02f);
}

TEST_F(TextRendererTest, PackColor_clamps)
{
    // Out-of-range floats should clamp
    uint32_t c = rt::TextRenderer::packColorF(2.0f, -1.0f, 0.5f, 1.0f);
    float r, g, b, a;
    rt::TextRenderer::unpackColor(c, r, g, b, a);
    EXPECT_NEAR(r, 1.0f, 0.01f);
    EXPECT_NEAR(g, 0.0f, 0.01f);
}

TEST_F(TextRendererTest, PackColor_roundTrip)
{
    for (int i = 0; i < 256; ++i) {
        auto u8 = static_cast<uint8_t>(i);
        uint32_t c = rt::TextRenderer::packColor(u8, u8, u8, u8);
        float r, g, b, a;
        rt::TextRenderer::unpackColor(c, r, g, b, a);
        EXPECT_NEAR(r, static_cast<float>(i) / 255.0f, 0.004f);
    }
}

TEST_F(TextRendererTest, PackColor_white)
{
    uint32_t white = rt::TextRenderer::packColor(255, 255, 255, 255);
    EXPECT_EQ(white, 0xFFFFFFFF);
}

TEST_F(TextRendererTest, PackColor_red)
{
    uint32_t red = rt::TextRenderer::packColor(255, 0, 0, 255);
    // ABGR layout: A=0xFF, B=0x00, G=0x00, R=0xFF → 0xFF0000FF
    EXPECT_EQ(red, 0xFF0000FF);
}

TEST_F(TextRendererTest, VertexStruct_size)
{
    // 2 floats position + 2 floats UV + 1 uint32 color = 20 bytes
    EXPECT_EQ(sizeof(rt::TextVertex), 20u);
}

TEST_F(TextRendererTest, GlyphInfoStruct)
{
    rt::GlyphInfo g;
    EXPECT_EQ(g.codepoint, 0u);
    EXPECT_FALSE(g.valid);
    EXPECT_FLOAT_EQ(g.advance, 0.0f);
}

TEST_F(TextRendererTest, FullPipeline)
{
    // End-to-end: create renderer, populate atlas, layout, vertices
    rt::TextRenderer renderer;
    populateTestAtlas(renderer.atlas());

    rt::TextLayoutOptions opts;
    opts.hAlign = rt::TextAlign::Center;

    auto layout = rt::TextRenderer::layoutText("Test", renderer.atlas(), opts);
    EXPECT_EQ(layout.glyphs.size(), 4u);

    uint32_t color = rt::TextRenderer::packColorF(1, 1, 1, 1);
    auto verts = rt::TextRenderer::layoutToVertices(layout, renderer.atlas(), color);
    EXPECT_EQ(verts.size(), 24u);  // 4 glyphs × 6 vertices

    // All vertices should have correct color
    for (const auto& v : verts)
        EXPECT_EQ(v.color, color);
}
