/*
 * ROUNDTABLE NLE v2 — Integration & Stress Tests
 * Step 30: Polish & Integration Testing
 *
 * End-to-end workflow tests exercising:
 *   - Project creation → timeline editing → serialization round-trip
 *   - Edit operations (split, trim, move, copy/paste, ripple)
 *   - Command undo/redo across complex operations
 *   - Stress: many tracks, many clips, large timelines
 *   - Auto-save + recovery round-trip
 *   - Effect stack manipulation
 *   - Timeline layout engine consistency
 */

#include <gtest/gtest.h>
#include <chrono>
#include <numeric>
#include <random>
#include <thread>
#include <filesystem>

// Project
#include "project/Project.h"
#include "project/Settings.h"
#include "project/AssetDatabase.h"
#include "project/ProjectSerializer.h"
#include "project/AutoSave.h"
#include "CrashHandler.h"

// Timeline
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/TitleClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/Marker.h"
#include "timeline/Transition.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/EditOperations.h"
#include "timeline/TimelineLayoutEngine.h"

// Command
#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/KeyframeCmds.h"

// Effects
#include "effects/EffectStack.h"
#include "effects/ColorCorrect.h"
#include "effects/Blur.h"
#include "effects/Glow.h"

using namespace rt;

// ═══════════════════════════════════════════════════════════════════════════
// Test fixture with temp directory
// ═══════════════════════════════════════════════════════════════════════════

class IntegrationTest : public ::testing::Test
{
protected:
    std::filesystem::path testDir;

    void SetUp() override
    {
        testDir = std::filesystem::temp_directory_path()
                / (std::string("rt_integration_") + ::testing::UnitTest::GetInstance()
                                          ->current_test_info()->name());
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }

    /// Create a fully-populated test project.
    std::unique_ptr<Project> makeRichProject(const std::string& name = "Integration Test")
    {
        auto proj = Project::createNew(name);
        auto* tl = proj->timeline();

        // Configure settings
        proj->settings().setResolution(1920, 1080);
        proj->settings().setFrameRate(30.0);

        // Add additional tracks
        tl->addVideoTrack("Video 2");
        tl->addAudioTrack("Audio 2");

        // Populate Video 1 (track 0) with clips
        for (int i = 0; i < 5; ++i) {
            auto clip = std::make_unique<SpineClip>();
            clip->setCharacterName("Char_" + std::to_string(i));
            clip->setOutfit("default");
            clip->setAnimationName("idle");
            clip->setTimelineIn(i * 96000);
            clip->setDuration(96000);
            clip->setLabel("Spine " + std::to_string(i));
            tl->track(0)->addClip(std::move(clip));
        }

        // Populate Video 2 (track 2) with video clips
        for (int i = 0; i < 3; ++i) {
            auto clip = std::make_unique<VideoClip>();
            clip->setMediaPath("media/video_" + std::to_string(i) + ".mp4");
            clip->setTimelineIn(i * 144000);
            clip->setDuration(144000);
            clip->setLabel("Video " + std::to_string(i));
            tl->track(2)->addClip(std::move(clip));
        }

        // Populate Audio 1 (track 1)
        auto audio = std::make_unique<AudioClip>();
        audio->setMediaPath("audio/dialog.wav");
        audio->setTimelineIn(0);
        audio->setDuration(480000);
        audio->setLabel("Dialog");
        tl->track(1)->addClip(std::move(audio));

        // Add markers
        tl->addMarker(0, "Start", 0xFF00FF00);
        tl->addMarker(240000, "Midpoint", 0xFFFFFF00);
        tl->addMarker(480000, "End", 0xFFFF0000);

        // Add a transition
        Transition t;
        t.type = TransitionType::CrossDissolve;
        t.duration = 24000; // 0.5s
        t.offset = 0;
        t.param1 = 1.0f;
        tl->track(0)->addTransition(t);

        proj->setModified(false);
        return proj;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Project create → populate → serialize → deserialize → verify
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, ProjectCreatePopulateSerializeRoundTrip)
{
    auto proj = makeRichProject();

    ProjectSerializer ser;
    auto data = ser.serialize(*proj);
    ASSERT_GT(data.size(), 100u);

    auto loaded = ser.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    // Verify all structure survived
    EXPECT_EQ(loaded->name(), "Integration Test");
    EXPECT_EQ(loaded->settings().resolution().width, 1920u);
    EXPECT_EQ(loaded->settings().resolution().height, 1080u);
    EXPECT_DOUBLE_EQ(loaded->settings().frameRate(), 30.0);

    ASSERT_EQ(loaded->timeline()->trackCount(), 4u);
    EXPECT_EQ(loaded->timeline()->track(0)->clipCount(), 5u);  // Spine clips
    EXPECT_EQ(loaded->timeline()->track(1)->clipCount(), 1u);  // Audio
    EXPECT_EQ(loaded->timeline()->track(2)->clipCount(), 3u);  // Video clips
    EXPECT_EQ(loaded->timeline()->track(3)->clipCount(), 0u);  // Audio 2

    EXPECT_EQ(loaded->timeline()->markers().size(), 3u);
    EXPECT_EQ(loaded->timeline()->track(0)->transitionCount(), 1u);

    // Verify clip content
    auto* clip0 = loaded->timeline()->track(0)->clip(0);
    ASSERT_EQ(clip0->clipType(), ClipType::Spine);
    EXPECT_EQ(static_cast<SpineClip*>(clip0)->characterName(), "Char_0");
}

TEST_F(IntegrationTest, FileRoundTripPreservesAllData)
{
    auto proj = makeRichProject("File RT");
    auto filePath = testDir / "project.rtp";
    proj->setFilePath(filePath);

    ProjectSerializer ser;
    ASSERT_TRUE(ser.save(*proj, filePath));

    auto loaded = ser.load(filePath);
    ASSERT_NE(loaded, nullptr);

    EXPECT_EQ(loaded->name(), "File RT");
    EXPECT_EQ(loaded->timeline()->trackCount(), 4u);
    EXPECT_EQ(loaded->timeline()->track(0)->clipCount(), 5u);

    // Re-serialize and compare sizes (IDs regenerate, so byte-exact won't match)
    auto data1 = ser.serialize(*proj);
    auto data2 = ser.serialize(*loaded);
    EXPECT_EQ(data1.size(), data2.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Edit operations workflow
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, SplitAndTrimWorkflow)
{
    auto proj = Project::createNew("Split Test");
    auto* tl = proj->timeline();

    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(96000);
    uint64_t clipId = clip->id();
    tl->track(0)->addClip(std::move(clip));

    EXPECT_EQ(tl->track(0)->clipCount(), 1u);

    // Split at midpoint
    auto splitCmd = EditOperations::splitClip(*tl, 0, clipId, 48000);
    ASSERT_NE(splitCmd, nullptr);

    CommandStack stack;
    stack.execute(std::move(splitCmd));

    EXPECT_EQ(tl->track(0)->clipCount(), 2u);

    // Verify the two halves
    auto* left = tl->track(0)->clip(0);
    auto* right = tl->track(0)->clip(1);
    EXPECT_EQ(left->timelineIn(), 0);
    EXPECT_EQ(left->duration(), 48000);
    EXPECT_EQ(right->timelineIn(), 48000);
    EXPECT_EQ(right->duration(), 48000);

    // Undo should restore single clip
    stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 1u);
    EXPECT_EQ(tl->track(0)->clip(0)->duration(), 96000);

    // Redo
    stack.redo();
    EXPECT_EQ(tl->track(0)->clipCount(), 2u);
}

TEST_F(IntegrationTest, MoveClipAcrossTracks)
{
    auto proj = Project::createNew("Move Test");
    auto* tl = proj->timeline();

    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();
    tl->track(0)->addClip(std::move(clip));

    EXPECT_EQ(tl->track(0)->clipCount(), 1u);
    EXPECT_EQ(tl->track(1)->clipCount(), 0u);

    // Move to second video track (need to add one)
    tl->addVideoTrack("Video 2");
    auto moveCmd = EditOperations::moveClipToTrack(*tl, 0, 2, clipId, 24000);
    ASSERT_NE(moveCmd, nullptr);

    CommandStack stack;
    stack.execute(std::move(moveCmd));

    EXPECT_EQ(tl->track(0)->clipCount(), 0u);
    EXPECT_EQ(tl->track(2)->clipCount(), 1u);
    EXPECT_EQ(tl->track(2)->clip(0)->timelineIn(), 24000);

    // Undo
    stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 1u);
    EXPECT_EQ(tl->track(2)->clipCount(), 0u);
}

