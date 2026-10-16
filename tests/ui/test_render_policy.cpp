#include "CompositeService.h"
#include "CompositeEngine.h"
#include "cache/FrameCache.h"
#include "playback/EngineContracts.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackScheduler.h"
#include "playback/PlaybackController.h"
#include "playback/AVSyncClock.h"
#include "panels/export/ExportRenderSession.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "GpuContext.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/AdjustmentClip.h"
#include "effects/ColorGrading.h"

#include <gtest/gtest.h>

#include <string>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <iostream>

namespace rt {
namespace {

TEST(RenderPolicyTest, IsolatedGpuResourceModeIsExplicitPerService)
{
    CompositeService sharedService;
    CompositeService isolatedService(
        CompositeService::GpuResourceMode::Isolated);

    EXPECT_FALSE(sharedService.usesIsolatedGpuResources());
    EXPECT_TRUE(isolatedService.usesIsolatedGpuResources());
}

TEST(RenderPolicyTest, RequestControlsOutputWithoutChangingLegacyDefaults)
{
    Timeline timeline;
    CompositeService compositor;
    compositor.setTimeline(&timeline);

    // Deliberately oppose the request. These settings are compatibility
    // defaults for positional callers, not mutable state for this render.
    compositor.setGpuDisplayMode(true);
    compositor.setForceFullResolution(false);
    compositor.setExportAlpha(false);

    RenderRequest alphaRequest;
    alphaRequest.type = RenderRequestType::Export;
    alphaRequest.quality = RenderQuality::Full;
    alphaRequest.exactness = RenderExactness::ExactRequired;
    alphaRequest.outputWidth = 2;
    alphaRequest.outputHeight = 2;
    alphaRequest.scrubMode = true;
    alphaRequest.stillFrame = true;
    alphaRequest.preferGpuOutput = false;
    alphaRequest.forceFullResolution = true;
    alphaRequest.preserveAlpha = true;

    auto transparent = compositor.compositeFrame(alphaRequest);
    ASSERT_NE(transparent, nullptr);
    ASSERT_FALSE(transparent->pixels.empty());
    ASSERT_GE(transparent->pixels.size(), 8u);
    EXPECT_EQ(transparent->pixels.size(),
              static_cast<size_t>(transparent->stride) * transparent->height);
    EXPECT_TRUE(transparent->preservesAlpha);
    EXPECT_EQ(transparent->pixels[3], 0u);
    EXPECT_EQ(transparent->pixels[7], 0u);

    EXPECT_TRUE(compositor.gpuDisplayMode());
    EXPECT_FALSE(compositor.forceFullResolution());
    EXPECT_FALSE(compositor.exportAlpha());

    auto opaqueRequest = alphaRequest;
    opaqueRequest.forceFullResolution = false;
    opaqueRequest.preserveAlpha = false;
    auto opaque = compositor.compositeFrame(opaqueRequest);
    ASSERT_NE(opaque, nullptr);
    ASSERT_FALSE(opaque->pixels.empty());
    ASSERT_GE(opaque->pixels.size(), 8u);
    EXPECT_EQ(opaque->pixels.size(),
              static_cast<size_t>(opaque->stride) * opaque->height);
    EXPECT_FALSE(opaque->preservesAlpha);
    EXPECT_EQ(opaque->pixels[3], 0xFFu);
    EXPECT_EQ(opaque->pixels[7], 0xFFu);

    EXPECT_TRUE(compositor.gpuDisplayMode());
    EXPECT_FALSE(compositor.forceFullResolution());
    EXPECT_FALSE(compositor.exportAlpha());
}

TEST(RenderPolicyTest, LiveSequenceTargetDoesNotRebindService)
{
    auto project = std::shared_ptr<Project>(Project::createNew("Graph").release());
    auto* primary = project->sequence(0);
    ASSERT_NE(primary, nullptr);
    ASSERT_NE(project->addSequence("Secondary"), nullptr);

    CompositeService compositor;
    compositor.setProject(project.get());
    compositor.setTimeline(primary);

    RenderRequest sourceRequest;
    sourceRequest.type = RenderRequestType::SourceMonitor;
    sourceRequest.quality = RenderQuality::Full;
    sourceRequest.exactness = RenderExactness::ExactRequired;
    sourceRequest.timelineTick = 0;
    sourceRequest.outputWidth = 2;
    sourceRequest.outputHeight = 2;
    sourceRequest.scrubMode = true;
    sourceRequest.stillFrame = true;
    sourceRequest.preferGpuOutput = false;
    sourceRequest.targetSequenceIndex = 1;

    // Secondary graph evaluation must not change the service's persistent
    // project/timeline binding.
    ASSERT_NE(compositor.compositeFrame(
                  sourceRequest, /*isNestedRecursion=*/true),
              nullptr);

    auto snapshot = std::make_shared<RenderSnapshot>();
    snapshot->project = project;
    snapshot->timeline = std::shared_ptr<const Timeline>(project, primary);

    RenderRequest boundRequest = sourceRequest;
    boundRequest.targetSequenceIndex.reset();
    boundRequest.snapshot = std::move(snapshot);
    EXPECT_NE(compositor.compositeFrame(boundRequest), nullptr);

    auto invalidRequest = sourceRequest;
    invalidRequest.targetSequenceIndex = 2;
    EXPECT_EQ(compositor.compositeFrame(invalidRequest), nullptr);
}

TEST(RenderPolicyTest, ExportSessionOwnsAndRebindsQueueSnapshots)
{
    auto makeSnapshot = [](const char* name, uint64_t version) {
        auto project = std::shared_ptr<Project>(
            Project::createNew(name).release());
        auto snapshot = std::make_shared<RenderSnapshot>();
        snapshot->project = project;
        snapshot->timeline = std::shared_ptr<const Timeline>(
            project, project->sequence(0));
        snapshot->editVersion = version;
        snapshot->rangeStartTick = 0;
        snapshot->rangeEndTick = 48000;
        return snapshot;
    };

    const auto first = makeSnapshot("First export graph", 1);
    const auto second = makeSnapshot("Second export graph", 2);
    ExportRenderSession session({});

    const auto firstPreflight = session.preflight(first);
    EXPECT_EQ(firstPreflight.status, RenderResultStatus::Ready);
    EXPECT_EQ(firstPreflight.totalResources, 0u);

    const auto firstResult = session.renderFrame(
        first, 0, 2, 2, /*scrubMode=*/true, /*preserveAlpha=*/false);
    ASSERT_TRUE(firstResult.isComplete());
    ASSERT_NE(firstResult.frame, nullptr);
    EXPECT_TRUE(session.storeFrame(first, 0, firstResult.frame));
    EXPECT_FALSE(session.storeFrame(second, 0, firstResult.frame));

    const auto secondPreflight = session.preflight(second);
    EXPECT_EQ(secondPreflight.status, RenderResultStatus::Ready);
    EXPECT_EQ(secondPreflight.totalResources, 0u);

    const auto secondResult = session.renderFrame(
        second, 0, 2, 2, /*scrubMode=*/true, /*preserveAlpha=*/true);
    ASSERT_TRUE(secondResult.isComplete());
    ASSERT_NE(secondResult.frame, nullptr);
    EXPECT_TRUE(secondResult.frame->preservesAlpha);
    EXPECT_FALSE(session.storeFrame(first, 0, firstResult.frame));
    EXPECT_TRUE(session.storeFrame(second, 0, secondResult.frame));
}

TEST(RenderPolicyTest, CompositeLruRequiresCompleteRenderIdentity)
{
    ::CompositeEngine engine;
    int graphA = 0;
    int graphB = 0;
    auto frame = std::make_shared<CachedFrame>();

    const CompositeCacheKey key{
        &graphA,
        7,
        2400,
        1920,
        1080,
        ResolutionTier::Half,
        false,
    };
    engine.insertLru(key, frame);
    EXPECT_EQ(engine.checkLru(key), frame);

    auto different = key;
    different.graph = &graphB;
    EXPECT_EQ(engine.checkLru(different), nullptr);

    different = key;
    ++different.editVersion;
    EXPECT_EQ(engine.checkLru(different), nullptr);

    different = key;
    different.tier = ResolutionTier::Full;
    EXPECT_EQ(engine.checkLru(different), nullptr);

    different = key;
    different.preservesAlpha = true;
    EXPECT_EQ(engine.checkLru(different), nullptr);

    different = key;
    different.w = 1280;
    EXPECT_EQ(engine.checkLru(different), nullptr);

    auto graphBKey = key;
    graphBKey.graph = &graphB;
    auto graphBFrame = std::make_shared<CachedFrame>();
    engine.insertLru(graphBKey, graphBFrame);
    engine.invalidateLruRange(&graphA, key.tick, key.tick);
    EXPECT_EQ(engine.checkLru(key), nullptr);
    EXPECT_EQ(engine.checkLru(graphBKey), graphBFrame);
}

TEST(RenderPolicyTest, FullInvalidationAdvancesLiveCacheGeneration)
{
    CompositeService compositor;
    const uint64_t initial = compositor.liveCacheGeneration();

    compositor.requestCacheInvalidation();
    const uint64_t deferred = compositor.liveCacheGeneration();
    EXPECT_GT(deferred, initial);

    compositor.invalidateCacheDirect();
    EXPECT_GT(compositor.liveCacheGeneration(), deferred);
}

TEST(RenderPolicyTest, ResultDistinguishesBlankPendingAndInvalidRequests)
{
    Timeline timeline;
    CompositeService compositor;
    compositor.setTimeline(&timeline);

    RenderRequest playback;
    playback.requestId = 42;
    playback.type = RenderRequestType::Playback;
    playback.exactness = RenderExactness::BestEffortAllowed;
    playback.outputWidth = 64;
    playback.outputHeight = 36;

    const auto blank = compositor.renderFrame(playback);
    EXPECT_EQ(blank.status, RenderResultStatus::Blank);
    EXPECT_NE(blank.frame, nullptr);
    EXPECT_EQ(blank.diagnostics.requestId, 42u);
    EXPECT_EQ(blank.diagnostics.status, RenderResultStatus::Blank);
    EXPECT_TRUE(blank.isComplete());
    EXPECT_FALSE(blank.isRetryable());

    auto exact = playback;
    exact.type = RenderRequestType::Export;
    exact.exactness = RenderExactness::ExactRequired;
    exact.scrubMode = true;
    exact.stillFrame = true;
    exact.forceFullResolution = true;
    const auto exactBlank = compositor.renderFrame(exact);
    EXPECT_EQ(exactBlank.status, RenderResultStatus::Ready);
    ASSERT_NE(exactBlank.frame, nullptr);
    EXPECT_EQ(exactBlank.frame->width, 64u);
    EXPECT_EQ(exactBlank.frame->height, 36u);

    auto invalid = playback;
    invalid.outputWidth = 0;
    const auto failed = compositor.renderFrame(invalid);
    EXPECT_EQ(failed.status, RenderResultStatus::Failed);
    EXPECT_EQ(failed.frame, nullptr);
    EXPECT_FALSE(failed.diagnostics.warning.empty());
}

TEST(RenderPolicyTest, ConfirmedOfflineSourcePropagatesAsMissingMedia)
{
    const std::string missingPath =
        "__roundtable_missing_media_contract__/offline_source.mp4";

    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    ASSERT_NE(videoTrack, nullptr);
    auto clip = std::make_unique<VideoClip>(missingPath);
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    ASSERT_NE(videoTrack->addClip(std::move(clip)), nullptr);

    MediaPool mediaPool;
    EXPECT_EQ(mediaPool.pathState(missingPath), MediaPathState::Unresolved);

    CompositeService compositor;
    compositor.setTimeline(&timeline);
    compositor.setMediaPool(&mediaPool);

    RenderRequest request;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;
    request.quality = RenderQuality::Full;
    request.outputWidth = 64;
    request.outputHeight = 36;
    request.scrubMode = true;
    request.stillFrame = true;
    request.forceFullResolution = true;

    const auto result = compositor.renderFrame(request);
    EXPECT_EQ(result.status, RenderResultStatus::MissingMedia);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_NE(result.diagnostics.warning.find(missingPath), std::string::npos);
    EXPECT_EQ(mediaPool.pathState(missingPath), MediaPathState::Missing);
}

TEST(RenderPolicyTest, ExistingButUndecodableSourceRemainsRetryable)
{
    const auto path = std::filesystem::temp_directory_path() /
        "roundtable_existing_but_undecodable_media.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "not a media container";
    }

