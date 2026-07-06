/*
 * test_mask_raster — Premiere-parity mask rasterizer + OpacityMask model.
 *
 * Covers the v30 mask work:
 *  - additive (union) combine, inversion, mask opacity
 *  - distance-based feather/expansion for every shape
 *  - TRUE bezier curve rasterization (curves, not vertex polygons)
 *  - Mask Path keyframe interpolation + stopwatch write semantics
 *  - render-state hashing (rasterization cache key)
 *
 * rasterizeMasks is pure CPU (no Vulkan) but lives in roundtable_gpu, so
 * this test links the gpu lib like test_media_source_service does.
 */

#include <gtest/gtest.h>

#include "CompositeServiceBlend.h"
#include "timeline/OpacityMask.h"

#include <cmath>

using namespace rt;

namespace {

constexpr uint32_t W = 128;
constexpr uint32_t H = 128;

/// Alpha at pixel (all four channels are equal; use channel 0).
uint8_t alphaAt(const std::vector<uint8_t>& px, uint32_t x, uint32_t y)
{
    return px[(static_cast<size_t>(y) * W + x) * 4];
}

MaskRenderState makeEllipse(float cx, float cy, float w, float h)
{
    MaskRenderState s;
    s.shape = MaskShape::Ellipse;
    s.geometry.centerX = cx;
    s.geometry.centerY = cy;
    s.geometry.width = w;
    s.geometry.height = h;
    return s;
}

MaskRenderState makeRect(float cx, float cy, float w, float h)
{
    MaskRenderState s = makeEllipse(cx, cy, w, h);
    s.shape = MaskShape::Rectangle;
    return s;
}

} // namespace

TEST(MaskRaster, EllipseInsideOutside)
{
    auto px = rasterizeMasks({makeEllipse(0.5f, 0.5f, 0.5f, 0.5f)}, W, H);
    EXPECT_EQ(alphaAt(px, 64, 64), 255);   // center
    EXPECT_EQ(alphaAt(px, 88, 64), 255);   // inside (r=32px, offset 24)
    EXPECT_EQ(alphaAt(px, 4, 4), 0);       // far corner
    EXPECT_EQ(alphaAt(px, 110, 64), 0);    // outside on axis (offset 46)
}

TEST(MaskRaster, UnionCombine_PremiereSemantics)
{
    // Two disjoint rectangles: BOTH regions visible (additive/union),
    // not their (empty) intersection as the old multiply produced.
    auto px = rasterizeMasks(
        {makeRect(0.25f, 0.5f, 0.2f, 0.2f), makeRect(0.75f, 0.5f, 0.2f, 0.2f)},
        W, H);
    EXPECT_EQ(alphaAt(px, 32, 64), 255);   // center of rect A
    EXPECT_EQ(alphaAt(px, 96, 64), 255);   // center of rect B
    EXPECT_EQ(alphaAt(px, 64, 64), 0);     // between them
}

TEST(MaskRaster, InvertedCutsHole)
{
    auto inv = makeEllipse(0.5f, 0.5f, 0.5f, 0.5f);
    inv.inverted = true;
    auto px = rasterizeMasks({inv}, W, H);
    EXPECT_EQ(alphaAt(px, 64, 64), 0);     // hole in the middle
    EXPECT_EQ(alphaAt(px, 4, 4), 255);     // everything else opaque
}

TEST(MaskRaster, MaskOpacityScalesContribution)
{
    auto half = makeRect(0.5f, 0.5f, 0.5f, 0.5f);
    half.maskOpacity = 0.5f;
    auto px = rasterizeMasks({half}, W, H);
    EXPECT_NEAR(alphaAt(px, 64, 64), 128, 4);
}

