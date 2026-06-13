/*
 * test_viewport.cpp — Tests for Step 11: Viewport Layout & Gizmo Math
 *
 * Tests the pure-math viewport logic (no Qt or GPU dependency):
 *   1. ViewportLayout: zoom, pan, aspect ratio, letterboxing, coordinate
 *      conversion, safe areas, fit/fill zoom calculations.
 *   2. GizmoMath: handle positions, hit testing, drag (translate/scale/rotate),
 *      aspect-ratio-constrained resize, cursor hints.
 *
 * All tests are CPU-only — no Vulkan or Qt needed.
 */

#include <gtest/gtest.h>

#include "ViewportLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

#include <cmath>

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

static constexpr float kEps = 0.01f; // tolerance for float comparisons

static bool nearEqual(float a, float b, float eps = kEps)
{
    return std::abs(a - b) <= eps;
}

static bool nearEqual(glm::vec2 a, glm::vec2 b, float eps = kEps)
{
    return nearEqual(a.x, b.x, eps) && nearEqual(a.y, b.y, eps);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, DefaultValues)
{
    rt::ViewportLayout layout;
    EXPECT_EQ(layout.contentWidth(), 1920u);
    EXPECT_EQ(layout.contentHeight(), 1080u);
    EXPECT_EQ(layout.widgetWidth(), 800u);
    EXPECT_EQ(layout.widgetHeight(), 450u);
    EXPECT_EQ(layout.zoomMode(), rt::ZoomMode::FitToWindow);
    EXPECT_FLOAT_EQ(layout.zoomLevel(), 1.0f);
    EXPECT_FLOAT_EQ(layout.panOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(layout.panOffset().y, 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Aspect ratio
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, ContentAspectRatio)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    EXPECT_TRUE(nearEqual(layout.contentAspect(), 16.0f / 9.0f));

    layout.setContentSize(1080, 1920);
    EXPECT_TRUE(nearEqual(layout.contentAspect(), 9.0f / 16.0f));

    layout.setContentSize(1000, 1000);
    EXPECT_FLOAT_EQ(layout.contentAspect(), 1.0f);
}

TEST(ViewportLayout, WidgetAspectRatio)
{
    rt::ViewportLayout layout;
    layout.setWidgetSize(800, 600);
    EXPECT_TRUE(nearEqual(layout.widgetAspect(), 4.0f / 3.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — FitToWindow zoom
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, FitToWindowExactMatch)
{
    // Widget is exactly 2x the content
    rt::ViewportLayout layout;
    layout.setContentSize(960, 540);
    layout.setWidgetSize(1920, 1080);
    EXPECT_FLOAT_EQ(layout.fitZoom(), 2.0f);
    EXPECT_FLOAT_EQ(layout.effectiveZoom(), 2.0f);
}

TEST(ViewportLayout, FitToWindowLetterbox)
{
    // Wide content in a tall widget → letterbox (black bars top/bottom)
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080); // 16:9
    layout.setWidgetSize(800, 800);    // 1:1
    // Fit: min(800/1920, 800/1080) = min(0.4167, 0.7407) = 0.4167
    float expected = 800.0f / 1920.0f;
    EXPECT_TRUE(nearEqual(layout.fitZoom(), expected));
}

TEST(ViewportLayout, FitToWindowPillarbox)
{
    // Tall content in a wide widget → pillarbox (black bars left/right)
    rt::ViewportLayout layout;
    layout.setContentSize(1080, 1920); // 9:16
    layout.setWidgetSize(800, 800);    // 1:1
    float expected = 800.0f / 1920.0f;
    EXPECT_TRUE(nearEqual(layout.fitZoom(), expected));
}

TEST(ViewportLayout, FillZoom)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(800, 800);
    float expected = 800.0f / 1080.0f;
    EXPECT_TRUE(nearEqual(layout.fillZoom(), expected));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Percentage zoom
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, PercentageZoom100)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(1.0f);
    EXPECT_FLOAT_EQ(layout.effectiveZoom(), 1.0f);
}

TEST(ViewportLayout, PercentageZoom200)
{
    rt::ViewportLayout layout;
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(2.0f);
    EXPECT_FLOAT_EQ(layout.effectiveZoom(), 2.0f);
}

