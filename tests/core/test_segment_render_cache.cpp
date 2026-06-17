/*
 * test_segment_render_cache.cpp — composited-frame cache (§4.6 slice 2b).
 *
 * Covers the staleness contract (a hit requires the SAME configHash the frame
 * was rendered under), (tick,tier) keying, LRU byte-budget eviction, and the
 * non-promoting freshness probe used by the render bar.
 */

#include <gtest/gtest.h>

#include "cache/SegmentRenderCache.h"
#include "cache/FrameCache.h"   // CachedFrame, ResolutionTier

#include <filesystem>
#include <memory>
#include <system_error>

namespace rt {
namespace {

std::shared_ptr<CachedFrame> makeFrame(size_t pixelBytes)
{
    auto f = std::make_shared<CachedFrame>();
    f->pixels.resize(pixelBytes);   // drives memoryUsage()
    return f;
}

// Disk round-trip needs real dims (the reader sanity-checks pixel count).
std::shared_ptr<CachedFrame> makeFrameWH(uint32_t w, uint32_t h, uint8_t fill)
{
    auto f = std::make_shared<CachedFrame>();
    f->width  = w;
    f->height = h;
    f->stride = w * 4;
    f->pixels.assign(static_cast<size_t>(w) * h * 4, fill);
    return f;
}

std::filesystem::path makeTempDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "rt_segcache_test" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TEST(SegmentRenderCache, HitRequiresMatchingConfigHash)
{
    SegmentRenderCache cache;
    cache.put(1000, ResolutionTier::Full, /*configHash=*/0xAAAA, makeFrame(1024));

    EXPECT_NE(cache.get(1000, ResolutionTier::Full, 0xAAAA), nullptr);  // fresh
    // Same tick/tier but the config moved on → stale miss.
    EXPECT_EQ(cache.get(1000, ResolutionTier::Full, 0xBBBB), nullptr);
}

TEST(SegmentRenderCache, StaleEntryIsDroppedOnMiss)
{
    SegmentRenderCache cache;
    cache.put(1000, ResolutionTier::Full, 0xAAAA, makeFrame(1024));
    EXPECT_EQ(cache.count(), 1u);

    // A stale lookup should drop the entry (not keep probing a dead frame).
    EXPECT_EQ(cache.get(1000, ResolutionTier::Full, 0xBBBB), nullptr);
    EXPECT_EQ(cache.count(), 0u);
}

TEST(SegmentRenderCache, TierIsPartOfKey)
{
    SegmentRenderCache cache;
    cache.put(1000, ResolutionTier::Full, 0x1, makeFrame(1024));
    cache.put(1000, ResolutionTier::Half, 0x1, makeFrame(1024));

    EXPECT_NE(cache.get(1000, ResolutionTier::Full, 0x1), nullptr);
    EXPECT_NE(cache.get(1000, ResolutionTier::Half, 0x1), nullptr);
    EXPECT_EQ(cache.count(), 2u);
}

TEST(SegmentRenderCache, PutReplacesEntryForSameKey)
{
    SegmentRenderCache cache;
    cache.put(1000, ResolutionTier::Full, 0x1, makeFrame(1024));
    cache.put(1000, ResolutionTier::Full, 0x2, makeFrame(2048));   // re-render, new config

    EXPECT_EQ(cache.count(), 1u);                                  // replaced, not added
    // Probe the stale config with the NON-dropping hasFresh (a stale get()
    // would proactively drop the entry — see StaleEntryIsDroppedOnMiss).
    EXPECT_FALSE(cache.hasFresh(1000, ResolutionTier::Full, 0x1));  // old config gone
    EXPECT_NE(cache.get(1000, ResolutionTier::Full, 0x2), nullptr); // new config present
}

TEST(SegmentRenderCache, HasFreshProbeDoesNotPromoteOrFetch)
{
    SegmentRenderCache cache;
    cache.put(1000, ResolutionTier::Full, 0x1, makeFrame(1024));

    EXPECT_TRUE(cache.hasFresh(1000, ResolutionTier::Full, 0x1));
    EXPECT_FALSE(cache.hasFresh(1000, ResolutionTier::Full, 0x2));   // wrong config
    EXPECT_FALSE(cache.hasFresh(2000, ResolutionTier::Full, 0x1));   // absent tick
    // Probe must not drop the entry.
    EXPECT_EQ(cache.count(), 1u);
}

TEST(SegmentRenderCache, InvalidateRangeDropsOnlyInRange)
{
    SegmentRenderCache cache;
    cache.put(100,  ResolutionTier::Full, 0x1, makeFrame(512));
    cache.put(500,  ResolutionTier::Full, 0x1, makeFrame(512));
    cache.put(900,  ResolutionTier::Full, 0x1, makeFrame(512));

    cache.invalidateRange(400, 600);   // drops tick 500 only

    EXPECT_TRUE(cache.hasFresh(100, ResolutionTier::Full, 0x1));
    EXPECT_FALSE(cache.hasFresh(500, ResolutionTier::Full, 0x1));
    EXPECT_TRUE(cache.hasFresh(900, ResolutionTier::Full, 0x1));
}

TEST(SegmentRenderCache, EvictsLruWhenOverBudget)
{
    // Budget fits ~2 frames of 1 MB.  sizeof(CachedFrame) overhead is tiny
    // next to 1 MB, so the count settles at 2.
    constexpr size_t kFrameBytes = 1024 * 1024;
    SegmentRenderCache cache(/*budgetBytes=*/2 * kFrameBytes + 4096);

    cache.put(1, ResolutionTier::Full, 0x1, makeFrame(kFrameBytes));
    cache.put(2, ResolutionTier::Full, 0x1, makeFrame(kFrameBytes));
    // Touch tick 1 so tick 2 becomes the LRU victim.
    EXPECT_NE(cache.get(1, ResolutionTier::Full, 0x1), nullptr);
    cache.put(3, ResolutionTier::Full, 0x1, makeFrame(kFrameBytes));

    EXPECT_LE(cache.count(), 2u);
    EXPECT_TRUE(cache.hasFresh(1, ResolutionTier::Full, 0x1));   // recently used → kept
    EXPECT_TRUE(cache.hasFresh(3, ResolutionTier::Full, 0x1));   // newest → kept
    EXPECT_FALSE(cache.hasFresh(2, ResolutionTier::Full, 0x1));  // LRU → evicted
    EXPECT_LE(cache.sizeBytes(), 2 * kFrameBytes + 4096);
}

TEST(SegmentRenderCache, ClearEmptiesEverything)
{
    SegmentRenderCache cache;
    cache.put(1, ResolutionTier::Full, 0x1, makeFrame(1024));
    cache.put(2, ResolutionTier::Full, 0x1, makeFrame(1024));
    cache.clear();
    EXPECT_EQ(cache.count(), 0u);
    EXPECT_EQ(cache.sizeBytes(), 0u);
}

// ── Disk persistence (survives restart) ─────────────────────────────────────

TEST(SegmentRenderCache, DiskPersistsAcrossRestart)
{
    auto dir = makeTempDir("persist");
    {
        SegmentRenderCache cache;
        cache.enableDiskCache(dir, 64ull * 1024 * 1024);
        cache.put(1000, ResolutionTier::Full, 0xABCD, makeFrameWH(4, 2, 0x7F));
        // ~SegmentRenderCache drains the write-behind queue + joins the writer.
    }
    // "Restart": a fresh cache over the same dir rebuilds its index from disk.
    SegmentRenderCache cache2;
    cache2.enableDiskCache(dir, 64ull * 1024 * 1024);
    EXPECT_TRUE(cache2.hasFresh(1000, ResolutionTier::Full, 0xABCD));

    auto f = cache2.get(1000, ResolutionTier::Full, 0xABCD);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->width, 4u);
    EXPECT_EQ(f->height, 2u);
    ASSERT_EQ(f->pixels.size(), static_cast<size_t>(4 * 2 * 4));
    EXPECT_EQ(f->pixels[0], 0x7F);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(SegmentRenderCache, DiskMissOnDifferentConfigHash)
{
    auto dir = makeTempDir("confighash");
    {
        SegmentRenderCache cache;
        cache.enableDiskCache(dir, 64ull * 1024 * 1024);
        cache.put(1000, ResolutionTier::Full, 0x1111, makeFrameWH(4, 2, 0x10));
    }
    SegmentRenderCache cache2;
    cache2.enableDiskCache(dir, 64ull * 1024 * 1024);
    EXPECT_TRUE(cache2.hasFresh(1000, ResolutionTier::Full, 0x1111));
    // An edit changes the configHash → different filename → not the same frame.
    EXPECT_FALSE(cache2.hasFresh(1000, ResolutionTier::Full, 0x2222));
    EXPECT_EQ(cache2.get(1000, ResolutionTier::Full, 0x2222), nullptr);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(SegmentRenderCache, NoDiskMeansNoPersistence)
{
    auto dir = makeTempDir("disabled");
    {
        SegmentRenderCache cache;   // disk never enabled
        cache.put(1000, ResolutionTier::Full, 0x1, makeFrameWH(4, 2, 0x1));
    }
    SegmentRenderCache cache2;
    cache2.enableDiskCache(dir, 64ull * 1024 * 1024);   // dir is empty
    EXPECT_FALSE(cache2.hasFresh(1000, ResolutionTier::Full, 0x1));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace
} // namespace rt