TEST(MaskRaster, FeatherIsTwoSidedGradient)
{
    auto soft = makeRect(0.5f, 0.5f, 0.5f, 0.5f);
    soft.feather = 16.0f;
    auto px = rasterizeMasks({soft}, W, H);
    // Rect edge at x = 96 (center 64 + half-width 32).
    const uint8_t atEdge   = alphaAt(px, 96, 64);
    const uint8_t inside   = alphaAt(px, 90, 64);
    const uint8_t outside  = alphaAt(px, 102, 64);
    const uint8_t deepIn   = alphaAt(px, 64, 64);
    const uint8_t farOut   = alphaAt(px, 120, 64);
    EXPECT_NEAR(atEdge, 128, 48);          // ~half at the edge (centered ramp)
    EXPECT_GT(inside, atEdge);
    EXPECT_GT(atEdge, outside);
    EXPECT_EQ(deepIn, 255);
    EXPECT_EQ(farOut, 0);
}

TEST(MaskRaster, ExpansionGrowsAndShrinks)
{
    // Rect half-width 32px; sample 6px outside the base edge.
    auto grown = makeRect(0.5f, 0.5f, 0.5f, 0.5f);
    grown.expansion = 12.0f;
    auto px = rasterizeMasks({grown}, W, H);
    EXPECT_GT(alphaAt(px, 102, 64), 200);  // outside base edge, inside expanded

    auto shrunk = makeRect(0.5f, 0.5f, 0.5f, 0.5f);
    shrunk.expansion = -12.0f;
    px = rasterizeMasks({shrunk}, W, H);
    EXPECT_LT(alphaAt(px, 90, 64), 60);    // inside base edge, outside shrunk
}

TEST(MaskRaster, BezierRasterizesTrueCurves)
{
    // 4-vertex cubic circle approximation (kappa tangents), radius 0.25.
    constexpr float k = 0.5522847498f;
    constexpr float r = 0.25f;
    MaskRenderState s;
    s.shape = MaskShape::FreeDrawBezier;
    s.geometry.vertices = {
        {0.5f + r, 0.5f,      0, -k * r, 0,  k * r},   // right
        {0.5f, 0.5f + r,   k * r, 0, -k * r, 0},        // bottom
        {0.5f - r, 0.5f,      0,  k * r, 0, -k * r},    // left
        {0.5f, 0.5f - r,  -k * r, 0,  k * r, 0},        // top
    };
    auto px = rasterizeMasks({s}, W, H);

    // A point at 45°, 27px from center: INSIDE the true circle (r = 32px)
    // but OUTSIDE the straight-line polygon through the 4 vertices (its
    // 45° boundary is at only ~22.6px). The old rasterizer ignored the
    // tangents and failed exactly here.
    EXPECT_EQ(alphaAt(px, 83, 83), 255);
    EXPECT_EQ(alphaAt(px, 64, 64), 255);   // center
    EXPECT_EQ(alphaAt(px, 110, 110), 0);   // outside the circle
}

TEST(MaskRaster, BezierFeatherAndExpansionApply)
{
    // Straight-edged bezier square (zero tangents), half-size 32px.
    MaskRenderState s;
    s.shape = MaskShape::FreeDrawBezier;
    s.geometry.vertices = {
        {0.25f, 0.25f, 0, 0, 0, 0},
        {0.75f, 0.25f, 0, 0, 0, 0},
        {0.75f, 0.75f, 0, 0, 0, 0},
        {0.25f, 0.75f, 0, 0, 0, 0},
    };
    s.feather = 16.0f;
    auto px = rasterizeMasks({s}, W, H);
    // Feather previously did NOTHING for bezier masks. Edge at x=96.
    EXPECT_GT(alphaAt(px, 90, 64), alphaAt(px, 96, 64));
    EXPECT_GT(alphaAt(px, 96, 64), alphaAt(px, 102, 64));
    EXPECT_GT(alphaAt(px, 102, 64), 0);    // soft skirt outside the edge

    s.feather = 0.0f;
    s.expansion = 12.0f;
    px = rasterizeMasks({s}, W, H);
    EXPECT_GT(alphaAt(px, 102, 64), 200);  // expansion now applies too
}

TEST(MaskRaster, HashChangesWithStateOnly)
{
    std::vector<MaskRenderState> a{makeEllipse(0.5f, 0.5f, 0.5f, 0.5f)};
    std::vector<MaskRenderState> b = a;
    EXPECT_EQ(hashMaskStates(a, W, H), hashMaskStates(b, W, H));

    b[0].geometry.centerX += 0.01f;
    EXPECT_NE(hashMaskStates(a, W, H), hashMaskStates(b, W, H));

    b = a;
    b[0].feather = 4.0f;
    EXPECT_NE(hashMaskStates(a, W, H), hashMaskStates(b, W, H));

    EXPECT_NE(hashMaskStates(a, W, H), hashMaskStates(a, W / 2, H / 2));
}