TEST(ViewportLayout, ZoomClampMin)
{
    rt::ViewportLayout layout;
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(0.001f);
    EXPECT_FLOAT_EQ(layout.effectiveZoom(), rt::ViewportLayout::kMinZoom);
}

TEST(ViewportLayout, ZoomClampMax)
{
    rt::ViewportLayout layout;
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(100.0f);
    EXPECT_FLOAT_EQ(layout.effectiveZoom(), rt::ViewportLayout::kMaxZoom);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Content rect (letterboxing)
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, ContentRectFitExact)
{
    // Content exactly fills widget
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    auto rect = layout.contentRect();
    EXPECT_TRUE(nearEqual(rect.x, 0.0f));
    EXPECT_TRUE(nearEqual(rect.y, 0.0f));
    EXPECT_TRUE(nearEqual(rect.width, 1920.0f));
    EXPECT_TRUE(nearEqual(rect.height, 1080.0f));
}

TEST(ViewportLayout, ContentRectLetterbox)
{
    // 16:9 content in 1:1 widget → horizontal bars
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(800, 800);

    auto rect = layout.contentRect();
    // zoom = 800/1920 ≈ 0.4167
    float zoom = 800.0f / 1920.0f;
    float expectedW = 1920.0f * zoom; // = 800
    float expectedH = 1080.0f * zoom; // ≈ 450
    float expectedY = (800.0f - expectedH) * 0.5f; // ≈ 175

    EXPECT_TRUE(nearEqual(rect.x, 0.0f));
    EXPECT_TRUE(nearEqual(rect.y, expectedY));
    EXPECT_TRUE(nearEqual(rect.width, expectedW));
    EXPECT_TRUE(nearEqual(rect.height, expectedH));
}

TEST(ViewportLayout, ContentRectPillarbox)
{
    // 9:16 content in 1:1 widget → vertical bars
    rt::ViewportLayout layout;
    layout.setContentSize(1080, 1920);
    layout.setWidgetSize(800, 800);

    auto rect = layout.contentRect();
    float zoom = 800.0f / 1920.0f;
    float expectedW = 1080.0f * zoom;
    float expectedH = 1920.0f * zoom;
    float expectedX = (800.0f - expectedW) * 0.5f;

    EXPECT_TRUE(nearEqual(rect.x, expectedX));
    EXPECT_TRUE(nearEqual(rect.y, 0.0f));
    EXPECT_TRUE(nearEqual(rect.width, expectedW));
    EXPECT_TRUE(nearEqual(rect.height, expectedH));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Coordinate conversion
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, CoordConversionIdentity)
{
    // When content fills widget exactly at zoom 1.0
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    auto content = layout.widgetToContent(960.0f, 540.0f);
    EXPECT_TRUE(nearEqual(content, glm::vec2(960.0f, 540.0f)));

    auto widget = layout.contentToWidget(960.0f, 540.0f);
    EXPECT_TRUE(nearEqual(widget, glm::vec2(960.0f, 540.0f)));
}

TEST(ViewportLayout, CoordConversionRoundtrip)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(800, 600);

    // Pick a content point and verify roundtrip
    glm::vec2 original(500.0f, 300.0f);
    auto widget = layout.contentToWidget(original.x, original.y);
    auto back   = layout.widgetToContent(widget.x, widget.y);
    EXPECT_TRUE(nearEqual(back, original));
}

TEST(ViewportLayout, CoordConversionWithPan)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);
    layout.setPanOffset(100.0f, 50.0f);

    // Pan shifts the content rect
    auto rect = layout.contentRect();
    EXPECT_TRUE(nearEqual(rect.x, 100.0f));
    EXPECT_TRUE(nearEqual(rect.y, 50.0f));

    // Widget center (960, 540) should map to content (860, 490)
    auto content = layout.widgetToContent(960.0f, 540.0f);
    EXPECT_TRUE(nearEqual(content, glm::vec2(860.0f, 490.0f)));
}

TEST(ViewportLayout, CoordConversionWithZoom)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(2.0f);

    // At 2x zoom, content is 3840x2160 in widget space centered
    auto rect = layout.contentRect();
    EXPECT_TRUE(nearEqual(rect.width, 3840.0f));
    EXPECT_TRUE(nearEqual(rect.height, 2160.0f));

    // Widget center should map to content center
    auto content = layout.widgetToContent(960.0f, 540.0f);
    EXPECT_TRUE(nearEqual(content, glm::vec2(960.0f, 540.0f)));
}

