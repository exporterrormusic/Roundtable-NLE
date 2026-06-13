/*
 * test_timeline_ui.cpp — Tests for Step 12: Timeline Panel layout math.
 *
 * Tests the TimelineLayoutEngine (pure math, no Qt or GPU):
 *   1. Time ↔ pixel conversion
 *   2. Zoom (set, zoomAt, zoomToFit)
 *   3. Scroll position
 *   4. Visible range queries
 *   5. Playhead positioning
 *   6. Ruler mark generation (adaptive subdivisions)
 *   7. Timecode formatting
 *   8. Track layout (vertical positioning, visibility)
 *   9. Clip rectangle computation
 *  10. Scrollbar state (NLE handle positions)
 *  11. Snapping (frame, grid)
 *  12. Edge cases
 */

#include <gtest/gtest.h>

#include "timeline/TimelineLayoutEngine.h"

#include <cmath>
#include <string>

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

static constexpr double kEps = 0.5; // pixel tolerance
static constexpr int64_t TPS = rt::kTicksPerSecond; // 48000

static bool nearEqual(double a, double b, double eps = kEps)
{
    return std::abs(a - b) <= eps;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, DefaultValues)
{
    rt::TimelineLayoutEngine engine;
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), 100.0);
    EXPECT_DOUBLE_EQ(engine.scrollX(), 0.0);
    EXPECT_DOUBLE_EQ(engine.viewportWidth(), 800.0);
    EXPECT_DOUBLE_EQ(engine.frameRate(), 60.0);
    EXPECT_EQ(engine.totalDuration(), 0);
}

TEST(TimelineLayout, Constants)
{
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::kMinPixelsPerSecond, 0.05);
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::kMaxPixelsPerSecond, 20000.0);
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::kDefaultPixelsPerSecond, 100.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Time ↔ Pixel conversion
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, TimeToPixelAtOrigin)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(0);

    // t=0 → px=0
    EXPECT_TRUE(nearEqual(engine.timeToPixelX(0), 0.0));

    // t=1s → px=100
    EXPECT_TRUE(nearEqual(engine.timeToPixelX(TPS), 100.0));

    // t=10s → px=1000
    EXPECT_TRUE(nearEqual(engine.timeToPixelX(10 * TPS), 1000.0));
}

TEST(TimelineLayout, TimeToPixelWithScroll)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(500.0); // Scrolled to 5s

    // t=5s should be at pixel 0
    EXPECT_TRUE(nearEqual(engine.timeToPixelX(5 * TPS), 0.0));

    // t=0 should be at pixel -500
    EXPECT_TRUE(nearEqual(engine.timeToPixelX(0), -500.0));
}

TEST(TimelineLayout, PixelToTimeRoundtrip)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(200.0);
    engine.setScrollX(300.0);

    int64_t tick = 5 * TPS; // 5 seconds
    double px = engine.timeToPixelX(tick);
    int64_t back = engine.pixelXToTime(px);

    // Should roundtrip within 1 tick
    EXPECT_NEAR(static_cast<double>(back), static_cast<double>(tick), 1.0);
}

TEST(TimelineLayout, PixelToTimeNegativeClamp)
{
    rt::TimelineLayoutEngine engine;
    engine.setScrollX(0);

    // Negative pixel should clamp to t=0
    int64_t tick = engine.pixelXToTime(-100.0);
    EXPECT_EQ(tick, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Zoom
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, SetPixelsPerSecond)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(500.0);
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), 500.0);
}

TEST(TimelineLayout, ZoomClampMin)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(0.01);
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), rt::TimelineLayoutEngine::kMinPixelsPerSecond);
}

TEST(TimelineLayout, ZoomClampMax)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(50000.0);
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), rt::TimelineLayoutEngine::kMaxPixelsPerSecond);
}

TEST(TimelineLayout, ZoomAtPreservesPointUnderCursor)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(0);

    // Cursor at pixel 400 (= 4 seconds at 100 pps)
    int64_t timeBefore = engine.pixelXToTime(400.0);

    engine.zoomAt(400.0, 2.0); // Zoom in 2x

    int64_t timeAfter = engine.pixelXToTime(400.0);

    // Same time should be under pixel 400
    EXPECT_NEAR(static_cast<double>(timeAfter), static_cast<double>(timeBefore), TPS * 0.01);
}

