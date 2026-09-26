#include <gtest/gtest.h>

#include "FrameTime.h"

#include <utility>

namespace rt {
namespace {

TEST(FrameTime, RecognizesIntegerAndNtscRates)
{
    EXPECT_EQ(canonicalFrameRate(60.0).numerator, 60u);
    EXPECT_EQ(canonicalFrameRate(60.0).denominator, 1u);
    EXPECT_EQ(canonicalFrameRate(29.97).numerator, 30000u);
    EXPECT_EQ(canonicalFrameRate(29.97).denominator, 1001u);
    EXPECT_EQ(canonicalFrameRate(59.94).numerator, 60000u);
    EXPECT_EQ(canonicalFrameRate(59.94).denominator, 1001u);
}

TEST(FrameTime, ExportAndPreRenderUseIdenticalTicks)
{
    for (double displayRate : {23.976, 29.97, 59.94, 60.0}) {
        const RationalFrameRate preRenderRate = canonicalFrameRate(displayRate);
        for (int64_t frame : {0, 1, 2, 100, 1001, 100000}) {
            const int64_t preRenderTick = frameIndexToTick(frame, preRenderRate);
            const int64_t exportTick = frameIndexToTick(
                frame, preRenderRate.numerator, preRenderRate.denominator);
            EXPECT_EQ(preRenderTick, exportTick)
                << "rate=" << displayRate << " frame=" << frame;
        }
    }
}

TEST(FrameTime, RoundsInsteadOfTruncatingFractionalTicks)
{
    EXPECT_EQ(frameIndexToTick(1, 30000, 1001), 1602);
    EXPECT_EQ(frameIndexToTick(2, 30000, 1001), 3203);
}

TEST(FrameTime, TickToFrameIsExactInverse)
{
    for (auto [num, den] : {std::pair{24000u, 1001u}, std::pair{30000u, 1001u},
                            std::pair{60000u, 1001u}, std::pair{24u, 1u},
                            std::pair{25u, 1u}, std::pair{30u, 1u}, std::pair{60u, 1u}}) {
        for (int64_t frame = 0; frame < 5000; ++frame) {
            const int64_t tick = frameIndexToTick(frame, num, den);
            ASSERT_EQ(tickToFrameIndex(tick, num, den), frame)
                << num << "/" << den << " frame=" << frame;
            // One tick before a frame start is still the previous frame.
            if (frame > 0) {
                ASSERT_EQ(tickToFrameIndex(tick - 1, num, den), frame - 1);
            }
            // Exclusive end on a boundary ends exactly there.
            ASSERT_EQ(tickToEndFrameIndex(tick, num, den), frame);
        }
    }
}

TEST(FrameTime, NtscInPointOnFrameIsNotTheFrameBefore)
{
    // 29.97: frame 2 starts at tick 3203; 3203 / 48000 * 29.97 = 1.9999.
    EXPECT_EQ(tickToFrameIndex(3203, 30000, 1001), 2);
}

TEST(FrameTime, MidFrameEndIncludesPartialFrame)
{
    // 24fps: frame 10 = tick 20000. An end 1 tick into frame 10 includes it.
    EXPECT_EQ(tickToEndFrameIndex(20001, 24, 1), 11);
    EXPECT_EQ(tickToEndFrameIndex(20000, 24, 1), 10);
}

TEST(FrameTime, MidFrameInPointStartsAtNextFrameNotTheOneBefore)
{
    // Export start = first frame starting at/after the in point. An in point
    // on an off-grid cut (tick 20001, 1 tick into 24fps frame 10) must start
    // at frame 11 — frame 10 begins BEFORE the cut and shows the prior shot.
    EXPECT_EQ(tickToEndFrameIndex(20001, 24, 1), 11);
    EXPECT_GE(frameIndexToTick(tickToEndFrameIndex(20001, 24, 1), 24, 1), 20001);
    // On-grid in points are unchanged, and tick 0 stays frame 0.
    EXPECT_EQ(tickToEndFrameIndex(20000, 24, 1), 10);
    EXPECT_EQ(tickToEndFrameIndex(0, 30000, 1001), 0);
    EXPECT_EQ(tickToEndFrameIndex(3203, 30000, 1001), 2);
}

} // namespace
} // namespace rt