TEST(ViewportLayout, NormalizedConversion)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    // Center = (0.5, 0.5) in normalized
    auto norm = layout.widgetToNormalized(960.0f, 540.0f);
    EXPECT_TRUE(nearEqual(norm, glm::vec2(0.5f, 0.5f)));

    auto widget = layout.normalizedToWidget(0.5f, 0.5f);
    EXPECT_TRUE(nearEqual(widget, glm::vec2(960.0f, 540.0f)));
}

TEST(ViewportLayout, NormalizedConversionCorners)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    auto topLeft = layout.widgetToNormalized(0.0f, 0.0f);
    EXPECT_TRUE(nearEqual(topLeft, glm::vec2(0.0f, 0.0f)));

    auto bottomRight = layout.widgetToNormalized(1920.0f, 1080.0f);
    EXPECT_TRUE(nearEqual(bottomRight, glm::vec2(1.0f, 1.0f)));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — isInsideContent
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, InsideContentFit)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(800, 800);

    auto rect = layout.contentRect();

    // Center should be inside
    EXPECT_TRUE(layout.isInsideContent(400.0f, 400.0f));

    // Top-left corner of content
    EXPECT_TRUE(layout.isInsideContent(rect.x + 1.0f, rect.y + 1.0f));

    // In the letterbox area (above content)
    EXPECT_FALSE(layout.isInsideContent(400.0f, rect.y - 5.0f));

    // Below content
    EXPECT_FALSE(layout.isInsideContent(400.0f, rect.y + rect.height + 5.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Pan
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, PanAddsToOffset)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    layout.pan(100.0f, 50.0f);
    auto pan = layout.panOffset();
    EXPECT_TRUE(nearEqual(pan.x, 100.0f));
    EXPECT_TRUE(nearEqual(pan.y, 50.0f));

    layout.pan(-50.0f, 25.0f);
    pan = layout.panOffset();
    EXPECT_TRUE(nearEqual(pan.x, 50.0f));
    EXPECT_TRUE(nearEqual(pan.y, 75.0f));
}

TEST(ViewportLayout, PanScaledByZoom)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(2.0f);

    // At 2x zoom, a 100-widget-pixel drag should be 50 content pixels
    layout.pan(100.0f, 100.0f);
    auto pan = layout.panOffset();
    EXPECT_TRUE(nearEqual(pan.x, 50.0f));
    EXPECT_TRUE(nearEqual(pan.y, 50.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — ZoomAt
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, ZoomAtSwitchesMode)
{
    rt::ViewportLayout layout;
    EXPECT_EQ(layout.zoomMode(), rt::ZoomMode::FitToWindow);

    layout.zoomAt(400.0f, 225.0f, 1.0f);
    EXPECT_EQ(layout.zoomMode(), rt::ZoomMode::Percentage);
}

TEST(ViewportLayout, ZoomAtCenter)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    // Get starting zoom
    float startZoom = layout.effectiveZoom(); // 1.0 (exact fit)

    // Zoom in at center
    layout.zoomAt(960.0f, 540.0f, 1.0f);
    float newZoom = layout.effectiveZoom();
    EXPECT_GT(newZoom, startZoom);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — ResetView
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, ResetViewRestoresDefaults)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(800, 600);
    layout.setZoomMode(rt::ZoomMode::Percentage);
    layout.setZoomLevel(3.0f);
    layout.setPanOffset(200.0f, -100.0f);

    layout.resetView();

    EXPECT_EQ(layout.zoomMode(), rt::ZoomMode::FitToWindow);
    EXPECT_FLOAT_EQ(layout.panOffset().x, 0.0f);
    EXPECT_FLOAT_EQ(layout.panOffset().y, 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Safe areas
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, SafeAreaContentTitleSafe)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);

    auto sa = layout.safeAreaContent(rt::SafeAreaType::TitleSafe);
    EXPECT_TRUE(nearEqual(sa.left, 192.0f));   // 10% of 1920
    EXPECT_TRUE(nearEqual(sa.top, 108.0f));    // 10% of 1080
    EXPECT_TRUE(nearEqual(sa.right, 1728.0f)); // 90% of 1920
    EXPECT_TRUE(nearEqual(sa.bottom, 972.0f)); // 90% of 1080
}

