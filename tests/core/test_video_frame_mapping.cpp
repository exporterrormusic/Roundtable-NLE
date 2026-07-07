/*
 * test_video_frame_mapping.cpp — tick → source-frame mapping authority.
 *
 * Exercises mapTickToSourceFrame() (decode/VideoFrameMapping.h), THE single
 * tick→frame mapping shared by render, prefetch scheduling and prewarm.
 * Every consumer must agree on the EXACT frame number or caches miss and
 * frames repeat/skip.  The contract under test (each rule was a bug once):
 *   - speed scaling is POINTWISE: srcTick = sourceIn +
 *     localTick × effectiveSpeed(localTick), where effectiveSpeed =
 *     clip.speed() × speedRamp.evaluate(localTick) — matches
 *     EditOperationsTrim, NOT a time-integral of the ramp;
 *   - negative srcTick clamps to 0 (transition overlap shows frame 0);
 *   - ticks→frame ROUNDS (llround), never truncates — the Wells 30fps
 *     "x.9999…" case must land on the right frame;
 *   - fps priority: clip sourceFps > MediaPool info fps > 24 default;
 *   - stills (frameCount ≤ 1) pin to frame 0, video characters wrap,
 *     everything else clamps to [0, frameCount-1].
 *
 * At 48000 ticks/s: 24 fps = 2000 ticks/frame, 30 fps = 1600 ticks/frame.
 */

#include <gtest/gtest.h>

#include <memory>

#include "decode/VideoFrameMapping.h"

namespace rt {
namespace {

std::unique_ptr<VideoClip> makeClip(int64_t timelineIn, int64_t sourceIn,
                   double speed, double sourceFps)
{
    auto clip = std::make_unique<VideoClip>();
    clip->setTimelineIn(timelineIn);
    clip->setSourceIn(sourceIn);
    clip->setSpeed(speed);
    clip->setSourceFps(sourceFps);
    clip->setDuration(100 * kTicksPerSecond);   // ample; mapping ignores it
    return clip;  // Clip is non-copyable — heap-build
}

VideoStreamInfo makeInfo(int64_t frameCount, double fps)
{
    VideoStreamInfo info;
    info.frameCount = frameCount;
    info.fps        = fps;
    return info;
}

// ── Constant speed ───────────────────────────────────────────────────────────

TEST(VideoFrameMapping, IdentityAtSpeed1)
{
    const auto clip = makeClip(0, 0, 1.0, 24.0);
    for (int64_t k = 0; k <= 100; ++k) {
        const auto m = mapTickToSourceFrame(*clip, k * 2000, nullptr);
        EXPECT_EQ(m.frame, k) << "tick " << k * 2000;
        EXPECT_DOUBLE_EQ(m.fps, 24.0);
    }
}

TEST(VideoFrameMapping, RoundsFrameAlignedTicksNotTruncates)
{
    // The documented Wells case: 30 fps → 1600 ticks/frame, and
    // ticksToSeconds(1600)*30 = 0.99999… — truncation would bias every
    // such frame one source frame early (repeat-then-skip flicker).
    const auto clip = makeClip(0, 0, 1.0, 30.0);
    for (int64_t k = 0; k <= 999; ++k) {
        const auto m = mapTickToSourceFrame(*clip, k * 1600, nullptr);
        EXPECT_EQ(m.frame, k) << "tick " << k * 1600;
    }
}

TEST(VideoFrameMapping, TimelineInOffset)
{
    const auto clip = makeClip(9600, 0, 1.0, 24.0);
    for (int64_t k = 0; k <= 10; ++k)
        EXPECT_EQ(mapTickToSourceFrame(*clip, 9600 + k * 2000, nullptr).frame, k);
}

TEST(VideoFrameMapping, SourceInOffset)
{
    // sourceIn = 1 s of 24 fps media → clip starts at source frame 24.
    const auto clip = makeClip(0, kTicksPerSecond, 1.0, 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 0, nullptr).frame, 24);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 2000, nullptr).frame, 25);
}

TEST(VideoFrameMapping, SpeedDoubleAdvancesSourceTwiceAsFast)
{
    const auto clip = makeClip(0, 0, 2.0, 24.0);
    for (int64_t k = 0; k <= 50; ++k)
        EXPECT_EQ(mapTickToSourceFrame(*clip, k * 2000, nullptr).frame, 2 * k);
}

TEST(VideoFrameMapping, SpeedHalfAdvancesSourceHalfAsFast)
{
    const auto clip = makeClip(0, 0, 0.5, 24.0);
    for (int64_t k = 0; k <= 50; ++k)
        EXPECT_EQ(mapTickToSourceFrame(*clip, k * 4000, nullptr).frame, k);
}

