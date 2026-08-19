/*
 * ROUNDTABLE NLE v2 — Command system unit tests
 * Step 4: CommandStack, CompoundCommand, all concrete commands
 *
 * Validation targets:
 * - 100 commands undo all, redo all — state matches
 * - Compound undo/redo works atomically
 * - Memory usage <10KB for 100 undo levels
 * - Merge continuous operations (dragging)
 */

#include <gtest/gtest.h>
#include "command/Command.h"
#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/KeyframeCmds.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/GraphicClip.h"

using namespace rt;

// ── Simple test command ─────────────────────────────────────────────────────

class SetValueCommand : public Command
{
public:
    SetValueCommand(int& target, int newValue)
        : m_target(target), m_oldValue(target), m_newValue(newValue) {}

    void execute() override { m_target = m_newValue; }
    void undo() override { m_target = m_oldValue; }
    std::string description() const override { return "Set Value to " + std::to_string(m_newValue); }
    int typeId() const override { return -1; } // negative → no auto-merge

    bool mergeWith(const Command& /*next*/) override
    {
        return false; // Don't merge — each command is independent
    }

private:
    int& m_target;
    int  m_oldValue;
    int  m_newValue;
};

// ═══════════════════════════════════════════════════════════════════════════
// CommandStack basics
// ═══════════════════════════════════════════════════════════════════════════

TEST(CommandStack, ExecuteAndUndo)
{
    int value = 0;
    CommandStack stack;

    stack.execute(std::make_unique<SetValueCommand>(value, 42));
    EXPECT_EQ(value, 42);
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());

    stack.undo();
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(stack.canUndo());
    EXPECT_TRUE(stack.canRedo());
}

TEST(CommandStack, Redo)
{
    int value = 0;
    CommandStack stack;

    stack.execute(std::make_unique<SetValueCommand>(value, 10));
    stack.undo();
    EXPECT_EQ(value, 0);

    stack.redo();
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
}

TEST(CommandStack, NewCommandClearsRedo)
{
    int value = 0;
    CommandStack stack;

    stack.execute(std::make_unique<SetValueCommand>(value, 10));
    stack.execute(std::make_unique<SetValueCommand>(value, 20));
    stack.undo();
    EXPECT_EQ(value, 10);
    EXPECT_TRUE(stack.canRedo());

    // New command should clear redo stack
    stack.execute(std::make_unique<SetValueCommand>(value, 30));
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(value, 30);
}

TEST(CommandStack, Descriptions)
{
    int value = 0;
    CommandStack stack;

    EXPECT_EQ(stack.undoDescription(), "");
    EXPECT_EQ(stack.redoDescription(), "");

    stack.execute(std::make_unique<SetValueCommand>(value, 10));
    EXPECT_EQ(stack.undoDescription(), "Set Value to 10");

    stack.undo();
    EXPECT_EQ(stack.redoDescription(), "Set Value to 10");
}

TEST(CommandStack, UndoRedoCounts)
{
    int value = 0;
    CommandStack stack;

    EXPECT_EQ(stack.undoCount(), 0u);
    EXPECT_EQ(stack.redoCount(), 0u);

    stack.execute(std::make_unique<SetValueCommand>(value, 1));
    stack.execute(std::make_unique<SetValueCommand>(value, 2));
    stack.execute(std::make_unique<SetValueCommand>(value, 3));
    EXPECT_EQ(stack.undoCount(), 3u);

    stack.undo();
    EXPECT_EQ(stack.undoCount(), 2u);
    EXPECT_EQ(stack.redoCount(), 1u);
}

TEST(CommandStack, MaxDepthTrims)
{
    int value = 0;
    CommandStack stack(5);

    for (int i = 0; i < 10; ++i)
        stack.execute(std::make_unique<SetValueCommand>(value, i));

    EXPECT_EQ(stack.undoCount(), 5u);
    EXPECT_EQ(value, 9);
}

TEST(CommandStack, Clear)
{
    int value = 0;
    CommandStack stack;

    stack.execute(std::make_unique<SetValueCommand>(value, 42));
    stack.clear();

    EXPECT_FALSE(stack.canUndo());
    EXPECT_FALSE(stack.canRedo());
    EXPECT_EQ(stack.undoCount(), 0u);
}

