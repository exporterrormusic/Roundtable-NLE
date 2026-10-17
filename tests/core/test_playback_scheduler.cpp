#include "playback/PlaybackScheduler.h"
#include "playback/PlaybackTelemetry.h"
#include "cache/FrameCache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

namespace rt {
namespace {

TEST(PlaybackSchedulerTest, ClockBuildsAndMaintainsTwoFrameRenderLead)
{
    for (int64_t direction : {int64_t(1), int64_t(-1)}) {
        frame_clock_detail::FrameRequestPacer pacer;
        for (int64_t n = 0; n < 60; ++n) {
            const int64_t playhead = 100 + direction * n;
            const auto requested = pacer.next(playhead, double(direction), 0);
            ASSERT_TRUE(requested.has_value());
            EXPECT_EQ(*requested, playhead + direction * std::min(n, int64_t(2)));
        }
        // A seek starts preparing frames at the new position immediately.
        EXPECT_EQ(pacer.next(1000, double(direction), 1), 1000 + direction * 2);
        pacer.reset();
        EXPECT_EQ(pacer.next(500, double(direction), 1), 500);
    }
}

TEST(PlaybackSchedulerTest, ClockKeepsCadenceAcrossAudioBoundaryJitter)
{
    frame_clock_detail::FrameRequestPacer pacer;
    EXPECT_EQ(pacer.next(0, 1.0, 0), 0);
    EXPECT_EQ(pacer.next(1, 1.0, 0), 2);
    EXPECT_EQ(pacer.next(2, 1.0, 0), 4);
    EXPECT_EQ(pacer.next(2, 1.0, 0), 5);  // duplicate audio frame observation
    EXPECT_EQ(pacer.next(4, 1.0, 0), 6);  // audio catches up; no skipped request
    EXPECT_EQ(pacer.next(5, 1.0, 0), 7);
}

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

// Publish one frame per tick (in order) through the producer thread.
void publishTicks(FrameProducer& producer, std::initializer_list<int64_t> ticks)
{
    std::atomic<int> produced{0};
    producer.setCompositeCallback(
        [&produced](int64_t, uint32_t, uint32_t, bool, bool) {
            auto f = std::make_shared<CachedFrame>();
            f->width = 64;
            f->height = 36;
            produced.fetch_add(1);
            return f;
        });
    producer.start();
    int expected = 0;
    for (int64_t t : ticks) {
        producer.requestScrubFrame(t, 64, 36, false);
        ++expected;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (produced.load() < expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_EQ(produced.load(), expected);
        // The publish follows the callback; give it a moment.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    producer.stop();
}

TEST(PlaybackSchedulerTest, RenderAheadQueueShowsTheFrameDueAtTheAudioTick)
{
    constexpr int64_t tpf = 1600;  // 30 fps at 48 kHz ticks
    FrameProducer producer;
    publishTicks(producer, {0, tpf, 2 * tpf, 3 * tpf});

    std::shared_ptr<CachedFrame> frame;
    int64_t tick = -1;
    // Mid-frame 1: frame 1 is due, frame 0 is superseded, 2/3 stay queued.
    ASSERT_TRUE(producer.consumeFrameForTick(tpf + 100, tpf, false, frame, tick));
    EXPECT_EQ(tick, tpf);
    // Still inside frame 1: frame 2 is not due yet → hold.
    EXPECT_FALSE(producer.consumeFrameForTick(tpf + 200, tpf, false, frame, tick));
    // A little before frame 2 starts (within 1/3 frame) counts as due.
    ASSERT_TRUE(producer.consumeFrameForTick(2 * tpf - tpf / 4, tpf, false, frame, tick));
    EXPECT_EQ(tick, 2 * tpf);
    // After a large forward seek the remaining frame is stale, not shown.
    EXPECT_FALSE(producer.consumeFrameForTick(100 * tpf, tpf, false, frame, tick));
    EXPECT_FALSE(producer.consumeFrameForTick(3 * tpf, tpf, false, frame, tick));
}

TEST(PlaybackSchedulerTest, RenderAheadQueueHandlesReversePlayback)
{
    constexpr int64_t tpf = 1600;
    FrameProducer producer;
    publishTicks(producer, {10 * tpf, 9 * tpf, 8 * tpf});

    std::shared_ptr<CachedFrame> frame;
    int64_t tick = -1;
    // Playhead moving down inside frame 9: frame 9 is due, 10 superseded.
    ASSERT_TRUE(producer.consumeFrameForTick(9 * tpf + 800, tpf, true, frame, tick));
    EXPECT_EQ(tick, 9 * tpf);
    // Still inside frame 9: frame 8 is not due yet → hold.
    EXPECT_FALSE(producer.consumeFrameForTick(9 * tpf + 700, tpf, true, frame, tick));
    // Frame 8 becomes due once the playhead enters its span.
    ASSERT_TRUE(producer.consumeFrameForTick(8 * tpf + 900, tpf, true, frame, tick));
    EXPECT_EQ(tick, 8 * tpf);
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

// A mask/transform drag re-requests the same paused frame on every mouse
// move. When a render is slower than those requests arrive, the in-flight
// one must still be shown — discarding it starved the monitor until the
// drag stopped.
TEST(PlaybackSchedulerTest, SameFrameRefreshDoesNotStarveInFlightStillRender)
{
    FrameProducer producer;
    std::mutex mutex;
    std::condition_variable cv;
    int calls = 0;
    bool releaseFirst = false;
    bool releaseSecond = false;
    auto first = std::make_shared<CachedFrame>();
    first->width = 320;
    first->height = 180;
    auto second = std::make_shared<CachedFrame>();
    second->width = 320;
    second->height = 180;

    producer.setCompositeCallback(
        [&](int64_t, uint32_t, uint32_t, bool, bool) {
            std::unique_lock lock(mutex);
            const int call = ++calls;
            cv.notify_all();
            bool& release = call == 1 ? releaseFirst : releaseSecond;
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return release; });
            return call == 1 ? first : second;
        });
    producer.start();
    producer.requestScrubFrame(1000, 320, 180, false);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2),
                                [&] { return calls >= 1; }));
    }

    // An edit refresh of the same paused frame arrives mid-render.
    producer.requestScrubFrame(1000, 320, 180, false);
    {
        std::lock_guard lock(mutex);
        releaseFirst = true;
    }
    cv.notify_all();

    ASSERT_TRUE(producer.waitForFrame(std::chrono::seconds(2)));
    std::shared_ptr<CachedFrame> produced;
    int64_t producedTick = 0;
    ASSERT_TRUE(producer.consumeFrame(produced, producedTick));
    EXPECT_EQ(produced, first);
    EXPECT_EQ(producedTick, 1000);

    {
        std::lock_guard lock(mutex);
        releaseSecond = true;
    }
    cv.notify_all();
    producer.stop();
}