TEST(VideoFrameMapping, SpeedAndOffsetsCompose)
{
    // timelineIn 4800, sourceIn 1 s, speed 2, 24 fps:
    // frame(k) = 24 + 2k.
    const auto clip = makeClip(4800, kTicksPerSecond, 2.0, 24.0);
    for (int64_t k = 0; k <= 20; ++k)
        EXPECT_EQ(mapTickToSourceFrame(*clip, 4800 + k * 2000, nullptr).frame,
                  24 + 2 * k);
}

// ── Transition overlap: negative srcTick clamps to 0 ────────────────────────

TEST(VideoFrameMapping, NegativeSrcTickClampsToFrame0)
{
    // During transition overlap the incoming clip is asked for ticks before
    // its timelineIn; frame 0 is the correct visual (clamp, NOT skip).
    const auto clip = makeClip(48000, 0, 1.0, 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 24000, nullptr).frame, 0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 0, nullptr).frame, 0);

    // Even with a sourceIn offset the clamp floor is source tick 0.
    const auto clipIn = makeClip(48000, 4000, 1.0, 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clipIn, 0, nullptr).frame, 0);
}

// ── fps resolution priority: clip > media info > 24 default ─────────────────

TEST(VideoFrameMapping, ClipSourceFpsBeatsMediaInfoFps)
{
    const auto clip = makeClip(0, 0, 1.0, 30.0);
    const auto info = makeInfo(100000, 60.0);
    const auto m = mapTickToSourceFrame(*clip, 1600, &info);
    EXPECT_EQ(m.frame, 1);              // 30 fps mapping, not 60
    EXPECT_DOUBLE_EQ(m.fps, 30.0);
}

TEST(VideoFrameMapping, MediaInfoFpsUsedWhenClipHasNone)
{
    const auto clip = makeClip(0, 0, 1.0, 0.0);
    const auto info = makeInfo(100000, 60.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 800, &info).frame, 1);   // 60 fps
    const auto m = mapTickToSourceFrame(*clip, 1600, &info);
    EXPECT_EQ(m.frame, 2);
    EXPECT_DOUBLE_EQ(m.fps, 60.0);
}

TEST(VideoFrameMapping, DefaultsTo24FpsWithoutClipOrMediaFps)
{
    const auto clip = makeClip(0, 0, 1.0, 0.0);
    const auto m = mapTickToSourceFrame(*clip, 2000, nullptr);
    EXPECT_EQ(m.frame, 1);
    EXPECT_DOUBLE_EQ(m.fps, 24.0);
}

// ── frameCount policy: stills pin, characters wrap, others clamp ────────────

TEST(VideoFrameMapping, StillImageAlwaysFrame0)
{
    const auto clip = makeClip(0, 0, 1.0, 24.0);
    const auto one  = makeInfo(1, 24.0);
    const auto zero = makeInfo(0, 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 0, &one).frame, 0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 500 * 2000, &one).frame, 0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 500 * 2000, &zero).frame, 0);
}

TEST(VideoFrameMapping, NonCharacterClampsToLastFrame)
{
    const auto clip = makeClip(0, 0, 1.0, 24.0);
    const auto info = makeInfo(10, 24.0);
    // Raw frame 50 → clamps to frameCount-1 = 9.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 50 * 2000, &info).frame, 9);
    // In range stays untouched.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 7 * 2000, &info).frame, 7);
}

TEST(VideoFrameMapping, VideoCharacterWrapsIdleLoop)
{
    auto clip = makeClip(0, 0, 1.0, 24.0);
    clip->setCharacterName("Wells");         // isVideoCharacter() == true
    const auto info = makeInfo(10, 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 53 * 2000, &info).frame, 3);   // 53 % 10
    EXPECT_EQ(mapTickToSourceFrame(*clip, 10 * 2000, &info).frame, 0);   // 10 % 10
    EXPECT_EQ(mapTickToSourceFrame(*clip, 9 * 2000, &info).frame, 9);
}

// ── Speed ramps (keyframed effectiveSpeed, POINTWISE per contract) ───────────

TEST(VideoFrameMapping, ConstantRampMultipliesBaseSpeed)
{
    // effectiveSpeed = speed × ramp: base 1.5 × ramp 2.0 = 3.0.
    auto clip = makeClip(0, 0, 1.5, 24.0);
    clip->speedRamp().addKeyframe(0, 2.0f);
    for (int64_t k = 0; k <= 20; ++k)
        EXPECT_EQ(mapTickToSourceFrame(*clip, k * 2000, nullptr).frame, 3 * k);
}

