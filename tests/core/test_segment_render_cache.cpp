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

#include <memory>

namespace rt {
namespace {

std::shared_ptr<CachedFrame> makeFrame(size_t pixelBytes)
{
    auto f = std::make_shared<CachedFrame>();
    f->pixels.resize(pixelBytes);   // drives memoryUsage()
    return f;
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

} // namespace
} // namespace rt
