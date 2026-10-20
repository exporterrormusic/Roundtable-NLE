/*
 * test_clip_playback.cpp — audiosync::ClipPlayback: what AudioSync plays for
 * a sync clip (trimmed, deleted regions cut out, at the engine rate) and the
 * map between that buffer and the source file's time.
 *
 * The source audio's sample i holds the value i, so a test reads which
 * source samples were kept. Times are quarters of a second (exact in binary).
 */

#include <gtest/gtest.h>

#include "audiosync/ClipPlayback.h"

#include <utility>
#include <vector>

using rt::AudioSampleData;
using rt::audiosync::ClipPlayback;
using rt::audiosync::buildClipPlayback;

namespace {

constexpr double kRate = 1000.0;

/// 3 s at 1 kHz; sample i == i.
AudioSampleData ramp()
{
    std::vector<float> samples(3000);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(i);
    AudioSampleData audio;
    audio.sampleRate = 1000;
    audio.samples = rt::SampleBuffer(std::move(samples));
    return audio;
}

using Map = std::vector<std::pair<int64_t, double>>;

} // namespace

TEST(ClipPlayback, WithoutCutsItIsTheTrimmedRange)
{
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {}, kRate);
    ASSERT_EQ(p.samples.size(), 1500u);
    EXPECT_EQ(p.samples.front(), 500.0f);
    EXPECT_EQ(p.samples.back(), 1999.0f);
    EXPECT_EQ(p.timeMap, (Map{{0, 0.5}}));
    EXPECT_EQ(p.rate, kRate);
}

TEST(ClipPlayback, ADeletedRegionIsCutOut)
{
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {{1.0, 1.25}}, kRate);
    ASSERT_EQ(p.samples.size(), 1250u);
    EXPECT_EQ(p.samples[499], 999.0f);
    EXPECT_EQ(p.samples[500], 1250.0f);
    EXPECT_EQ(p.timeMap, (Map{{0, 0.5}, {500, 1.25}}));
}

TEST(ClipPlayback, RegionsAreSortedAndClippedToTheClip)
{
    // Given out of order; the second one starts before the clip does.
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {{1.5, 1.75}, {0.25, 0.75}}, kRate);
    ASSERT_EQ(p.samples.size(), 1000u);
    EXPECT_EQ(p.samples.front(), 750.0f);
    EXPECT_EQ(p.samples[749], 1499.0f);
    EXPECT_EQ(p.samples[750], 1750.0f);
    EXPECT_EQ(p.timeMap, (Map{{0, 0.75}, {750, 1.75}}));
}

TEST(ClipPlayback, RegionsAtOrPastTheEnd)
{
    const ClipPlayback past = buildClipPlayback(ramp(), 0.5, 2.0, {{2.5, 2.75}}, kRate);
    EXPECT_EQ(past.samples.size(), 1500u);
    EXPECT_EQ(past.timeMap, (Map{{0, 0.5}}));

    const ClipPlayback tail = buildClipPlayback(ramp(), 0.5, 2.0, {{1.75, 2.5}}, kRate);
    EXPECT_EQ(tail.samples.size(), 1250u);
    EXPECT_EQ(tail.samples.back(), 1749.0f);
    EXPECT_EQ(tail.timeMap, (Map{{0, 0.5}}));
}

TEST(ClipPlayback, AClipPastTheAudioIsClippedOrEmpty)
{
    const ClipPlayback clipped = buildClipPlayback(ramp(), 2.5, 4.0, {}, kRate);
    EXPECT_EQ(clipped.samples.size(), 500u);
    EXPECT_EQ(clipped.samples.back(), 2999.0f);

    const ClipPlayback none = buildClipPlayback(ramp(), 3.5, 4.0, {}, kRate);
    EXPECT_TRUE(none.samples.empty());
    EXPECT_TRUE(none.timeMap.empty());
}

TEST(ClipPlayback, ADifferentEngineRateResamplesAndScalesTheMap)
{
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {{1.0, 1.25}}, 2000.0);
    ASSERT_EQ(p.samples.size(), 2501u);   // 1250 * 2 + 1
    EXPECT_EQ(p.samples[0], 500.0f);
    EXPECT_EQ(p.samples[1], 500.5f);       // linear between neighbours
    EXPECT_EQ(p.samples[2], 501.0f);
    EXPECT_EQ(p.timeMap, (Map{{0, 0.5}, {1000, 1.25}}));
    EXPECT_EQ(p.rate, 2000.0);

    // Within 1 Hz: played as is.
    const ClipPlayback same = buildClipPlayback(ramp(), 0.5, 2.0, {}, 1000.5);
    EXPECT_EQ(same.samples.size(), 1500u);
    EXPECT_EQ(same.rate, kRate);
}

TEST(ClipPlayback, BufferFramesMapBackToSourceTime)
{
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {{1.0, 1.25}}, kRate);
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(0, -1.0), 0.5);
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(499, -1.0), 0.999);
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(500, -1.0), 1.25);   // across the cut
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(1249, -1.0), 1.999);
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(1250, -1.0), -1.0);  // past the end
    EXPECT_DOUBLE_EQ(p.sourceTimeAt(-5, 7.0), 7.0);
}

TEST(ClipPlayback, SourceTimesMapToBufferFrames)
{
    const ClipPlayback p = buildClipPlayback(ramp(), 0.5, 2.0, {{1.0, 1.25}}, kRate);
    EXPECT_EQ(p.frameAt(0.5), 0);
    EXPECT_EQ(p.frameAt(0.75), 250);
    EXPECT_EQ(p.frameAt(1.125), 500);   // inside the cut: the next kept stretch
    EXPECT_EQ(p.frameAt(1.5), 750);
    EXPECT_EQ(p.frameAt(0.25), 0);      // before the clip
    EXPECT_EQ(p.frameAt(3.0), 1250);    // past it: the end
}
