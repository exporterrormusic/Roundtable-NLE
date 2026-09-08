#include "playback/PlaybackScheduler.h"
#include "playback/PlaybackTelemetry.h"
#include "cache/FrameCache.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

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
    EXPECT_FALSE(scheduled.request.scrubMode);
    EXPECT_FALSE(scheduled.request.stillFrame);
    EXPECT_FALSE(scheduled.request.forceFullResolution);
    EXPECT_FALSE(scheduled.request.preserveAlpha);
    EXPECT_GT(scheduled.request.deadline, now);
    EXPECT_EQ(scheduled.diagnostics.status, RenderResultStatus::Pending);
    EXPECT_EQ(scheduler.stats().requestedFrames, 1u);
}

TEST(PlaybackSchedulerTest, RealtimeCostPolicyRejectsNonPlaybackPasses)
{
    RenderRequest request;
    request.type = RenderRequestType::Playback;
    EXPECT_TRUE(request.isRealtimePlaybackPass());

    request.stillFrame = true;
    EXPECT_FALSE(request.isRealtimePlaybackPass());
    request.stillFrame = false;

    request.scrubMode = true;
    EXPECT_FALSE(request.isRealtimePlaybackPass());
    request.scrubMode = false;

    request.forceFullResolution = true;
    EXPECT_FALSE(request.isRealtimePlaybackPass());
    request.forceFullResolution = false;

    request.type = RenderRequestType::Still;
    EXPECT_FALSE(request.isRealtimePlaybackPass());
    request.type = RenderRequestType::Export;
    EXPECT_FALSE(request.isRealtimePlaybackPass());
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
    EXPECT_TRUE(first.request.scrubMode);
    EXPECT_TRUE(first.request.stillFrame);
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
    EXPECT_FALSE(still.request.scrubMode);
    EXPECT_TRUE(still.request.stillFrame);
    EXPECT_GE(still.request.deadline, now + std::chrono::milliseconds(500));
}

TEST(PlaybackSchedulerTest, RenderSnapshotRequiresBothProjectAndTimeline)
{
    RenderSnapshot snapshot;
    EXPECT_FALSE(snapshot.hasTimeline());
    EXPECT_FALSE(snapshot.isFullProject());

    // ExportRenderSnapshot derives from this contract, so the renderer can
    // receive one snapshot type without depending on the export subsystem.
    EXPECT_EQ(snapshot.sequenceIndex, 0u);
    EXPECT_EQ(snapshot.editVersion, 0u);
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

TEST(PlaybackSchedulerTest, ProducerResetDiscardsHeldAndPublishedFrame)
{
    FrameProducer producer;
    auto expected = std::make_shared<CachedFrame>();
    expected->width = 320;
    expected->height = 180;

    producer.setCompositeCallback(
        [expected](int64_t, uint32_t, uint32_t, bool, bool) {
            return expected;
        });
    producer.start();
    producer.requestScrubFrame(1000, 320, 180, false);

    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    std::shared_ptr<CachedFrame> produced;
    int64_t producedTick = 0;
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, expected);
    EXPECT_EQ(producedTick, 1000);

    producer.stop();
    producer.reset();

    EXPECT_EQ(producer.lastProducedFrame(), nullptr);
    EXPECT_FALSE(producer.consumeFrame(produced, producedTick));
}

TEST(PlaybackSchedulerTest, CancelingInFlightScrubLetsPlaybackFrameWin)
{
    FrameProducer producer;
    std::mutex mutex;
    std::condition_variable cv;
    bool scrubEntered = false;
    bool releaseScrub = false;

    producer.setCompositeCallback(
        [&](int64_t, uint32_t, uint32_t, bool, bool still) {
            if (still) {
                std::unique_lock lock(mutex);
                scrubEntered = true;
                cv.notify_all();
                cv.wait_for(lock, std::chrono::seconds(2),
                            [&] { return releaseScrub; });
            }
            auto frame = std::make_shared<CachedFrame>();
            frame->width = 320;
            frame->height = 180;
            return frame;
        });
    producer.start();
    producer.requestScrubFrame(1000, 320, 180, true);

    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = cv.wait_for(lock, std::chrono::seconds(2),
                              [&] { return scrubEntered; });
    }

    producer.cancelPendingScrub();
    producer.requestFrame(2000);
    {
        std::lock_guard lock(mutex);
        releaseScrub = true;
    }
    cv.notify_all();

    EXPECT_TRUE(entered);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    std::shared_ptr<CachedFrame> produced;
    int64_t producedTick = 0;
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_NE(produced, nullptr);
    EXPECT_EQ(producedTick, 2000);

    producer.stop();
}