TEST(ViewportLayout, SafeAreaContentActionSafe)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);

    auto sa = layout.safeAreaContent(rt::SafeAreaType::ActionSafe);
    EXPECT_TRUE(nearEqual(sa.left, 96.0f));     // 5% of 1920
    EXPECT_TRUE(nearEqual(sa.top, 54.0f));      // 5% of 1080
    EXPECT_TRUE(nearEqual(sa.right, 1824.0f));  // 95% of 1920
    EXPECT_TRUE(nearEqual(sa.bottom, 1026.0f)); // 95% of 1080
}

TEST(ViewportLayout, SafeAreaCustomInset)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1000, 1000);

    auto sa = layout.safeAreaContent(rt::SafeAreaType::Custom, 0.15f);
    EXPECT_TRUE(nearEqual(sa.left, 150.0f));
    EXPECT_TRUE(nearEqual(sa.top, 150.0f));
    EXPECT_TRUE(nearEqual(sa.right, 850.0f));
    EXPECT_TRUE(nearEqual(sa.bottom, 850.0f));
}

TEST(ViewportLayout, SafeAreaWidth)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);

    auto sa = layout.safeAreaContent(rt::SafeAreaType::TitleSafe);
    EXPECT_TRUE(nearEqual(sa.width(), 1536.0f));  // 80% of 1920
    EXPECT_TRUE(nearEqual(sa.height(), 864.0f));  // 80% of 1080
}

TEST(ViewportLayout, SafeAreaWidget)
{
    // Safe area in widget coords when fit-to-window
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(1920, 1080);

    auto sa = layout.safeArea(rt::SafeAreaType::TitleSafe);
    // At 1:1 zoom, widget coords = content coords
    EXPECT_TRUE(nearEqual(sa.left, 192.0f));
    EXPECT_TRUE(nearEqual(sa.top, 108.0f));
    EXPECT_TRUE(nearEqual(sa.right, 1728.0f));
    EXPECT_TRUE(nearEqual(sa.bottom, 972.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — Edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportLayout, MinContentSize)
{
    rt::ViewportLayout layout;
    layout.setContentSize(0, 0);
    EXPECT_EQ(layout.contentWidth(), 1u);
    EXPECT_EQ(layout.contentHeight(), 1u);
}

TEST(ViewportLayout, MinWidgetSize)
{
    rt::ViewportLayout layout;
    layout.setWidgetSize(0, 0);
    EXPECT_EQ(layout.widgetWidth(), 1u);
    EXPECT_EQ(layout.widgetHeight(), 1u);
}

TEST(ViewportLayout, Constants)
{
    EXPECT_FLOAT_EQ(rt::ViewportLayout::kMinZoom, 0.05f);
    EXPECT_FLOAT_EQ(rt::ViewportLayout::kMaxZoom, 32.0f);
    EXPECT_FLOAT_EQ(rt::ViewportLayout::kTitleSafeInset, 0.10f);
    EXPECT_FLOAT_EQ(rt::ViewportLayout::kActionSafeInset, 0.05f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  ViewportLayout — ViewportRect contains
// ═════════════════════════════════════════════════════════════════════════════

TEST(ViewportRect, Contains)
{
    rt::ViewportRect rect{100.0f, 200.0f, 300.0f, 150.0f};
    EXPECT_TRUE(rect.contains(100.0f, 200.0f));
    EXPECT_TRUE(rect.contains(250.0f, 275.0f));
    EXPECT_FALSE(rect.contains(99.0f, 200.0f));
    EXPECT_FALSE(rect.contains(400.0f, 200.0f)); // at boundary: 100+300 = 400 (open)
    EXPECT_FALSE(rect.contains(100.0f, 350.0f)); // at boundary: 200+150 = 350 (open)
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Setup helper
// ═════════════════════════════════════════════════════════════════════════════

class GizmoMathTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        layout.setContentSize(1920, 1080);
        layout.setWidgetSize(1920, 1080);
        // At 1:1, content = widget coords

        gizmo.setLayout(&layout);
        gizmo.setConfig(rt::GizmoConfig{});
    }

    rt::ViewportLayout layout;
    rt::GizmoMath      gizmo;
};

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Handle positions
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, ComputeHandlesCenter)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    auto handles = gizmo.computeHandles(transform);

    // Center should be midpoint of the bounding box
    EXPECT_TRUE(nearEqual(handles.center, glm::vec2(300.0f, 350.0f)));
}

TEST_F(GizmoMathTest, ComputeHandlesCorners)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    auto handles = gizmo.computeHandles(transform);

    EXPECT_TRUE(nearEqual(handles.topLeft,     glm::vec2(100.0f, 200.0f)));
    EXPECT_TRUE(nearEqual(handles.topRight,    glm::vec2(500.0f, 200.0f)));
    EXPECT_TRUE(nearEqual(handles.bottomLeft,  glm::vec2(100.0f, 500.0f)));
    EXPECT_TRUE(nearEqual(handles.bottomRight, glm::vec2(500.0f, 500.0f)));
}