TEST(VideoFrameMapping, LinearAccelRampBoundaryValues)
{
    // Ramp 1.0 → 2.0 over [0, 48000], base speed 1, 24 fps.
    // Pointwise contract: srcTick(t) = t × (1 + t/48000); the sample points
    // are chosen so the float lerp inside KeyframeTrack is exact.
    auto clip = makeClip(0, 0, 1.0, 24.0);
    clip->speedRamp().addKeyframe(0,     1.0f);
    clip->speedRamp().addKeyframe(48000, 2.0f);

    EXPECT_EQ(mapTickToSourceFrame(*clip, 0,     nullptr).frame, 0);
    // t=12000: speed 1.25 → srcTick 15000 → 7.5 frames → llround = 8.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 12000, nullptr).frame, 8);
    // t=24000: speed 1.5  → srcTick 36000 → frame 18.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 24000, nullptr).frame, 18);
    // t=36000: speed 1.75 → srcTick 63000 → 31.5 → llround = 32.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 36000, nullptr).frame, 32);
    // t=48000 (last key): speed 2.0 → srcTick 96000 → frame 48.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 48000, nullptr).frame, 48);
    // Past the last keyframe the ramp extrapolates constant (2.0).
    EXPECT_EQ(mapTickToSourceFrame(*clip, 96000, nullptr).frame, 96);
}

TEST(VideoFrameMapping, LinearAccelRampIsMonotonic)
{
    // An accelerating ramp must never move the source backwards: with
    // s(t) non-decreasing and positive, t×s(t) is non-decreasing.
    auto clip = makeClip(0, 0, 1.0, 24.0);
    clip->speedRamp().addKeyframe(0,     1.0f);
    clip->speedRamp().addKeyframe(48000, 2.0f);

    int64_t prev = -1;
    for (int64_t t = 0; t <= 96000; t += 400) {
        const int64_t frame = mapTickToSourceFrame(*clip, t, nullptr).frame;
        EXPECT_GE(frame, prev) << "source went backwards at localTick " << t;
        prev = frame;
    }
}

TEST(VideoFrameMapping, RampExtrapolatesConstantBeforeFirstKeyframe)
{
    auto clip = makeClip(0, 0, 1.0, 24.0);
    clip->speedRamp().addKeyframe(24000, 1.0f);
    clip->speedRamp().addKeyframe(48000, 2.0f);

    // Before the first keyframe: constant 1.0.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 12000, nullptr).frame, 6);
    // Segment midpoint t=36000: speed 1.5 → srcTick 54000 → frame 27.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 36000, nullptr).frame, 27);
    // After the last keyframe: constant 2.0 → srcTick 144000 → frame 72.
    EXPECT_EQ(mapTickToSourceFrame(*clip, 72000, nullptr).frame, 72);
}

TEST(VideoFrameMapping, SpeedZeroHoldsFrame)
{
    // speed 0 → srcTick pinned at sourceIn: a frame hold on source frame 48.
    const auto clip = makeClip(0, 2 * kTicksPerSecond, 0.0, 24.0);
    const auto info = makeInfo(100, 24.0);
    for (int64_t t : {int64_t(0), int64_t(2000), int64_t(94000), int64_t(999999)})
        EXPECT_EQ(mapTickToSourceFrame(*clip, t, &info).frame, 48) << "tick " << t;
}

TEST(VideoFrameMapping, DecelRampFollowsPointwiseContractNotIntegration)
{
    // DOCUMENTS THE CONTRACT: the header pins srcTick to
    // localTick × effectiveSpeed(localTick) — a POINTWISE product (matching
    // EditOperationsTrim), not an integral of the ramp.  A consequence is
    // that a steep decelerating ramp moves the source BACKWARDS (here
    // 9 → 12 → 9 → 0): t×s(t) is not monotonic when s falls fast enough.
    // A time-integrated model would keep advancing.  If the mapping is ever
    // changed to integrate the ramp, these expectations (and the header
    // contract, and EditOperationsTrim) must all change together.
    auto clip = makeClip(0, 0, 1.0, 24.0);
    clip->speedRamp().addKeyframe(0,     2.0f);
    clip->speedRamp().addKeyframe(48000, 0.0f);

    EXPECT_EQ(mapTickToSourceFrame(*clip, 12000, nullptr).frame, 9);   // s=1.5
    EXPECT_EQ(mapTickToSourceFrame(*clip, 24000, nullptr).frame, 12);  // s=1.0
    EXPECT_EQ(mapTickToSourceFrame(*clip, 36000, nullptr).frame, 9);   // s=0.5
    EXPECT_EQ(mapTickToSourceFrame(*clip, 48000, nullptr).frame, 0);   // s=0.0
}

} // namespace
} // namespace rt
