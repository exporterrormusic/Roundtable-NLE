/*
 * test_transport.cpp — Tests for Step 14: Transport & Playback.
 *
 * Tests the PlaybackController (pure logic, no Qt):
 *   1. Timecode conversion (tickToTimecode, timecodeToTick)
 *   2. Transport state machine (play, pause, stop, toggle)
 *   3. Frame stepping (forward, backward)
 *   4. JKL shuttle (forward speeds, reverse speeds, reset on direction change)
 *   5. Navigation (start, end, edit points, in/out points)
 *   6. Loop playback
 *   7. Position polling
 *   8. Callbacks
 *   9. Clock integration
 */

#include <gtest/gtest.h>

#include "playback/PlaybackController.h"
#include "playback/AVSyncClock.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <cmath>
#include <memory>
#include <string>

// ═════════════════════════════════════════════════════════════════════════════
//  Helpers
// ═════════════════════════════════════════════════════════════════════════════

static constexpr int64_t TPS = 48000; // ticks per second

/// Create a timeline with a single video track and one clip.
struct TestSetup
{
    rt::Timeline  timeline;
    rt::Track*    vTrack{nullptr};
    uint64_t      clipId{0};
    rt::AVSyncClock clock;
    rt::PlaybackController controller;

    TestSetup(double clipStartSec = 0.0, double clipDurSec = 10.0)
    {
        vTrack = timeline.addVideoTrack("V1");
        auto clip = std::make_unique<rt::VideoClip>();
        clip->setTimelineIn(static_cast<int64_t>(clipStartSec * TPS));
        clip->setDuration(static_cast<int64_t>(clipDurSec * TPS));
        clipId = clip->id();
        vTrack->addClip(std::move(clip));

        controller.setTimeline(&timeline);
        controller.setSyncClock(&clock);
        controller.setFrameRate(24.0);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  Timecode Conversion
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, TimecodeZero)
{
    auto tc = rt::tickToTimecode(0, 24.0);
    EXPECT_EQ(tc.hours, 0);
    EXPECT_EQ(tc.minutes, 0);
    EXPECT_EQ(tc.seconds, 0);
    EXPECT_EQ(tc.frames, 0);
    EXPECT_EQ(tc.toString(), "00:00:00:00");
}

TEST(Transport, TimecodeOneSecond)
{
    auto tc = rt::tickToTimecode(TPS, 24.0);
    EXPECT_EQ(tc.hours, 0);
    EXPECT_EQ(tc.minutes, 0);
    EXPECT_EQ(tc.seconds, 1);
    EXPECT_EQ(tc.frames, 0);
    EXPECT_EQ(tc.toString(), "00:00:01:00");
}

TEST(Transport, TimecodeOneMinute)
{
    auto tc = rt::tickToTimecode(60 * TPS, 24.0);
    EXPECT_EQ(tc.hours, 0);
    EXPECT_EQ(tc.minutes, 1);
    EXPECT_EQ(tc.seconds, 0);
    EXPECT_EQ(tc.frames, 0);
}

TEST(Transport, TimecodeOneHour)
{
    auto tc = rt::tickToTimecode(3600 * TPS, 24.0);
    EXPECT_EQ(tc.hours, 1);
    EXPECT_EQ(tc.minutes, 0);
    EXPECT_EQ(tc.seconds, 0);
}

TEST(Transport, TimecodePartialSecond)
{
    // 1 frame at 24fps = 48000/24 = 2000 ticks
    auto tc = rt::tickToTimecode(2000, 24.0);
    EXPECT_EQ(tc.seconds, 0);
    EXPECT_EQ(tc.frames, 1);
}

TEST(Transport, TimecodeAt30fps)
{
    // 1 frame at 30fps = 48000/30 = 1600 ticks
    auto tc = rt::tickToTimecode(1600, 30.0);
    EXPECT_EQ(tc.frames, 1);
}

TEST(Transport, TimecodeToString)
{
    rt::Timecode tc{1, 23, 45, 12};
    EXPECT_EQ(tc.toString(), "01:23:45:12");
}

TEST(Transport, TimecodeRoundTrip)
{
    rt::Timecode original{0, 5, 30, 12};
    int64_t tick = rt::timecodeToTick(original, 24.0);
    auto result = rt::tickToTimecode(tick, 24.0);

    EXPECT_EQ(result.hours, original.hours);
    EXPECT_EQ(result.minutes, original.minutes);
    EXPECT_EQ(result.seconds, original.seconds);
    EXPECT_EQ(result.frames, original.frames);
}

TEST(Transport, TimecodeNegativeClamped)
{
    auto tc = rt::tickToTimecode(-1000, 24.0);
    EXPECT_EQ(tc.hours, 0);
    EXPECT_EQ(tc.frames, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transport State Machine
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, InitialStateStopped)
{
    rt::PlaybackController ctrl;
    EXPECT_EQ(ctrl.state(), rt::PlayState::Stopped);
    EXPECT_FALSE(ctrl.isPlaying());
}

TEST(Transport, PlayState)
{
    TestSetup ts;
    ts.controller.play();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Playing);
    EXPECT_TRUE(ts.controller.isPlaying());
}

TEST(Transport, PauseState)
{
    TestSetup ts;
    ts.controller.play();
    ts.controller.pause();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Paused);
    EXPECT_FALSE(ts.controller.isPlaying());
}