TEST_F(GizmoMathTest, ComputeHandlesEdges)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    auto handles = gizmo.computeHandles(transform);

    EXPECT_TRUE(nearEqual(handles.topEdge,    glm::vec2(300.0f, 200.0f)));
    EXPECT_TRUE(nearEqual(handles.bottomEdge, glm::vec2(300.0f, 500.0f)));
    EXPECT_TRUE(nearEqual(handles.leftEdge,   glm::vec2(100.0f, 350.0f)));
    EXPECT_TRUE(nearEqual(handles.rightEdge,  glm::vec2(500.0f, 350.0f)));
}

TEST_F(GizmoMathTest, RotationHandlesOutsideCorners)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    auto handles = gizmo.computeHandles(transform);

    // Rotation handles should be offset from corners towards outside
    // They should be farther from center than the corners
    glm::vec2 center(300.0f, 350.0f);

    float cornerDist = glm::length(handles.topLeft - center);
    float rotDist    = glm::length(handles.rotTopLeft - center);
    EXPECT_GT(rotDist, cornerDist);
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Hit testing
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, HitTestCorner)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Hit exactly on top-left corner
    auto hit = gizmo.hitTest(transform, 100.0f, 200.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::TopLeft);

    // Hit exactly on bottom-right corner
    hit = gizmo.hitTest(transform, 500.0f, 500.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::BottomRight);
}

TEST_F(GizmoMathTest, HitTestCenter)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Click in the middle of the box
    auto hit = gizmo.hitTest(transform, 300.0f, 350.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::Center);
}

TEST_F(GizmoMathTest, HitTestNone)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Click far outside
    auto hit = gizmo.hitTest(transform, 1800.0f, 900.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::None);
}

TEST_F(GizmoMathTest, HitTestEdge)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Hit on top edge midpoint
    auto hit = gizmo.hitTest(transform, 300.0f, 200.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::TopEdge);
}

TEST_F(GizmoMathTest, HitTestNearMiss)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Far outside all handles and the bounding box — definitely no hit
    auto hit = gizmo.hitTest(transform, 100.0f - 50.0f, 200.0f - 50.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::None);
}

TEST_F(GizmoMathTest, HitTestNearHit)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Just barely within the hit radius of top-left corner
    auto hit = gizmo.hitTest(transform, 100.0f + 3.0f, 200.0f + 3.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::TopLeft);
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Drag: translate
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, DragTranslate)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag center from (300, 350) to (400, 450) → delta = (100, 100) in widget/content space
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::Center,
                                   glm::vec2(300.0f, 350.0f),
                                   glm::vec2(400.0f, 450.0f));

    EXPECT_TRUE(nearEqual(result.position, glm::vec2(200.0f, 300.0f)));
    EXPECT_TRUE(nearEqual(result.size, transform.size)); // Size unchanged
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Drag: scale
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, DragScaleBottomRight)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag bottom-right from (500, 500) to (600, 600) → size grows by (100, 100)
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::BottomRight,
                                   glm::vec2(500.0f, 500.0f),
                                   glm::vec2(600.0f, 600.0f));

    EXPECT_TRUE(nearEqual(result.position, transform.position)); // Position unchanged
    EXPECT_TRUE(nearEqual(result.size, glm::vec2(500.0f, 400.0f)));
}

