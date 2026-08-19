/*
 * ROUNDTABLE NLE v2 — Timeline unit tests
 * Step 3: Core data model validation
 */

#include <gtest/gtest.h>
#include "Constants.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/MediaRelinker.h"
#include "timeline/TitleClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/TimelineObserver.h"
#include "project/Project.h"

using namespace rt;

// ── Test observer to capture callbacks ──────────────────────────────────────

class TestObserver : public TimelineObserver
{
public:
    int trackAddedCount{0};
    int trackRemovedCount{0};
    int trackMovedCount{0};
    int markerChangedCount{0};
    int playheadChangedCount{0};
    int inOutChangedCount{0};
    int structureChangedCount{0};

    // RAII registration: an observer MUST deregister before it dies (the same
    // contract production observers honour in their dtors), otherwise ~Timeline
    // would notify freed memory. Binding to the Timeline on construction and
    // removing on destruction makes that automatic for every test.
    explicit TestObserver(Timeline& tl) : m_timeline(&tl) { m_timeline->addObserver(this); }
    ~TestObserver() override { if (m_timeline) m_timeline->removeObserver(this); }

    void onTrackAdded(size_t) override      { ++trackAddedCount; }
    void onTrackRemoved(size_t) override    { ++trackRemovedCount; }
    void onTrackMoved(size_t, size_t) override { ++trackMovedCount; }
    void onMarkerChanged() override         { ++markerChangedCount; }
    void onPlayheadChanged(int64_t) override { ++playheadChangedCount; }
    void onInOutChanged() override          { ++inOutChangedCount; }
    void onTimelineStructureChanged() override { ++structureChangedCount; }
    void onTimelineDestroyed(Timeline*) override { m_timeline = nullptr; }

private:
    Timeline* m_timeline{nullptr};
};

// ── Timeline fixture ────────────────────────────────────────────────────────

class TimelineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        timeline = std::make_unique<Timeline>();
    }

    std::unique_ptr<Timeline> timeline;
};

// ── Track management ────────────────────────────────────────────────────────

TEST_F(TimelineTest, StartsEmpty)
{
    EXPECT_EQ(timeline->trackCount(), 0u);
    EXPECT_EQ(timeline->duration(), 0);
}

TEST_F(TimelineTest, AddVideoTrack)
{
    auto* track = timeline->addVideoTrack("V1");
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(timeline->trackCount(), 1u);
    EXPECT_EQ(track->name(), "V1");
    EXPECT_EQ(track->type(), TrackType::Video);
}

TEST_F(TimelineTest, AddAudioTrack)
{
    auto* track = timeline->addAudioTrack("A1");
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->type(), TrackType::Audio);
}

TEST_F(TimelineTest, AddMultipleTracks)
{
    timeline->addVideoTrack("V1");
    timeline->addVideoTrack("V2");
    timeline->addAudioTrack("A1");
    EXPECT_EQ(timeline->trackCount(), 3u);
}

TEST_F(TimelineTest, RemoveTrack)
{
    timeline->addVideoTrack("V1");
    EXPECT_EQ(timeline->trackCount(), 1u);
    timeline->removeTrack(0);
    EXPECT_EQ(timeline->trackCount(), 0u);
}

TEST_F(TimelineTest, MoveTrack)
{
    timeline->addVideoTrack("V1");
    timeline->addVideoTrack("V2");
    timeline->addVideoTrack("V3");
    timeline->moveTrack(0, 2);
    EXPECT_EQ(timeline->track(0)->name(), "V2");
    EXPECT_EQ(timeline->track(2)->name(), "V1");
}

TEST_F(TimelineTest, MoveAudioTrackAcrossUserDividerIsAtomicAndKeepsRows)
{
    Track* video = timeline->addVideoTrack("Video");
    Track* permanent = timeline->addDividerTrack(1, true);
    Track* audio1 = timeline->addAudioTrack("Dialogue");
    Track* userDivider = timeline->addDividerTrack(3, false);
    Track* audio2 = timeline->addAudioTrack("Music");
    ASSERT_EQ(timeline->trackCount(), 5u);

    TestObserver observer(*timeline);
    ASSERT_TRUE(timeline->moveTrackToInsertion(2, 5));

    ASSERT_EQ(timeline->trackCount(), 5u);
    EXPECT_EQ(timeline->track(0), video);
    EXPECT_EQ(timeline->track(1), permanent);
    EXPECT_EQ(timeline->track(2), userDivider);
    EXPECT_EQ(timeline->track(3), audio2);
    EXPECT_EQ(timeline->track(4), audio1);
    EXPECT_EQ(observer.trackMovedCount, 1);
    EXPECT_EQ(observer.trackAddedCount, 0);
    EXPECT_EQ(observer.trackRemovedCount, 0);
}