TEST(Transport, StopState)
{
    TestSetup ts;
    ts.controller.play();
    ts.controller.stop();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Stopped);
    EXPECT_FALSE(ts.controller.isPlaying());
}

TEST(Transport, TogglePlayPause)
{
    TestSetup ts;

    ts.controller.togglePlayPause();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Playing);

    ts.controller.togglePlayPause();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Paused);

    ts.controller.togglePlayPause();
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Playing);
}

TEST(Transport, PlayIdempotent)
{
    TestSetup ts;
    ts.controller.play();
    ts.controller.play(); // Should not crash or change state
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Playing);
}

TEST(Transport, PauseIdempotent)
{
    TestSetup ts;
    ts.controller.pause(); // Already stopped → no change
    EXPECT_EQ(ts.controller.state(), rt::PlayState::Stopped);
}

TEST(Transport, StopReturnsToInPoint)
{
    TestSetup ts;
    ts.timeline.setInPoint(5 * TPS);
    ts.controller.seekTo(8 * TPS);
    ts.controller.play();
    ts.controller.stop();

    EXPECT_EQ(ts.controller.currentTick(), 5 * TPS);
}

TEST(Transport, StopReturnsToStartIfNoInPoint)
{
    TestSetup ts;
    ts.controller.seekTo(5 * TPS);
    ts.controller.play();
    ts.controller.stop();

    EXPECT_EQ(ts.controller.currentTick(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Seek
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, SeekTo)
{
    TestSetup ts;
    ts.controller.seekTo(3 * TPS);
    EXPECT_EQ(ts.controller.currentTick(), 3 * TPS);
}

TEST(Transport, SeekClampsNegative)
{
    TestSetup ts;
    ts.controller.seekTo(-5 * TPS);
    EXPECT_EQ(ts.controller.currentTick(), 0);
}

TEST(Transport, SeekUpdatesTimeline)
{
    TestSetup ts;
    ts.controller.seekTo(7 * TPS);
    EXPECT_EQ(ts.timeline.playheadPosition(), 7 * TPS);
}

TEST(Transport, SeekUpdatesClock)
{
    TestSetup ts;
    ts.controller.seekTo(4 * TPS);
    EXPECT_EQ(ts.clock.currentTick(), 4 * TPS);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame Stepping
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, StepForward)
{
    TestSetup ts;
    ts.controller.seekTo(0);
    ts.controller.stepForward();

    // 1 frame at 24fps = 2000 ticks
    EXPECT_EQ(ts.controller.currentTick(), 2000);
}

TEST(Transport, StepBackward)
{
    TestSetup ts;
    ts.controller.seekTo(5 * TPS);
    ts.controller.stepBackward();

    EXPECT_EQ(ts.controller.currentTick(), 5 * TPS - 2000);
}

TEST(Transport, StepBackwardClampsToZero)
{
    TestSetup ts;
    ts.controller.seekTo(0);
    ts.controller.stepBackward();

    EXPECT_EQ(ts.controller.currentTick(), 0);
}

TEST(Transport, StepForwardClampsToEnd)
{
    TestSetup ts(0.0, 0.1); // Very short clip: 0.1s = 4800 ticks
    ts.controller.seekTo(4800); // At the end

    ts.controller.stepForward();

    // Should not go beyond timeline duration
    EXPECT_LE(ts.controller.currentTick(), ts.timeline.duration());
}

TEST(Transport, StepPausesPlayback)
{
    TestSetup ts;
    ts.controller.play();
    EXPECT_TRUE(ts.controller.isPlaying());

    ts.controller.stepForward();
    EXPECT_FALSE(ts.controller.isPlaying());
}

TEST(Transport, StepForwardAt30fps)
{
    TestSetup ts;
    ts.controller.setFrameRate(30.0);
    ts.controller.seekTo(0);
    ts.controller.stepForward();

    // 1 frame at 30fps = 48000/30 = 1600 ticks
    EXPECT_EQ(ts.controller.currentTick(), 1600);
}

// ═════════════════════════════════════════════════════════════════════════════
//  JKL Shuttle
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, ShuttleForward1x)
{
    TestSetup ts;
    ts.controller.shuttleForward();

    EXPECT_EQ(ts.controller.state(), rt::PlayState::Shuttling);
    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), 1.0);
}

