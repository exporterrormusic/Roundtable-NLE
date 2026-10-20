/*
 * test_export_plan.cpp — audiosync::ExportPlan: which clips the AudioSync
 * export places and where (planExport), the full export and the in-place
 * re-export (applyExport), and the locked-track preflight. The shot layers
 * come from a fake PlaceGroupVisualsFn: one VideoClip per group.
 */

#include <gtest/gtest.h>

#include "audiosync/ExportPlan.h"
#include "Constants.h"
#include "ai/ScriptMatcher.h"
#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <memory>
#include <string>
#include <vector>

using namespace rt;
using namespace rt::audiosync;

namespace {

const char* const kScript =
    "ALICE: Welcome back to the morning show everyone.\n"
    "BOB: Thanks Alice it is great to be here today.\n"
    "BOB: It will be sunny with a light breeze on Saturday.\n"
    "ALICE: That sounds perfect for a picnic in the park.\n";

SyncClip clipOf(int id, int line, const std::string& file, double start, double end,
                int state = 2, float confidence = 0.9f)
{
    SyncClip c;
    c.id = id;
    c.scriptLineNumber = line;
    c.sourceFile = file;
    c.start = start;
    c.end = end;
    c.matchState = state;
    c.confidence = confidence;
    c.character = "clip-speaker";
    return c;
}

std::vector<int> idsOf(const std::vector<const SyncClip*>& clips)
{
    std::vector<int> ids;
    for (const SyncClip* c : clips) ids.push_back(c->id);
    return ids;
}

Track* trackNamed(Timeline& timeline, const std::string& name, TrackType type = TrackType::Audio)
{
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        Track* t = timeline.track(i);
        if (t && t->type() == type && t->name() == name) return t;
    }
    return nullptr;
}

/// The fake shot layers: one VideoClip per group on the first video track.
struct FakeShots
{
    Timeline* timeline{nullptr};
    std::vector<int> placedFor;   ///< group first lines, in call order

    PlaceGroupVisualsFn fn()
    {
        return [this](const PlannedGroup& group) {
            placedFor.push_back(group.firstLine);
            Track* video = nullptr;
            for (size_t i = 0; i < timeline->trackCount() && !video; ++i) {
                Track* t = timeline->track(i);
                if (t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack())
                    video = t;
            }
            if (!video) video = timeline->addVideoTrack("");
            auto clip = std::make_unique<VideoClip>("shot-" + group.character + ".mp4");
            clip->setTimelineIn(group.timelineStart);
            clip->setDuration(group.totalDuration);
            clip->setSyncLine(group.firstLine);
            video->addClip(std::move(clip));
            return 1;
        };
    }
};

class ExportPlanTest : public ::testing::Test
{
protected:
    Script script = Script::load(kScript);
    std::vector<int> lines;   ///< the script's line numbers, in order
    std::string alice;        ///< the speakers as the script names them
    std::string bob;

    void SetUp() override
    {
        ASSERT_EQ(script.lines.size(), 4u);
        for (const auto& line : script.lines) lines.push_back(line.lineNumber);
        alice = script.lines[0].character;
        bob = script.lines[1].character;
        ASSERT_FALSE(alice.empty());
        ASSERT_NE(alice, bob);
    }

    /// One confirmed clip per line, 1 s each, Alice's from alice.wav.
    std::vector<SyncClip> oneClipPerLine() const
    {
        return {clipOf(10, lines[0], "C:/audio/alice.wav", 1.0, 2.0),
                clipOf(11, lines[1], "C:/audio/bob.wav", 1.0, 2.0),
                clipOf(12, lines[2], "C:/audio/bob.wav", 3.0, 4.0),
                clipOf(13, lines[3], "C:/audio/alice.wav", 3.0, 4.0)};
    }
};

} // namespace

TEST_F(ExportPlanTest, EachLineGetsItsMostConfidentConfirmedClipInScriptOrder)
{
    const std::vector<SyncClip> clips = {
        clipOf(1, lines[2], "b.wav", 0, 1),
        clipOf(2, lines[0], "a.wav", 0, 1, 2, 0.5f),
        clipOf(3, lines[0], "a.wav", 2, 3, 2, 0.8f),   // more confident: wins line 0
        clipOf(4, lines[1], "b.wav", 2, 3, 1),         // matched, not confirmed
        clipOf(5, -1, "a.wav", 4, 5),                  // no line
    };
    EXPECT_EQ(idsOf(exportedClips(&script, clips)), (std::vector<int>{3, 1}));
    // Without a script: every confirmed clip with a line, as listed.
    EXPECT_EQ(idsOf(exportedClips(nullptr, clips)), (std::vector<int>{1, 2, 3}));
}