    MediaPool mediaPool;
    EXPECT_EQ(mediaPool.pathState(path), MediaPathState::Unresolved);
    EXPECT_EQ(mediaPool.open(path), InvalidMedia);
    // The file exists, so a decoder/open failure may be transient and must not
    // be promoted to MissingMedia.
    EXPECT_EQ(mediaPool.pathState(path), MediaPathState::Unresolved);

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    EXPECT_FALSE(removeError);
}

TEST(RenderPolicyTest, PlaybackDecodeMissNeverBecomesBlankOrPartialAdjustmentFrame)
{
    // An existing file whose decoder has not produced a frame must remain
    // retryable, including after the old 250ms settle window expires.
    const auto path = std::filesystem::temp_directory_path() /
        "roundtable_pending_playback_source.bin";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "decoder has no frame yet";
    }
    for (bool withAdjustment : {false, true}) {
        MediaPool mediaPool;
        Timeline timeline;
        if (withAdjustment) {
            auto* track = timeline.addVideoTrack("Grade");
            auto adjustment = std::make_unique<AdjustmentClip>();
            adjustment->setDuration(48000);
            adjustment->effects().addEffect(std::make_unique<ColorGrading>());
            track->addClip(std::move(adjustment));
        }
        auto* track = timeline.addVideoTrack("New shot");
        auto video = std::make_unique<VideoClip>(path.string());
        video->setDuration(48000);
        track->addClip(std::move(video));
        CompositeService compositor;
        compositor.setTimeline(&timeline);
        compositor.setMediaPool(&mediaPool);
        RenderRequest request;
        request.type = RenderRequestType::Playback;
        request.exactness = RenderExactness::BestEffortAllowed;
        request.outputWidth = 64;
        request.outputHeight = 36;
        for (int sample = 0; sample < 2; ++sample) {
            const auto result = compositor.renderFrame(request);
            EXPECT_EQ(result.status, RenderResultStatus::Pending)
                << "adjustment=" << withAdjustment << " sample=" << sample;
            EXPECT_EQ(result.frame, nullptr);
            if (sample == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(270));
            request.timelineTick += 800;
        }
        // A real gap still clears the viewport.
        request.timelineTick = 96000;
        EXPECT_EQ(compositor.renderFrame(request).status, RenderResultStatus::Blank);
    }
    std::filesystem::remove(path);
}

