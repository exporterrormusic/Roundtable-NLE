#include "media/MediaSourceService.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace rt {
namespace {

TEST(MediaSourceServiceTest, EmptyServiceRejectsInvalidHandles)
{
    MediaSourceService service;

    EXPECT_FALSE(service.isValid(InvalidMedia));
    EXPECT_TRUE(service.pathFor(InvalidMedia).empty());
    EXPECT_FALSE(service.sourceInfo(InvalidMedia).has_value());
}

TEST(MediaSourceServiceTest, BoundServiceRejectsInvalidHandlesWithoutOpeningMedia)
{
    MediaPool pool;
    MediaSourceService service(&pool);

    EXPECT_EQ(service.mediaPool(), &pool);
    EXPECT_FALSE(service.isValid(InvalidMedia));
    EXPECT_TRUE(service.pathFor(InvalidMedia).empty());
    EXPECT_FALSE(service.sourceInfo(InvalidMedia).has_value());
}

TEST(MediaSourceServiceTest, PlaybackRequestsAreBestEffortAndNonBlocking)
{
    MediaSourceService service;
    MediaFrameRequest request;
    request.handle = 42;
    request.frameNumber = 7;
    request.type = RenderRequestType::Playback;
    request.exactness = RenderExactness::BestEffortAllowed;
    request.tier = ResolutionTier::Half;
    request.preferNativeGpuFormat = true;

    const auto options = service.frameOptionsFor(request);
    const auto key = service.cacheKeyFor(request);

    EXPECT_FALSE(service.allowsBlockingDecode(request));
    EXPECT_EQ(options.tier, ResolutionTier::Half);
    EXPECT_FALSE(options.exactDecode);
    EXPECT_TRUE(options.preferNativeGpuFormat);
    EXPECT_EQ(key.source, 42);
    EXPECT_EQ(key.frameNumber, 7);
    EXPECT_EQ(key.type, RenderRequestType::Playback);
    EXPECT_EQ(key.exactness, RenderExactness::BestEffortAllowed);
}

TEST(MediaSourceServiceTest, SourceIdsAreStableForEquivalentPaths)
{
    MediaSourceService service;

    const auto idA = service.sourceIdForPath(std::filesystem::path("assets") / "videos" / ".." / "videos" / "clip.mp4");
    const auto idB = service.sourceIdForPath(std::filesystem::path("assets") / "videos" / "clip.mp4");

    EXPECT_NE(idA, 0u);
    EXPECT_EQ(idA, idB);
    EXPECT_EQ(service.sourceIdForPath({}), 0u);
    EXPECT_EQ(service.sourceIdForHandle(InvalidMedia), 0u);
}

TEST(MediaSourceServiceTest, BestEffortSourceMonitorRequestsStayNonBlocking)
{
    MediaSourceService service;
    MediaFrameRequest request;
    request.handle = 12;
    request.type = RenderRequestType::SourceMonitor;
    request.exactness = RenderExactness::BestEffortAllowed;
    request.tier = ResolutionTier::Half;

    const auto options = service.frameOptionsFor(request);

    EXPECT_FALSE(service.allowsBlockingDecode(request));
    EXPECT_FALSE(options.exactDecode);
    EXPECT_EQ(options.tier, ResolutionTier::Half);
}

TEST(MediaSourceServiceTest, ExactRequestTypesAllowBlockingDecode)
{
    MediaSourceService service;
    for (const auto type : {RenderRequestType::Scrub,
                           RenderRequestType::Still,
                           RenderRequestType::SourceMonitor,
                           RenderRequestType::Export,
                           RenderRequestType::Thumbnail}) {
        MediaFrameRequest request;
        request.handle = 12;
        request.type = type;
        request.exactness = RenderExactness::ExactRequired;
        request.tier = ResolutionTier::Quarter;

        const auto options = service.frameOptionsFor(request);
        EXPECT_TRUE(service.allowsBlockingDecode(request));
        EXPECT_TRUE(options.exactDecode);
        EXPECT_EQ(options.tier, ResolutionTier::Quarter);
    }
}

TEST(MediaSourceServiceTest, ThumbnailFrameRequestsUseContractDefaults)
{
    MediaSourceService service;
    MediaFrameRequest request;
    request.handle = 12;
    request.type = RenderRequestType::Thumbnail;
    request.quality = defaultQualityFor(request.type);
    request.exactness = defaultExactnessFor(request.type);
    request.tier = ResolutionTier::Quarter;
    request.pixelFormat = MediaPixelFormatPreference::Bgra;

    const auto options = service.frameOptionsFor(request);
    const auto key = service.cacheKeyFor(request);

    EXPECT_TRUE(service.allowsBlockingDecode(request));
    EXPECT_TRUE(options.exactDecode);
    EXPECT_EQ(options.tier, ResolutionTier::Quarter);
    EXPECT_EQ(key.type, RenderRequestType::Thumbnail);
    EXPECT_EQ(key.quality, RenderQuality::Quarter);
    EXPECT_EQ(key.exactness, RenderExactness::ExactRequired);
    EXPECT_EQ(key.pixelFormat, MediaPixelFormatPreference::Bgra);
}

TEST(MediaSourceServiceTest, InvalidFrameRequestsReturnEmptyResults)
{
    MediaSourceService service;
    MediaFrameRequest request;
    request.handle = InvalidMedia;
    request.type = RenderRequestType::Export;
    request.exactness = RenderExactness::ExactRequired;

    const auto result = service.requestFrame(request);

    EXPECT_FALSE(result);
    EXPECT_FALSE(result.frame);
    EXPECT_TRUE(result.blockingDecodeAllowed);
    EXPECT_EQ(result.key.source, InvalidMedia);
}

TEST(MediaSourceServiceTest, InvalidCacheAndPrefetchRequestsAreNoOps)
{
    MediaSourceService service;
    MediaFrameRequest request;
    request.handle = InvalidMedia;
    request.frameNumber = 99;
    request.type = RenderRequestType::Playback;
    request.exactness = RenderExactness::BestEffortAllowed;

    EXPECT_FALSE(service.isFrameCached(request));
    EXPECT_NO_THROW(service.schedulePrefetch(request, 3, true));
    EXPECT_NO_THROW(service.startLoopPreDecode(request));
}

TEST(MediaSourceServiceTest, ThumbnailAndWaveformRequestsHandleMissingDependencies)
{
    MediaSourceService service;

    EXPECT_FALSE(service.requestThumbnailSync({}).get());

    auto waveform = service.buildWaveformEnvelope({});
    EXPECT_FALSE(waveform.valid());
}

} // namespace
} // namespace rt