TEST_F(ExportPlanTest, EffectiveRangesSkipCutsAndDropSlivers)
{
    SyncClip c = clipOf(1, 0, "a.wav", 1.0, 3.0);
    EXPECT_EQ(effectiveRanges(c), (std::vector<std::pair<double, double>>{{1.0, 3.0}}));
    c.deletedRegions = {{2.5, 2.98}, {1.5, 2.0}};   // unsorted; 2.98..3.0 is under 50 ms
    EXPECT_EQ(effectiveRanges(c), (std::vector<std::pair<double, double>>{{1.0, 1.5}, {2.0, 2.5}}));
}

TEST_F(ExportPlanTest, ThePlanLaysClipsBackToBackAndGroupsRunsOfOneSpeaker)
{
    std::vector<SyncClip> clips = oneClipPerLine();
    clips[1].deletedRegions = {{1.25, 1.5}};   // Bob's first line plays in two pieces
    const std::vector<const SyncClip*> order = exportedClips(&script, clips);
    const ExportPlan plan = planExport(&script, order);

    ASSERT_EQ(plan.audio.size(), 5u);
    const TimeTick second = secondsToTicks(1.0);
    const TimeTick quarter = secondsToTicks(0.25);
    EXPECT_EQ(plan.audio[0].character, alice);
    EXPECT_EQ(plan.audio[0].label, "alice.wav");
    EXPECT_EQ(plan.audio[0].timelineIn, 0);
    EXPECT_EQ(plan.audio[0].duration, second);
    EXPECT_EQ(plan.audio[0].sourceIn, second);
    EXPECT_EQ(plan.audio[0].sourceDuration, secondsToTicks(2.0));
    EXPECT_EQ(plan.audio[1].character, bob);
    EXPECT_EQ(plan.audio[1].timelineIn, second);
    EXPECT_EQ(plan.audio[1].duration, quarter);
    EXPECT_EQ(plan.audio[2].line, lines[1]);
    EXPECT_EQ(plan.audio[2].timelineIn, second + quarter);
    EXPECT_EQ(plan.audio[2].sourceIn, secondsToTicks(1.5));
    EXPECT_EQ(plan.audio[2].duration, 2 * quarter);
    EXPECT_EQ(plan.audio[4].timelineIn, second + 3 * quarter + second);

    // Alice | Bob Bob | Alice
    ASSERT_EQ(plan.groups.size(), 3u);
    EXPECT_EQ(plan.groups[0].firstLine, lines[0]);
    EXPECT_EQ(plan.groups[1].firstLine, lines[1]);
    EXPECT_EQ(plan.groups[1].timelineStart, second);
    EXPECT_EQ(plan.groups[1].totalDuration, 3 * quarter + second);
    EXPECT_EQ(plan.groups[2].character, alice);
}

TEST_F(ExportPlanTest, AFullExportBuildsSpeakerTracksAndKeepsTheUsersTracks)
{
    Timeline timeline;
    Track* music = timeline.addAudioTrack("Music");
    music->addClip(std::make_unique<AudioClip>("C:/audio/song.wav"));
    timeline.addAudioTrack("Empty");
    ASSERT_FALSE(isReexport(timeline));

    FakeShots shots{&timeline};
    const std::vector<SyncClip> clips = oneClipPerLine();
    const ExportPlan plan = planExport(&script, exportedClips(&script, clips));
    ASSERT_FALSE(lockedTrackInTheWay(timeline, plan, false));
    EXPECT_EQ(applyExport(timeline, plan, false, true, shots.fn()), 4);

    Track* aliceTrack = trackNamed(timeline, alice);
    Track* bobTrack = trackNamed(timeline, bob);
    ASSERT_TRUE(aliceTrack && bobTrack);
    EXPECT_EQ(aliceTrack->clipCount(), 2u);
    EXPECT_EQ(bobTrack->clipCount(), 2u);
    EXPECT_FLOAT_EQ(aliceTrack->height(), 30.0f);
    EXPECT_EQ(aliceTrack->clip(0)->syncLine(), lines[0]);
    EXPECT_NE(trackNamed(timeline, "Music"), nullptr);
    EXPECT_EQ(trackNamed(timeline, "Empty"), nullptr);
    EXPECT_EQ(shots.placedFor, (std::vector<int>{lines[0], lines[1], lines[3]}));
    EXPECT_TRUE(isReexport(timeline));

    // Without shots the video tracks are left alone.
    Timeline plain;
    EXPECT_EQ(applyExport(plain, plan, false, false, shots.fn()), 4);
    EXPECT_EQ(shots.placedFor.size(), 3u);
}