TEST(Transport, ShuttleForward2x)
{
    TestSetup ts;
    ts.controller.shuttleForward(); // 1x
    ts.controller.shuttleForward(); // 2x

    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), 2.0);
}

TEST(Transport, ShuttleForward4x)
{
    TestSetup ts;
    ts.controller.shuttleForward(); // 1x
    ts.controller.shuttleForward(); // 2x
    ts.controller.shuttleForward(); // 4x

    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), 4.0);
}

TEST(Transport, ShuttleForwardCaps8x)
{
    TestSetup ts;
    ts.controller.shuttleForward(); // 1x
    ts.controller.shuttleForward(); // 2x
    ts.controller.shuttleForward(); // 4x
    ts.controller.shuttleForward(); // Still capped at 4x (level 3)

    // Level is capped at 3 → 2^(3-1) = 4x
    EXPECT_LE(ts.controller.shuttleSpeed(), 8.0);
}

TEST(Transport, ShuttleReverse1x)
{
    TestSetup ts;
    ts.controller.shuttleReverse();

    EXPECT_EQ(ts.controller.state(), rt::PlayState::Shuttling);
    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), -1.0);
}

TEST(Transport, ShuttleReverse2x)
{
    TestSetup ts;
    ts.controller.shuttleReverse(); // -1x
    ts.controller.shuttleReverse(); // -2x

    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), -2.0);
}

TEST(Transport, ShuttlePauseStops)
{
    TestSetup ts;
    ts.controller.shuttleForward();
    ts.controller.shuttlePause();

    EXPECT_EQ(ts.controller.state(), rt::PlayState::Paused);
    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), 0.0);
}

TEST(Transport, ShuttleReverseResetsForward)
{
    TestSetup ts;
    ts.controller.shuttleForward(); // L → 1x
    ts.controller.shuttleForward(); // L → 2x

    ts.controller.shuttleReverse(); // J → resets forward, starts at -1x
    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), -1.0);
}