TEST(CommandStack, ChangeCallback)
{
    int value = 0;
    int callbackCount = 0;
    CommandStack stack;
    stack.setChangeCallback([&]() { callbackCount++; });

    stack.execute(std::make_unique<SetValueCommand>(value, 1));
    EXPECT_EQ(callbackCount, 1);

    stack.undo();
    EXPECT_EQ(callbackCount, 2);

    stack.redo();
    EXPECT_EQ(callbackCount, 3);

    stack.clear();
    EXPECT_EQ(callbackCount, 4);
}

TEST(CommandStack, UndoOnEmptyReturnsFalse)
{
    CommandStack stack;
    EXPECT_FALSE(stack.undo());
    EXPECT_FALSE(stack.redo());
}

// ═══════════════════════════════════════════════════════════════════════════
// 100 commands undo/redo cycle (validation target)
// ═══════════════════════════════════════════════════════════════════════════

TEST(CommandStack, HundredCommandsCycle)
{
    int value = 0;
    CommandStack stack(200);

    // Execute 100 commands
    for (int i = 1; i <= 100; ++i)
        stack.execute(std::make_unique<SetValueCommand>(value, i));

    EXPECT_EQ(value, 100);
    EXPECT_EQ(stack.undoCount(), 100u);

    // Undo all 100
    for (int i = 0; i < 100; ++i)
        EXPECT_TRUE(stack.undo());

    EXPECT_EQ(value, 0);
    EXPECT_EQ(stack.redoCount(), 100u);

    // Redo all 100
    for (int i = 0; i < 100; ++i)
        EXPECT_TRUE(stack.redo());

    EXPECT_EQ(value, 100);
    EXPECT_EQ(stack.undoCount(), 100u);
}

// ═══════════════════════════════════════════════════════════════════════════
// CompoundCommand
// ═══════════════════════════════════════════════════════════════════════════

TEST(CompoundCommand, AtomicExecuteUndo)
{
    int a = 0, b = 0, c = 0;

    auto compound = std::make_unique<CompoundCommand>("Set ABC");
    compound->addCommand(std::make_unique<SetValueCommand>(a, 10));
    compound->addCommand(std::make_unique<SetValueCommand>(b, 20));
    compound->addCommand(std::make_unique<SetValueCommand>(c, 30));
    EXPECT_EQ(compound->size(), 3u);

    CommandStack stack;
    stack.execute(std::move(compound));

    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    EXPECT_EQ(c, 30);
    EXPECT_EQ(stack.undoCount(), 1u); // Single undo step

    stack.undo();
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
    EXPECT_EQ(c, 0);

    stack.redo();
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 20);
    EXPECT_EQ(c, 30);
}

TEST(CompoundCommand, Description)
{
    auto compound = std::make_unique<CompoundCommand>("Delete Selected");
    EXPECT_EQ(compound->description(), "Delete Selected");
}

// ═══════════════════════════════════════════════════════════════════════════
// ClipCommands
// ═══════════════════════════════════════════════════════════════════════════

TEST(ClipCommands, AddAndRemoveClip)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();

    CommandStack stack;

    // Add
    stack.execute(std::make_unique<AddClipCommand>(&track, std::move(clip)));
    EXPECT_EQ(track.clipCount(), 1u);
    EXPECT_EQ(track.clip(0)->id(), clipId);

    // Undo add
    stack.undo();
    EXPECT_EQ(track.clipCount(), 0u);

    // Redo add
    stack.redo();
    EXPECT_EQ(track.clipCount(), 1u);
    EXPECT_EQ(track.clip(0)->id(), clipId);

    // Remove
    stack.execute(std::make_unique<RemoveClipCommand>(&track, clipId));
    EXPECT_EQ(track.clipCount(), 0u);

    // Undo remove
    stack.undo();
    EXPECT_EQ(track.clipCount(), 1u);
    EXPECT_EQ(track.clip(0)->id(), clipId);
}

TEST(ClipCommands, MoveClip)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();
    track.addClip(std::move(clip));

    CommandStack stack;
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 96000));

    size_t idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->timelineIn(), 96000);

    stack.undo();
    idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->timelineIn(), 0);
}

