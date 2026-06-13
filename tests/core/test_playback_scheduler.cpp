#include "playback/PlaybackScheduler.h"

#include <gtest/gtest.h>

#include <chrono>

namespace rt {
namespace {

TEST(PlaybackSchedulerTest, PlaybackFramesUseBestEffortDeadlinePolicy)
{
    PlaybackScheduler scheduler;
    const auto now = PlaybackScheduler::Clock::now();

    const auto scheduled = scheduler.schedulePlaybackFrame(48000, 1920, 1080, 24.0, now);

    EXPECT_EQ(scheduled.action, ScheduledFrameAction::Render);
    EXPECT_EQ(scheduled.request.type, RenderRequestType::Playback);
    EXPECT_EQ(scheduled.request.quality, RenderQuality::Auto);
    EXPECT_EQ(scheduled.request.exactness, RenderExactness::BestEffortAllowed);
    EXPECT_EQ(scheduled.request.timelineTick, 48000);
    EXPECT_EQ(scheduled.request.outputWidth, 1920u);
    EXPECT_EQ(scheduled.request.outputHeight, 1080u);
    EXPECT_GT(scheduled.request.deadline, now);
    EXPECT_EQ(scheduled.diagnostics.status, RenderResultStatus::Pending);
    EXPECT_EQ(scheduler.stats().requestedFrames, 1u);
}

TEST(PlaybackSchedulerTest, DropsNonAdvancingPlaybackTicks)
{
    PlaybackScheduler scheduler;
    const auto now = PlaybackScheduler::Clock::now();

    const auto first = scheduler.schedulePlaybackFrame(1000, 1280, 720, 60.0, now);
    const auto duplicate = scheduler.schedulePlaybackFrame(1000, 1280, 720, 60.0, now);
    const auto reverse = scheduler.schedulePlaybackFrame(900, 1280, 720, 60.0, now);

    EXPECT_EQ(first.action, ScheduledFrameAction::Render);
    EXPECT_EQ(duplicate.action, ScheduledFrameAction::DropLate);
    EXPECT_EQ(reverse.action, ScheduledFrameAction::Render);
    EXPECT_TRUE(duplicate.diagnostics.droppedFrame);
    EXPECT_EQ(scheduler.stats().requestedFrames, 3u);
    EXPECT_EQ(scheduler.stats().droppedFrames, 1u);
    EXPECT_EQ(scheduler.stats().canceledFrames, 1u);
}

TEST(PlaybackSchedulerTest, ScrubRequestsAreExactAndGenerationCoalesced)
{
    PlaybackScheduler scheduler;
    const auto now = PlaybackScheduler::Clock::now();

    const auto first = scheduler.scheduleStillFrame(2000, 640, 360, true, now);
    const auto second = scheduler.scheduleStillFrame(2500, 640, 360, true, now);

    EXPECT_EQ(first.request.type, RenderRequestType::Scrub);
    EXPECT_EQ(first.request.exactness, RenderExactness::ExactRequired);
    EXPECT_EQ(second.request.type, RenderRequestType::Scrub);
    EXPECT_EQ(second.request.exactness, RenderExactness::ExactRequired);
    EXPECT_GT(second.generation, first.generation);
    EXPECT_EQ(scheduler.latestGeneration(), second.generation);
}

TEST(PlaybackSchedulerTest, StillRequestsUseLongerExactDeadline)
{
    PlaybackScheduler scheduler;
    const auto now = PlaybackScheduler::Clock::now();

    const auto still = scheduler.scheduleStillFrame(3000, 320, 180, false, now);

    EXPECT_EQ(still.action, ScheduledFrameAction::Render);
    EXPECT_EQ(still.request.type, RenderRequestType::Still);
    EXPECT_EQ(still.request.exactness, RenderExactness::ExactRequired);
    EXPECT_GE(still.request.deadline, now + std::chrono::milliseconds(500));
}

TEST(PlaybackSchedulerTest, PresentationCountersTrackHeldAndDroppedFrames)
{
    PlaybackScheduler scheduler;
    const auto scheduled = scheduler.schedulePlaybackFrame(1000, 1920, 1080, 30.0);

    scheduler.noteFramePresented(scheduled.request.requestId);
    scheduler.noteHeldFrame(scheduled.request.requestId);
    scheduler.noteDroppedFrame(scheduled.request.requestId);
    scheduler.cancelPendingScrub();

    const auto stats = scheduler.stats();
    EXPECT_EQ(stats.renderedFrames, 1u);
    EXPECT_EQ(stats.heldFrames, 1u);
    EXPECT_EQ(stats.droppedFrames, 1u);
    EXPECT_EQ(stats.canceledFrames, 0u);
}

} // namespace
} // namespace rt