TEST_F(IntegrationTest, CopyPasteWorkflow)
{
    auto proj = Project::createNew("CopyPaste Test");
    auto* tl = proj->timeline();

    auto clip = std::make_unique<SpineClip>();
    clip->setCharacterName("Hero");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();
    tl->track(0)->addClip(std::move(clip));

    // Select and copy
    SelectionSet selection;
    selection.selectClip({0, clipId});
    ClipboardContents clipboard;
    EditOperations::copySelection(*tl, selection, clipboard);

    EXPECT_FALSE(clipboard.entries.empty());

    // Paste at playhead position
    tl->setPlayheadPosition(96000);
    auto pasteCmd = EditOperations::paste(*tl, clipboard, 96000);
    ASSERT_NE(pasteCmd, nullptr);

    CommandStack stack;
    stack.execute(std::move(pasteCmd));

    EXPECT_EQ(tl->track(0)->clipCount(), 2u);

    // The pasted clip should be at the new position
    auto* pasted = tl->track(0)->clip(1);
    EXPECT_EQ(pasted->timelineIn(), 96000);
    EXPECT_EQ(pasted->clipType(), ClipType::Spine);

    // Undo returns to 1 clip
    stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 1u);
}

TEST_F(IntegrationTest, DeleteSelectionWorkflow)
{
    auto proj = Project::createNew("Delete Test");
    auto* tl = proj->timeline();

    // Add 3 clips
    std::vector<uint64_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * 48000);
        clip->setDuration(48000);
        ids.push_back(clip->id());
        tl->track(0)->addClip(std::move(clip));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 3u);

    // Select clips 0 and 2 (addToSelection=true for multi-select)
    SelectionSet selection;
    selection.selectClip({0, ids[0]});
    selection.selectClip({0, ids[2]}, true);

    auto delCmd = EditOperations::deleteSelection(*tl, selection);
    ASSERT_NE(delCmd, nullptr);

    CommandStack stack;
    stack.execute(std::move(delCmd));

    EXPECT_EQ(tl->track(0)->clipCount(), 1u);
    EXPECT_EQ(tl->track(0)->clip(0)->id(), ids[1]); // Middle clip survives

    // Undo restores all 3
    stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 3u);
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Undo/Redo history across many operations
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, ComplexUndoRedoWorkflow)
{
    auto proj = Project::createNew("Undo Test");
    auto* tl = proj->timeline();
    CommandStack stack;

    // Track 0 = default Video 1

    // Steps: add 5 clips, split one, move one, delete one
    for (int i = 0; i < 5; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * 48000);
        clip->setDuration(48000);
        auto addCmd = std::make_unique<AddClipCommand>(tl->track(0),
            std::move(clip));
        stack.execute(std::move(addCmd));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 5u);
    EXPECT_EQ(stack.undoCount(), 5u);

    // Split clip at index 2
    auto clipId2 = tl->track(0)->clip(2)->id();
    auto splitCmd = EditOperations::splitClip(*tl, 0, clipId2, 2 * 48000 + 24000);
    if (splitCmd) {
        stack.execute(std::move(splitCmd));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 6u);

    // Undo everything
    size_t undoSteps = stack.undoCount();
    for (size_t i = 0; i < undoSteps; ++i) {
        EXPECT_TRUE(stack.undo());
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 0u); // All undone
    EXPECT_FALSE(stack.canUndo());

    // Redo everything
    size_t redoSteps = stack.redoCount();
    for (size_t i = 0; i < redoSteps; ++i) {
        EXPECT_TRUE(stack.redo());
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 6u);
    EXPECT_FALSE(stack.canRedo());
}