// A paused frame answered from a stand-in source (the exact one still
// decoding) is rendered again, unprompted, until it is exact.
TEST(PlaybackSchedulerTest, ProvisionalPausedFrameIsRenderedAgainUntilExact)
{
    FrameProducer producer;
    std::atomic<int> calls{0};
    auto standIn = std::make_shared<CachedFrame>();
    standIn->width = 320;
    standIn->height = 180;
    auto exact = std::make_shared<CachedFrame>();
    exact->width = 320;
    exact->height = 180;

    producer.setCompositeResultCallback(
        [&](int64_t tick, uint32_t, uint32_t, bool, bool) {
            RenderResult result;
            result.timelineTick = tick;
            result.status = RenderResultStatus::Ready;
            const bool stillDecoding = ++calls <= 3;
            result.frame = stillDecoding ? standIn : exact;
            result.diagnostics.provisionalSource = stillDecoding;
            return result;
        });
    producer.start();
    producer.requestScrubFrame(1000, 320, 180, false);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::shared_ptr<CachedFrame> shown;
    int64_t shownTick = 0;
    while (std::chrono::steady_clock::now() < deadline && shown != exact) {
        if (producer.waitForFrame(std::chrono::milliseconds(50)))
            producer.consumeFrame(shown, shownTick);
    }
    EXPECT_EQ(shown, exact);
    EXPECT_EQ(shownTick, 1000);
    // Exact once: no more renders after that.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(calls.load(), 4);
    producer.stop();
}

// A newer request supersedes a pending provisional retry.
TEST(PlaybackSchedulerTest, NewerRequestDropsProvisionalRetry)
{
    FrameProducer producer;
    std::mutex mutex;
    std::vector<int64_t> rendered;
    producer.setCompositeResultCallback(
        [&](int64_t tick, uint32_t, uint32_t, bool, bool) {
            {
                std::lock_guard lock(mutex);
                rendered.push_back(tick);
            }
            RenderResult result;
            result.timelineTick = tick;
            result.status = RenderResultStatus::Ready;
            result.frame = std::make_shared<CachedFrame>();
            result.frame->width = 64;
            result.frame->height = 36;
            result.diagnostics.provisionalSource = tick == 1000;   // never exact
            return result;
        });
    producer.start();
    producer.requestScrubFrame(1000, 64, 36, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    producer.requestScrubFrame(2000, 64, 36, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    producer.stop();

    std::lock_guard lock(mutex);
    ASSERT_FALSE(rendered.empty());
    EXPECT_EQ(rendered.back(), 2000);
    EXPECT_GE(std::count(rendered.begin(), rendered.end(), 1000), 2)
        << "the provisional frame was not rendered again";
    EXPECT_EQ(std::count(rendered.begin(), rendered.end(), 2000), 1);
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