TEST(RenderPolicyTest, OptionalRealProjectPlaybackProfile)
{
    const char* path = std::getenv("ROUNDTABLE_PLAYBACK_PROFILE_PROJECT");
    if (!path) GTEST_SKIP() << "Set ROUNDTABLE_PLAYBACK_PROFILE_PROJECT for real-media profiling";
    // Optional knobs: ROUNDTABLE_PLAYBACK_PROFILE_SEQUENCE (index; default
    // the project's active sequence), ..._QUALITY ("full" or "half", default
    // half) and ..._START_SECONDS (default 0).
    const auto envOr = [](const char* name, const char* fallback) {
        const char* value = std::getenv(name);
        return std::string(value ? value : fallback);
    };
    ProjectSerializer serializer;
    auto project = serializer.load(path);
    ASSERT_NE(project, nullptr);
    const std::string sequenceArg = envOr("ROUNDTABLE_PLAYBACK_PROFILE_SEQUENCE", "");
    const size_t sequenceIndex = sequenceArg.empty()
        ? project->activeSequenceIndex()
        : static_cast<size_t>(std::stoul(sequenceArg));
    ASSERT_LT(sequenceIndex, project->sequenceCount());
    auto* timeline = project->sequence(sequenceIndex);
    const bool fullQuality = envOr("ROUNDTABLE_PLAYBACK_PROFILE_QUALITY", "half") == "full";
    const int64_t startTick = static_cast<int64_t>(
        std::stod(envOr("ROUNDTABLE_PLAYBACK_PROFILE_START_SECONDS", "0")) * 48000.0);
    auto& gpu = GpuContext::get();
    ASSERT_TRUE(gpu.init());
    {
        auto cache = std::make_shared<FrameCache>(1024ull * 1024 * 1024);
        MediaPool pool(cache);
        pool.onGpuContextReady();
        CompositeService compositor;
        compositor.setProject(project.get());
        compositor.setTimeline(timeline);
        compositor.setMediaPool(&pool);
        // The Program Monitor's resolution menu sets both the compositor's
        // playback tier (which lookahead prewarm decodes at) and the
        // request quality; keep them in step like the app does.
        compositor.setPlaybackTier(fullQuality ? ResolutionTier::Full : ResolutionTier::Half);
        // The Program Monitor tells the pool the sequence rate during
        // playback; prefetch strides by it.
        pool.setProjectFps(timeline->settings().frameRate());
        RenderRequest request;
        request.outputWidth = 1920;
        request.outputHeight = 1080;
        request.quality = fullQuality ? RenderQuality::Full : RenderQuality::Half;
        request.preferGpuOutput = true;
        // Exercise the opening, alternating shots, restarted background and
        // first section transition at actual 60fps request cadence.
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        std::vector<double> costs;
        size_t ready = 0, pending = 0, blank = 0, failed = 0, held = 0;
        for (int frame = 0; frame < 1500; ++frame) {
            std::this_thread::sleep_until(start + std::chrono::microseconds(frame * 1000000ll / 60));
            request.timelineTick = startTick + frame * 800ll;
            const auto before = Clock::now();
            const auto result = compositor.renderFrame(request);
            costs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
            switch (result.status) {
            case RenderResultStatus::Ready: ++ready; break;
            case RenderResultStatus::Pending: ++pending; break;
            case RenderResultStatus::Blank: ++blank; break;
            case RenderResultStatus::HeldPrevious: ++held; break;
            default: ++failed; break;
            }
        }
        std::sort(costs.begin(), costs.end());
        std::cout << "PLAYBACK_PROFILE ready=" << ready << " pending=" << pending
                  << " blank=" << blank << " held=" << held << " failed=" << failed
                  << " medianMs=" << costs[costs.size()/2]
                  << " p95Ms=" << costs[costs.size()*95/100]
                  << " maxMs=" << costs.back() << std::endl;
        EXPECT_EQ(blank, 0u);
        EXPECT_EQ(failed, 0u);

        // Also exercise the real clock/producer/presenter threads. The sink
        // replaces only the window swapchain, so this measures scheduling,
        // not desktop presentation or the audio-device callback.
        AVSyncClock sync;
        PlaybackController controller;
        controller.setSyncClock(&sync);
        controller.setFrameRate(60.0);
        PlaybackScheduler scheduler;
        scheduler.setController(&controller);
        scheduler.setOutputResolution(1920, 1080);
        scheduler.setAdaptiveEnabled(false);
        size_t presentations = 0, repeats = 0, skips = 0, empty = 0;
        int64_t previousTick = -1;
        scheduler.setCompositeResultCallback(
            [&](int64_t tick, uint32_t, uint32_t, bool, bool) {
                request.timelineTick = tick;
                return compositor.renderFrame(request);
            });
        scheduler.setPresentCallback([&](const auto& frame) {
            if (!frame || frame->width == 0) ++empty;
            return true;
        });
        scheduler.setPresentNotify([&](int64_t tick) {
            ++presentations;
            if (tick == previousTick) ++repeats;
            else if (previousTick >= 0 && tick - previousTick > 800) ++skips;
            previousTick = tick;
        });
        sync.reset(startTick);
        controller.play();
        scheduler.start();
        std::this_thread::sleep_for(std::chrono::seconds(10));
        scheduler.stop();
        controller.pause();
        std::cout << "PIPELINE_PROFILE presentations=" << presentations
                  << " repeats=" << repeats << " skips=" << skips
                  << " empty=" << empty << std::endl;
        EXPECT_EQ(empty, 0u);
    }
    gpu.shutdown();
}

