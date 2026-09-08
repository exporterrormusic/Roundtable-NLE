#include "CompositeService.h"
#include "CompositeEngine.h"
#include "cache/FrameCache.h"
#include "playback/EngineContracts.h"
#include "playback/MediaPool.h"
#include "panels/export/ExportRenderSession.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/ImageClip.h"
#include "timeline/PngPuppetClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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
