/*
 * test_frame_label.cpp - FrameLabel.h: a decoded picture's timestamp against
 * the frame number it is labelled with.
 */
#include <gtest/gtest.h>

#include "decode/FrameLabel.h"

using namespace rt;

TEST(FrameLabel, FrameFromTimestampRoundsToNearestFrame)
{
    EXPECT_EQ(frameFromTimestamp(0.0, 30.0), 0);
    EXPECT_EQ(frameFromTimestamp(1.0, 30.0), 30);
    EXPECT_EQ(frameFromTimestamp(1.0 / 30.0 * 7.0, 30.0), 7);
    // 29.97 fps in a 1/30000 timebase: frame 1001 sits at 1001 * 1001 / 30000 s.
    EXPECT_EQ(frameFromTimestamp(1001.0 * 1001.0 / 30000.0, 30000.0 / 1001.0), 1001);
}

TEST(FrameLabel, ExactTimestampMatches)
{
    for (int64_t f = 0; f < 5000; f += 37)
        EXPECT_TRUE(timestampMatchesFrame(f / 24.0, f, 24.0)) << f;
}

TEST(FrameLabel, OnePictureOffDoesNotMatch)
{
    EXPECT_FALSE(timestampMatchesFrame(11.0 / 30.0, 10, 30.0));
    EXPECT_FALSE(timestampMatchesFrame(9.0 / 30.0, 10, 30.0));
    EXPECT_FALSE(timestampMatchesFrame(0.0, 1, 30.0));
}

TEST(FrameLabel, SmallRateDriftStillMatches)
{
    // Average rate 0.01% off the picture spacing, far into a long file.
    const double spacingFps = 30.0;
    const double avgFps = 30.0 * 1.0001;
    const int64_t f = 5000;
    EXPECT_TRUE(timestampMatchesFrame(f / spacingFps, f, avgFps));
}

TEST(FrameLabel, UnknownRateNeverFlagsAMismatch)
{
    EXPECT_TRUE(timestampMatchesFrame(3.0, 1, 0.0));
}