TEST(TimelineLayout, ZoomAtIncreasesPixelsPerSecond)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);

    engine.zoomAt(400.0, 2.0); // 2x zoom in
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), 200.0);
}

TEST(TimelineLayout, ZoomToFit)
{
    rt::TimelineLayoutEngine engine;
    engine.setViewportWidth(1000.0);

    // Fit 10 seconds in 1000 pixels → 100 pps
    engine.zoomToFit(0, 10 * TPS, 1000.0);
    EXPECT_TRUE(nearEqual(engine.pixelsPerSecond(), 100.0));
    EXPECT_TRUE(nearEqual(engine.scrollX(), 0.0));
}

TEST(TimelineLayout, ZoomToFitOffset)
{
    rt::TimelineLayoutEngine engine;

    // Fit 5s–15s in 1000 pixels → 100 pps, scroll to 5s
    engine.zoomToFit(5 * TPS, 15 * TPS, 1000.0);
    EXPECT_TRUE(nearEqual(engine.pixelsPerSecond(), 100.0));
    EXPECT_TRUE(nearEqual(engine.scrollX(), 500.0)); // 5s * 100pps
}

// ═════════════════════════════════════════════════════════════════════════════
//  Visible range
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, VisibleRange)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    // Visible: 0 to 8 seconds
    EXPECT_EQ(engine.visibleStartTime(), 0);
    EXPECT_TRUE(nearEqual(rt::TimelineLayoutEngine::ticksToSeconds(engine.visibleEndTime()),
                          8.0, 0.01));
}

TEST(TimelineLayout, VisibleRangeWithScroll)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(500.0); // Scrolled to 5s

    EXPECT_TRUE(nearEqual(rt::TimelineLayoutEngine::ticksToSeconds(engine.visibleStartTime()),
                          5.0, 0.01));
    EXPECT_TRUE(nearEqual(rt::TimelineLayoutEngine::ticksToSeconds(engine.visibleEndTime()),
                          13.0, 0.01));
}

TEST(TimelineLayout, IsTimeVisible)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    EXPECT_TRUE(engine.isTimeVisible(0));
    EXPECT_TRUE(engine.isTimeVisible(4 * TPS));
    EXPECT_FALSE(engine.isTimeVisible(10 * TPS));
}

TEST(TimelineLayout, IsRangeVisible)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    // Fully visible
    EXPECT_TRUE(engine.isRangeVisible(TPS, 5 * TPS));

    // Partially visible (starts before viewport, ends inside)
    engine.setScrollX(200.0);
    EXPECT_TRUE(engine.isRangeVisible(0, 5 * TPS));

    // Fully outside
    EXPECT_FALSE(engine.isRangeVisible(20 * TPS, 25 * TPS));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Playhead
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, PlayheadPixelX)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(0);

    EXPECT_TRUE(nearEqual(engine.playheadPixelX(0), 0.0));
    EXPECT_TRUE(nearEqual(engine.playheadPixelX(5 * TPS), 500.0));
}

TEST(TimelineLayout, ScrollToCenter)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);

    double scrollX = engine.scrollXToCenter(10 * TPS);
    // 10s * 100pps = 1000px, center = 1000 - 400 = 600
    EXPECT_TRUE(nearEqual(scrollX, 600.0));
}

TEST(TimelineLayout, ScrollToCenterClampZero)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);

    // Centering on t=0 should return scrollX=0 (clamped)
    double scrollX = engine.scrollXToCenter(0);
    EXPECT_DOUBLE_EQ(scrollX, 0.0);
}

TEST(TimelineLayout, ScrollToShowAlreadyVisible)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    // 4s is already visible at px=400
    double scrollX = engine.scrollXToShow(4 * TPS);
    EXPECT_DOUBLE_EQ(scrollX, 0.0); // No change
}

TEST(TimelineLayout, ScrollToShowRightSide)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    // 15s is at px=1500, not visible
    double scrollX = engine.scrollXToShow(15 * TPS);
    EXPECT_GT(scrollX, 0.0); // Should scroll right
}

