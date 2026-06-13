/*
 * ROUNDTABLE NLE v2 — Keyframe & interpolation unit tests
 * Step 3: Core data model validation
 */

#include <gtest/gtest.h>
#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"

using namespace rt;

// ── KeyframeTrack tests ─────────────────────────────────────────────────────

class KeyframeTrackTest : public ::testing::Test
{
protected:
    // Default-constructed KeyframeTrack has no keyframes, but evaluates to 0.0f
    KeyframeTrack<float> track{0.0f};
};

TEST_F(KeyframeTrackTest, HasDefaultKeyframe)
{
    // Default-constructed with value 0.0f → no keyframes, evaluates to default
    EXPECT_EQ(track.keyframeCount(), 0u);
    EXPECT_FLOAT_EQ(track.evaluate(0), 0.0f);
    EXPECT_FLOAT_EQ(track.defaultValue(), 0.0f);
}

TEST_F(KeyframeTrackTest, AddKeyframe)
{
    track.addKeyframe(48000, 1.0f);
    EXPECT_EQ(track.keyframeCount(), 1u);
}

TEST_F(KeyframeTrackTest, EvaluateConstantTrack)
{
    // Default keyframe at time=0 with value=0.0f is static
    EXPECT_TRUE(track.isStatic());
    EXPECT_FLOAT_EQ(track.evaluate(0), 0.0f);
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.0f);
}

TEST_F(KeyframeTrackTest, EvaluateLinearInterpolation)
{
    // Add two keyframes for interpolation
    track.addKeyframe(0, 0.0f, InterpMode::Linear);
    track.addKeyframe(48000, 100.0f, InterpMode::Linear);

    EXPECT_FLOAT_EQ(track.evaluate(0), 0.0f);
    EXPECT_FLOAT_EQ(track.evaluate(48000), 100.0f);
    EXPECT_NEAR(track.evaluate(24000), 50.0f, 0.01f); // Midpoint
}

TEST_F(KeyframeTrackTest, EvaluateHoldInterpolation)
{
    // Overwrite default kf at time 0 to value=10, Hold mode
    track.addKeyframe(0, 10.0f, InterpMode::Hold);
    track.addKeyframe(48000, 90.0f, InterpMode::Hold);

    EXPECT_FLOAT_EQ(track.evaluate(0), 10.0f);
    EXPECT_FLOAT_EQ(track.evaluate(24000), 10.0f);     // Stays at 10 until next kf
    EXPECT_FLOAT_EQ(track.evaluate(48000), 90.0f);      // Jumps at keyframe
}

TEST_F(KeyframeTrackTest, EvaluateBeforeFirstKeyframe)
{
    // Track has no default keyframe, add at 48000:
    track.addKeyframe(48000, 42.0f);
    EXPECT_FLOAT_EQ(track.evaluate(0), 42.0f); // Should return first keyframe value
}

TEST_F(KeyframeTrackTest, EvaluateAfterLastKeyframe)
{
    track.addKeyframe(0, 10.0f);
    track.addKeyframe(48000, 20.0f);
    EXPECT_FLOAT_EQ(track.evaluate(96000), 20.0f); // Should return last keyframe value
}

TEST_F(KeyframeTrackTest, RemoveKeyframe)
{
    track.addKeyframe(0, 1.0f);
    track.addKeyframe(48000, 2.0f);
    EXPECT_EQ(track.keyframeCount(), 2u);
    track.removeKeyframe(0);
    EXPECT_EQ(track.keyframeCount(), 1u);
}

TEST_F(KeyframeTrackTest, KeyframesAreSorted)
{
    track.addKeyframe(0, 1.0f);
    track.addKeyframe(96000, 3.0f);
    track.addKeyframe(48000, 2.0f);

    // Keyframes should be ordered by time (0, 48000, 96000)
    auto& kfs = track.keyframes();
    ASSERT_EQ(kfs.size(), 3u);
    EXPECT_LE(kfs[0].time, kfs[1].time);
    EXPECT_LE(kfs[1].time, kfs[2].time);
}

TEST_F(KeyframeTrackTest, IsStaticWithSingleKeyframe)
{
    // Default-constructed track has 0 keyframes → isStatic
    EXPECT_TRUE(track.isStatic());
}

TEST_F(KeyframeTrackTest, IsNotStaticWithMultipleKeyframes)
{
    track.addKeyframe(0, 0.0f);
    track.addKeyframe(48000, 10.0f);
    EXPECT_FALSE(track.isStatic());
}

// ── Bezier interpolation ────────────────────────────────────────────────────

TEST_F(KeyframeTrackTest, BezierInterpolationEndpoints)
{
    // Bezier should still hit the keyframe values exactly at endpoints
    track.addKeyframe(0, 0.0f, InterpMode::Bezier);
    track.addKeyframe(48000, 100.0f, InterpMode::Bezier);

    EXPECT_NEAR(track.evaluate(0), 0.0f, 0.1f);
    EXPECT_NEAR(track.evaluate(48000), 100.0f, 0.1f);
}

TEST_F(KeyframeTrackTest, BezierMidpointIsReasonable)
{
    track.addKeyframe(0, 0.0f, InterpMode::Bezier);
    track.addKeyframe(48000, 100.0f, InterpMode::Bezier);

    float mid = track.evaluate(24000);
    EXPECT_GT(mid, 0.0f);
    EXPECT_LT(mid, 100.0f);
}
