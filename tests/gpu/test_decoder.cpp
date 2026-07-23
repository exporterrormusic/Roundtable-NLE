/*
 * test_decoder.cpp — Tests for FrameCache and MediaPool
 *
 * Tests cover:
 *   - FrameCache: insert, lookup, LRU eviction, capacity, statistics
 *   - MediaPool: (decoder tests require actual video files, guarded)
 *
 * VideoDecoder open/decode tests are conditionally compiled when
 * ROUNDTABLE_HAS_FFMPEG is defined and a test video exists.
 */

#ifdef _MSC_VER
#  pragma warning(disable : 4996) // getenv — safe in test code
#endif

#include <gtest/gtest.h>

#include <cache/FrameCache.h>
#include <decode/VideoFrameMapping.h>
#include <playback/MediaPool.h>
#include <playback/FrameFallbackPolicy.h>
#include <playback/MediaPoolPrefetchInternal.h>

#ifdef ROUNDTABLE_HAS_FFMPEG
#include <decode/VideoDecoder.h>
#endif

#include <filesystem>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace rt;

// ─── Helper ─────────────────────────────────────────────────────────────────

static std::shared_ptr<CachedFrame> makeFrame(uint64_t mediaId, int64_t frameNum,
                                                uint32_t w = 64, uint32_t h = 64)
{
    auto f = std::make_shared<CachedFrame>();
    f->mediaId     = mediaId;
    f->frameNumber = frameNum;
    f->width       = w;
    f->height      = h;
    f->stride      = w * 4;
    f->tier        = ResolutionTier::Full;
    f->isKeyframe  = (frameNum % 30 == 0);
    f->timestamp   = frameNum / 30.0;
    f->pixels.resize(static_cast<size_t>(w) * h * 4, 0xAA); // BGRA
    return f;
}

// ═════════════════════════════════════════════════════════════════════════════
//  FrameCache tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(FrameCache, InsertAndLookup)
{
    FrameCache cache(1024 * 1024); // 1 MB
    auto frame = makeFrame(1, 0);
    cache.put(frame);

    auto result = cache.get(1, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->mediaId, 1u);
    EXPECT_EQ(result->frameNumber, 0);
    EXPECT_EQ(result->width, 64u);
    EXPECT_EQ(result->height, 64u);
}

TEST(FrameCache, CacheMiss)
{
    FrameCache cache(1024 * 1024);
    auto result = cache.get(1, 0);
    EXPECT_EQ(result, nullptr);
}

TEST(FrameCache, MultipleFrames)
{
    FrameCache cache(1024 * 1024);

    for (int64_t i = 0; i < 10; ++i) {
        cache.put(makeFrame(1, i));
    }

    EXPECT_EQ(cache.frameCount(), 10u);

    for (int64_t i = 0; i < 10; ++i) {
        auto result = cache.get(1, i);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->frameNumber, i);
    }
}

TEST(FrameCache, LRU_Eviction)
{
    // Each frame is 64*64*4 = 16384 bytes + sizeof(CachedFrame) overhead.
    // Set capacity to hold ~3 frames.
    size_t frameSize = 64 * 64 * 4 + sizeof(CachedFrame) + 64; // generous estimate
    FrameCache cache(frameSize * 3);

    cache.put(makeFrame(1, 0)); // oldest
    cache.put(makeFrame(1, 1));
    cache.put(makeFrame(1, 2)); // newest

    // All 3 should be present
    EXPECT_NE(cache.get(1, 0), nullptr);
    EXPECT_NE(cache.get(1, 1), nullptr);
    EXPECT_NE(cache.get(1, 2), nullptr);

    // Adding a 4th should evict the least recently used
    // After the gets above, LRU order is: 2 (most recent get), 1, 0
    // Actually the last get was for frame 2, then 1, then 0 was gotten first.
    // After gets: 0 → 1 → 2, so LRU order front→back: 2, 1, 0
    // Wait, we get 0, then 1, then 2. Each get promotes to front.
    // After get(0): front=[0], back=[]
    // After get(1): front=[1,0], ... 
    // After get(2): front=[2,1,0]
    // So 0 is LRU (back). Adding frame 3 should evict frame 0.
    cache.put(makeFrame(1, 3));

    EXPECT_EQ(cache.get(1, 0), nullptr);  // evicted
    EXPECT_NE(cache.get(1, 1), nullptr);
    EXPECT_NE(cache.get(1, 2), nullptr);
    EXPECT_NE(cache.get(1, 3), nullptr);
}