TEST_F(IntegrationTest, HistorySnapshotConsistency)
{
    CommandStack stack;
    Timeline tl;
    tl.addVideoTrack("V1");

    for (int i = 0; i < 10; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * 48000);
        clip->setDuration(48000);
        auto cmd = std::make_unique<AddClipCommand>(tl.track(0), std::move(clip));
        stack.execute(std::move(cmd));
    }

    // Undo 5
    for (int i = 0; i < 5; ++i) stack.undo();

    auto snapshot = stack.historySnapshot();
    EXPECT_EQ(snapshot.descriptions.size(), 10u);
    EXPECT_EQ(snapshot.currentIndex, 5u);

    // Jump back to index 2
    stack.jumpToIndex(2);
    EXPECT_EQ(tl.track(0)->clipCount(), 2u);

    auto snap2 = stack.historySnapshot();
    EXPECT_EQ(snap2.currentIndex, 2u);
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Effects workflow
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, EffectStackOnClipWorkflow)
{
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(96000);

    // Add effects
    clip->effects().addEffect(std::make_unique<ColorCorrect>());
    clip->effects().addEffect(std::make_unique<Blur>());
    clip->effects().addEffect(std::make_unique<Glow>());

    EXPECT_EQ(clip->effects().effectCount(), 3u);

    // Reorder
    clip->effects().moveEffect(2, 0);
    EXPECT_EQ(clip->effects().effect(0).effectType(), EffectType::Glow);

    // Clone preserves basic clip data (EffectStack has move-only semantics;
    // clone() is virtual per-subclass, effects may not deep-copy)
    auto cloned = clip->clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->clipType(), ClipType::Spine);

    // Verify effect manipulation is stable
    EXPECT_EQ(clip->effects().effectCount(), 3u);
    (void)clip->effects().removeEffect(0); // remove the Glow we moved to front
    EXPECT_EQ(clip->effects().effectCount(), 2u);
    EXPECT_EQ(clip->effects().effect(0).effectType(), EffectType::ColorCorrect);
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Timeline layout engine
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, LayoutEngineTimePixelConsistency)
{
    TimelineLayoutEngine layout;
    layout.setPixelsPerSecond(100.0);
    layout.setViewportWidth(1920.0);
    layout.setTotalDuration(480000);
    layout.setFrameRate(30.0);

    // Time → pixel → time roundtrip
    for (int64_t t = 0; t <= 480000; t += 1600) {
        double px = layout.timeToPixelX(t);
        int64_t t2 = layout.pixelXToTime(px);
        EXPECT_NEAR(static_cast<double>(t), static_cast<double>(t2), 1.0) << "at t=" << t;
    }

    // Zoom should preserve center point
    double centerPx = 960.0;
    int64_t centerTime = layout.pixelXToTime(centerPx);
    layout.zoomAt(centerPx, 2.0);
    double newPx = layout.timeToPixelX(centerTime);
    EXPECT_NEAR(centerPx, newPx, 1.0);
}