TEST(Transport, ShuttleForwardResetsReverse)
{
    TestSetup ts;
    ts.controller.shuttleReverse(); // J → -1x
    ts.controller.shuttleReverse(); // J → -2x

    ts.controller.shuttleForward(); // L → resets reverse, starts at 1x
    EXPECT_DOUBLE_EQ(ts.controller.shuttleSpeed(), 1.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Navigation
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, GoToStart)
{
    TestSetup ts;
    ts.controller.seekTo(5 * TPS);
    ts.controller.goToStart();
    EXPECT_EQ(ts.controller.currentTick(), 0);
}

TEST(Transport, GoToEnd)
{
    TestSetup ts;
    ts.controller.goToEnd();
    EXPECT_EQ(ts.controller.currentTick(), ts.timeline.duration());
}

TEST(Transport, GoToNextEditPoint)
{
    TestSetup ts;
    // Clip is 0–10s. Edit points: 0, 10s
    // From 0, next is 10s (the out point of the clip)
    ts.controller.seekTo(0);
    ts.controller.goToNextEditPoint();
    EXPECT_EQ(ts.controller.currentTick(), 10 * TPS);
}

TEST(Transport, GoToPrevEditPoint)
{
    TestSetup ts;
    ts.controller.seekTo(10 * TPS);
    ts.controller.goToPrevEditPoint();
    EXPECT_EQ(ts.controller.currentTick(), 0);
}

TEST(Transport, GoToInPoint)
{
    TestSetup ts;
    ts.timeline.setInPoint(3 * TPS);
    ts.controller.goToInPoint();
    EXPECT_EQ(ts.controller.currentTick(), 3 * TPS);
}

TEST(Transport, GoToOutPoint)
{
    TestSetup ts;
    ts.timeline.setOutPoint(7 * TPS);
    ts.controller.goToOutPoint();
    EXPECT_EQ(ts.controller.currentTick(), 7 * TPS);
}

TEST(Transport, GoToInPointNotSet)
{
    TestSetup ts;
    ts.controller.seekTo(5 * TPS);
    ts.controller.goToInPoint(); // No in-point → stays
    EXPECT_EQ(ts.controller.currentTick(), 5 * TPS);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Loop
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, LoopDefaultOff)
{
    rt::PlaybackController ctrl;
    EXPECT_FALSE(ctrl.isLoopEnabled());
}

TEST(Transport, LoopToggle)
{
    rt::PlaybackController ctrl;
    ctrl.setLoopEnabled(true);
    EXPECT_TRUE(ctrl.isLoopEnabled());
    ctrl.setLoopEnabled(false);
    EXPECT_FALSE(ctrl.isLoopEnabled());
}

TEST(Transport, LoopWrapsAtEnd)
{
    TestSetup ts;
    ts.timeline.setInPoint(2 * TPS);
    ts.timeline.setOutPoint(8 * TPS);
    ts.controller.setLoopEnabled(true);
    ts.controller.play();

    // Simulate position past the out-point
    ts.clock.reset(9 * TPS);
    int64_t result = ts.controller.pollPosition();

    // Should have wrapped back to within the loop range
    EXPECT_GE(result, 2 * TPS);
    EXPECT_LT(result, 8 * TPS);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Position Polling
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, PollReturnsCurrentTick)
{
    TestSetup ts;
    ts.controller.seekTo(3 * TPS);
    int64_t tick = ts.controller.pollPosition();
    EXPECT_EQ(tick, 3 * TPS);
}

TEST(Transport, PollUpdatesTimeline)
{
    TestSetup ts;
    // Establish the position through the controller (play() resyncs the sync
    // clock to the authoritative playhead, so poking the clock alone would be
    // overwritten). pollPosition() must then propagate the current tick into
    // the Timeline. The sync clock extrapolates with wall time once running,
    // so allow a small forward tolerance rather than exact equality.
    ts.controller.seekTo(5 * TPS);
    ts.controller.play();
    (void)ts.controller.pollPosition();
    const int64_t pos = ts.timeline.playheadPosition();
    EXPECT_GE(pos, 5 * TPS);
    EXPECT_LT(pos, 5 * TPS + TPS / 10);  // within 0.1s of the start tick
}

// ═════════════════════════════════════════════════════════════════════════════
//  Callbacks
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, StateChangeCallback)
{
    TestSetup ts;
    rt::PlayState lastState = rt::PlayState::Stopped;
    ts.controller.onStateChanged = [&](rt::PlayState s) { lastState = s; };

    ts.controller.play();
    EXPECT_EQ(lastState, rt::PlayState::Playing);

    ts.controller.pause();
    EXPECT_EQ(lastState, rt::PlayState::Paused);

    ts.controller.stop();
    EXPECT_EQ(lastState, rt::PlayState::Stopped);
}

TEST(Transport, PositionCallback)
{
    TestSetup ts;
    int64_t lastTick = -1;
    ts.controller.onPositionChanged = [&](int64_t t) { lastTick = t; };

    ts.controller.seekTo(7 * TPS);
    EXPECT_EQ(lastTick, 7 * TPS);
}