TEST_F(ExportPlanTest, AReexportUpdatesClipsInPlaceAndDropsWhatIsGone)
{
    Timeline timeline;
    FakeShots shots{&timeline};
    std::vector<SyncClip> clips = oneClipPerLine();
    applyExport(timeline, planExport(&script, exportedClips(&script, clips)), false, true,
                shots.fn());
    Track* aliceTrack = trackNamed(timeline, alice);
    ASSERT_TRUE(aliceTrack && trackNamed(timeline, bob));
    const uint64_t firstAlice = aliceTrack->clip(0)->id();
    const uint64_t lastAlice = aliceTrack->clip(1)->id();
    shots.placedFor.clear();

    // Alice's first line is longer now; Bob's lines are unconfirmed.
    clips[0].end = 2.5;
    clips[1].matchState = 1;
    clips[2].matchState = 1;
    const ExportPlan plan = planExport(&script, exportedClips(&script, clips));
    ASSERT_TRUE(isReexport(timeline));
    ASSERT_FALSE(lockedTrackInTheWay(timeline, plan, true));
    EXPECT_EQ(applyExport(timeline, plan, true, true, shots.fn()), 2);

    aliceTrack = trackNamed(timeline, alice);
    ASSERT_NE(aliceTrack, nullptr);
    ASSERT_EQ(aliceTrack->clipCount(), 2u);
    EXPECT_EQ(aliceTrack->clip(0)->id(), firstAlice);   // updated, same clip
    EXPECT_EQ(aliceTrack->clip(0)->duration(), secondsToTicks(1.5));
    EXPECT_EQ(aliceTrack->clip(1)->id(), lastAlice);    // moved up, same clip
    EXPECT_EQ(aliceTrack->clip(1)->timelineIn(), secondsToTicks(1.5));
    EXPECT_EQ(trackNamed(timeline, bob), nullptr);   // emptied, dropped

    // Alice's two groups merged into one: the first is kept and re-spanned,
    // the other's visuals are removed; nothing is rebuilt.
    EXPECT_TRUE(shots.placedFor.empty());
    size_t visuals = 0;
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        const Track* t = timeline.track(i);
        if (t->type() != TrackType::Video) continue;
        for (size_t c = 0; c < t->clipCount(); ++c) {
            ++visuals;
            EXPECT_EQ(t->clip(c)->syncLine(), lines[0]);
            EXPECT_EQ(t->clip(c)->duration(), secondsToTicks(2.5));
        }
    }
    EXPECT_EQ(visuals, 1u);
}

TEST_F(ExportPlanTest, AReexportRebuildsAGroupWhoseSpeakerChanged)
{
    Timeline timeline;
    FakeShots shots{&timeline};
    std::vector<SyncClip> clips = oneClipPerLine();
    applyExport(timeline, planExport(&script, exportedClips(&script, clips)), false, true,
                shots.fn());
    shots.placedFor.clear();

    // Without a script the clips' own characters decide; the first line's
    // clip is now Bob's, so its group (Bob, Bob, Bob from line 0) has a new
    // speaker and is rebuilt. Alice's last group is kept.
    for (size_t i = 0; i < clips.size(); ++i) clips[i].character = script.lines[i].character;
    clips[0].character = bob;
    const ExportPlan plan = planExport(nullptr, exportedClips(nullptr, clips));
    ASSERT_EQ(plan.groups.size(), 2u);
    applyExport(timeline, plan, true, true, shots.fn());
    EXPECT_EQ(shots.placedFor, (std::vector<int>{lines[0]}));
}

TEST_F(ExportPlanTest, ALockedTrackInTheWayStopsTheExport)
{
    const std::vector<SyncClip> clips = oneClipPerLine();
    const ExportPlan plan = planExport(&script, exportedClips(&script, clips));

    // Full export: a locked speaker track, or locked shot-group visuals.
    {
        Timeline timeline;
        timeline.addAudioTrack(alice)->setLocked(true);
        const auto blocked = lockedTrackInTheWay(timeline, plan, false);
        ASSERT_TRUE(blocked);
        EXPECT_EQ(blocked->track, alice);
    }
    {
        Timeline timeline;
        timeline.addAudioTrack("Music")->setLocked(true);   // not the export's
        EXPECT_FALSE(lockedTrackInTheWay(timeline, plan, false));
    }
    {
        Timeline timeline;
        Track* video = timeline.addVideoTrack("Shots");
        auto shot = std::make_unique<VideoClip>("shot.mp4");
        shot->setGroupId(4);
        shot->setShotName("show/WIDE");
        video->addClip(std::move(shot));
        video->setLocked(true);
        const auto blocked = lockedTrackInTheWay(timeline, plan, false);
        ASSERT_TRUE(blocked);
        EXPECT_EQ(blocked->track, "Shots");
    }

    // Re-export: a locked track holding a back-linked clip.
    {
        Timeline timeline;
        FakeShots shots{&timeline};
        applyExport(timeline, plan, false, true, shots.fn());
        Track* bobTrack = trackNamed(timeline, bob);
        ASSERT_NE(bobTrack, nullptr);
        bobTrack->setLocked(true);
        const auto blocked = lockedTrackInTheWay(timeline, plan, true);
        ASSERT_TRUE(blocked);
        EXPECT_EQ(blocked->track, bob);
        EXPECT_EQ(blocked->clipLabel, "bob.wav");
        EXPECT_EQ(blocked->line, lines[1]);
    }
}
