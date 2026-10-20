/*
 * test_audio_sync_player.cpp — AudioSyncPlayer on an AudioEngine with no
 * audio device (transport calls that need a stream are no-ops; positions,
 * sources and the sync clock are real): every kind of playback detaches the
 * timeline's clock and gives it back, and clips start and seek on the
 * frames ClipPlayback maps.
 */

#include <gtest/gtest.h>

#include "panels/audio/AudioSyncPlayer.h"
#include "audio/AudioEngine.h"
#include "playback/AVSyncClock.h"

#include <utility>
#include <vector>

using rt::AudioSampleData;
using rt::AudioSyncPlayer;
using rt::SyncClip;

namespace {

AudioSampleData tone(double seconds, uint32_t rate = 16000)
{
    AudioSampleData audio;
    audio.sampleRate = rate;
    audio.samples = rt::SampleBuffer(
        std::vector<float>(static_cast<size_t>(seconds * rate), 0.25f));
    return audio;
}

SyncClip clipOf(double start, double end, std::vector<std::pair<double, double>> cuts = {})
{
    SyncClip clip;
    clip.id = 7;
    clip.start = start;
    clip.end = end;
    clip.deletedRegions = std::move(cuts);
    return clip;
}

class AudioSyncPlayerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        engine.setSyncClock(&timelineClock);
        player.setEngine(&engine);
    }
    void TearDown() override
    {
        engine.clearTrackSources();
        engine.setSyncClock(nullptr);
    }

    rt::AVSyncClock timelineClock;
    rt::AudioEngine engine;
    AudioSyncPlayer player;
    const AudioSampleData audio = tone(4.0);
};

} // namespace

TEST_F(AudioSyncPlayerTest, AClipDetachesTheTimelineClockUntilStopped)
{
    ASSERT_TRUE(player.playClip(audio, clipOf(0.5, 2.0), 0.5));
    EXPECT_EQ(engine.syncClock(), nullptr);
    EXPECT_TRUE(engine.hasTrackSources());
    player.stop();
    EXPECT_EQ(engine.syncClock(), &timelineClock);
    EXPECT_FALSE(engine.hasTrackSources());
}

TEST_F(AudioSyncPlayerTest, AClipStartsAndSeeksOnTheMappedFrames)
{
    const SyncClip clip = clipOf(0.5, 3.0, {{1.0, 1.5}});
    const auto expected = rt::audiosync::buildClipPlayback(
        audio, clip.start, clip.end, clip.deletedRegions,
        static_cast<double>(engine.config().sampleRate));

    ASSERT_TRUE(player.playClip(audio, clip, 2.0));
    EXPECT_EQ(engine.currentFrame(), expected.frameAt(2.0));
    EXPECT_DOUBLE_EQ(player.clipTime(-1.0), 2.0);

    // From the clip start (or before it): the first frame.
    player.stop();
    ASSERT_TRUE(player.playClip(audio, clip, 0.5));
    EXPECT_EQ(engine.currentFrame(), 0);

    // A seek into the cut lands on its end.
    EXPECT_DOUBLE_EQ(player.seekClip(clip, 1.25), 1.5);
    EXPECT_EQ(engine.currentFrame(), expected.frameAt(1.5));
    EXPECT_DOUBLE_EQ(player.clipTime(-1.0), 1.5);
    EXPECT_DOUBLE_EQ(player.seekClip(clip, 2.5), 2.5);
    EXPECT_DOUBLE_EQ(player.clipTime(-1.0), 2.5);
    player.stop();
}

TEST_F(AudioSyncPlayerTest, AClipWithNoAudioInRangeDoesNotPlay)
{
    EXPECT_FALSE(player.playClip(audio, clipOf(5.0, 6.0), 5.0));
    EXPECT_EQ(engine.syncClock(), &timelineClock);
    EXPECT_FALSE(engine.hasTrackSources());
}

TEST_F(AudioSyncPlayerTest, AScrubGivesTheClockBackAtOnce)
{
    player.scrub(audio, 1.0);
    EXPECT_EQ(engine.syncClock(), &timelineClock);
    EXPECT_EQ(engine.currentFrame(), 16000);   // the source's own rate
    EXPECT_EQ(engine.transportState(), rt::TransportState::Scrubbing);
}

TEST_F(AudioSyncPlayerTest, APreviewIsKeptByKeyAndStopsCleanly)
{
    EXPECT_FALSE(player.hasPreview("take|1"));
    ASSERT_TRUE(player.loadPreview("take|1", audio, 1.0, 2.0));
    EXPECT_TRUE(player.previewActive());
    EXPECT_TRUE(player.hasPreview("take|1"));
    EXPECT_FALSE(player.hasPreview("take|2"));
    EXPECT_EQ(engine.syncClock(), nullptr);
    EXPECT_DOUBLE_EQ(player.previewTime(), -1.0);   // not playing (no device)

    player.stopPreview();
    EXPECT_FALSE(player.previewActive());
    EXPECT_FALSE(player.hasPreview("take|1"));
    EXPECT_EQ(engine.syncClock(), &timelineClock);
    EXPECT_FALSE(engine.hasTrackSources());

    // An empty range loads nothing.
    EXPECT_FALSE(player.loadPreview("take|3", audio, 2.0, 2.0));
    EXPECT_EQ(engine.syncClock(), &timelineClock);
}

TEST_F(AudioSyncPlayerTest, StopEndsAPreviewToo)
{
    ASSERT_TRUE(player.loadPreview("take|1", audio, 0.0, 1.0));
    player.stop();
    EXPECT_FALSE(player.previewActive());
    EXPECT_EQ(engine.syncClock(), &timelineClock);
}

TEST_F(AudioSyncPlayerTest, AFilePreviewDetachesTheClockUntilStopped)
{
    player.playFile(audio, 8000);
    EXPECT_EQ(engine.syncClock(), nullptr);
    EXPECT_EQ(engine.currentFrame(), 8000);
    player.stopFile();
    EXPECT_EQ(engine.syncClock(), &timelineClock);
    EXPECT_FALSE(engine.hasTrackSources());
}

TEST_F(AudioSyncPlayerTest, ADraftPlaysUntilStopped)
{
    EXPECT_FALSE(player.playDraft({}, 1, 48000));
    ASSERT_TRUE(player.playDraft(std::vector<float>(9600, 0.1f), 2, 48000));
    EXPECT_TRUE(player.draftActive());
    EXPECT_EQ(engine.syncClock(), nullptr);
    player.stop();
    EXPECT_FALSE(player.draftActive());
    EXPECT_EQ(engine.syncClock(), &timelineClock);
}

TEST(AudioSyncPlayerNoEngine, EverythingIsANoOp)
{
    AudioSyncPlayer player;
    const AudioSampleData audio = tone(1.0);
    EXPECT_FALSE(player.playClip(audio, clipOf(0.0, 1.0), 0.0));
    EXPECT_FALSE(player.loadPreview("k", audio, 0.0, 1.0));
    EXPECT_FALSE(player.playDraft(std::vector<float>(10, 0.0f), 1, 48000));
    EXPECT_FALSE(player.isPlaying());
    EXPECT_DOUBLE_EQ(player.clipTime(3.0), 3.0);
    player.playFile(audio, 0);
    player.scrub(audio, 0.5);
    player.stopFile();
    player.stop();
}