TEST_F(GizmoMathTest, DragScaleTopLeft)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag top-left from (100, 200) to (50, 150) → position moves, size grows
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::TopLeft,
                                   glm::vec2(100.0f, 200.0f),
                                   glm::vec2(50.0f, 150.0f));

    EXPECT_TRUE(nearEqual(result.position, glm::vec2(50.0f, 150.0f)));
    EXPECT_TRUE(nearEqual(result.size, glm::vec2(450.0f, 350.0f)));
}

TEST_F(GizmoMathTest, DragScaleRightEdge)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag right edge to the right by 100
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::RightEdge,
                                   glm::vec2(500.0f, 350.0f),
                                   glm::vec2(600.0f, 350.0f));

    EXPECT_TRUE(nearEqual(result.size.x, 500.0f));
    EXPECT_TRUE(nearEqual(result.size.y, 300.0f)); // Height unchanged
}

TEST_F(GizmoMathTest, DragScaleTopEdge)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 200.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag top edge up by 50
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::TopEdge,
                                   glm::vec2(300.0f, 200.0f),
                                   glm::vec2(300.0f, 150.0f));

    EXPECT_TRUE(nearEqual(result.position.y, 150.0f));
    EXPECT_TRUE(nearEqual(result.size.y, 350.0f));
    EXPECT_TRUE(nearEqual(result.size.x, 400.0f)); // Width unchanged
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Drag: constrained (shift) scale
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, DragScaleConstrainedBottomRight)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(0.0f, 0.0f);
    transform.size     = glm::vec2(400.0f, 200.0f); // 2:1 aspect

    // Drag bottom-right with shift held — should maintain 2:1 aspect
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::BottomRight,
                                   glm::vec2(400.0f, 200.0f),
                                   glm::vec2(600.0f, 200.0f), // Only move X
                                   true);

    float aspect = result.size.x / result.size.y;
    EXPECT_TRUE(nearEqual(aspect, 2.0f, 0.05f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Rotation
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, DragRotate)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 100.0f);
    transform.size     = glm::vec2(200.0f, 200.0f);
    transform.rotation = 0.0f;

    // Center of transform in widget space = (200, 200)
    // Drag from right (300, 200) to below (200, 300) → 90° clockwise
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::RotateTopRight,
                                   glm::vec2(300.0f, 200.0f),
                                   glm::vec2(200.0f, 300.0f));

    // Should be +90° (or near it)
    EXPECT_TRUE(nearEqual(std::abs(result.rotation), 90.0f, 1.0f));
}

TEST_F(GizmoMathTest, ComputeRotationZeroDrag)
{
    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 100.0f);
    transform.size     = glm::vec2(200.0f, 200.0f);
    transform.rotation = 45.0f;

    // Same start and end → no rotation change
    float rot = gizmo.computeRotation(transform,
                                       glm::vec2(300.0f, 200.0f),
                                       glm::vec2(300.0f, 200.0f));
    EXPECT_TRUE(nearEqual(rot, 45.0f));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — isNearHandle
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(GizmoMathTest, IsNearHandleExact)
{
    EXPECT_TRUE(gizmo.isNearHandle(glm::vec2(100.0f, 200.0f), glm::vec2(100.0f, 200.0f)));
}

TEST_F(GizmoMathTest, IsNearHandleJustInside)
{
    // Handle radius = handleSize/2 + hitPadding = 4 + 4 = 8
    EXPECT_TRUE(gizmo.isNearHandle(glm::vec2(107.0f, 200.0f), glm::vec2(100.0f, 200.0f)));
}

TEST_F(GizmoMathTest, IsNearHandleJustOutside)
{
    // Diagonally at distance > 8
    EXPECT_FALSE(gizmo.isNearHandle(glm::vec2(107.0f, 207.0f), glm::vec2(100.0f, 200.0f)));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — Cursor hints
// ═════════════════════════════════════════════════════════════════════════════

TEST(GizmoMath, CursorForHandle)
{
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::None), 0);      // Arrow
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::Center), 13);   // SizeAll
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::TopLeft), 11);  // SizeFDiag
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::TopRight), 10); // SizeBDiag
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::TopEdge), 8);   // SizeVer
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::LeftEdge), 9);  // SizeHor
    EXPECT_EQ(rt::GizmoMath::cursorForHandle(rt::GizmoHandle::RotateTopLeft), 17); // ClosedHand
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — No layout safety
// ═════════════════════════════════════════════════════════════════════════════