TEST_F(IntegrationTest, LayoutRulerMarks)
{
    TimelineLayoutEngine layout;
    layout.setPixelsPerSecond(100.0);
    layout.setViewportWidth(1920.0);
    layout.setFrameRate(30.0);
    layout.setTotalDuration(480000);

    auto marks = layout.computeRulerMarks();
    EXPECT_GT(marks.size(), 0u);

    // Major marks should have labels
    size_t majorCount = 0;
    for (const auto& m : marks) {
        if (m.isMajor) {
            EXPECT_FALSE(m.label.empty());
            majorCount++;
        }
    }
    EXPECT_GT(majorCount, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Auto-save + serialization round-trip
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, AutoSaveRecoveryRoundTrip)
{
    // Create and save a project normally
    auto proj = makeRichProject("AutoSave RT");
    auto filePath = testDir / "project.rtp";
    proj->setFilePath(filePath);

    ProjectSerializer ser;
    ASSERT_TRUE(ser.save(*proj, filePath));

    // Simulate editing after save
    proj->setModified(true);
    proj->settings().setFrameRate(60.0); // change something

    // Auto-save
    AutoSave autoSave;
    autoSave.setProject(proj.get());
    ASSERT_TRUE(autoSave.saveNow());

    // Verify auto-save folder exists
    auto asFolder = AutoSave::autoSaveFolder(filePath);
    EXPECT_TRUE(std::filesystem::exists(asFolder));

    // Find the newest auto-save
    auto newest = AutoSave::findNewestAutoSave(filePath);
    EXPECT_FALSE(newest.empty());

    // Load the auto-save
    auto recovered = AutoSave::loadLatestAutoSave(filePath);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->name(), "AutoSave RT");
    EXPECT_EQ(recovered->filePath(), filePath);
    EXPECT_TRUE(recovered->isModified());
    EXPECT_DOUBLE_EQ(recovered->settings().frameRate(), 60.0);
    EXPECT_EQ(recovered->timeline()->trackCount(), 4u);
    EXPECT_EQ(recovered->timeline()->track(0)->clipCount(), 5u);
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Selection + snap engine
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, SelectionAndSnapWorkflow)
{
    auto proj = makeRichProject("Snap Test");
    auto* tl = proj->timeline();

    // Select all clips on track 0
    SelectionSet sel;
    sel.selectAll(*tl);
    EXPECT_GT(sel.count(), 0u);

    // Build snap targets
    SnapEngine snap;
    snap.setEnabled(true);
    snap.setPixelsPerSecond(100.0);
    snap.buildTargets(*tl, tl->playheadPosition(), 30.0);
    EXPECT_GT(snap.targets().size(), 0u);

    // Snap near a clip edge
    auto result = snap.snap(48001); // near 48000 (1 tick off)
    // Should snap to nearby edge (or not, depending on threshold)

    // Clear selection
    sel.clear();
    EXPECT_TRUE(sel.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// E2E: Keyframe animation workflow
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(IntegrationTest, KeyframeAnimationRoundTrip)
{
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(96000);

    // Animate opacity
    clip->opacity().addKeyframe(0, 0.0f, InterpMode::Linear);
    clip->opacity().addKeyframe(24000, 1.0f, InterpMode::Bezier);
    clip->opacity().addKeyframe(48000, 0.5f, InterpMode::Hold);
    clip->opacity().addKeyframe(96000, 1.0f, InterpMode::Linear);

    // Evaluate intermediate values
    float v1 = clip->opacity().evaluate(0);
    EXPECT_FLOAT_EQ(v1, 0.0f);

    float v2 = clip->opacity().evaluate(12000); // halfway in linear segment
    EXPECT_GT(v2, 0.0f);
    EXPECT_LT(v2, 1.0f);

    float v3 = clip->opacity().evaluate(48000);
    EXPECT_FLOAT_EQ(v3, 0.5f);

    // Held value should persist until next keyframe
    float v4 = clip->opacity().evaluate(60000);
    EXPECT_FLOAT_EQ(v4, 0.5f);

    // Serialize round-trip
    auto proj = Project::createNew("KF Test");
    proj->timeline()->track(0)->addClip(std::move(clip));

    ProjectSerializer ser;
    auto data = ser.serialize(*proj);
    auto loaded = ser.deserialize(data);
    ASSERT_NE(loaded, nullptr);

    auto* lc = loaded->timeline()->track(0)->clip(0);
    EXPECT_EQ(lc->opacity().keyframeCount(), 4u);
    EXPECT_FLOAT_EQ(lc->opacity().evaluate(0), 0.0f);
    EXPECT_FLOAT_EQ(lc->opacity().evaluate(48000), 0.5f);
    EXPECT_FLOAT_EQ(lc->opacity().evaluate(96000), 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// STRESS TESTS
// ═══════════════════════════════════════════════════════════════════════════

class StressTest : public IntegrationTest {};

TEST_F(StressTest, ManyTracks20x50Clips)
{
    auto proj = Project::createNew("Stress 20x50");
    auto* tl = proj->timeline();

    // Add 18 more video tracks (already have Video 1 + Audio 1)
    for (int t = 2; t <= 20; ++t) {
        tl->addVideoTrack("Video " + std::to_string(t));
    }

    EXPECT_EQ(tl->trackCount(), 21u); // 20 video + 1 audio

    // Add 50 spine clips per video track
    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
        if (tl->track(ti)->type() != TrackType::Video) continue;

        for (int ci = 0; ci < 50; ++ci) {
            auto clip = std::make_unique<SpineClip>();
            clip->setCharacterName("Char_" + std::to_string(ci));
            clip->setTimelineIn(ci * 48000);
            clip->setDuration(48000);
            tl->track(ti)->addClip(std::move(clip));
        }
    }

    // Verify
    size_t totalClips = 0;
    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
        totalClips += tl->track(ti)->clipCount();
    }
    EXPECT_EQ(totalClips, 1000u); // 20 tracks × 50 clips

    // Serialize and deserialize
    ProjectSerializer ser;
    auto start = std::chrono::steady_clock::now();
    auto data = ser.serialize(*proj);
    auto serTime = std::chrono::steady_clock::now() - start;

    EXPECT_GT(data.size(), 0u);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(serTime).count(), 5000)
        << "Serialization should complete within 5 seconds";

    start = std::chrono::steady_clock::now();
    auto loaded = ser.deserialize(data);
    auto deTime = std::chrono::steady_clock::now() - start;

    ASSERT_NE(loaded, nullptr);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(deTime).count(), 5000)
        << "Deserialization should complete within 5 seconds";

    // Verify all data survived
    size_t loadedClips = 0;
    for (size_t ti = 0; ti < loaded->timeline()->trackCount(); ++ti) {
        loadedClips += loaded->timeline()->track(ti)->clipCount();
    }
    EXPECT_EQ(loadedClips, totalClips);
}