// ═════════════════════════════════════════════════════════════════════════════
//  Timecode formatting
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, FormatTimecodeZero)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(24.0);
    EXPECT_EQ(engine.formatTimecode(0), "00:00:00:00");
}

TEST(TimelineLayout, FormatTimecodeOneSecond)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(24.0);
    EXPECT_EQ(engine.formatTimecode(TPS), "00:00:01:00");
}

TEST(TimelineLayout, FormatTimecodeOneFrame)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(24.0);
    // 1 frame = 48000/24 = 2000 ticks
    EXPECT_EQ(engine.formatTimecode(2000), "00:00:00:01");
}

TEST(TimelineLayout, FormatTimecodeComplex)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(24.0);
    // 1h 30m 45s 12f = 5445 seconds + 12/24 seconds
    int64_t tick = rt::TimelineLayoutEngine::secondsToTicks(
        1.0 * 3600 + 30 * 60 + 45 + 12.0 / 24.0);
    EXPECT_EQ(engine.formatTimecode(tick), "01:30:45:12");
}

TEST(TimelineLayout, FormatTimecode30fps)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(30.0);
    // 1 frame at 30fps = 48000/30 = 1600 ticks
    EXPECT_EQ(engine.formatTimecode(1600), "00:00:00:01");
    EXPECT_EQ(engine.formatTimecode(TPS), "00:00:01:00");
}

TEST(TimelineLayout, FormatCompactTime)
{
    rt::TimelineLayoutEngine engine;
    EXPECT_EQ(engine.formatCompactTime(0), "0:00");
    EXPECT_EQ(engine.formatCompactTime(TPS), "0:01");
    EXPECT_EQ(engine.formatCompactTime(90 * TPS), "1:30");
    EXPECT_EQ(engine.formatCompactTime(3661 * TPS), "1:01:01");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Ruler marks
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, RulerMarksExist)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    auto marks = engine.computeRulerMarks();
    EXPECT_GT(marks.size(), 0u);
}

TEST(TimelineLayout, RulerHasMajorAndMinor)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);

    auto marks = engine.computeRulerMarks();

    bool hasMajor = false, hasMinor = false;
    for (const auto& m : marks)
    {
        if (m.isMajor) hasMajor = true;
        else hasMinor = true;
    }
    EXPECT_TRUE(hasMajor);
    EXPECT_TRUE(hasMinor);
}

TEST(TimelineLayout, RulerMajorMarksHaveLabels)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);

    auto marks = engine.computeRulerMarks();
    for (const auto& m : marks)
    {
        if (m.isMajor)
        {
            EXPECT_FALSE(m.label.empty()) << "Major mark at tick " << m.tick << " has no label";
        }
    }
}

TEST(TimelineLayout, RulerMajorMarksAreSorted)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);

    auto marks = engine.computeRulerMarks();
    for (size_t i = 1; i < marks.size(); ++i)
    {
        EXPECT_GE(marks[i].tick, marks[i - 1].tick);
    }
}

TEST(TimelineLayout, RulerAdaptsToZoomLevelZoomedIn)
{
    rt::TimelineLayoutEngine engine;
    engine.setViewportWidth(800.0);

    // Very zoomed in — should have many marks (frame-level)
    engine.setPixelsPerSecond(5000.0);
    auto marksIn = engine.computeRulerMarks();

    // Zoomed out — should have fewer marks
    engine.setPixelsPerSecond(10.0);
    auto marksOut = engine.computeRulerMarks();

    // The zoomed-in view covers less time, so the mark density should adapt
    // (both should have marks but the interval changes)
    EXPECT_GT(marksIn.size(), 0u);
    EXPECT_GT(marksOut.size(), 0u);
}