TEST_F(TimelineTest, TrackInsertionCannotBreakPermanentMediaBoundary)
{
    Track* video1 = timeline->addVideoTrack("Video 1");
    Track* video2 = timeline->addVideoTrack("Video 2");
    Track* permanent = timeline->addDividerTrack(2, true);
    Track* audio1 = timeline->addAudioTrack("Audio 1");
    Track* audio2 = timeline->addAudioTrack("Audio 2");

    // Dragging audio above the boundary clamps it to the first audio slot.
    ASSERT_TRUE(timeline->moveTrackToInsertion(4, 0));
    EXPECT_EQ(timeline->track(0), video1);
    EXPECT_EQ(timeline->track(1), video2);
    EXPECT_EQ(timeline->track(2), permanent);
    EXPECT_EQ(timeline->track(3), audio2);
    EXPECT_EQ(timeline->track(4), audio1);

    // Dragging video below the boundary clamps it to the last video slot.
    ASSERT_TRUE(timeline->moveTrackToInsertion(0, timeline->trackCount()));
    EXPECT_EQ(timeline->track(0), video2);
    EXPECT_EQ(timeline->track(1), video1);
    EXPECT_EQ(timeline->track(2), permanent);
    EXPECT_EQ(timeline->track(3), audio2);
    EXPECT_EQ(timeline->track(4), audio1);
}

TEST_F(TimelineTest, UserDividerCanMoveAcrossPermanentDivider)
{
    Track* video = timeline->addVideoTrack("Video");
    Track* permanent = timeline->addDividerTrack(1, true);
    Track* audio = timeline->addAudioTrack("Audio");
    Track* userDivider = timeline->addDividerTrack(3, false);

    ASSERT_TRUE(timeline->moveTrackToInsertion(3, 1));
    ASSERT_EQ(timeline->trackCount(), 4u);
    EXPECT_EQ(timeline->track(0), video);
    EXPECT_EQ(timeline->track(1), userDivider);
    EXPECT_EQ(timeline->track(2), permanent);
    EXPECT_EQ(timeline->track(3), audio);
    EXPECT_TRUE(timeline->track(1)->isDivider());
    EXPECT_FALSE(timeline->track(1)->isPermanentDivider());
    EXPECT_TRUE(timeline->track(2)->isPermanentDivider());
}

TEST_F(TimelineTest, ClipHostLookupSkipsDividerBetweenExportAndMusic)
{
    Track* exportTrack = timeline->addAudioTrack("EXPORT");
    Track* musicTrack = timeline->addAudioTrack("MUSIC");
    Track* divider = timeline->addDividerTrack(1, false);

    ASSERT_EQ(timeline->track(0), exportTrack);
    ASSERT_EQ(timeline->track(1), divider);
    ASSERT_EQ(timeline->track(2), musicTrack);

    // Hovering the divider must resolve to a real audio track, never the
    // divider itself. Once the cursor reaches MUSIC it resolves there.
    EXPECT_EQ(timeline->nearestClipHostTrack(1, TrackType::Audio), 0u);
    EXPECT_EQ(timeline->nearestClipHostTrack(2, TrackType::Audio), 2u);
    EXPECT_EQ(timeline->nearestClipHostTrack(-10, TrackType::Audio), 0u);
    EXPECT_EQ(timeline->nearestClipHostTrack(99, TrackType::Audio), 2u);

    musicTrack->setLocked(true);
    EXPECT_EQ(timeline->nearestClipHostTrack(2, TrackType::Audio), 0u);
    exportTrack->setLocked(true);
    EXPECT_EQ(timeline->nearestClipHostTrack(1, TrackType::Audio),
              static_cast<size_t>(-1));
}

TEST_F(TimelineTest, AccessTrackByIndex)
{
    timeline->addVideoTrack("V1");
    timeline->addAudioTrack("A1");
    EXPECT_EQ(timeline->track(0)->name(), "V1");
    EXPECT_EQ(timeline->track(1)->name(), "A1");
}