TEST(ClipCommands, LockedTrackRejectsNewCommandButUndoRedoRestoresHistory)
{
    Track track(TrackType::Video, "V1");
    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    const uint64_t clipId = clip->id();
    ASSERT_NE(track.addClip(std::move(clip)), nullptr);

    CommandStack stack;
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 48000));
    ASSERT_EQ(track.clip(0)->timelineIn(), 48000);

    track.setLocked(true);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(track.clip(0)->timelineIn(), 0);
    ASSERT_TRUE(stack.redo());
    EXPECT_EQ(track.clip(0)->timelineIn(), 48000);

    // A command first created while locked is unauthorized and remains a
    // no-op; unlocking later must not retroactively apply it through undo.
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 96000));
    EXPECT_EQ(track.clip(0)->timelineIn(), 48000);
    ASSERT_TRUE(stack.undo());
    EXPECT_EQ(track.clip(0)->timelineIn(), 48000);
}

TEST(ClipCommands, MoveClipMerge)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();
    track.addClip(std::move(clip));

    CommandStack stack;

    // Simulate dragging: multiple small moves
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 1000));
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 2000));
    stack.execute(std::make_unique<MoveClipCommand>(&track, clipId, 3000));

    // Should merge into single undo step
    EXPECT_EQ(stack.undoCount(), 1u);

    size_t idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->timelineIn(), 3000);

    // Single undo should go back to original
    stack.undo();
    idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->timelineIn(), 0);
}

TEST(ClipCommands, TrimClip)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<VideoClip>("test.mp4");
    clip->setTimelineIn(0);
    clip->setDuration(96000);
    clip->setSourceIn(0);
    uint64_t clipId = clip->id();
    track.addClip(std::move(clip));

    CommandStack stack;

    // Trim: move in point forward
    stack.execute(std::make_unique<TrimClipCommand>(
        &track, clipId, 24000, 72000, 24000));

    size_t idx = track.findClipIndexById(clipId);
    Clip* c = track.clip(idx);
    EXPECT_EQ(c->timelineIn(), 24000);
    EXPECT_EQ(c->duration(), 72000);
    EXPECT_EQ(c->sourceIn(), 24000);

    stack.undo();
    idx = track.findClipIndexById(clipId);
    c = track.clip(idx);
    EXPECT_EQ(c->timelineIn(), 0);
    EXPECT_EQ(c->duration(), 96000);
    EXPECT_EQ(c->sourceIn(), 0);
}

TEST(ClipCommands, HeadTrimPreservesGraphicKeyframeTimelineTimes)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<GraphicClip>();
    clip->setTimelineIn(10000);
    clip->setDuration(60000);
    clip->positionX().addKeyframe(12000, 100.0f);
    TextLayer* text = clip->addTextLayer("Trim me");
    ASSERT_NE(text, nullptr);
    text->transform().opacity.addKeyframe(18000, 0.5f);
    text->tracking().addKeyframe(24000, 20.0f);

    const uint64_t clipId = clip->id();
    track.addClip(std::move(clip));

    auto expectTimelineTimes = [&track, clipId](int64_t expectedIn,
                                                 int64_t expectedPosition,
                                                 int64_t expectedOpacity,
                                                 int64_t expectedTracking) {
        const size_t idx = track.findClipIndexById(clipId);
        ASSERT_NE(idx, track.clipCount());
        auto* graphic = dynamic_cast<GraphicClip*>(track.clip(idx));
        ASSERT_NE(graphic, nullptr);
        auto* layer = dynamic_cast<TextLayer*>(graphic->layer(0));
        ASSERT_NE(layer, nullptr);
        EXPECT_EQ(graphic->timelineIn(), expectedIn);
        ASSERT_EQ(graphic->positionX().keyframeCount(), 1u);
        ASSERT_EQ(layer->transform().opacity.keyframeCount(), 1u);
        ASSERT_EQ(layer->tracking().keyframeCount(), 1u);
        EXPECT_EQ(graphic->timelineIn() + graphic->positionX().keyframe(0).time,
                  expectedPosition);
        EXPECT_EQ(graphic->timelineIn() + layer->transform().opacity.keyframe(0).time,
                  expectedOpacity);
        EXPECT_EQ(graphic->timelineIn() + layer->tracking().keyframe(0).time,
                  expectedTracking);
    };

    constexpr int64_t positionTime = 22000;
    constexpr int64_t opacityTime = 28000;
    constexpr int64_t trackingTime = 34000;
    expectTimelineTimes(10000, positionTime, opacityTime, trackingTime);

    CommandStack stack;
    // Multiple drag updates merge into one command. Each update must preserve
    // absolute keyframe times, and the merged undo/redo must remain symmetric.
    stack.execute(std::make_unique<TrimClipCommand>(
        &track, clipId, 15000, 55000, 5000));
    stack.execute(std::make_unique<TrimClipCommand>(
        &track, clipId, 20000, 50000, 10000));

    EXPECT_EQ(stack.undoCount(), 1u);
    expectTimelineTimes(20000, positionTime, opacityTime, trackingTime);

    ASSERT_TRUE(stack.undo());
    expectTimelineTimes(10000, positionTime, opacityTime, trackingTime);

    ASSERT_TRUE(stack.redo());
    expectTimelineTimes(20000, positionTime, opacityTime, trackingTime);
}