TEST(FrameCache, EvictMedia)
{
    FrameCache cache(1024 * 1024);

    cache.put(makeFrame(1, 0));
    cache.put(makeFrame(1, 1));
    cache.put(makeFrame(2, 0));
    cache.put(makeFrame(2, 1));

    EXPECT_EQ(cache.frameCount(), 4u);

    cache.evictMedia(1);

    EXPECT_EQ(cache.frameCount(), 2u);
    EXPECT_EQ(cache.get(1, 0), nullptr);
    EXPECT_EQ(cache.get(1, 1), nullptr);
    EXPECT_NE(cache.get(2, 0), nullptr);
    EXPECT_NE(cache.get(2, 1), nullptr);
}

TEST(FrameCache, Clear)
{
    FrameCache cache(1024 * 1024);

    for (int64_t i = 0; i < 5; ++i) {
        cache.put(makeFrame(1, i));
    }

    EXPECT_EQ(cache.frameCount(), 5u);
    cache.clear();
    EXPECT_EQ(cache.frameCount(), 0u);
    EXPECT_EQ(cache.memoryUsed(), 0u);
}

TEST(FrameCache, ResolutionTiers)
{
    FrameCache cache(64 * 1024 * 1024); // 64 MB — enough for HD frames

    auto full = makeFrame(1, 0, 1920, 1080);
    full->tier = ResolutionTier::Full;

    auto half = makeFrame(1, 0, 960, 540);
    half->tier = ResolutionTier::Half;

    cache.put(full);
    cache.put(half);

    // Both should coexist — different tier = different cache key
    EXPECT_NE(cache.get(1, 0, ResolutionTier::Full), nullptr);
    EXPECT_NE(cache.get(1, 0, ResolutionTier::Half), nullptr);
    EXPECT_EQ(cache.get(1, 0, ResolutionTier::Quarter), nullptr);

    EXPECT_EQ(cache.frameCount(), 2u);
}

TEST(FrameCache, ResolutionTierDimensionsAreSourceRelative)
{
    const auto full = resolutionTierDimensions(
        1920, 10919, ResolutionTier::Full);
    EXPECT_EQ(full.width, 1920);
    EXPECT_EQ(full.height, 10919);

    const auto half = resolutionTierDimensions(
        1920, 10919, ResolutionTier::Half);
    EXPECT_EQ(half.width, 960);
    EXPECT_EQ(half.height, 5458);

    const auto quarter = resolutionTierDimensions(
        1920, 10919, ResolutionTier::Quarter);
    EXPECT_EQ(quarter.width, 480);
    EXPECT_EQ(quarter.height, 2728);
}

TEST(FrameCache, Statistics)
{
    FrameCache cache(1024 * 1024);

    cache.put(makeFrame(1, 0));

    (void)cache.get(1, 0); // hit
    (void)cache.get(1, 0); // hit
    (void)cache.get(1, 1); // miss

    auto s = cache.stats();
    EXPECT_EQ(s.hitCount, 2u);
    EXPECT_EQ(s.missCount, 1u);
    EXPECT_EQ(s.frameCount, 1u);
    EXPECT_GT(s.memoryUsed, 0u);
    EXPECT_NEAR(s.hitRate(), 2.0 / 3.0, 1e-9);
}

// FrameCache::put is FIRST-DECODE-WINS for duplicate keys: NVDEC and the
// software decoder render H.264 with subtly different RGB, and replacing
// the cached frame when both pipelines raced caused visible flicker on
// dark areas (see the comment in FrameCache::put).  A duplicate put keeps
// the original pixels and just promotes the entry to MRU.
TEST(FrameCache, DuplicateInsertKeepsFirst)
{
    FrameCache cache(1024 * 1024);

    auto f1 = makeFrame(1, 0);
    f1->pixels[0] = 0x11;
    cache.put(f1);

    auto f2 = makeFrame(1, 0);
    f2->pixels[0] = 0x22;
    cache.put(f2);

    EXPECT_EQ(cache.frameCount(), 1u);
    auto result = cache.get(1, 0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->pixels[0], 0x11);  // first decode wins
}