TEST_F(TimelineTest, AccessOutOfBoundsReturnsNull)
{
    EXPECT_EQ(timeline->track(0), nullptr);
    EXPECT_EQ(timeline->track(99), nullptr);
}

// ── Playhead ────────────────────────────────────────────────────────────────

TEST_F(TimelineTest, PlayheadStartsAtZero)
{
    EXPECT_EQ(timeline->playheadPosition(), 0);
}

TEST_F(TimelineTest, SetPlayhead)
{
    timeline->setPlayheadPosition(kTicksPerSecond); // 1 second
    EXPECT_EQ(timeline->playheadPosition(), kTicksPerSecond);
}

// ── Markers ─────────────────────────────────────────────────────────────────

TEST_F(TimelineTest, AddMarker)
{
    timeline->addMarker(kTicksPerSecond, "Chapter 1");
    EXPECT_EQ(timeline->markers().size(), 1u);
    EXPECT_EQ(timeline->markers()[0].label, "Chapter 1");
}

TEST_F(TimelineTest, MarkersAreSortedByTime)
{
    timeline->addMarker(96000, "C");
    timeline->addMarker(24000, "A");
    timeline->addMarker(48000, "B");
    ASSERT_EQ(timeline->markers().size(), 3u);
    EXPECT_EQ(timeline->markers()[0].label, "A");
    EXPECT_EQ(timeline->markers()[1].label, "B");
    EXPECT_EQ(timeline->markers()[2].label, "C");
}

TEST_F(TimelineTest, RemoveMarker)
{
    timeline->addMarker(kTicksPerSecond, "Chapter 1");
    timeline->removeMarker(0);
    EXPECT_TRUE(timeline->markers().empty());
}

// ── In/Out Points ───────────────────────────────────────────────────────────

TEST_F(TimelineTest, InOutPointsDefault)
{
    EXPECT_EQ(timeline->inPoint(), -1);
    EXPECT_EQ(timeline->outPoint(), -1);
}

TEST_F(TimelineTest, SetInOutPoints)
{
    timeline->setInPoint(24000);
    timeline->setOutPoint(96000);
    EXPECT_EQ(timeline->inPoint(), 24000);
    EXPECT_EQ(timeline->outPoint(), 96000);
}

TEST_F(TimelineTest, ClearInOutPoints)
{
    timeline->setInPoint(24000);
    timeline->setOutPoint(96000);
    timeline->clearInOutPoints();
    EXPECT_EQ(timeline->inPoint(), -1);
    EXPECT_EQ(timeline->outPoint(), -1);
}

// ── Name ────────────────────────────────────────────────────────────────────

TEST_F(TimelineTest, DefaultName)
{
    EXPECT_EQ(timeline->name(), "Sequence 1");
}

TEST_F(TimelineTest, SetName)
{
    timeline->setName("My Sequence");
    EXPECT_EQ(timeline->name(), "My Sequence");
}

// ── Duration ────────────────────────────────────────────────────────────────

TEST_F(TimelineTest, DurationReflectsClips)
{
    auto* track = timeline->addVideoTrack("V1");
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(48000); // 1 second
    track->addClip(std::move(clip));
    EXPECT_EQ(timeline->duration(), 48000);
}

TEST_F(TimelineTest, DurationIsMaxAcrossTracks)
{
    auto* v1 = timeline->addVideoTrack("V1");
    auto* v2 = timeline->addVideoTrack("V2");

    auto c1 = std::make_unique<SpineClip>();
    c1->setDuration(48000);
    v1->addClip(std::move(c1));

    auto c2 = std::make_unique<SpineClip>();
    c2->setTimelineIn(0);
    c2->setDuration(96000);
    v2->addClip(std::move(c2));

    EXPECT_EQ(timeline->duration(), 96000);
}

// ── Observer callbacks ──────────────────────────────────────────────────────

TEST_F(TimelineTest, ObserverTrackAdded)
{
    TestObserver obs(*timeline);
    timeline->addVideoTrack("V1");
    EXPECT_EQ(obs.trackAddedCount, 1);
}

TEST_F(TimelineTest, ObserverTrackRemoved)
{
    TestObserver obs(*timeline);
    timeline->addVideoTrack("V1");
    timeline->removeTrack(0);
    EXPECT_EQ(obs.trackRemovedCount, 1);
}