TEST(GizmoMath, NoLayoutSafe)
{
    rt::GizmoMath gizmo;
    // No layout set — should not crash

    rt::GizmoTransform transform;
    auto handles = gizmo.computeHandles(transform);
    auto hit = gizmo.hitTest(transform, 0.0f, 0.0f);
    EXPECT_EQ(hit, rt::GizmoHandle::None);

    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::Center,
                                   glm::vec2(0.0f), glm::vec2(10.0f));
    // Should return original unchanged
    EXPECT_TRUE(nearEqual(result.position, transform.position));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoMath — With zoomed viewport
// ═════════════════════════════════════════════════════════════════════════════

TEST(GizmoMath, HandlePositionsWithZoom)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(960, 540);
    // Fit zoom = 0.5

    rt::GizmoMath gizmo;
    gizmo.setLayout(&layout);

    rt::GizmoTransform transform;
    transform.position = glm::vec2(0.0f, 0.0f);
    transform.size     = glm::vec2(1920.0f, 1080.0f);

    auto handles = gizmo.computeHandles(transform);

    // Content (0,0) → widget (0,0) at fit zoom 0.5
    EXPECT_TRUE(nearEqual(handles.topLeft, glm::vec2(0.0f, 0.0f)));

    // Content (1920, 1080) → widget (960, 540)
    EXPECT_TRUE(nearEqual(handles.bottomRight, glm::vec2(960.0f, 540.0f)));
}

TEST(GizmoMath, DragTranslateWithZoom)
{
    rt::ViewportLayout layout;
    layout.setContentSize(1920, 1080);
    layout.setWidgetSize(960, 540);
    // Fit zoom = 0.5

    rt::GizmoMath gizmo;
    gizmo.setLayout(&layout);

    rt::GizmoTransform transform;
    transform.position = glm::vec2(100.0f, 100.0f);
    transform.size     = glm::vec2(400.0f, 300.0f);

    // Drag by 50 in widget space → 100 in content space (at 0.5 zoom)
    auto result = gizmo.applyDrag(transform, rt::GizmoHandle::Center,
                                   glm::vec2(200.0f, 200.0f),
                                   glm::vec2(250.0f, 225.0f));

    EXPECT_TRUE(nearEqual(result.position, glm::vec2(200.0f, 150.0f)));
}

// ═════════════════════════════════════════════════════════════════════════════
//  GizmoTransform defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(GizmoTransform, DefaultValues)
{
    rt::GizmoTransform t;
    EXPECT_FLOAT_EQ(t.position.x, 0.0f);
    EXPECT_FLOAT_EQ(t.position.y, 0.0f);
    EXPECT_FLOAT_EQ(t.size.x, 1920.0f);
    EXPECT_FLOAT_EQ(t.size.y, 1080.0f);
    EXPECT_FLOAT_EQ(t.rotation, 0.0f);
    EXPECT_FLOAT_EQ(t.anchor.x, 0.5f);
    EXPECT_FLOAT_EQ(t.anchor.y, 0.5f);
}

TEST(GizmoConfig, DefaultValues)
{
    rt::GizmoConfig c;
    EXPECT_FLOAT_EQ(c.handleSize, 8.0f);
    EXPECT_FLOAT_EQ(c.rotateHandleOffset, 20.0f);
    EXPECT_FLOAT_EQ(c.hitPadding, 4.0f);
    EXPECT_FLOAT_EQ(c.lineWidth, 1.5f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SafeAreaRect
// ═════════════════════════════════════════════════════════════════════════════

TEST(SafeAreaRect, WidthAndHeight)
{
    rt::SafeAreaRect r{10.0f, 20.0f, 110.0f, 70.0f};
    EXPECT_FLOAT_EQ(r.width(), 100.0f);
    EXPECT_FLOAT_EQ(r.height(), 50.0f);
}
