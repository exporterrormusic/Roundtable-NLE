/*
 * DiskFrameCache — persistent disk-backed second-level frame cache.
 *
 * Sits below the in-memory FrameCache. When video frames are decoded they
 * are written to disk asynchronously so that subsequent accesses (even
 * after restarting the application) can read from SSD instead of re-decoding.
 *
 * Design:
 *   • Frames are written via a background writer thread (putAsync)
 *   • Frames are read synchronously on cache miss (~2-3 ms on NVMe SSD)
 *   • Files are keyed by a persistent hash of (canonical path + size + mtime)
 *   • In-memory index rebuilt from directory scan at startup
 *   • LRU eviction (oldest mtime) when disk budget is exceeded
 *
 * Binary file format per frame (little-endian):
 *   [4B] magic 0x43465452 ("RTFC")   [4B] version 1
 *   [4B] width   [4B] height   [4B] stride
 *   [1B] tier    [1B] flags    [8B] timestamp (double)
 *   [8B] pixelDataSize          [N B] raw BGRA pixels
 *
 * Thread-safe: look-ups from any thread, writes are serialised by the
 * background writer.
 */

#pragma once

#include "FrameCache.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace rt {

class DiskFrameCache
{
public:
    /// @param cacheDir   Root directory for cached frame files
    /// @param budgetBytes  Maximum disk space to use (default 20 GB)
    // Default budget: 8 GB on disk.  App::init() normally overrides this
    // with a size tuned to the system.  The default still exists so
    // DiskFrameCache is usable without the app's dynamic sizing (tests).
    explicit DiskFrameCache(const std::filesystem::path& cacheDir,
                            size_t budgetBytes = 4ULL * 1024 * 1024 * 1024);
    ~DiskFrameCache();

    DiskFrameCache(const DiskFrameCache&) = delete;
    DiskFrameCache& operator=(const DiskFrameCache&) = delete;

    // ── Media registration ──────────────────────────────────────────────

    /// Register a runtime media handle with its file path for persistent keying.
    /// Must be called before get/put for this handle (typically in MediaPool::open).
    void registerMedia(uint64_t mediaId, const std::filesystem::path& filePath);

    /// Unregister a media handle (on close).  Does NOT remove files from disk.
    void unregisterMedia(uint64_t mediaId);

    // ── Cache operations ────────────────────────────────────────────────

    /// Read a frame from disk.  Returns nullptr if not found.
    /// Synchronous — typically ~2-3 ms on NVMe SSD for a 1080p BGRA frame.
    [[nodiscard]] std::shared_ptr<CachedFrame> get(
        uint64_t mediaId, int64_t frameNumber,
        ResolutionTier tier = ResolutionTier::Full);

    /// Queue a frame for asynchronous write to disk.
    /// Returns immediately — the background writer thread handles I/O.
    void putAsync(std::shared_ptr<CachedFrame> frame);

    /// Check if a frame exists on disk (uses in-memory index, no I/O).
    [[nodiscard]] bool contains(uint64_t mediaId, int64_t frameNumber,
                                ResolutionTier tier) const;

    /// Remove all cached frames for a media file from disk.
    void evictMedia(uint64_t mediaId);

    // ── Configuration ───────────────────────────────────────────────────

    void setCacheDirectory(const std::filesystem::path& dir);
    void setBudget(size_t budgetBytes);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] size_t diskUsed()   const noexcept { return m_diskUsed.load(); }
    [[nodiscard]] size_t entryCount() const noexcept { return m_entryCount.load(); }
    [[nodiscard]] size_t budget()     const noexcept { return m_budget; }

    /// Pending entries in the write-behind queue.  Each pending entry
    /// pins a shared_ptr<CachedFrame> whose lazyReadback captures the
    /// source GPU texture — so this counter is also "GPU textures
    /// pinned by the disk writer".  Plateaus at kMaxWriteQueue under
    /// sustained pressure (queue starts dropping oldest from there).
    [[nodiscard]] size_t writeQueueSize() const
    {
        std::lock_guard lock(m_writerMutex);
        return m_writeQueue.size();
    }