TEST(FrameCache, OversizedFrameRejected)
{
    // Capacity smaller than a single frame
    FrameCache cache(100);

    auto frame = makeFrame(1, 0); // 64*64*4 = 16KB
    cache.put(frame);

    // Should not be cached (too large)
    EXPECT_EQ(cache.frameCount(), 0u);
    EXPECT_EQ(cache.get(1, 0), nullptr);
}

TEST(FrameCache, SetCapacityShrink)
{
    size_t frameSize = 64 * 64 * 4 + sizeof(CachedFrame) + 64;
    FrameCache cache(frameSize * 5);

    for (int64_t i = 0; i < 5; ++i) {
        cache.put(makeFrame(1, i));
    }
    EXPECT_EQ(cache.frameCount(), 5u);

    // Shrink to hold ~2 frames
    cache.setCapacity(frameSize * 2);

    EXPECT_LE(cache.frameCount(), 2u);
    EXPECT_LE(cache.memoryUsed(), frameSize * 2);
}

TEST(FrameCache, Contains)
{
    FrameCache cache(1024 * 1024);
    cache.put(makeFrame(1, 5));

    EXPECT_TRUE(cache.contains(1, 5));
    EXPECT_FALSE(cache.contains(1, 6));
    EXPECT_FALSE(cache.contains(2, 5));
}

// ═════════════════════════════════════════════════════════════════════════════
//  MediaPool tests (basic — no real video files)
// ═════════════════════════════════════════════════════════════════════════════

TEST(MediaPool, DefaultConstruction)
{
    MediaPool pool;
    EXPECT_EQ(pool.openCount(), 0u);
}

TEST(MediaPool, OpenNonexistentFile)
{
    MediaPool pool;
    // Explicit std::string — a raw literal is ambiguous between the
    // std::filesystem::path and UTF-8 std::string overloads of open().
    auto handle = pool.open(std::string("nonexistent_test_file.mp4"));
    EXPECT_EQ(handle, InvalidMedia);
    EXPECT_EQ(pool.openCount(), 0u);
}

TEST(MediaPool, IsValidInvalidHandle)
{
    MediaPool pool;
    EXPECT_FALSE(pool.isValid(0));
    EXPECT_FALSE(pool.isValid(42));
}

TEST(MediaPool, CloseAll)
{
    MediaPool pool;
    pool.closeAll();
    EXPECT_EQ(pool.openCount(), 0u);
}

TEST(MediaPool, SlowRetimeExactEndpointsBypassLastGoodAntiRewind)
{
    constexpr MediaHandle kHandle = 42;
    auto cache = std::make_shared<FrameCache>(1024 * 1024);
    cache->put(makeFrame(kHandle, 0));
    cache->put(makeFrame(kHandle, 1));
    MediaPool pool(cache);

    VideoClip clip;
    clip.setTimelineIn(0);
    clip.setSourceIn(0);
    clip.setSpeed(0.25);
    clip.setSourceFps(24.0);
    clip.setDuration(48000);

    // Fetching the upper endpoint establishes frame 1 as ordinary playback's
    // last-good frame. Its anti-rewind policy then intentionally substitutes
    // frame 1 for a repeated request of frame 0.
    ASSERT_EQ(pool.tryGetFrame(kHandle, 1)->frameNumber, 1);
    ASSERT_EQ(pool.tryGetFrame(kHandle, 0)->frameNumber, 1);

    std::shared_ptr<CachedFrame> heldLower;
    std::shared_ptr<CachedFrame> heldUpper;
    for (const int64_t tick : {int64_t(2000), int64_t(4000), int64_t(6000)}) {
        const auto mapped = mapTickToSourceFrame(clip, tick, nullptr);
        ASSERT_EQ(mapped.lowerFrame, 0);
        ASSERT_EQ(mapped.upperFrame, 1);

        heldLower = pool.tryGetExactFrame(kHandle, mapped.lowerFrame);
        heldUpper = pool.tryGetExactFrame(kHandle, mapped.upperFrame);
        ASSERT_NE(heldLower, nullptr);
        ASSERT_NE(heldUpper, nullptr);
        EXPECT_EQ(heldLower->frameNumber, mapped.lowerFrame);
        EXPECT_EQ(heldUpper->frameNumber, mapped.upperFrame);
    }

    // Endpoint ownership is held by the layer through GPU submission even if
    // the CPU cache evicts the keys in the meantime.
    cache->evictMedia(kHandle);
    ASSERT_NE(heldLower, nullptr);
    ASSERT_NE(heldUpper, nullptr);
    EXPECT_EQ(heldLower->frameNumber, 0);
    EXPECT_EQ(heldUpper->frameNumber, 1);
}

