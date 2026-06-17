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

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace rt {

class SegmentRenderCache
{
public:
    /// Default 512 MB; App can override from installed RAM like FrameCache does.
    explicit SegmentRenderCache(size_t budgetBytes = 512ULL * 1024 * 1024);
    ~SegmentRenderCache();

    SegmentRenderCache(const SegmentRenderCache&) = delete;
    SegmentRenderCache& operator=(const SegmentRenderCache&) = delete;

    /// Enable disk-backed persistence so pre-rendered segments survive an app
    /// restart.  Frames are written behind on a background thread and read back
    /// on an in-memory miss.  The on-disk key is (tick, tier, configHash) — it
    /// is content-addressed, so a file is valid for ANY session whose config
    /// hashes equal (an edit changes the hash → a different file → never stale).
    /// Seeds the in-memory index by scanning `dir`.  Safe to call once at init.
    void enableDiskCache(const std::filesystem::path& dir, size_t budgetBytes);

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
    // Insert into the in-memory store only (no disk write-behind) — shared by
    // put() and the disk-read promotion path.
    void insertMemory_locked(const Key& key, uint64_t configHash,
                             std::shared_ptr<CachedFrame> frame);

    mutable std::mutex m_mtx;
    Map                m_entries;
    std::list<Key>     m_lru;          // front = most-recently used
    size_t             m_budgetBytes;
    size_t             m_curBytes{0};

    // ── Disk persistence (optional, enabled via enableDiskCache) ─────────
    static constexpr uint32_t kDiskMagic   = 0x47534752; // "RGSG" (LE)
    static constexpr uint32_t kDiskVersion = 1;
    static constexpr size_t   kMaxWriteQueue = 24;       // bounds pinned RAM

    struct DiskKey {
        int64_t  tick;
        uint8_t  tier;
        uint64_t configHash;
        bool operator==(const DiskKey& o) const noexcept {
            return tick == o.tick && tier == o.tier && configHash == o.configHash;
        }
    };
    struct DiskKeyHash {
        size_t operator()(const DiskKey& k) const noexcept {
            size_t h = std::hash<int64_t>{}(k.tick);
            h ^= std::hash<uint64_t>{}(k.configHash) + 0x9e3779b97f4a7c15ULL
               + (h << 6) + (h >> 2);
            h ^= static_cast<size_t>(k.tier) * 0x9e3779b9u;
            return h;
        }
    };
    struct PendingWrite {
        DiskKey                      key;
        std::shared_ptr<CachedFrame> frame;
    };

    [[nodiscard]] std::filesystem::path diskPathFor(const DiskKey& k) const;
    [[nodiscard]] static bool parseDiskName(const std::string& filename, DiskKey& out);
    [[nodiscard]] bool writeFrameToDisk(const std::filesystem::path& path,
                                        const DiskKey& k,
                                        const CachedFrame& frame) const;
    [[nodiscard]] std::shared_ptr<CachedFrame> readFrameFromDisk(
        const std::filesystem::path& path, const DiskKey& k) const;
    void enqueueDiskWrite(const DiskKey& k, std::shared_ptr<CachedFrame> frame);
    void diskWriterLoop();
    void scanDiskCache();              // rebuild index + used-bytes from dir
    void enforceDiskBudget();          // LRU-by-mtime delete when over budget

    bool                  m_diskEnabled{false};
    std::filesystem::path m_diskDir;
    size_t                m_diskBudget{0};
    std::unordered_set<DiskKey, DiskKeyHash> m_diskIndex; // guarded by m_mtx
    std::atomic<size_t>   m_diskUsed{0};

    std::thread             m_writer;
    mutable std::mutex      m_writerMtx;
    std::condition_variable m_writerCv;
    std::deque<PendingWrite> m_writeQueue;
    std::atomic<bool>       m_writerRunning{false};
};

} // namespace rt
