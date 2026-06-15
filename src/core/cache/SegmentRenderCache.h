/*
 * SegmentRenderCache — cache of COMPOSITED timeline-output frames (§4.6 slice 2b).
 *
 * Unlike FrameCache / DiskFrameCache, which hold decoded SOURCE frames, this
 * holds the compositor's OUTPUT for a timeline tick — so replaying a "green"
 * (pre-rendered) stretch costs a cache lookup instead of a full re-composite.
 *
 * Keyed by (tick, tier).  Each stored frame remembers the configHash
 * (hashCompositeConfigAt) it was produced under; a lookup only succeeds when
 * the caller's CURRENT configHash still matches.  Any edit that changes the
 * composite changes that hash, so the stale entry silently misses (and is
 * dropped) — there is no separate invalidation bookkeeping to forget.  We key
 * by (tick, tier) rather than (tick, tier, hash) so a re-render under a new
 * config REPLACES the stale frame instead of leaking it until LRU eviction.
 *
 * In-memory, LRU byte budget (its own, separate from FrameCache's source-frame
 * budget).  Thread-safe: the background render driver (slice 2c) writes while
 * the UI/compositor read.  Disk write-behind can layer on later, mirroring
 * DiskFrameCache; 2b is the in-memory store + correct staleness semantics.
 */

#pragma once

#include "cache/FrameCache.h"   // CachedFrame, ResolutionTier

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rt {

class SegmentRenderCache
{
public:
    /// Default 512 MB; App can override from installed RAM like FrameCache does.
    explicit SegmentRenderCache(size_t budgetBytes = 512ULL * 1024 * 1024);

    SegmentRenderCache(const SegmentRenderCache&) = delete;
    SegmentRenderCache& operator=(const SegmentRenderCache&) = delete;

    /// Store the composited frame for (tick, tier), produced under configHash.
    /// Replaces any existing entry for (tick, tier).  Null frame is ignored.
    void put(int64_t tick, ResolutionTier tier, uint64_t configHash,
             std::shared_ptr<CachedFrame> frame);

    /// Fetch the frame for (tick, tier) IFF it was produced under `configHash`.
    /// Returns nullptr on miss or stale config (a stale entry is dropped).
    /// Promotes the entry in LRU order on a hit.
    [[nodiscard]] std::shared_ptr<CachedFrame>
        get(int64_t tick, ResolutionTier tier, uint64_t configHash);

    /// Non-fetching, non-promoting freshness probe — for the render bar to
    /// decide Cached vs NeedsRender without disturbing LRU order.
    [[nodiscard]] bool hasFresh(int64_t tick, ResolutionTier tier,
                                uint64_t configHash) const;

    /// Drop everything (project/sequence switch).
    void clear();

    /// Drop entries whose tick is in [fromTick, toTick] (range edits).
    void invalidateRange(int64_t fromTick, int64_t toTick);

    void setBudgetBytes(size_t budgetBytes);

    [[nodiscard]] size_t sizeBytes() const;
    [[nodiscard]] size_t count() const;

private:
    struct Key
    {
        int64_t tick;
        uint8_t tier;
        bool operator==(const Key& o) const noexcept
        {
            return tick == o.tick && tier == o.tier;
        }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        {
            return std::hash<int64_t>{}(k.tick)
                 ^ (static_cast<size_t>(k.tier) * 0x9E3779B97F4A7C15ULL);
        }
    };
    struct Entry
    {
        uint64_t                     configHash{0};
        std::shared_ptr<CachedFrame> frame;
        size_t                       bytes{0};
        std::list<Key>::iterator     lruIt;
    };
    using Map = std::unordered_map<Key, Entry, KeyHash>;

    void dropEntry_locked(Map::iterator it);
    void evictToFit_locked();

    mutable std::mutex m_mtx;
    Map                m_entries;
    std::list<Key>     m_lru;          // front = most-recently used
    size_t             m_budgetBytes;
    size_t             m_curBytes{0};
};

} // namespace rt