TEST(MediaPool, DistantForwardSeekDoesNotReturnLastGoodFrame)
{
    constexpr MediaHandle kHandle = 43;
    auto cache = std::make_shared<FrameCache>(1024 * 1024);
    cache->put(makeFrame(kHandle, 10));
    MediaPool pool(cache);

    // Establish an ordinary playback last-good frame, then jump well beyond
    // the bounded fallback window with no decoded frame at the destination.
    ASSERT_EQ(pool.tryGetFrame(kHandle, 10)->frameNumber, 10);
    EXPECT_EQ(pool.tryGetFrame(
        kHandle, 10 + kMaxPlaybackFallbackDistanceFrames + 100), nullptr);
}

TEST(MediaPool, DistantTimelineSeekIsNotACompositeHold)
{
    constexpr int64_t kOldTick = (15 * 60 + 21) * kTicksPerSecond;
    constexpr int64_t kNewTick = (17 * 60 + 54) * kTicksPerSecond;
    EXPECT_FALSE(isNearbyPlaybackCompositeTick(kNewTick, kOldTick));
    EXPECT_TRUE(isNearbyPlaybackCompositeTick(
        kOldTick + kMaxPlaybackCompositeHoldTicks, kOldTick));
}

TEST(MediaPoolPrefetch, StrideTwoBothMissingQueuesAdjacentExactPair)
{
    constexpr MediaHandle kHandle = 77;
    constexpr int64_t kLower = 100;
    constexpr int64_t kUpper = kLower + 1;
    std::deque<PrefetchTask> queue;

    // Simulate the ordinary 60 fps source / 30 fps project queue. Both
    // endpoints are absent from cache, but stride-two lookahead only queued
    // N, N+2, N+4... and therefore omitted N+1.
    for (int i = 0; i < 32; ++i) {
        PrefetchTask task;
        task.handle = kHandle;
        task.frameNumber = kLower + static_cast<int64_t>(i) * 2;
        task.tier = ResolutionTier::Full;
        task.info.frameCount = 1000;
        task.info.fps = 60.0;
        queue.push_back(std::move(task));
    }
    ASSERT_FALSE(std::any_of(queue.begin(), queue.end(), [](const auto& task) {
        return task.frameNumber == kUpper;
    }));

    PrefetchTask lower;
    lower.handle = kHandle;
    lower.frameNumber = kLower;
    lower.tier = ResolutionTier::Full;
    lower.info.frameCount = 1000;
    lower.info.fps = 60.0;
    PrefetchTask upper = lower;
    upper.frameNumber = kUpper;

    // N is promoted from ordinary lookahead and N+1 is independently merged
    // despite the queue already being full. The pair remains bounded.
    EXPECT_FALSE(prefetch_detail::mergeExactTask(queue, lower, 32));
    EXPECT_TRUE(prefetch_detail::mergeExactTask(queue, upper, 32));
    EXPECT_LE(queue.size(), 32u);
    ASSERT_GE(queue.size(), 2u);
    EXPECT_EQ(queue[0].frameNumber, kLower);
    EXPECT_EQ(queue[1].frameNumber, kUpper);

    const auto exactQueued = [&](int64_t frameNumber) {
        return std::any_of(queue.begin(), queue.end(), [&](const auto& task) {
            return task.handle == kHandle && task.frameNumber == frameNumber &&
                   task.tier == ResolutionTier::Full && task.urgent &&
                   task.exactRequest;
        });
    };
    EXPECT_TRUE(exactQueued(kLower));
    EXPECT_TRUE(exactQueued(kUpper));

    // A normal fallback request may rebuild decode-ahead immediately after
    // the pair is queued. Exact endpoints are not replaceable by that pass.
    queue.erase(std::remove_if(queue.begin(), queue.end(), [&](const auto& task) {
        return prefetch_detail::replaceableByDecodeAheadRebuild(
            task, kHandle, kLower);
    }), queue.end());
    EXPECT_TRUE(exactQueued(kLower));
    EXPECT_TRUE(exactQueued(kUpper));
    EXPECT_EQ(queue.size(), 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
//  VideoDecoder tests (require FFmpeg + actual file)
// ═════════════════════════════════════════════════════════════════════════════

#ifdef ROUNDTABLE_HAS_FFMPEG

TEST(VideoDecoder, DefaultConstruction)
{
    VideoDecoder decoder;
    EXPECT_FALSE(decoder.isOpen());
}

TEST(VideoDecoder, OpenNonexistent)
{
    VideoDecoder decoder;
    EXPECT_FALSE(decoder.open("nonexistent_test_file.mp4"));
    EXPECT_FALSE(decoder.isOpen());
}

TEST(VideoDecoder, OpenPngDoesNotRetainSourceHandle)
{
    // Locate a real repository PNG, then work on a disposable copy. CTest
    // normally runs under build/, so walk upward rather than assuming cwd.
    std::filesystem::path root = std::filesystem::current_path();
    std::filesystem::path source;
    for (int depth = 0; depth < 8; ++depth) {
        const auto candidate = root / "assets" / "NikkeBKG" / "Ark.png";
        if (std::filesystem::exists(candidate)) {
            source = candidate;
            break;
        }
        if (!root.has_parent_path() || root.parent_path() == root) break;
        root = root.parent_path();
    }
    if (source.empty())
        GTEST_SKIP() << "Repository PNG fixture not found";

    const auto tempPath = std::filesystem::temp_directory_path()
                        / "roundtable_decoder_overwrite_test.png";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    ec.clear();
    ASSERT_TRUE(std::filesystem::copy_file(
        source, tempPath, std::filesystem::copy_options::overwrite_existing, ec))
        << ec.message();

    struct TempGuard {
        std::filesystem::path path;
        ~TempGuard() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } guard{tempPath};

    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(tempPath, /*forceSoftware=*/true));

#ifdef _WIN32
    // Models an editor/Explorer replacement that asks for exclusive access.
    // Any retained decoder read HANDLE makes this fail with error 32.
    HANDLE exclusive = ::CreateFileW(
        tempPath.wstring().c_str(), GENERIC_WRITE | DELETE,
        /*dwShareMode=*/0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(exclusive, INVALID_HANDLE_VALUE)
        << "PNG remained locked; GetLastError=" << ::GetLastError();
    ::CloseHandle(exclusive);
#endif

    // The decoder owns an immutable memory snapshot and remains usable after
    // the pathname is replaced or removed.
    ASSERT_TRUE(std::filesystem::remove(tempPath, ec)) << ec.message();
    DecodedFrame frame;
    EXPECT_TRUE(decoder.decodeNext(frame));
    EXPECT_GT(frame.width, 0u);
    EXPECT_GT(frame.height, 0u);
}

// Integration test — requires a real video file.
// Set ROUNDTABLE_TEST_VIDEO env var to a .mp4 path to enable.
class VideoDecoderFileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const char* env = std::getenv("ROUNDTABLE_TEST_VIDEO");
        if (!env || !std::filesystem::exists(env)) {
            GTEST_SKIP() << "Set ROUNDTABLE_TEST_VIDEO to a .mp4 path to run this test";
        }
        videoPath = env;
    }
    std::string videoPath;
};