TEST(ClipCommands, SetClipProperty)
{
    Track track(TrackType::Video, "V1");

    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    uint64_t clipId = clip->id();
    track.addClip(std::move(clip));

    CommandStack stack;

    // Set label
    auto getLabel = [](const Clip& c) -> std::string { return c.label(); };
    auto setLabel = [](Clip& c, std::string v) { c.setLabel(v); };
    stack.execute(std::make_unique<SetClipPropertyCommand<std::string>>(
        &track, clipId, "My Clip", getLabel, setLabel, "Set Label"));

    size_t idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->label(), "My Clip");

    stack.undo();
    idx = track.findClipIndexById(clipId);
    EXPECT_EQ(track.clip(idx)->label(), "Spine Clip"); // default label from SpineClip()
}

// ═══════════════════════════════════════════════════════════════════════════
// TrackCommands
// ═══════════════════════════════════════════════════════════════════════════

TEST(TrackCommands, AddAndRemoveTrack)
{
    Timeline timeline;
    CommandStack stack;

    stack.execute(std::make_unique<AddTrackCommand>(&timeline, TrackType::Video, "V1"));
    EXPECT_EQ(timeline.trackCount(), 1u);
    EXPECT_EQ(timeline.track(0)->name(), "V1");

    stack.undo();
    EXPECT_EQ(timeline.trackCount(), 0u);

    stack.redo();
    EXPECT_EQ(timeline.trackCount(), 1u);
}