TEST(Transport, SpeedCallback)
{
    TestSetup ts;
    double lastSpeed = 0.0;
    ts.controller.onSpeedChanged = [&](double s) { lastSpeed = s; };

    ts.controller.shuttleForward();
    EXPECT_DOUBLE_EQ(lastSpeed, 1.0);

    ts.controller.shuttleForward();
    EXPECT_DOUBLE_EQ(lastSpeed, 2.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Clock integration
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, PlayStartsClock)
{
    TestSetup ts;
    EXPECT_FALSE(ts.clock.isRunning());

    ts.controller.play();
    EXPECT_TRUE(ts.clock.isRunning());
}

TEST(Transport, PauseStopsClock)
{
    TestSetup ts;
    ts.controller.play();
    ts.controller.pause();
    EXPECT_FALSE(ts.clock.isRunning());
}

TEST(Transport, SeekResetsClock)
{
    TestSetup ts;
    ts.controller.seekTo(6 * TPS);
    EXPECT_EQ(ts.clock.currentTick(), 6 * TPS);
}

TEST(Transport, ShuttleSetsClockSpeed)
{
    TestSetup ts;
    ts.controller.shuttleForward(); // 1x
    EXPECT_DOUBLE_EQ(ts.clock.speed(), 1.0);

    ts.controller.shuttleForward(); // 2x
    EXPECT_DOUBLE_EQ(ts.clock.speed(), 2.0);
}

TEST(Transport, ShuttleReverseClockSpeed)
{
    TestSetup ts;
    ts.controller.shuttleReverse(); // -1x
    EXPECT_DOUBLE_EQ(ts.clock.speed(), -1.0);

    ts.controller.shuttleReverse(); // -2x
    EXPECT_DOUBLE_EQ(ts.clock.speed(), -2.0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Frame rate
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, DefaultFrameRate)
{
    rt::PlaybackController ctrl;
    // Default should be valid (24.0 or similar)
    EXPECT_GT(ctrl.frameRate(), 0.0);
}

TEST(Transport, SetFrameRate)
{
    rt::PlaybackController ctrl;
    ctrl.setFrameRate(30.0);
    EXPECT_DOUBLE_EQ(ctrl.frameRate(), 30.0);
}

TEST(Transport, InvalidFrameRateIgnored)
{
    rt::PlaybackController ctrl;
    double original = ctrl.frameRate();
    ctrl.setFrameRate(-1.0);
    EXPECT_DOUBLE_EQ(ctrl.frameRate(), original);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Duration
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, DurationTicks)
{
    TestSetup ts;
    EXPECT_EQ(ts.controller.durationTicks(), 10 * TPS);
}

TEST(Transport, DurationNoTimeline)
{
    rt::PlaybackController ctrl;
    EXPECT_EQ(ctrl.durationTicks(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Current Timecode
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, CurrentTimecodeAtZero)
{
    TestSetup ts;
    ts.controller.seekTo(0);
    EXPECT_EQ(ts.controller.currentTimecodeString(), "00:00:00:00");
}

TEST(Transport, CurrentTimecodeAt1s)
{
    TestSetup ts;
    ts.controller.seekTo(TPS);
    EXPECT_EQ(ts.controller.currentTimecodeString(), "00:00:01:00");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Edge cases
// ═════════════════════════════════════════════════════════════════════════════

TEST(Transport, NoTimelineOperations)
{
    rt::PlaybackController ctrl;
    // Should not crash with no timeline attached
    ctrl.play();
    ctrl.pause();
    ctrl.stop();
    ctrl.stepForward();
    ctrl.stepBackward();
    ctrl.goToStart();
    ctrl.goToEnd();
    ctrl.goToNextEditPoint();
    ctrl.goToPrevEditPoint();
    ctrl.goToInPoint();
    ctrl.goToOutPoint();
    (void)ctrl.pollPosition();
    SUCCEED();
}

TEST(Transport, NoClock)
{
    rt::PlaybackController ctrl;
    rt::Timeline timeline;
    timeline.addVideoTrack("V1");
    ctrl.setTimeline(&timeline);
    // No clock attached
    ctrl.play();
    ctrl.seekTo(TPS);
    EXPECT_EQ(ctrl.currentTick(), TPS); // Falls back to timeline playhead
}

TEST(Transport, MultipleClipsEditPoints)
{
    rt::Timeline timeline;
    auto* track = timeline.addVideoTrack("V1");

    // Three clips: [0-2s] [2-5s] [5-8s]
    for (auto [start, dur] : std::initializer_list<std::pair<double,double>>{{0,2},{2,3},{5,3}})
    {
        auto clip = std::make_unique<rt::VideoClip>();
        clip->setTimelineIn(static_cast<int64_t>(start * TPS));
        clip->setDuration(static_cast<int64_t>(dur * TPS));
        track->addClip(std::move(clip));
    }

    rt::PlaybackController ctrl;
    ctrl.setTimeline(&timeline);
    ctrl.setFrameRate(24.0);

    ctrl.seekTo(0);
    ctrl.goToNextEditPoint(); // → 2s
    EXPECT_EQ(ctrl.currentTick(), 2 * TPS);

    ctrl.goToNextEditPoint(); // → 5s
    EXPECT_EQ(ctrl.currentTick(), 5 * TPS);

    ctrl.goToNextEditPoint(); // → 8s
    EXPECT_EQ(ctrl.currentTick(), 8 * TPS);
}
