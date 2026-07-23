#include <gtest/gtest.h>

#include "FrameTime.h"

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

} // namespace
} // namespace rt