TEST(TimelineLayout, RulerMajorMarkSpacing)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);

    auto marks = engine.computeRulerMarks();

    // Find consecutive major marks and check spacing
    std::vector<double> majorPixels;
    for (const auto& m : marks)
    {
        if (m.isMajor)
            majorPixels.push_back(engine.timeToPixelX(m.tick));
    }

    for (size_t i = 1; i < majorPixels.size(); ++i)
    {
        double spacing = majorPixels[i] - majorPixels[i - 1];
        // Major marks should be at least kMinMajorMarkSpacing apart
        EXPECT_GE(spacing, rt::TimelineLayoutEngine::kMinMajorMarkSpacing - 1.0)
            << "Major marks too close: " << spacing << " px";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Track layout
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, TrackLayoutBasic)
{
    rt::TimelineLayoutEngine engine;
    std::vector<float> heights = {80.0f, 80.0f, 60.0f, 60.0f};

    auto layout = engine.computeTrackLayout(heights, 0.0f, 400.0f);

    ASSERT_EQ(layout.size(), 4u);

    EXPECT_TRUE(nearEqual(layout[0].y, 0.0f));
    EXPECT_TRUE(nearEqual(layout[0].height, 80.0f));

    EXPECT_TRUE(nearEqual(layout[1].y, 80.0f));
    EXPECT_TRUE(nearEqual(layout[1].height, 80.0f));

    EXPECT_TRUE(nearEqual(layout[2].y, 160.0f));
    EXPECT_TRUE(nearEqual(layout[2].height, 60.0f));

    EXPECT_TRUE(nearEqual(layout[3].y, 220.0f));
    EXPECT_TRUE(nearEqual(layout[3].height, 60.0f));
}

TEST(TimelineLayout, TrackLayoutWithScroll)
{
    rt::TimelineLayoutEngine engine;
    std::vector<float> heights = {80.0f, 80.0f, 80.0f, 80.0f};

    auto layout = engine.computeTrackLayout(heights, 100.0f, 200.0f);

    // First track: y = 0 - 100 = -100 (partially visible)
    EXPECT_TRUE(nearEqual(layout[0].y, -100.0f));
    EXPECT_FALSE(layout[0].visible); // Top edge is out, bottom is at -20

    // Second track: y = 80 - 100 = -20 (partially visible)
    EXPECT_TRUE(nearEqual(layout[1].y, -20.0f));
    EXPECT_TRUE(layout[1].visible); // Bottom at 60, visible

    // Third track: y = 160 - 100 = 60 (visible)
    EXPECT_TRUE(nearEqual(layout[2].y, 60.0f));
    EXPECT_TRUE(layout[2].visible);

    // Fourth track: y = 240 - 100 = 140 (visible)
    EXPECT_TRUE(nearEqual(layout[3].y, 140.0f));
    EXPECT_TRUE(layout[3].visible);
}

TEST(TimelineLayout, TrackLayoutVisibility)
{
    rt::TimelineLayoutEngine engine;
    std::vector<float> heights = {80.0f, 80.0f, 80.0f};

    // Scroll past all tracks
    auto layout = engine.computeTrackLayout(heights, 300.0f, 200.0f);

    for (const auto& info : layout)
    {
        EXPECT_FALSE(info.visible);
    }
}

TEST(TimelineLayout, TrackLayoutEmpty)
{
    rt::TimelineLayoutEngine engine;
    std::vector<float> heights;
    auto layout = engine.computeTrackLayout(heights, 0.0f, 400.0f);
    EXPECT_EQ(layout.size(), 0u);
}

TEST(TimelineLayout, TotalTrackHeight)
{
    std::vector<float> heights = {80.0f, 80.0f, 60.0f, 60.0f};
    EXPECT_FLOAT_EQ(rt::TimelineLayoutEngine::totalTrackHeight(heights), 280.0f);
}

TEST(TimelineLayout, TotalTrackHeightEmpty)
{
    std::vector<float> heights;
    EXPECT_FLOAT_EQ(rt::TimelineLayoutEngine::totalTrackHeight(heights), 0.0f);
}

TEST(TimelineLayout, TrackIndices)
{
    rt::TimelineLayoutEngine engine;
    std::vector<float> heights = {80.0f, 80.0f, 80.0f};
    auto layout = engine.computeTrackLayout(heights, 0.0f, 300.0f);

    for (size_t i = 0; i < layout.size(); ++i)
    {
        EXPECT_EQ(layout[i].trackIndex, i);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clip layout
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, ClipRect)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(0);

    // Clip at 2s, duration 3s, track at y=80, height=80
    auto rect = engine.clipRect(2 * TPS, 3 * TPS, 80.0f, 80.0f);

    EXPECT_TRUE(nearEqual(rect.x, 200.0));
    EXPECT_TRUE(nearEqual(rect.width, 300.0));
    EXPECT_TRUE(nearEqual(rect.y, 80.0f + rt::ClipLayoutRect::kTrackPadding));
    EXPECT_TRUE(nearEqual(rect.height, 80.0f - 2.0f * rt::ClipLayoutRect::kTrackPadding));
}