TEST(RenderPolicyTest, OptionalFormatPlaybackProfile)
{
    // One clip, played alone at its own frame rate through the real
    // compositor and prefetch workers — how well a single source format
    // keeps up. Set ROUNDTABLE_FORMAT_PROFILE_FILE to a video file;
    // ROUNDTABLE_PLAYBACK_PROFILE_QUALITY=full|half as above. The MediaPool
    // delivery reports in the log give exact/nearby/miss counts.
    const char* path = std::getenv("ROUNDTABLE_FORMAT_PROFILE_FILE");
    if (!path) GTEST_SKIP() << "Set ROUNDTABLE_FORMAT_PROFILE_FILE for per-format profiling";
    const char* quality = std::getenv("ROUNDTABLE_PLAYBACK_PROFILE_QUALITY");
    const bool fullQuality = quality && std::string(quality) == "full";
    const ResolutionTier tier = fullQuality ? ResolutionTier::Full : ResolutionTier::Half;

    auto& gpu = GpuContext::get();
    ASSERT_TRUE(gpu.init());
    {
        auto cache = std::make_shared<FrameCache>(1024ull * 1024 * 1024);
        MediaPool pool(cache);
        pool.onGpuContextReady();
        const MediaHandle probe = pool.open(std::string(path));
        ASSERT_NE(probe, InvalidMedia);
        const VideoStreamInfo info = *pool.getInfo(probe);
        std::cout << "FORMAT_SOURCE codec=" << info.codecName << ' ' << info.width << 'x'
                  << info.height << " fps=" << info.fps << std::endl;
        const double fps = info.fps > 0.0 ? info.fps : 30.0;
        const int64_t durationTicks = static_cast<int64_t>(info.duration * 48000.0);

        Timeline timeline;
        auto* track = timeline.addVideoTrack("V1");
        ASSERT_NE(track, nullptr);
        auto clip = std::make_unique<VideoClip>(std::string(path));
        clip->setTimelineIn(0);
        clip->setDuration(durationTicks);
        clip->setSourceFps(fps);
        clip->setSourceDuration(durationTicks);
        ASSERT_NE(track->addClip(std::move(clip)), nullptr);

        CompositeService compositor;
        compositor.setTimeline(&timeline);
        compositor.setMediaPool(&pool);
        compositor.setPlaybackTier(tier);
        pool.setProjectFps(fps);   // the sequence runs at the clip's rate
        RenderRequest request;
        request.outputWidth = 1920;
        request.outputHeight = 1080;
        request.quality = fullQuality ? RenderQuality::Full : RenderQuality::Half;
        request.preferGpuOutput = true;

        // Paused on the first frame, then Play: the frame clock advances in
        // real time at the clip's rate.
        using Clock = std::chrono::steady_clock;
        for (int i = 0; i < 50; ++i) {
            request.timelineTick = 0;
            if (compositor.renderFrame(request).status == RenderResultStatus::Ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const int64_t frames = static_cast<int64_t>(info.duration * fps) - 1;
        const auto start = Clock::now();
        size_t ready = 0, notReady = 0;
        std::vector<double> costs;
        for (int64_t frame = 0; frame < frames; ++frame) {
            std::this_thread::sleep_until(start + std::chrono::microseconds(
                static_cast<int64_t>(frame * 1000000.0 / fps)));
            request.timelineTick = static_cast<int64_t>(frame * 48000.0 / fps);
            const auto before = Clock::now();
            const auto result = compositor.renderFrame(request);
            costs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
            if (result.status == RenderResultStatus::Ready) ++ready; else ++notReady;
        }
        std::sort(costs.begin(), costs.end());
        std::cout << "FORMAT_PROFILE file=" << std::filesystem::path(path).filename().string()
                  << " ready=" << ready << " notReady=" << notReady
                  << " medianMs=" << costs[costs.size() / 2]
                  << " p95Ms=" << costs[costs.size() * 95 / 100]
                  << " maxMs=" << costs.back() << std::endl;
    }
    gpu.shutdown();
}

TEST(RenderPolicyTest, ConfirmedOfflineStillImagePropagatesAsMissingMedia)
{
    const std::string missingPath =
        "__roundtable_missing_image_contract__/offline_source.png";

    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    ASSERT_NE(videoTrack, nullptr);
    auto clip = std::make_unique<ImageClip>(missingPath);
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    ASSERT_NE(videoTrack->addClip(std::move(clip)), nullptr);

    MediaPool mediaPool;
    CompositeService compositor;
    compositor.setTimeline(&timeline);
    compositor.setMediaPool(&mediaPool);

    RenderRequest request;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;
    request.quality = RenderQuality::Full;
    request.outputWidth = 64;
    request.outputHeight = 36;
    request.scrubMode = true;
    request.stillFrame = true;
    request.forceFullResolution = true;

    const auto result = compositor.renderFrame(request);
    EXPECT_EQ(result.status, RenderResultStatus::MissingMedia);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_NE(result.diagnostics.warning.find(missingPath), std::string::npos);
}

TEST(RenderPolicyTest, MissingPngPuppetFacePropagatesAsMissingMedia)
{
    const std::string missingPath =
        "__roundtable_missing_puppet_contract__/idle.png";

    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    ASSERT_NE(videoTrack, nullptr);
    auto clip = std::make_unique<PngPuppetClip>("Offline Puppet", "default");
    clip->setFacePath(PngPuppetClip::MouthClosedEyesOpen, missingPath);
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    ASSERT_NE(videoTrack->addClip(std::move(clip)), nullptr);

    CompositeService compositor;
    compositor.setTimeline(&timeline);

    RenderRequest request;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;
    request.quality = RenderQuality::Full;
    request.outputWidth = 64;
    request.outputHeight = 36;
    request.scrubMode = true;
    request.stillFrame = true;
    request.forceFullResolution = true;

    const auto result = compositor.renderFrame(request);
    EXPECT_EQ(result.status, RenderResultStatus::MissingMedia);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_NE(result.diagnostics.warning.find(missingPath), std::string::npos);
}

#ifdef ROUNDTABLE_HAS_SPINE
TEST(RenderPolicyTest, MissingSpineAssetsPropagateAsMissingMedia)
{
    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    ASSERT_NE(videoTrack, nullptr);
    auto clip = std::make_unique<SpineClip>("Offline Character", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    auto* spineClip = static_cast<SpineClip*>(
        videoTrack->addClip(std::move(clip)));
    ASSERT_NE(spineClip, nullptr);

    CompositeService compositor;
    compositor.setTimeline(&timeline);
    const auto shared = compositor.getOrCreateSharedSpineData(
        *spineClip, "__roundtable_missing_spine_contract__");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->loadState, ResourceLoadState::Missing);
    EXPECT_EQ(compositor.spineResourceState(*spineClip),
              ResourceLoadState::Missing);

    RenderRequest request;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;
    request.quality = RenderQuality::Full;
    request.outputWidth = 64;
    request.outputHeight = 36;
    request.scrubMode = true;
    request.stillFrame = true;
    request.forceFullResolution = true;

    const auto result = compositor.renderFrame(request);
    EXPECT_EQ(result.status, RenderResultStatus::MissingMedia);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_NE(result.diagnostics.warning.find("Offline Character"),
              std::string::npos);
}

TEST(RenderPolicyTest, PresentButInvalidSpineAssetsPropagateAsFailed)
{
    const auto root = std::filesystem::temp_directory_path() /
        "roundtable_invalid_spine_contract";
    const auto assetDir = root / "characters" / "Invalid Character" / "default";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    cleanupError.clear();
    ASSERT_TRUE(std::filesystem::create_directories(assetDir));
    std::ofstream(assetDir / "invalid.skel", std::ios::binary);
    std::ofstream(assetDir / "invalid.atlas", std::ios::binary);

    Timeline timeline;
    auto* videoTrack = timeline.addVideoTrack("V1");
    ASSERT_NE(videoTrack, nullptr);
    auto clip = std::make_unique<SpineClip>("Invalid Character", "default");
    clip->setTimelineIn(0);
    clip->setDuration(48000);
    auto* spineClip = static_cast<SpineClip*>(
        videoTrack->addClip(std::move(clip)));
    ASSERT_NE(spineClip, nullptr);

    CompositeService compositor;
    compositor.setTimeline(&timeline);
    const auto shared = compositor.getOrCreateSharedSpineData(
        *spineClip, root.string());
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->loadState, ResourceLoadState::Failed);

    RenderRequest request;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;
    request.quality = RenderQuality::Full;
    request.outputWidth = 64;
    request.outputHeight = 36;
    request.scrubMode = true;
    request.stillFrame = true;
    request.forceFullResolution = true;

    const auto result = compositor.renderFrame(request);
    EXPECT_EQ(result.status, RenderResultStatus::Failed);
    EXPECT_EQ(result.frame, nullptr);
    EXPECT_NE(result.diagnostics.warning.find("Invalid Character"),
              std::string::npos);

    std::filesystem::remove_all(root, cleanupError);
    EXPECT_FALSE(cleanupError);
}
#endif

} // namespace
} // namespace rt