TEST_F(TimelineTest, ObserverTrackMoved)
{
    TestObserver obs(*timeline);
    timeline->addVideoTrack("V1");
    timeline->addVideoTrack("V2");
    timeline->moveTrack(0, 1);
    EXPECT_EQ(obs.trackMovedCount, 1);
}

TEST_F(TimelineTest, ObserverMarkerChanged)
{
    TestObserver obs(*timeline);
    timeline->addMarker(48000, "M1");
    EXPECT_EQ(obs.markerChangedCount, 1);
    timeline->removeMarker(0);
    EXPECT_EQ(obs.markerChangedCount, 2);
}

TEST_F(TimelineTest, ObserverPlayheadChanged)
{
    TestObserver obs(*timeline);
    timeline->setPlayheadPosition(48000);
    EXPECT_EQ(obs.playheadChangedCount, 1);
    // Setting same position doesn't fire again
    timeline->setPlayheadPosition(48000);
    EXPECT_EQ(obs.playheadChangedCount, 1);
}

TEST_F(TimelineTest, ObserverInOutChanged)
{
    TestObserver obs(*timeline);
    timeline->setInPoint(24000);
    EXPECT_EQ(obs.inOutChangedCount, 1);
    timeline->setOutPoint(96000);
    EXPECT_EQ(obs.inOutChangedCount, 2);
    timeline->clearInOutPoints();
    EXPECT_EQ(obs.inOutChangedCount, 3);
}

TEST_F(TimelineTest, RemoveObserverStopsCallbacks)
{
    TestObserver obs(*timeline);
    timeline->addVideoTrack("V1");
    EXPECT_EQ(obs.trackAddedCount, 1);
    timeline->removeObserver(&obs);
    timeline->addVideoTrack("V2");
    EXPECT_EQ(obs.trackAddedCount, 1); // No change
}

// ── Time conversion ─────────────────────────────────────────────────────────

TEST(TimeConversion, SecondsToTicks)
{
    EXPECT_EQ(secondsToTicks(1.0), 48000);
    EXPECT_EQ(secondsToTicks(0.5), 24000);
    EXPECT_EQ(secondsToTicks(0.0), 0);
}

TEST(TimeConversion, TicksToSeconds)
{
    EXPECT_DOUBLE_EQ(ticksToSeconds(48000), 1.0);
    EXPECT_DOUBLE_EQ(ticksToSeconds(24000), 0.5);
    EXPECT_DOUBLE_EQ(ticksToSeconds(0), 0.0);
}

TEST(MediaRelinker, UpdatesMatchingClipsAcrossEveryProjectSequence)
{
    Project project;
    auto* first = project.timeline();
    ASSERT_NE(first, nullptr);
    auto* firstVideo = first->addVideoTrack("V1");
    auto* firstAudio = first->addAudioTrack("A1");

    auto video = std::make_unique<VideoClip>("offline/source.mov");
    video->setDuration(48000);
    auto* videoPtr = static_cast<VideoClip*>(firstVideo->addClip(std::move(video)));
    auto audio = std::make_unique<AudioClip>("offline/source.mov");
    audio->setDuration(48000);
    auto* audioPtr = static_cast<AudioClip*>(firstAudio->addClip(std::move(audio)));

    auto* second = project.addSequence("Second");
    ASSERT_NE(second, nullptr);
    auto* secondVideo = second->addVideoTrack("V1");
    auto image = std::make_unique<ImageClip>("offline/source.mov");
    image->setDuration(48000);
    auto* imagePtr = static_cast<ImageClip*>(secondVideo->addClip(std::move(image)));

    auto unrelated = std::make_unique<VideoClip>("already/online.mov");
    unrelated->setDuration(48000);
    auto* unrelatedPtr =
        static_cast<VideoClip*>(secondVideo->addClip(std::move(unrelated)));

    EXPECT_EQ(MediaRelinker::relinkPath(
                  &project, "offline/source.mov", "fixed/source.mov"),
              3);
    EXPECT_EQ(videoPtr->mediaPath(), "fixed/source.mov");
    EXPECT_EQ(audioPtr->mediaPath(), "fixed/source.mov");
    EXPECT_EQ(imagePtr->mediaPath(), "fixed/source.mov");
    EXPECT_EQ(unrelatedPtr->mediaPath(), "already/online.mov");
}