TEST_F(VideoDecoderFileTest, OpenAndQueryInfo)
{
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(videoPath));
    EXPECT_TRUE(decoder.isOpen());

    auto info = decoder.info();
    EXPECT_GT(info.width, 0u);
    EXPECT_GT(info.height, 0u);
    EXPECT_GT(info.fps, 0.0);
    // Still-image demuxers legitimately report zero duration.
    EXPECT_GE(info.duration, 0.0);
}

TEST_F(VideoDecoderFileTest, DecodeFirstFrame)
{
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(videoPath));

    DecodedFrame frame;
    ASSERT_TRUE(decoder.decodeNext(frame));
    EXPECT_GT(frame.width, 0u);
    EXPECT_GT(frame.height, 0u);
    EXPECT_NE(frame.data[0], nullptr);
}

// A paused Program Monitor can consume a Full-tier frame already produced by
// ordinary preview caching. Keep that non-export route covered so native still
// dimensions survive the complete decode/cache path.
TEST_F(VideoDecoderFileTest, MediaPoolFullTierPreservesStillDimensions)
{
    VideoDecoder probe;
    ASSERT_TRUE(probe.open(videoPath, /*forceSoftware=*/true));
    const VideoStreamInfo expected = probe.info();
    probe.close();

    auto cache = std::make_shared<FrameCache>(128ULL * 1024 * 1024);
    MediaPool pool(cache);
    const MediaHandle handle = pool.open(videoPath);
    ASSERT_NE(handle, InvalidMedia);

    auto frame = pool.getFrame(handle, 0, ResolutionTier::Full,
                               /*scrubMode=*/true,
                               /*forceExact=*/false);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->tier, ResolutionTier::Full);
    EXPECT_EQ(frame->frameNumber, 0);
    EXPECT_EQ(frame->width, expected.width);
    EXPECT_EQ(frame->height, expected.height);
    EXPECT_EQ(frame->stride, expected.width * 4u);
    EXPECT_GE(frame->pixels.size(),
              static_cast<size_t>(expected.width) * expected.height * 4u);
}