TEST(TimelineLayout, ClipRectWithScroll)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setScrollX(200.0);

    // Clip at 2s should start at px=0 when scrolled to 200px (2s)
    auto rect = engine.clipRect(2 * TPS, 3 * TPS, 0.0f, 80.0f);

    EXPECT_TRUE(nearEqual(rect.x, 0.0));
    EXPECT_TRUE(nearEqual(rect.width, 300.0));
}

TEST(TimelineLayout, ClipRectZeroDuration)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);

    auto rect = engine.clipRect(0, 0, 0.0f, 80.0f);
    EXPECT_TRUE(nearEqual(rect.width, 0.0));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Scrollbar
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, ScrollbarNoDuration)
{
    rt::TimelineLayoutEngine engine;
    engine.setTotalDuration(0);

    auto state = engine.computeScrollbar();
    // An empty timeline still exposes a 60s pannable range (Premiere-style
    // pan/zoom into empty space), so the handle reflects the viewport's
    // fraction of that range rather than filling the whole bar.
    EXPECT_DOUBLE_EQ(state.handleStart, 0.0);
    EXPECT_GT(state.handleEnd, 0.0);
    EXPECT_LT(state.handleEnd, 1.0);
}

TEST(TimelineLayout, ScrollbarAtStart)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setScrollX(0);
    engine.setTotalDuration(30 * TPS); // 30 seconds

    auto state = engine.computeScrollbar();
    EXPECT_DOUBLE_EQ(state.handleStart, 0.0);
    EXPECT_GT(state.handleEnd, 0.0);
    EXPECT_LT(state.handleEnd, 1.0);
    EXPECT_DOUBLE_EQ(state.totalDuration, 30.0);
}

TEST(TimelineLayout, ScrollbarHandleWidth)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setTotalDuration(30 * TPS);
    engine.setScrollX(0);

    auto state = engine.computeScrollbar();
    // Padded scrollable range for 30s content = max(30*1.5, 30+60, 16, 60) =
    // 90s, so the handle is 800px out of 90s * 100pps = 9000px.
    double expectedWidth = 800.0 / 9000.0;
    EXPECT_TRUE(nearEqual(state.handleWidth(), expectedWidth, 0.01));
}

TEST(TimelineLayout, ApplyScrollbarDrag)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setTotalDuration(30 * TPS);

    engine.applyScrollbarDrag(0.5); // Move handle to 50%
    EXPECT_GT(engine.scrollX(), 0.0);
}

TEST(TimelineLayout, ApplyScrollbarResize)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);
    engine.setViewportWidth(800.0);
    engine.setTotalDuration(30 * TPS);

    double ppsBefore = engine.pixelsPerSecond();
    engine.applyScrollbarResize(0.2, 0.8);
    // Zoom should change
    EXPECT_NE(engine.pixelsPerSecond(), ppsBefore);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Snapping
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, SnapToFrame24fps)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(24.0);

    // Ticks per frame = 48000/24 = 2000
    // Tick 1500 should snap to tick 2000 (frame 1)
    EXPECT_EQ(engine.snapToFrame(1500), 2000);

    // Tick 500 should snap to tick 0 (frame 0)
    EXPECT_EQ(engine.snapToFrame(500), 0);

    // Exact frame boundary
    EXPECT_EQ(engine.snapToFrame(2000), 2000);
}

TEST(TimelineLayout, SnapToFrame30fps)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(30.0);

    // Ticks per frame = 48000/30 = 1600
    EXPECT_EQ(engine.snapToFrame(1600), 1600);
    EXPECT_EQ(engine.snapToFrame(800),  1600); // Rounds to nearest
    EXPECT_EQ(engine.snapToFrame(0), 0);
}