// ─── OpacityMask model (Mask Path keyframes + stopwatch writes) ─────────────

TEST(OpacityMaskModel, GeometryInterpolatesBetweenPathKeys)
{
    OpacityMask m;
    m.shape = MaskShape::Ellipse;
    m.pathAnimated = true;
    MaskGeometry g0; g0.centerX = 0.2f; g0.centerY = 0.5f;
    MaskGeometry g1; g1.centerX = 0.6f; g1.centerY = 0.5f;
    m.addPathKey(0, g0);
    m.addPathKey(1000, g1);

    EXPECT_FLOAT_EQ(m.geometryAt(0).centerX, 0.2f);
    EXPECT_FLOAT_EQ(m.geometryAt(1000).centerX, 0.6f);
    EXPECT_NEAR(m.geometryAt(500).centerX, 0.4f, 1e-4f);
    // Constant extrapolation outside the keyed range
    EXPECT_FLOAT_EQ(m.geometryAt(-100).centerX, 0.2f);
    EXPECT_FLOAT_EQ(m.geometryAt(5000).centerX, 0.6f);
}

TEST(OpacityMaskModel, VertexPathsInterpolatePairwise)
{
    OpacityMask m;
    m.shape = MaskShape::FreeDrawBezier;
    m.pathAnimated = true;
    MaskGeometry g0, g1;
    g0.vertices = {{0.1f, 0.1f, 0, 0, 0, 0}, {0.3f, 0.1f, 0, 0, 0, 0},
                   {0.2f, 0.3f, 0, 0, 0, 0}};
    g1 = g0;
    for (auto& v : g1.vertices) v.x += 0.2f;
    m.addPathKey(0, g0);
    m.addPathKey(100, g1);

    auto mid = m.geometryAt(50);
    ASSERT_EQ(mid.vertices.size(), 3u);
    EXPECT_NEAR(mid.vertices[0].x, 0.2f, 1e-4f);
    EXPECT_NEAR(mid.vertices[1].x, 0.4f, 1e-4f);
}

TEST(OpacityMaskModel, WriteGeometryStopwatchSemantics)
{
    OpacityMask m;
    // Static: writes update the base path, no keyframes appear.
    MaskGeometry g; g.centerX = 0.7f;
    m.writeGeometry(500, g);
    EXPECT_TRUE(m.pathKeys.empty());
    EXPECT_FLOAT_EQ(m.base.centerX, 0.7f);

    // Animated: writes record/update a keyframe at the given time.
    m.pathAnimated = true;
    m.writeGeometry(500, g);
    ASSERT_EQ(m.pathKeys.size(), 1u);
    EXPECT_EQ(m.pathKeys[0].time, 500);

    MaskGeometry g2; g2.centerX = 0.9f;
    m.writeGeometry(500, g2);              // same time → update, not insert
    ASSERT_EQ(m.pathKeys.size(), 1u);
    EXPECT_FLOAT_EQ(m.pathKeys[0].geometry.centerX, 0.9f);

    m.writeGeometry(800, g);               // new time → new key, sorted
    ASSERT_EQ(m.pathKeys.size(), 2u);
    EXPECT_EQ(m.pathKeys[1].time, 800);
}

TEST(OpacityMaskModel, EvalRenderStateUsesTracks)
{
    OpacityMask m;
    m.shape = MaskShape::Rectangle;
    m.feather.addKeyframe(0, 0.0f);
    m.feather.addKeyframe(100, 20.0f);
    m.maskOpacity.setDefaultValue(0.75f);

    auto s = m.evalRenderState(50);
    EXPECT_NEAR(s.feather, 10.0f, 1e-3f);
    EXPECT_FLOAT_EQ(s.maskOpacity, 0.75f);
    EXPECT_EQ(s.shape, MaskShape::Rectangle);
}