TEST_F(VideoDecoderFileTest, SeekAndDecode)
{
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(videoPath));

    // Seek to 1 second
    ASSERT_TRUE(decoder.seek(1.0, SeekMode::Keyframe));

    DecodedFrame frame;
    ASSERT_TRUE(decoder.decodeNext(frame));
    EXPECT_GT(frame.width, 0u);
}

TEST_F(VideoDecoderFileTest, DecodeMultipleFrames)
{
    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(videoPath));

    int decoded = 0;
    for (int i = 0; i < 30; ++i) {
        DecodedFrame frame;
        if (decoder.decodeNext(frame)) {
            ++decoded;
        } else {
            break;
        }
    }
    EXPECT_GT(decoded, 0);
}

#endif // ROUNDTABLE_HAS_FFMPEG

// ═════════════════════════════════════════════════════════════════════════════
//  DiskFrameCache tests
// ═════════════════════════════════════════════════════════════════════════════

#include <cache/DiskFrameCache.h>
#include <fstream>
#include <thread>

class DiskFrameCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Use a temp directory per test
        m_cacheDir = std::filesystem::temp_directory_path() / "rt_disk_cache_test";
        std::error_code ec;
        std::filesystem::remove_all(m_cacheDir, ec);
        std::filesystem::create_directories(m_cacheDir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(m_cacheDir, ec);
    }

    std::filesystem::path m_cacheDir;
};

TEST_F(DiskFrameCacheTest, PutAndGet)
{
    // Create a small test video file to use as the media source key
    auto testFile = m_cacheDir / "test_media.bin";
    {
        std::ofstream f(testFile, std::ios::binary);
        f << "fake video data for hashing";
    }

    DiskFrameCache cache(m_cacheDir / "cache", 1ULL * 1024 * 1024 * 1024);
    cache.registerMedia(42, testFile);

    auto frame = makeFrame(42, 10);
    frame->unpackedAlpha = true;
    frame->premultipliedAlpha = true;

    // Write synchronously by calling putAsync and waiting
    cache.putAsync(frame);

    // Give the writer thread time to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Should be found on disk now
    EXPECT_TRUE(cache.contains(42, 10, ResolutionTier::Full));

    auto loaded = cache.get(42, 10);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->mediaId, 42u);
    EXPECT_EQ(loaded->frameNumber, 10);
    EXPECT_EQ(loaded->width, 64u);
    EXPECT_EQ(loaded->height, 64u);
    EXPECT_EQ(loaded->stride, 64u * 4);
    EXPECT_TRUE(loaded->unpackedAlpha);
    EXPECT_TRUE(loaded->premultipliedAlpha);
    EXPECT_EQ(loaded->pixels.size(), frame->pixels.size());
    EXPECT_EQ(loaded->pixels[0], 0xAA);
}