TEST(TrackCommands, RemoveTrackPreservesClips)
{
    Timeline timeline;
    Track* track = timeline.addVideoTrack("V1");

    auto clip = std::make_unique<SpineClip>("Hero", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    track->addClip(std::move(clip));

    EXPECT_EQ(track->clipCount(), 1u);

    CommandStack stack;
    stack.execute(std::make_unique<RemoveTrackCommand>(&timeline, 0));
    EXPECT_EQ(timeline.trackCount(), 0u);

    // Undo should restore track with its clips
    stack.undo();
    EXPECT_EQ(timeline.trackCount(), 1u);
    EXPECT_EQ(timeline.track(0)->clipCount(), 1u);
}

TEST(TrackCommands, MoveTrack)
{
    Timeline timeline;
    timeline.addVideoTrack("V1");
    timeline.addVideoTrack("V2");
    timeline.addAudioTrack("A1");

    CommandStack stack;
    stack.execute(std::make_unique<MoveTrackCommand>(&timeline, 0, 2));

    EXPECT_EQ(timeline.track(0)->name(), "V2");
    EXPECT_EQ(timeline.track(1)->name(), "A1");
    EXPECT_EQ(timeline.track(2)->name(), "V1");

    stack.undo();
    EXPECT_EQ(timeline.track(0)->name(), "V1");
    EXPECT_EQ(timeline.track(1)->name(), "V2");
    EXPECT_EQ(timeline.track(2)->name(), "A1");
}

TEST(TrackCommands, SetTrackProperty)
{
    Track track(TrackType::Video, "V1");
    CommandStack stack;

    auto getName = [](const Track& t) -> std::string { return t.name(); };
    auto setName = [](Track& t, std::string v) { t.setName(v); };
    stack.execute(std::make_unique<SetTrackPropertyCommand<std::string>>(
        &track, "Renamed", getName, setName, "Rename Track",
        static_cast<int>(CommandTypeId::SetTrackName)));

    EXPECT_EQ(track.name(), "Renamed");

    stack.undo();
    EXPECT_EQ(track.name(), "V1");
}

TEST(TrackCommands, SetTrackMuted)
{
    Track track(TrackType::Audio, "A1");
    CommandStack stack;

    auto getMuted = [](const Track& t) -> bool { return t.isMuted(); };
    auto setMuted = [](Track& t, bool v) { t.setMuted(v); };
    stack.execute(std::make_unique<SetTrackPropertyCommand<bool>>(
        &track, true, getMuted, setMuted, "Mute Track"));

    EXPECT_TRUE(track.isMuted());
    stack.undo();
    EXPECT_FALSE(track.isMuted());
}

// ═══════════════════════════════════════════════════════════════════════════
// TransitionCmds
// ═══════════════════════════════════════════════════════════════════════════

TEST(TransitionCmds, AddAndRemove)
{
    Track track(TrackType::Video, "V1");

    Transition t;
    t.type = TransitionType::CrossDissolve;
    t.duration = 24000;

    CommandStack stack;
    stack.execute(std::make_unique<AddTransitionCommand>(&track, 0, 1, t));
    EXPECT_EQ(track.transitionCount(), 1u);

    stack.undo();
    EXPECT_EQ(track.transitionCount(), 0u);

    stack.redo();
    EXPECT_EQ(track.transitionCount(), 1u);
}

// A second add on the same (leftClipId, rightClipId) edit point must REPLACE
// the existing transition, not stack a duplicate — duplicates made the
// compositor apply the fade twice (opacity = progress², fades too dark).
TEST(TransitionCmds, AddReplacesDuplicateOnSameEditPoint)
{
    Track track(TrackType::Video, "V1");

    Transition t;
    t.type        = TransitionType::CrossDissolve;
    t.duration    = 24000;
    t.leftClipId  = 7;
    t.rightClipId = 9;
    EXPECT_EQ(track.addTransition(t), 0u);

    Transition t2 = t;
    t2.duration = 48000;
    EXPECT_EQ(track.addTransition(t2), 0u);          // replaced in place
    EXPECT_EQ(track.transitionCount(), 1u);
    EXPECT_EQ(track.transition(0)->duration, 48000);

    // Fully-unlinked (0,0) transitions are never merged — their endpoints
    // may simply not have been relinked yet.
    Transition unlinkedA;
    unlinkedA.duration = 100;
    Transition unlinkedB;
    unlinkedB.duration = 200;
    track.addTransition(unlinkedA);
    track.addTransition(unlinkedB);
    EXPECT_EQ(track.transitionCount(), 3u);
}

// Undo of an add that replaced an existing transition must restore the
// replaced value (not remove an unrelated trailing transition).
TEST(TransitionCmds, UndoRestoresReplacedTransition)
{
    Track track(TrackType::Video, "V1");

    Transition original;
    original.type        = TransitionType::CrossDissolve;
    original.duration    = 24000;
    original.leftClipId  = 7;
    original.rightClipId = 9;
    track.addTransition(original);

    Transition replacement = original;
    replacement.duration = 48000;

    CommandStack stack;
    stack.execute(std::make_unique<AddTransitionCommand>(&track, 0, 1, replacement));
    EXPECT_EQ(track.transitionCount(), 1u);
    EXPECT_EQ(track.transition(0)->duration, 48000);

    stack.undo();
    EXPECT_EQ(track.transitionCount(), 1u);
    EXPECT_EQ(track.transition(0)->duration, 24000);

    stack.redo();
    EXPECT_EQ(track.transitionCount(), 1u);
    EXPECT_EQ(track.transition(0)->duration, 48000);
}

TEST(TransitionCmds, SetProperty)
{
    Track track(TrackType::Video, "V1");

    Transition t;
    t.type = TransitionType::CrossDissolve;
    t.duration = 24000;
    track.addTransition(t);

    Transition newT;
    newT.type = TransitionType::FadeToBlack;
    newT.duration = 48000;
    newT.param1 = 0.5f;

    CommandStack stack;
    stack.execute(std::make_unique<SetTransitionPropertyCommand>(&track, 0, newT));

    const Transition* stored = track.transition(0);
    EXPECT_EQ(stored->type, TransitionType::FadeToBlack);
    EXPECT_EQ(stored->duration, 48000);

    stack.undo();
    stored = track.transition(0);
    EXPECT_EQ(stored->type, TransitionType::CrossDissolve);
    EXPECT_EQ(stored->duration, 24000);
}

// ═══════════════════════════════════════════════════════════════════════════
// KeyframeCmds
// ═══════════════════════════════════════════════════════════════════════════

TEST(KeyframeCmds, AddAndRemoveKeyframe)
{
    KeyframeTrack<float> track(1.0f);
    EXPECT_EQ(track.keyframeCount(), 0u); // starts empty

    CommandStack stack;

    // Add a keyframe
    stack.execute(std::make_unique<AddKeyframeCommand>(&track, 48000, 0.5f));
    EXPECT_EQ(track.keyframeCount(), 1u);
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.5f);

    // Undo
    stack.undo();
    EXPECT_EQ(track.keyframeCount(), 0u);

    // Redo
    stack.redo();
    EXPECT_EQ(track.keyframeCount(), 1u);
}

