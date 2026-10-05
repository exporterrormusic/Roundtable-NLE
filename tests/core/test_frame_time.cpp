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

// ── Frame-grid helpers ──────────────────────────────────────────────────────

constexpr RationalFrameRate kNtsc30{30000, 1001};
constexpr RationalFrameRate kNtsc60{60000, 1001};

TEST(FrameTime, NominalTicksPerFrameRoundsTheExactLength)
{
    EXPECT_EQ(nominalTicksPerFrame({30, 1}), 1600);
    EXPECT_EQ(nominalTicksPerFrame({24, 1}), 2000);
    EXPECT_EQ(nominalTicksPerFrame(kNtsc30), 1602);        // 1601.6
    EXPECT_EQ(nominalTicksPerFrame(kNtsc60), 801);         // 800.8
    EXPECT_EQ(nominalTicksPerFrame({24000, 1001}), 2002);  // 2002.0
    EXPECT_DOUBLE_EQ(exactTicksPerFrame(kNtsc30), 1601.6);
}

TEST(FrameTime, FrameStartAtOrBeforeIsTheFrameOnScreen)
{
    EXPECT_EQ(frameStartAtOrBefore(0, kNtsc30), 0);
    EXPECT_EQ(frameStartAtOrBefore(1601, kNtsc30), 0);     // frame 1 starts at 1602
    EXPECT_EQ(frameStartAtOrBefore(1602, kNtsc30), 1602);
    EXPECT_EQ(frameStartAtOrBefore(3202, kNtsc30), 1602);
    EXPECT_EQ(frameStartAtOrBefore(3203, kNtsc30), 3203);
}

TEST(FrameTime, NearestFrameSnapsBothWaysAndTiesGoLater)
{
    EXPECT_EQ(snapTickToNearestFrame(800, kNtsc30), 0);
    EXPECT_EQ(snapTickToNearestFrame(801, kNtsc30), 1602);  // 801 / 801 tie
    EXPECT_EQ(snapTickToNearestFrame(1601, kNtsc30), 1602);
    EXPECT_EQ(snapTickToNearestFrame(2400, kNtsc30), 1602);
    EXPECT_EQ(snapTickToNearestFrame(2403, kNtsc30), 3203);
    // Integer rates: identical to round-half-up on a 1600-tick grid.
    for (int64_t t = 0; t < 20000; t += 7)
        ASSERT_EQ(snapTickToNearestFrame(t, {30, 1}), ((t + 800) / 1600) * 1600) << t;
}

TEST(FrameTime, NearestFrameSnapsSignedOffsetsToWholeFrames)
{
    EXPECT_EQ(snapTickToNearestFrame(-1601, kNtsc30), -1602);
    EXPECT_EQ(snapTickToNearestFrame(-700, kNtsc30), 0);
    EXPECT_EQ(snapTickToNearestFrame(-3000, kNtsc30), -3203);
}

TEST(FrameTime, SteppingNeverDriftsOffTheGrid)
{
    // A fixed 1601-tick step (the old 48000/fps truncation) loses a frame
    // every ~2,669 frames at 29.97; stepping by frame index never does.
    for (RationalFrameRate rate : {kNtsc30, kNtsc60, RationalFrameRate{24000, 1001}}) {
        int64_t tick = 0;
        for (int64_t frame = 1; frame <= 6000; ++frame) {
            tick = stepFrames(tick, 1, rate);
            ASSERT_EQ(tick, frameIndexToTick(frame, rate))
                << rate.numerator << "/" << rate.denominator << " frame=" << frame;
        }
        for (int64_t frame = 5999; frame >= 0; --frame) {
            tick = stepFrames(tick, -1, rate);
            ASSERT_EQ(tick, frameIndexToTick(frame, rate));
        }
        EXPECT_EQ(stepFrames(0, -1, rate), 0);   // clamps at frame 0
    }
}

TEST(FrameTime, SteppingFromMidFrameAlwaysChangesTheFrame)
{
    // 2400 is inside frame 1 (1602..3202).
    EXPECT_EQ(stepFrames(2400, 1, kNtsc30), 3203);
    EXPECT_EQ(stepFrames(2400, -1, kNtsc30), 0);
}

TEST(FrameTime, TruncatedPlaybackGridWouldMissPreRenderedFrames)
{
    // Regression for the old preview clock: frameNum * int(48000 / fps).
    // Its ticks almost never equal the pre-render keys (frameIndexToTick),
    // so "Render In to Out" segments were not served during NTSC playback.
    const int64_t truncated = static_cast<int64_t>(48000.0 / 29.97);   // 1601
    int matches = 0;
    for (int64_t frame = 1; frame <= 1000; ++frame)
        matches += (frame * truncated == frameIndexToTick(frame, kNtsc30)) ? 1 : 0;
    EXPECT_LT(matches, 5);
    // The canonical grid is its own fixed point.
    for (int64_t frame = 0; frame <= 1000; ++frame) {
        const int64_t tick = frameIndexToTick(frame, kNtsc30);
        ASSERT_EQ(frameStartAtOrBefore(tick, kNtsc30), tick);
        ASSERT_EQ(snapTickToNearestFrame(tick, kNtsc30), tick);
    }
}

} // namespace
} // namespace rt
