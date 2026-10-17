/*
 * test_frame_request.cpp - MediaPool::requestFrame: each FramePolicy returns
 * what it promises, and FrameResult::exact tells a stand-in from the frame
 * that was asked for.
 */
#include <gtest/gtest.h>

#include <cache/FrameCache.h>
#include <playback/MediaPool.h>

#include "IndexedVideoFixture.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace rt;

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

using namespace rt_test;

class FrameRequestTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = std::filesystem::temp_directory_path() /
            (std::string("rt_frame_request_") + info->name() + ".mp4");
        if (!writeIndexedVideo(m_path.string()))
            GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
        m_cache = std::make_shared<FrameCache>(256ull * 1024 * 1024);
        m_pool = std::make_unique<MediaPool>(m_cache);
        m_pool->setProjectFps(kFps);
        m_handle = m_pool->open(m_path);
        ASSERT_NE(m_handle, InvalidMedia);
    }

    void TearDown() override
    {
        m_pool.reset();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    FrameResult request(int64_t frame, FramePolicy policy,
                        ResolutionTier tier = ResolutionTier::Full)
    {
        return m_pool->requestFrame({m_handle, frame, tier, policy});
    }

    /// A decoded-looking frame placed in the cache by hand.
    std::shared_ptr<CachedFrame> seed(int64_t frame)
    {
        auto f = std::make_shared<CachedFrame>();
        f->mediaId     = m_handle;
        f->frameNumber = frame;
        f->tier        = ResolutionTier::Full;
        f->width       = kW;
        f->height      = kH;
        f->stride      = kW * 4;
        f->pixels.assign(static_cast<size_t>(kW) * kH * 4, 0);
        m_cache->put(f);
        return f;
    }

    bool waitCached(int64_t frame)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_pool->isFrameCached(m_handle, frame, ResolutionTier::Full)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    std::filesystem::path       m_path;
    std::shared_ptr<FrameCache> m_cache;
    std::unique_ptr<MediaPool>  m_pool;
    MediaHandle                m_handle{InvalidMedia};
};

} // namespace

TEST_F(FrameRequestTest, ExactBlockingReturnsTheRequestedPicture)
{
    for (int target : {0, 37, 90, 12, 13}) {
        const auto result = request(target, FramePolicy::ExactBlocking);
        ASSERT_TRUE(result.frame) << "frame " << target;
        EXPECT_TRUE(result.exact) << "frame " << target;
        EXPECT_EQ(result.resolvedFrame, target);
        EXPECT_EQ(frameIndexOf(result.frame.get()), target);
    }
}

TEST_F(FrameRequestTest, PastTheEndClampsToTheLastFrameAndIsExact)
{
    const auto* info = m_pool->getInfo(m_handle);
    ASSERT_TRUE(info);
    ASSERT_GT(info->frameCount, 1);
    const int64_t last = info->frameCount - 1;

    const auto result = request(last + 10, FramePolicy::ExactBlocking);
    EXPECT_EQ(result.resolvedFrame, last);
    ASSERT_TRUE(result.frame);
    EXPECT_TRUE(result.exact);
    EXPECT_EQ(result.frame->frameNumber, last);
}

TEST_F(FrameRequestTest, ExactCacheOnlyNeverReturnsANeighbour)
{
    const auto seeded = seed(50);
    const auto miss = request(51, FramePolicy::ExactCacheOnly);
    EXPECT_FALSE(miss.frame);
    EXPECT_FALSE(miss.exact);

    const auto hit = request(50, FramePolicy::ExactCacheOnly);
    EXPECT_EQ(hit.frame, seeded);
    EXPECT_TRUE(hit.exact);
}

TEST_F(FrameRequestTest, ExactOrStandInFlagsTheStandInThenDeliversTheFrame)
{
    const auto seeded = seed(60);

    const auto t0 = std::chrono::steady_clock::now();
    const auto first = request(62, FramePolicy::ExactOrStandIn);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_LT(elapsed, std::chrono::milliseconds(50)) << "must not decode inline";
    EXPECT_EQ(first.frame, seeded);
    EXPECT_FALSE(first.exact);

    ASSERT_TRUE(waitCached(62)) << "the exact frame was never queued";
    const auto second = request(62, FramePolicy::ExactOrStandIn);
    ASSERT_TRUE(second.frame);
    EXPECT_TRUE(second.exact);
    EXPECT_EQ(second.frame->frameNumber, 62);
    EXPECT_NEAR(second.frame->timestamp * kFps, 62.0, 0.01);
}

TEST_F(FrameRequestTest, BestEffortReturnsANearbyFrameAsNotExact)
{
    const auto seeded = seed(80);
    const auto result = request(81, FramePolicy::BestEffort);
    EXPECT_EQ(result.frame, seeded);
    EXPECT_FALSE(result.exact);
    EXPECT_EQ(result.resolvedFrame, 81);
}

TEST_F(FrameRequestTest, ScrubDecodesAQuarterTierFrameOnAMiss)
{
    const auto result = request(20, FramePolicy::Scrub);
    ASSERT_TRUE(result.frame);
    EXPECT_TRUE(result.exact);
    EXPECT_EQ(result.frame->tier, ResolutionTier::Quarter);
    EXPECT_NEAR(result.frame->timestamp * kFps, 20.0, 0.01);
}

#endif