TEST(KeyframeCmds, AddKeyframeReplacesExisting)
{
    KeyframeTrack<float> track(1.0f);
    track.addKeyframe(48000, 0.5f);

    CommandStack stack;

    // Replace keyframe value at same time
    stack.execute(std::make_unique<AddKeyframeCommand>(&track, 48000, 0.8f));
    EXPECT_EQ(track.keyframeCount(), 1u); // Still 1 (replaced, not added)
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.8f);

    // Undo should restore original value
    stack.undo();
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.5f);
}

TEST(KeyframeCmds, RemoveKeyframe)
{
    KeyframeTrack<float> track(1.0f);
    track.addKeyframe(48000, 0.0f);

    CommandStack stack;
    stack.execute(std::make_unique<RemoveKeyframeCommand>(&track, 48000));
    EXPECT_EQ(track.keyframeCount(), 0u);

    stack.undo();
    EXPECT_EQ(track.keyframeCount(), 1u);
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.0f);
}

TEST(KeyframeCmds, MoveKeyframe)
{
    KeyframeTrack<float> track(1.0f);
    track.addKeyframe(48000, 0.5f);

    CommandStack stack;
    stack.execute(std::make_unique<MoveKeyframeCommand>(&track, 48000, 96000, 0.7f));

    EXPECT_EQ(track.keyframeCount(), 1u);
    EXPECT_FLOAT_EQ(track.evaluate(96000), 0.7f);

    stack.undo();
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.5f);
    EXPECT_EQ(track.keyframeCount(), 1u);
}

TEST(KeyframeCmds, MoveKeyframeUndoRedoPreservesAllHandles)
{
    KeyframeTrack<float> track(1.0f);
    Keyframe<float> original;
    original.time = 48000;
    original.value = 0.5f;
    original.interp = InterpMode::ContinuousBezier;
    original.bezierOutX = 0.18f;
    original.bezierOutY = 0.27f;
    original.bezierInX = 0.72f;
    original.bezierInY = 0.83f;
    original.spatialInterp = InterpMode::Bezier;
    original.spatialOutX = 120.0f;
    original.spatialOutY = -45.0f;
    original.spatialInX = -80.0f;
    original.spatialInY = 30.0f;
    track.restoreKeyframe(original);

    auto expectHandles = [&track](int64_t time, float value) {
        ASSERT_EQ(track.keyframeCount(), 1u);
        const auto& key = track.keyframe(0);
        EXPECT_EQ(key.time, time);
        EXPECT_FLOAT_EQ(key.value, value);
        EXPECT_EQ(key.interp, InterpMode::ContinuousBezier);
        EXPECT_FLOAT_EQ(key.bezierOutX, 0.18f);
        EXPECT_FLOAT_EQ(key.bezierOutY, 0.27f);
        EXPECT_FLOAT_EQ(key.bezierInX, 0.72f);
        EXPECT_FLOAT_EQ(key.bezierInY, 0.83f);
        EXPECT_EQ(key.spatialInterp, InterpMode::Bezier);
        EXPECT_FLOAT_EQ(key.spatialOutX, 120.0f);
        EXPECT_FLOAT_EQ(key.spatialOutY, -45.0f);
        EXPECT_FLOAT_EQ(key.spatialInX, -80.0f);
        EXPECT_FLOAT_EQ(key.spatialInY, 30.0f);
    };

    CommandStack stack;
    stack.execute(std::make_unique<MoveKeyframeCommand>(
        &track, 48000, 96000, 0.7f));
    expectHandles(96000, 0.7f);

    stack.undo();
    expectHandles(48000, 0.5f);

    stack.redo();
    expectHandles(96000, 0.7f);
}