TEST(PlaybackSchedulerTest, ProducerHonorsExplicitPendingAndBlankOutcomes)
{
    FrameProducer producer;
    auto readyFrame = std::make_shared<CachedFrame>();
    readyFrame->width = 320;
    readyFrame->height = 180;
    auto partialFrame = std::make_shared<CachedFrame>();
    partialFrame->width = 320;
    partialFrame->height = 180;
    auto blankFrame = std::make_shared<CachedFrame>();

    producer.setCompositeResultCallback(
        [readyFrame, partialFrame, blankFrame](
            int64_t tick, uint32_t, uint32_t, bool, bool) {
            RenderResult result;
            result.timelineTick = tick;
            if (tick == 1000) {
                result.status = RenderResultStatus::Ready;
                result.frame = readyFrame;
            } else if (tick == 2000) {
                result.status = RenderResultStatus::Pending;
            } else if (tick == 2250) {
                result.status = RenderResultStatus::Failed;
                result.frame = partialFrame;
            } else if (tick == 2300) {
                result.status = RenderResultStatus::Pending;
            } else if (tick == 2500) {
                result.status = RenderResultStatus::MissingMedia;
            } else {
                result.status = RenderResultStatus::Blank;
                result.frame = blankFrame;
            }
            result.diagnostics.status = result.status;
            return result;
        });
    producer.start();

    std::shared_ptr<CachedFrame> produced;
    int64_t producedTick = 0;

    producer.requestScrubFrame(1000, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, readyFrame);

    producer.requestScrubFrame(2000, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, readyFrame);
    EXPECT_EQ(producedTick, 2000);

    producer.requestScrubFrame(2250, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, partialFrame);
    EXPECT_EQ(producedTick, 2250);

    producer.requestScrubFrame(2300, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, partialFrame);
    EXPECT_EQ(producedTick, 2300);

    producer.requestScrubFrame(2500, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    ASSERT_NE(produced, nullptr);
    EXPECT_EQ(produced->width, 0u);
    EXPECT_EQ(producedTick, 2500);

    producer.requestScrubFrame(3000, 320, 180, false);
    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, blankFrame);
    EXPECT_EQ(producer.lastProducedFrame(), blankFrame);

    producer.stop();
}

TEST(PlaybackTelemetryTest, AggregatesNormalizesAndResetsAWindow)
{
    PlaybackTelemetryAccumulator telemetry;

    FrameDiagnostics ready;
    ready.cacheLookupMs = 2.0;
    ready.prewarmMs = 4.0;
    ready.shotBoundaryMs = 6.0;
    ready.mediaResolveMs = 8.0;
    ready.gpuRecordSubmitMs = 10.0;
    ready.readbackMs = 12.0;
    ready.renderMs = 20.0;
    ready.producerMs = 24.0;
    ready.cacheHit = true;
    ready.compositeCacheHit = true;
    ready.gpuTimingsValid = true;
    ready.gpuFrameMs = 14.0;
    ready.gpuUploadMs = 3.0;
    ready.gpuEffectMs = 5.0;
    ready.gpuCompositeMs = 6.0;
    telemetry.add(ready, RenderResultStatus::Ready);

    FrameDiagnostics held;
    held.cacheLookupMs = 4.0;
    held.renderMs = 10.0;
    held.producerMs = 12.0;
    held.heldFrame = true;
    held.droppedFrame = true;
    telemetry.add(held, RenderResultStatus::HeldPrevious);
    telemetry.addDropped(3);

    EXPECT_TRUE(telemetry.ready(2));
    const auto summary = telemetry.take();
    EXPECT_EQ(summary.samples, 2u);
    EXPECT_EQ(summary.ready, 1u);
    EXPECT_EQ(summary.held, 1u);
    EXPECT_EQ(summary.dropped, 4u);
    EXPECT_EQ(summary.cacheHits, 1u);
    EXPECT_EQ(summary.compositeCacheHits, 1u);
    EXPECT_EQ(summary.segmentCacheHits, 0u);
    EXPECT_EQ(summary.gpuSamples, 1u);
    EXPECT_DOUBLE_EQ(summary.avgCacheLookupMs, 3.0);
    EXPECT_DOUBLE_EQ(summary.avgPrewarmMs, 2.0);
    EXPECT_DOUBLE_EQ(summary.avgShotBoundaryMs, 3.0);
    EXPECT_DOUBLE_EQ(summary.avgMediaResolveMs, 4.0);
    EXPECT_DOUBLE_EQ(summary.avgGpuRecordSubmitMs, 5.0);
    EXPECT_DOUBLE_EQ(summary.avgReadbackMs, 6.0);
    EXPECT_DOUBLE_EQ(summary.avgRenderMs, 15.0);
    EXPECT_DOUBLE_EQ(summary.avgProducerMs, 18.0);
    EXPECT_DOUBLE_EQ(summary.avgGpuFrameMs, 14.0);
    EXPECT_DOUBLE_EQ(summary.avgGpuUploadMs, 3.0);
    EXPECT_DOUBLE_EQ(summary.avgGpuEffectMs, 5.0);
    EXPECT_DOUBLE_EQ(summary.avgGpuCompositeMs, 6.0);
    EXPECT_DOUBLE_EQ(summary.maxRenderMs, 20.0);
    EXPECT_DOUBLE_EQ(summary.maxProducerMs, 24.0);
    EXPECT_FALSE(telemetry.ready(1));
}

} // namespace
} // namespace rt