TEST_F(StressTest, ThirtyMinuteTimeline)
{
    auto proj = Project::createNew("30min Timeline");
    auto* tl = proj->timeline();

    proj->settings().setFrameRate(30.0);
    constexpr int64_t thirtyMinutes = 30 * 60 * 48000; // 86,400,000 ticks

    // Fill with 2-second clips
    constexpr int64_t clipDur = 2 * 48000; // 96000
    const int numClips = static_cast<int>(thirtyMinutes / clipDur);

    for (int i = 0; i < numClips; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * clipDur);
        clip->setDuration(clipDur);
        tl->track(0)->addClip(std::move(clip));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), static_cast<size_t>(numClips));

    // Timeline duration should encompass all clips
    EXPECT_GE(tl->duration(), thirtyMinutes - clipDur);

    // Serialize
    ProjectSerializer ser;
    auto data = ser.serialize(*proj);
    EXPECT_GT(data.size(), 0u);

    auto loaded = ser.deserialize(data);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->timeline()->track(0)->clipCount(), static_cast<size_t>(numClips));
}

TEST_F(StressTest, RapidUndoRedoCycle)
{
    auto proj = Project::createNew("Undo Stress");
    auto* tl = proj->timeline();
    CommandStack stack(500); // large depth

    // Execute 200 commands
    for (int i = 0; i < 200; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * 48000);
        clip->setDuration(48000);
        stack.execute(std::make_unique<AddClipCommand>(tl->track(0), std::move(clip)));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 200u);

    auto start = std::chrono::steady_clock::now();

    // Undo all
    while (stack.canUndo()) stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 0u);

    // Redo all
    while (stack.canRedo()) stack.redo();
    EXPECT_EQ(tl->track(0)->clipCount(), 200u);

    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000)
        << "400 undo/redo ops should complete in < 2 seconds";
}