TEST(KeyframeCmds, MoveKeyframeMerge)
{
    KeyframeTrack<float> track(1.0f);
    track.addKeyframe(48000, 0.5f);

    CommandStack stack;

    // Simulate dragging a keyframe
    stack.execute(std::make_unique<MoveKeyframeCommand>(&track, 48000, 50000, 0.5f));
    stack.execute(std::make_unique<MoveKeyframeCommand>(&track, 50000, 52000, 0.6f));
    stack.execute(std::make_unique<MoveKeyframeCommand>(&track, 52000, 96000, 0.7f));

    EXPECT_EQ(stack.undoCount(), 1u); // Merged

    stack.undo();
    EXPECT_FLOAT_EQ(track.evaluate(48000), 0.5f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Integration: Full timeline workflow
// ═══════════════════════════════════════════════════════════════════════════

TEST(CommandIntegration, FullWorkflow)
{
    Timeline timeline;
    CommandStack stack;

    // Add tracks
    stack.execute(std::make_unique<AddTrackCommand>(&timeline, TrackType::Video, "Video 1"));
    stack.execute(std::make_unique<AddTrackCommand>(&timeline, TrackType::Audio, "Audio 1"));
    EXPECT_EQ(timeline.trackCount(), 2u);

    Track* vTrack = timeline.track(0);
    Track* aTrack = timeline.track(1);

    // Add clips
    auto vClip = std::make_unique<SpineClip>("Hero", "default");
    vClip->setTimelineIn(0);
    vClip->setDuration(96000);
    uint64_t vClipId = vClip->id();
    stack.execute(std::make_unique<AddClipCommand>(vTrack, std::move(vClip)));

    auto aClip = std::make_unique<AudioClip>("dialog.wav");
    aClip->setTimelineIn(0);
    aClip->setDuration(96000);
    stack.execute(std::make_unique<AddClipCommand>(aTrack, std::move(aClip)));

    EXPECT_EQ(vTrack->clipCount(), 1u);
    EXPECT_EQ(aTrack->clipCount(), 1u);

    // Move video clip
    stack.execute(std::make_unique<MoveClipCommand>(vTrack, vClipId, 48000));

    // Undo everything in reverse
    EXPECT_EQ(stack.undoCount(), 5u);

    stack.undo(); // Undo move
    EXPECT_EQ(vTrack->clip(0)->timelineIn(), 0);

    stack.undo(); // Undo add audio clip
    EXPECT_EQ(aTrack->clipCount(), 0u);

    stack.undo(); // Undo add video clip
    EXPECT_EQ(vTrack->clipCount(), 0u);

    stack.undo(); // Undo add audio track
    EXPECT_EQ(timeline.trackCount(), 1u);

    stack.undo(); // Undo add video track
    EXPECT_EQ(timeline.trackCount(), 0u);

    // Redo everything
    for (int i = 0; i < 5; ++i)
        stack.redo();

    EXPECT_EQ(timeline.trackCount(), 2u);
    EXPECT_EQ(timeline.track(0)->clipCount(), 1u);
    EXPECT_EQ(timeline.track(1)->clipCount(), 1u);
}

TEST(CommandIntegration, CompoundDeleteSelectedClips)
{
    Track track(TrackType::Video, "V1");

    auto c1 = std::make_unique<SpineClip>("A", "default");
    c1->setTimelineIn(0);    c1->setDuration(48000);
    uint64_t id1 = c1->id();

    auto c2 = std::make_unique<SpineClip>("B", "default");
    c2->setTimelineIn(48000); c2->setDuration(48000);
    uint64_t id2 = c2->id();

    auto c3 = std::make_unique<SpineClip>("C", "default");
    c3->setTimelineIn(96000); c3->setDuration(48000);

    track.addClip(std::move(c1));
    track.addClip(std::move(c2));
    track.addClip(std::move(c3));
    EXPECT_EQ(track.clipCount(), 3u);

    // Compound: delete clips 1 and 2
    auto compound = std::make_unique<CompoundCommand>("Delete Selected");
    compound->addCommand(std::make_unique<RemoveClipCommand>(&track, id1));
    compound->addCommand(std::make_unique<RemoveClipCommand>(&track, id2));

    CommandStack stack;
    stack.execute(std::move(compound));

    EXPECT_EQ(track.clipCount(), 1u); // Only clip 3 remains

    // Single undo restores both
    stack.undo();
    EXPECT_EQ(track.clipCount(), 3u);
}

