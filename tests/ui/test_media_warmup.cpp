/*
 * test_media_warmup.cpp — MediaWarmupController with a real MediaPool.
 *
 * A warm-up opens the timeline's media on worker threads, reports
 * progress on the UI thread, then reports "warmed" (current batch) and
 * "finished" (no batch left). A cancelled batch still finishes, but is
 * not reported as warmed.
 */

#include <gtest/gtest.h>

#include "../gpu/IndexedVideoFixture.h"

#include "cache/FrameCache.h"
#include "panels/timeline/MediaWarmupController.h"
#include "playback/MediaPool.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QApplication>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <utility>

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

void ensureApp()
{
    static int argc = 1;
    static const char* argv[] = {"test_media_warmup"};
    if (!QApplication::instance())
        new QApplication(argc, const_cast<char**>(argv));
}

class MediaWarmupTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = std::filesystem::temp_directory_path() /
            (std::string("rt_media_warmup_") + info->name() + ".mp4");
        if (!rt_test::writeIndexedVideo(m_path.string()))
            GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
        m_cache = std::make_shared<rt::FrameCache>(64ull * 1024 * 1024);
        m_pool = std::make_unique<rt::MediaPool>(m_cache);
        m_pool->setProjectFps(rt_test::kFps);

        rt::Track* v1 = m_timeline.addVideoTrack("V1");
        auto clip = std::make_unique<rt::VideoClip>();
        clip->setMediaPath(m_path.string());
        clip->setDuration(48000);
        v1->addClip(std::move(clip));

        m_warmup = std::make_unique<rt::MediaWarmupController>(rt::MediaWarmupController::Config{
            /*timeline=*/  [this] { return &m_timeline; },
            /*mediaPool=*/ [this] { return m_pool.get(); },
            /*canWarm=*/   [] { return true; },
            /*destroying=*/[] { return false; },
            /*invalidateCompositeCache=*/[] {},
            /*progress=*/  [this](int done, int total) { m_progress = {done, total}; },
            /*warmed=*/    [this] { ++m_warmed; },
            /*finished=*/  [this] { ++m_finished; },
        });
    }

    void TearDown() override
    {
        if (m_warmup) m_warmup->cancelAndJoin();
        m_warmup.reset();
        m_pool.reset();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    bool pumpUntilFinished()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (m_finished == 0) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    std::filesystem::path m_path;
    std::shared_ptr<rt::FrameCache> m_cache;
    std::unique_ptr<rt::MediaPool> m_pool;
    rt::Timeline m_timeline;
    std::unique_ptr<rt::MediaWarmupController> m_warmup;
    std::pair<int, int> m_progress{0, 0};
    int m_warmed{0};
    int m_finished{0};
};

TEST_F(MediaWarmupTest, AWarmUpOpensTheMediaAndReportsWhenDone)
{
    m_warmup->start();
    EXPECT_TRUE(m_warmup->isActive());
    ASSERT_TRUE(pumpUntilFinished());

    EXPECT_EQ(m_progress, (std::pair<int, int>{1, 1}));
    EXPECT_EQ(m_warmed, 1);
    EXPECT_EQ(m_finished, 1);
    EXPECT_FALSE(m_warmup->isActive());
    EXPECT_NE(m_pool->getInfo(m_pool->open(m_path.string())), nullptr);
}

TEST_F(MediaWarmupTest, ACancelledWarmUpFinishesWithoutReportingWarmed)
{
    m_warmup->start();
    m_warmup->cancelAndJoin();
    ASSERT_TRUE(pumpUntilFinished());

    EXPECT_EQ(m_warmed, 0);
    EXPECT_EQ(m_finished, 1);
    EXPECT_FALSE(m_warmup->isActive());
}

TEST_F(MediaWarmupTest, AnEmptyTimelineStartsNothing)
{
    rt::Timeline empty;
    rt::MediaWarmupController warmup({
        [&] { return &empty; },
        [this] { return m_pool.get(); },
        [] { return true; },
        [] { return false; },
        [] {},
        [](int, int) {},
        [] {},
        [this] { ++m_finished; },
    });
    warmup.start();
    EXPECT_FALSE(warmup.isActive());
    QApplication::processEvents();
    EXPECT_EQ(m_finished, 0);
}

} // namespace
#endif