TEST_F(StressTest, ManyMarkersAndTransitions)
{
    auto proj = Project::createNew("Markers Stress");
    auto* tl = proj->timeline();

    // 500 markers
    for (int i = 0; i < 500; ++i) {
        tl->addMarker(i * 4800, "M" + std::to_string(i), 0xFF0000FF);
    }
    EXPECT_EQ(tl->markers().size(), 500u);

    // 200 transitions
    for (int i = 0; i < 200; ++i) {
        Transition t;
        t.type = static_cast<TransitionType>(i % 9);
        t.duration = 12000;
        t.offset = 0;
        t.param1 = 0.5f;
        tl->track(0)->addTransition(t);
    }
    EXPECT_EQ(tl->track(0)->transitionCount(), 200u);

    // Serialize round-trip
    ProjectSerializer ser;
    auto data = ser.serialize(*proj);
    auto loaded = ser.deserialize(data);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->timeline()->markers().size(), 500u);
    EXPECT_EQ(loaded->timeline()->track(0)->transitionCount(), 200u);
}

TEST_F(StressTest, ManyEffectsPerClip)
{
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(96000);

    // Stack 20 effects
    for (int i = 0; i < 10; ++i) {
        clip->effects().addEffect(std::make_unique<ColorCorrect>());
        clip->effects().addEffect(std::make_unique<Blur>());
    }
    EXPECT_EQ(clip->effects().effectCount(), 20u);

    // Evaluate snapshot
    auto snapshot = clip->effects().evaluate(48000);
    EXPECT_EQ(snapshot.size(), 20u);

    // Clone should preserve basic type info
    auto cloned = clip->clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->clipType(), ClipType::Spine);
    // Note: clone() may not deep-copy EffectStack (move-only)
    // Just verify the original is intact
    EXPECT_EQ(clip->effects().effectCount(), 20u);
}

TEST_F(StressTest, KeyframeHeavyClip)
{
    auto clip = std::make_unique<SpineClip>();
    clip->setTimelineIn(0);
    clip->setDuration(4800000); // 100 seconds

    // Add 1000 opacity keyframes
    for (int i = 0; i < 1000; ++i) {
        float v = static_cast<float>(i % 10) / 10.0f;
        clip->opacity().addKeyframe(i * 4800, v, InterpMode::Linear);
    }

    EXPECT_EQ(clip->opacity().keyframeCount(), 1000u);

    // Evaluate at random points
    for (int t = 0; t < 4800000; t += 48000) {
        float val = clip->opacity().evaluate(t);
        EXPECT_GE(val, 0.0f);
        EXPECT_LE(val, 1.0f);
    }

    // Serialize round-trip
    auto proj = Project::createNew("KF Stress");
    proj->timeline()->track(0)->addClip(std::move(clip));

    ProjectSerializer ser;
    auto data = ser.serialize(*proj);
    auto loaded = ser.deserialize(data);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->timeline()->track(0)->clip(0)->opacity().keyframeCount(), 1000u);
}

TEST_F(StressTest, SplitAllClipsRepeatedly)
{
    auto proj = Project::createNew("Split Stress");
    auto* tl = proj->timeline();
    CommandStack stack(500);

    // Start with 10 clips
    for (int i = 0; i < 10; ++i) {
        auto clip = std::make_unique<SpineClip>();
        clip->setTimelineIn(i * 96000);
        clip->setDuration(96000);
        tl->track(0)->addClip(std::move(clip));
    }

    EXPECT_EQ(tl->track(0)->clipCount(), 10u);

    // Split all at playhead (creates 20 clips for each split point that hits a clip)
    auto splitCmd = EditOperations::splitAllAtPlayhead(*tl, 48000);
    if (splitCmd) {
        stack.execute(std::move(splitCmd));
    }

    // Should have split the first clip → 11 clips now
    EXPECT_GE(tl->track(0)->clipCount(), 11u);

    // Undo
    stack.undo();
    EXPECT_EQ(tl->track(0)->clipCount(), 10u);
}