TEST_F(DiskFrameCacheTest, MissReturnsNull)
{
    auto testFile = m_cacheDir / "test_media.bin";
    {
        std::ofstream f(testFile, std::ios::binary);
        f << "fake video data";
    }

    DiskFrameCache cache(m_cacheDir / "cache", 1ULL * 1024 * 1024 * 1024);
    cache.registerMedia(1, testFile);

    EXPECT_FALSE(cache.contains(1, 0, ResolutionTier::Full));
    EXPECT_EQ(cache.get(1, 0), nullptr);
}

TEST_F(DiskFrameCacheTest, UnregisteredMediaReturnsNull)
{
    DiskFrameCache cache(m_cacheDir / "cache", 1ULL * 1024 * 1024 * 1024);

    // No media registered — should return null
    EXPECT_EQ(cache.get(99, 0), nullptr);
    EXPECT_FALSE(cache.contains(99, 0, ResolutionTier::Full));
}

TEST_F(DiskFrameCacheTest, ResolutionTiers)
{
    auto testFile = m_cacheDir / "test_media.bin";
    {
        std::ofstream f(testFile, std::ios::binary);
        f << "fake video for tier test";
    }

    DiskFrameCache cache(m_cacheDir / "cache", 1ULL * 1024 * 1024 * 1024);
    cache.registerMedia(1, testFile);

    auto full = makeFrame(1, 0, 1920, 1080);
    full->tier = ResolutionTier::Full;

    auto half = makeFrame(1, 0, 960, 540);
    half->tier = ResolutionTier::Half;

    cache.putAsync(full);
    cache.putAsync(half);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_TRUE(cache.contains(1, 0, ResolutionTier::Full));
    EXPECT_TRUE(cache.contains(1, 0, ResolutionTier::Half));
    EXPECT_FALSE(cache.contains(1, 0, ResolutionTier::Quarter));

    auto loadedFull = cache.get(1, 0, ResolutionTier::Full);
    auto loadedHalf = cache.get(1, 0, ResolutionTier::Half);
    ASSERT_NE(loadedFull, nullptr);
    ASSERT_NE(loadedHalf, nullptr);
    EXPECT_EQ(loadedFull->width, 1920u);
    EXPECT_EQ(loadedHalf->width, 960u);
}

TEST_F(DiskFrameCacheTest, PersistAcrossInstances)
{
    auto testFile = m_cacheDir / "test_media.bin";
    {
        std::ofstream f(testFile, std::ios::binary);
        f << "persistence test data";
    }

    auto cachePath = m_cacheDir / "cache";

    // Instance 1: write frames
    {
        DiskFrameCache cache1(cachePath, 1ULL * 1024 * 1024 * 1024);
        cache1.registerMedia(10, testFile);
        cache1.putAsync(makeFrame(10, 0));
        cache1.putAsync(makeFrame(10, 1));
        cache1.putAsync(makeFrame(10, 2));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(cache1.entryCount(), 3u);
    }

    // Instance 2: should find frames from disk scan
    {
        DiskFrameCache cache2(cachePath, 1ULL * 1024 * 1024 * 1024);
        cache2.registerMedia(10, testFile);
        EXPECT_EQ(cache2.entryCount(), 3u);
        EXPECT_TRUE(cache2.contains(10, 0, ResolutionTier::Full));
        EXPECT_TRUE(cache2.contains(10, 1, ResolutionTier::Full));
        EXPECT_TRUE(cache2.contains(10, 2, ResolutionTier::Full));

        auto loaded = cache2.get(10, 1);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->frameNumber, 1);
        EXPECT_EQ(loaded->pixels.size(), 64u * 64 * 4);
    }
}

TEST_F(DiskFrameCacheTest, EvictMedia)
{
    auto testFile = m_cacheDir / "test_media.bin";
    {
        std::ofstream f(testFile, std::ios::binary);
        f << "evict test";
    }

    DiskFrameCache cache(m_cacheDir / "cache", 1ULL * 1024 * 1024 * 1024);
    cache.registerMedia(1, testFile);

    cache.putAsync(makeFrame(1, 0));
    cache.putAsync(makeFrame(1, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(cache.entryCount(), 2u);

    cache.evictMedia(1);
    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_FALSE(cache.contains(1, 0, ResolutionTier::Full));
    EXPECT_FALSE(cache.contains(1, 1, ResolutionTier::Full));
}