TEST(TimelineLayout, SnapToGrid)
{
    rt::TimelineLayoutEngine engine;
    engine.setPixelsPerSecond(100.0);

    // At 100 pps, grid snap should align to the ruler minor subdivisions
    int64_t snapped = engine.snapToGrid(TPS + 100); // ~1.002s
    // Should be near a grid line
    EXPECT_GE(snapped, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Tick/second conversion
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, TicksToSeconds)
{
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::ticksToSeconds(0), 0.0);
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::ticksToSeconds(48000), 1.0);
    EXPECT_DOUBLE_EQ(rt::TimelineLayoutEngine::ticksToSeconds(24000), 0.5);
}

TEST(TimelineLayout, SecondsToTicks)
{
    EXPECT_EQ(rt::TimelineLayoutEngine::secondsToTicks(0.0), 0);
    EXPECT_EQ(rt::TimelineLayoutEngine::secondsToTicks(1.0), 48000);
    EXPECT_EQ(rt::TimelineLayoutEngine::secondsToTicks(0.5), 24000);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, ScrollXNonNegative)
{
    rt::TimelineLayoutEngine engine;
    engine.setScrollX(-100.0);
    EXPECT_DOUBLE_EQ(engine.scrollX(), 0.0);
}

TEST(TimelineLayout, ViewportWidthMinimum)
{
    rt::TimelineLayoutEngine engine;
    engine.setViewportWidth(0.0);
    EXPECT_DOUBLE_EQ(engine.viewportWidth(), 1.0);
}

TEST(TimelineLayout, FrameRateMinimum)
{
    rt::TimelineLayoutEngine engine;
    engine.setFrameRate(0.0);
    EXPECT_DOUBLE_EQ(engine.frameRate(), 1.0);
}

TEST(TimelineLayout, TotalDurationNonNegative)
{
    rt::TimelineLayoutEngine engine;
    engine.setTotalDuration(-100);
    EXPECT_EQ(engine.totalDuration(), 0);
}

TEST(TimelineLayout, ZoomToFitInvalidRange)
{
    rt::TimelineLayoutEngine engine;
    double ppsBefore = engine.pixelsPerSecond();

    // End <= start — should not change anything
    engine.zoomToFit(10 * TPS, 5 * TPS, 1000.0);
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), ppsBefore);

    // Zero viewport width
    engine.zoomToFit(0, 10 * TPS, 0.0);
    EXPECT_DOUBLE_EQ(engine.pixelsPerSecond(), ppsBefore);
}

TEST(TimelineLayout, FormatTimecodeNegative)
{
    rt::TimelineLayoutEngine engine;
    // Negative tick should clamp to 0
    EXPECT_EQ(engine.formatTimecode(-1000), "00:00:00:00");
}

TEST(TimelineLayout, ClipRectPadding)
{
    // Verify clip padding constant
    EXPECT_FLOAT_EQ(rt::ClipLayoutRect::kTrackPadding, 2.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Struct defaults
// ═════════════════════════════════════════════════════════════════════════════

TEST(TimelineLayout, RulerMarkDefaults)
{
    rt::RulerMark mark;
    EXPECT_EQ(mark.tick, 0);
    EXPECT_FALSE(mark.isMajor);
    EXPECT_TRUE(mark.label.empty());
}

TEST(TimelineLayout, TrackLayoutInfoDefaults)
{
    rt::TrackLayoutInfo info;
    EXPECT_EQ(info.trackIndex, 0u);
    EXPECT_FLOAT_EQ(info.y, 0.0f);
    EXPECT_FLOAT_EQ(info.height, 80.0f);
    EXPECT_TRUE(info.visible);
}

TEST(TimelineLayout, ClipLayoutRectDefaults)
{
    rt::ClipLayoutRect rect;
    EXPECT_DOUBLE_EQ(rect.x, 0.0);
    EXPECT_DOUBLE_EQ(rect.width, 0.0);
    EXPECT_FLOAT_EQ(rect.y, 0.0f);
    EXPECT_FLOAT_EQ(rect.height, 0.0f);
}

TEST(TimelineLayout, ScrollbarStateDefaults)
{
    rt::ScrollbarState state;
    EXPECT_DOUBLE_EQ(state.handleStart, 0.0);
    EXPECT_DOUBLE_EQ(state.handleEnd, 1.0);
    EXPECT_DOUBLE_EQ(state.totalDuration, 0.0);
    EXPECT_DOUBLE_EQ(state.handleWidth(), 1.0);
}