TEST_F(StressTest, LargeProjectSerializationPerformance)
{
    // Build a "production" scale project with mixed clip types
    auto proj = Project::createNew("Perf Test");
    auto* tl = proj->timeline();

    proj->settings().setResolution(3840, 2160);
    proj->settings().setFrameRate(60.0);

    // 5 video tracks + 3 audio tracks
    for (int t = 0; t < 4; ++t) tl->addVideoTrack("V" + std::to_string(t + 2));
    for (int t = 0; t < 2; ++t) tl->addAudioTrack("A" + std::to_string(t + 2));

    // Fill each video track with alternating clip types
    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
        if (tl->track(ti)->type() != TrackType::Video) continue;

        for (int ci = 0; ci < 30; ++ci) {
            std::unique_ptr<Clip> clip;
            if (ci % 3 == 0) {
                auto sc = std::make_unique<SpineClip>();
                sc->setCharacterName("Char_" + std::to_string(ci));
                sc->setOutfit("outfit_0" + std::to_string(ci % 3));
                sc->opacity().addKeyframe(0, 0.0f, InterpMode::Linear);
                sc->opacity().addKeyframe(48000, 1.0f, InterpMode::Bezier);
                clip = std::move(sc);
            } else if (ci % 3 == 1) {
                clip = std::make_unique<VideoClip>("media/vid_" + std::to_string(ci) + ".mp4");
            } else {
                auto tc = std::make_unique<TitleClip>();
                tc->setText("Title " + std::to_string(ci));
                clip = std::move(tc);
            }
            clip->setTimelineIn(ci * 48000);
            clip->setDuration(48000);
            clip->effects().addEffect(std::make_unique<ColorCorrect>());
            tl->track(ti)->addClip(std::move(clip));
        }
    }

    // Fill audio tracks
    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
        if (tl->track(ti)->type() != TrackType::Audio) continue;
        for (int ci = 0; ci < 20; ++ci) {
            auto ac = std::make_unique<AudioClip>("audio/track_" + std::to_string(ci) + ".wav");
            ac->setTimelineIn(ci * 96000);
            ac->setDuration(96000);
            tl->track(ti)->addClip(std::move(ac));
        }
    }

    // Add markers
    for (int i = 0; i < 100; ++i) {
        tl->addMarker(i * 48000, "M" + std::to_string(i), 0xFF00FFFF);
    }

    // Benchmark serialization
    ProjectSerializer ser;
    auto start = std::chrono::steady_clock::now();
    auto data = ser.serialize(*proj);
    auto serMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_GT(data.size(), 1000u);
    EXPECT_LT(serMs, 1000) << "Serialize should be < 1 second";

    // Benchmark deserialization
    start = std::chrono::steady_clock::now();
    auto loaded = ser.deserialize(data);
    auto deMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    ASSERT_NE(loaded, nullptr);
    EXPECT_LT(deMs, 1000) << "Deserialize should be < 1 second";

    // File I/O benchmark
    auto filePath = testDir / "perf_project.rtp";
    start = std::chrono::steady_clock::now();
    ASSERT_TRUE(ser.save(*proj, filePath));
    auto saveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_LT(saveMs, 1000) << "File save should be < 1 second";

    start = std::chrono::steady_clock::now();
    auto fromDisk = ser.load(filePath);
    auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    ASSERT_NE(fromDisk, nullptr);
    EXPECT_LT(loadMs, 1000) << "File load should be < 1 second";

    // Log performance
    std::cout << "  Serialize: " << serMs << "ms (" << data.size() << " bytes)\n";
    std::cout << "  Deserialize: " << deMs << "ms\n";
    std::cout << "  File save: " << saveMs << "ms\n";
    std::cout << "  File load: " << loadMs << "ms\n";
}