private:
    // ── File format constants ───────────────────────────────────────────
    static constexpr uint32_t kMagic   = 0x43465452; // "RTFC" little-endian
    static constexpr uint32_t kVersion = 1;

    // ── Disk key (for in-memory index) ──────────────────────────────────
    struct DiskKey
    {
        std::string pathHash;
        int64_t     frameNumber;
        uint8_t     tier;

        bool operator==(const DiskKey& o) const noexcept
        {
            return pathHash == o.pathHash &&
                   frameNumber == o.frameNumber &&
                   tier == o.tier;
        }
    };

    struct DiskKeyHash
    {
        size_t operator()(const DiskKey& k) const noexcept
        {
            size_t h = std::hash<std::string>{}(k.pathHash);
            h ^= std::hash<int64_t>{}(k.frameNumber) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint8_t>{}(k.tier)        + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // ── Helpers ─────────────────────────────────────────────────────────
    std::string computePathHash(const std::filesystem::path& filePath) const;
    std::string getPathHash(uint64_t mediaId) const;
    std::filesystem::path framePath(const std::string& hash,
                                    int64_t frameNumber,
                                    ResolutionTier tier) const;

    bool writeFrame(const std::filesystem::path& path,
                    const CachedFrame& frame) const;
    std::shared_ptr<CachedFrame> readFrame(
        const std::filesystem::path& path,
        uint64_t mediaId) const;

    void writerThread();
    void enforceBudget();
    void scanExistingCache(); // acquires m_mutex internally

    // ── State ───────────────────────────────────────────────────────────
    std::filesystem::path m_cacheDir;
    size_t                m_budget;

    mutable std::mutex m_mutex; // guards m_mediaToHash, m_diskIndex
    std::unordered_map<uint64_t, std::string>      m_mediaToHash; // runtime id → hash
    std::unordered_set<DiskKey, DiskKeyHash>        m_diskIndex;   // what's on disk

    // Write-behind thread
    std::thread              m_writer;
    mutable std::mutex       m_writerMutex;
    std::condition_variable  m_writerCv;
    std::deque<std::shared_ptr<CachedFrame>> m_writeQueue;
    std::atomic<bool>        m_running{true};

    // Hard ceiling on the write queue (UPGRADE_PLAN 2026-05-22 v4).
    //
    // GPU-resident CachedFrames carry a lazyReadback closure that
    // captures shared_ptr<Texture> for the source GPU texture.  While a
    // frame sits in this queue waiting to be written, that capture
    // keeps the texture alive in VRAM — bypassing the ownership
    // transfer in CompositeServiceLayerBuild and the eviction in
    // GpuTextureCache.  Each queued frame pins ~8 MB of VRAM.
    //
    // Without this cap, the writer thread (single, serialised on the
    // compute queue against prefetch via the readbackMutex) falls
    // behind ~30 fps decode by even 10 ms per frame, the queue grows
    // unboundedly, and VRAM climbs ~240 MB/sec until the OS budget is
    // hit and the driver pages textures — which is the "vmaAllocs +30
    // per second" growth observed at 22:28:39 onward in the
    // perf log (each prefetched frame's source texture leaked into
    // VRAM via the writer queue's shared_ptr ref chain).
    //
    // When the queue exceeds this cap, the OLDEST entry is dropped.
    // Premiere's media cache works the same way — old frames that
    // didn't make it to disk are simply re-decoded on demand if the
    // user scrubs back, which is cheap compared to VRAM exhaustion.
    static constexpr size_t kMaxWriteQueue = 30;

    // Statistics
    std::atomic<size_t> m_diskUsed{0};
    std::atomic<size_t> m_entryCount{0};
};

} // namespace rt
