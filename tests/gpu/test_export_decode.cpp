/*
 * test_export_decode.cpp — the export path of MediaPool::getFrame
 * (forceExact) returns exactly the requested source frame through the
 * patterns an export produces:
 *   - in-order frames with repeats (a 24p clip in a 30p sequence, slow
 *     motion) — a repeat is answered without decoding again;
 *   - forward skips (a fast clip);
 *   - backward jumps (a clip re-used later, a reversed clip), which reopen
 *     the frame-threaded export decoder instead of flushing it, and after
 *     enough of them fall back to slice threads.
 *
 * The fixture is a software-decoded MPEG-4 file, so the export decoder runs
 * on frame threads. Each frame's index is drawn as 7 black/white stripes.
 */
#include <gtest/gtest.h>

#include <cache/FrameCache.h>
#include <playback/MediaPool.h>

#include "IndexedVideoFixture.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <vector>

using namespace rt;

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

using namespace rt_test;

class ExportDecodeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = std::filesystem::temp_directory_path() /
            (std::string("rt_export_decode_") + info->name() + ".mp4");
        if (!writeIndexedVideo(m_path.string()))
            GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
        m_cache = std::make_shared<FrameCache>(256ull * 1024 * 1024);
        m_pool = std::make_unique<MediaPool>(m_cache);
        m_handle = m_pool->open(m_path);
        ASSERT_NE(m_handle, InvalidMedia);
    }

    void TearDown() override
    {
        m_pool.reset();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    std::shared_ptr<CachedFrame> exact(int frame)
    {
        return m_pool->getFrame(m_handle, frame, ResolutionTier::Full,
                                /*scrubMode=*/true, /*forceExact=*/true);
    }

    std::filesystem::path       m_path;
    std::shared_ptr<FrameCache> m_cache;
    std::unique_ptr<MediaPool>  m_pool;
    MediaHandle                m_handle{InvalidMedia};
};

} // namespace

TEST_F(ExportDecodeTest, InOrderWithRepeatsAndSkips)
{
    // 24p-in-30p cadence (every 4th source frame shown twice), then a 2x
    // stretch across the keyframe at 90.
    std::vector<int> sequence;
    for (int i = 0; i < 60; ++i) {
        sequence.push_back(i);
        if (i % 4 == 3) sequence.push_back(i);
    }
    for (int i = 60; i < 140; i += 2) sequence.push_back(i);

    std::shared_ptr<CachedFrame> previous;
    for (int target : sequence) {
        auto frame = exact(target);
        ASSERT_TRUE(frame) << "frame " << target;
        EXPECT_EQ(frameIndexOf(frame.get()), target);
        if (previous && previous->frameNumber == target)
            EXPECT_EQ(frame.get(), previous.get()) << "repeat " << target << " decoded again";
        previous = frame;
    }
}

TEST_F(ExportDecodeTest, BackwardJumpsLandExactly)
{
    // A clip re-used later in the edit, then a reversed run — well past the
    // number of reopens after which the decoder goes slice-threaded.
    for (int target : {10, 11, 12, 100, 101, 40, 41, 42, 130, 5})
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "frame " << target;
    for (int target = 120; target >= 100; --target)
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "reverse " << target;
    for (int target = 101; target < 110; ++target)
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "forward again " << target;
}

TEST_F(ExportDecodeTest, EveryPictureTimestampMatchesItsLabel)
{
    // In order, forward skips and backward jumps: each picture's own
    // timestamp agrees with the frame it is labelled, and a clean CFR file
    // with B-frames never trips the label check (no re-seek, no latch).
    std::vector<int> sequence;
    for (int i = 0; i < 50; ++i) sequence.push_back(i);
    for (int i = 50; i < 140; i += 3) sequence.push_back(i);
    for (int i : {20, 21, 22, 95, 96, 7}) sequence.push_back(i);
    for (int target : sequence) {
        auto frame = exact(target);
        ASSERT_TRUE(frame) << "frame " << target;
        EXPECT_EQ(frame->frameNumber, target);
        EXPECT_NEAR(frame->timestamp * kFps, target, 0.01) << "frame " << target;
    }
    EXPECT_EQ(m_pool->m_perf.labelReseeks.load(), 0u);
    EXPECT_EQ(m_pool->m_perf.labelMismatches.load(), 0u);
}

TEST_F(ExportDecodeTest, ScrubDecoderLabelsMatchPictures)
{
    // The scrub decoder decodes on from its last picture (delta 1 and short
    // forward jumps) — the path the label check guards.
    for (int target = 0; target < 40; ++target) {
        auto frame = m_pool->getFrame(m_handle, target, ResolutionTier::Full,
                                      /*scrubMode=*/true);
        ASSERT_TRUE(frame) << "frame " << target;
        if (frame->frameNumber != target) continue;  // a nearby cached frame
        EXPECT_EQ(frameIndexOf(frame.get()), target);
        EXPECT_NEAR(frame->timestamp * kFps, target, 0.01) << "frame " << target;
    }
    EXPECT_EQ(m_pool->m_perf.labelReseeks.load(), 0u);
    EXPECT_EQ(m_pool->m_perf.labelMismatches.load(), 0u);
}

TEST_F(ExportDecodeTest, ReleasedDecodersReopen)
{
    EXPECT_EQ(frameIndexOf(exact(30).get()), 30);
    m_pool->releaseExportDecoders();
    EXPECT_EQ(frameIndexOf(exact(31).get()), 31);
    EXPECT_EQ(frameIndexOf(exact(32).get()), 32);
}


TEST_F(ExportDecodeTest, ExactPlaybackHitTopsUpDecodeAhead)
{
    // Steady playback reads exact cache hits. Those used to schedule
    // nothing, so the decode-ahead window drained and was refilled only
    // after a frame had already missed. A hit with a gap ahead must queue
    // decode-ahead on its own.
    m_pool->setProjectFps(kFps);
    auto seeded = std::make_shared<CachedFrame>();
    seeded->mediaId     = m_handle;
    seeded->frameNumber = 100;
    seeded->tier        = ResolutionTier::Full;
    seeded->width       = kW;
    seeded->height      = kH;
    seeded->stride      = kW * 4;
    seeded->pixels.assign(static_cast<size_t>(kW) * kH * 4, 0);
    m_cache->put(seeded);
    ASSERT_FALSE(m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full));

    ASSERT_EQ(m_pool->tryGetFrame(m_handle, 100, ResolutionTier::Full), seeded);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full));
}

#endif
