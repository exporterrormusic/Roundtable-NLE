/*
 * DiskFrameCache.cpp — persistent disk-backed frame cache implementation
 */

#include "DiskFrameCache.h"
#include "PathUtils.h"

#include <algorithm>
#include <fstream>
#include <spdlog/spdlog.h>

namespace rt {

namespace {

/// FNV-1a 64-bit hash
uint64_t fnv1a(const void* data, size_t len)
{
    auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string toHex(uint64_t v)
{
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

// ─── Construction ───────────────────────────────────────────────────────────

DiskFrameCache::DiskFrameCache(const std::filesystem::path& cacheDir,
                               size_t budgetBytes)
    : m_cacheDir(cacheDir)
    , m_budget(budgetBytes)
{
    std::error_code ec;
    std::filesystem::create_directories(m_cacheDir, ec);

    scanExistingCache();

    m_writer = std::thread([this]() { writerThread(); });

    spdlog::info("DiskFrameCache: dir='{}', budget={:.1f} GB, existing={} entries ({:.1f} MB)",
                 pathToUtf8(m_cacheDir),
                 m_budget / (1024.0 * 1024.0 * 1024.0),
                 m_entryCount.load(),
                 m_diskUsed.load() / (1024.0 * 1024.0));
}

DiskFrameCache::~DiskFrameCache()
{
    m_running.store(false);
    m_writerCv.notify_all();
    if (m_writer.joinable())
        m_writer.join();
}

// ─── Media registration ─────────────────────────────────────────────────────

void DiskFrameCache::registerMedia(uint64_t mediaId,
                                   const std::filesystem::path& filePath)
{
    auto hash = computePathHash(filePath);
    std::lock_guard lock(m_mutex);
    m_mediaToHash[mediaId] = hash;
}

void DiskFrameCache::unregisterMedia(uint64_t mediaId)
{
    std::lock_guard lock(m_mutex);
    m_mediaToHash.erase(mediaId);
}

// ─── Persistent key computation ─────────────────────────────────────────────

std::string DiskFrameCache::computePathHash(const std::filesystem::path& filePath) const
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    std::string pathStr = ec ? pathToUtf8(filePath) : pathToUtf8(canonical);

    auto fileSize = std::filesystem::file_size(filePath, ec);
    auto mtime    = std::filesystem::last_write_time(filePath, ec);
    auto mtimeVal = mtime.time_since_epoch().count();

    // Combine: path + size + mtime via FNV-1a with bit mixing
    uint64_t h = fnv1a(pathStr.data(), pathStr.size());
    h ^= fnv1a(&fileSize, sizeof(fileSize)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= fnv1a(&mtimeVal, sizeof(mtimeVal)) + 0x9e3779b9 + (h << 6) + (h >> 2);

    return toHex(h);
}

std::string DiskFrameCache::getPathHash(uint64_t mediaId) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_mediaToHash.find(mediaId);
    return it != m_mediaToHash.end() ? it->second : std::string{};
}

// ─── File path computation ──────────────────────────────────────────────────

std::filesystem::path DiskFrameCache::framePath(const std::string& hash,
                                                int64_t frameNumber,
                                                ResolutionTier tier) const
{
    // Layout: <cacheDir>/frames/<hash>/<frameNumber>_<tier>.bin
    char name[64];
    snprintf(name, sizeof(name), "%lld_%d.bin",
             static_cast<long long>(frameNumber),
             static_cast<int>(tier));
    return m_cacheDir / "frames" / hash / name;
}

// ─── Read ───────────────────────────────────────────────────────────────────

std::shared_ptr<CachedFrame> DiskFrameCache::get(
    uint64_t mediaId, int64_t frameNumber, ResolutionTier tier)
{
    auto hash = getPathHash(mediaId);
    if (hash.empty()) return nullptr;

    // Check in-memory index first (no filesystem I/O)
    {
        std::lock_guard lock(m_mutex);
        DiskKey dk{hash, frameNumber, static_cast<uint8_t>(tier)};
        if (m_diskIndex.find(dk) == m_diskIndex.end())
            return nullptr;
    }

    auto path  = framePath(hash, frameNumber, tier);
    auto frame = readFrame(path, mediaId);
    if (frame) {
        frame->frameNumber = frameNumber;
        frame->tier        = tier;
    }
    return frame;
}

bool DiskFrameCache::contains(uint64_t mediaId, int64_t frameNumber,
                              ResolutionTier tier) const
{
    auto hash = getPathHash(mediaId);
    if (hash.empty()) return false;

    std::lock_guard lock(m_mutex);
    DiskKey dk{hash, frameNumber, static_cast<uint8_t>(tier)};
    return m_diskIndex.find(dk) != m_diskIndex.end();
}

// ─── Write (async) ──────────────────────────────────────────────────────────

void DiskFrameCache::putAsync(std::shared_ptr<CachedFrame> frame)
{
    if (!frame) return;
    // UPGRADE_PLAN Phase 7 K.2: GPU-resident frames arrive with empty
    // pixels and a lazyReadback closure that materialises on demand.
    // Skip only when there is truly nothing to write (neither CPU pixels
    // nor a way to get them).  The writer thread will call ensurePixels()
    // on lazy frames before writeFrame() so the synchronous GPU readback
    // happens off the prefetch hot path.
    if (frame->pixels.empty() && !frame->lazyReadback) return;

    auto hash = getPathHash(frame->mediaId);
    if (hash.empty()) return;

    // Skip if already on disk
    {
        std::lock_guard lock(m_mutex);
        DiskKey dk{hash, frame->frameNumber, static_cast<uint8_t>(frame->tier)};
        if (m_diskIndex.count(dk))
            return;
    }

    {
        std::lock_guard lock(m_writerMutex);

        // UPGRADE_PLAN 2026-05-22 v4: cap the queue.  See kMaxWriteQueue
        // in DiskFrameCache.h for the leak that motivates this.  Each
        // queued frame's lazyReadback closure pins a shared_ptr to its
        // source GPU texture; under heavy prefetch the writer can't
        // drain fast enough and VRAM grows ~8 MB per queued frame.
        //
        // When at capacity, drop the OLDEST entry to make room.  The
        // dropped frame's CachedFrame shared_ptr ref count drops here,
        // and (if this was the last ref) its destructor releases the
        // captured texture back to PrefetchTexturePool.  Worst case the
        // disk cache misses on that frame later and we re-decode — a
        // cheap operation compared to OS-budget VRAM exhaustion.
        while (m_writeQueue.size() >= kMaxWriteQueue) {
            m_writeQueue.pop_front();
        }
        m_writeQueue.push_back(std::move(frame));
    }
    m_writerCv.notify_one();
}

// ─── Writer thread ──────────────────────────────────────────────────────────

void DiskFrameCache::writerThread()
{
    while (m_running.load()) {
        std::shared_ptr<CachedFrame> frame;
        {
            std::unique_lock lock(m_writerMutex);
            m_writerCv.wait(lock, [this]() {
                return !m_writeQueue.empty() || !m_running.load();
            });
            if (!m_running.load() && m_writeQueue.empty())
                break;
            if (m_writeQueue.empty())
                continue;
            frame = std::move(m_writeQueue.front());
            m_writeQueue.pop_front();
        }

        if (!frame) continue;

        auto hash = getPathHash(frame->mediaId);
        if (hash.empty()) continue;

        // Double-check that another thread didn't write this frame while queued
        {
            std::lock_guard lock(m_mutex);
            DiskKey dk{hash, frame->frameNumber, static_cast<uint8_t>(frame->tier)};
            if (m_diskIndex.count(dk))
                continue;
        }

        auto path = framePath(hash, frame->frameNumber, frame->tier);

        // Create directory
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) continue;

        // UPGRADE_PLAN Phase 7 K.2: GPU-resident frames defer CPU pixels.
        // Materialise them here (off the prefetch worker's hot path) so
        // writeFrame can serialise the BGRA buffer.  ensurePixels is
        // thread-safe and one-shot — subsequent disk reads see the
        // populated buffer directly.
        if (frame->pixels.empty() && !frame->ensurePixels()) {
            spdlog::warn("DiskFrameCache: lazy readback failed for "
                         "mediaId={} frame={}, skipping disk write",
                         frame->mediaId, frame->frameNumber);
            continue;
        }

        if (writeFrame(path, *frame)) {
            auto bytes = std::filesystem::file_size(path, ec);

            std::lock_guard lock(m_mutex);
            DiskKey dk{hash, frame->frameNumber, static_cast<uint8_t>(frame->tier)};
            m_diskIndex.insert(dk);
            m_diskUsed.fetch_add(ec ? 0 : static_cast<size_t>(bytes));
            m_entryCount.fetch_add(1);
        }

        // Enforce budget periodically
        if (m_diskUsed.load() > m_budget)
            enforceBudget();
    }
}

// ─── File I/O ───────────────────────────────────────────────────────────────

bool DiskFrameCache::writeFrame(const std::filesystem::path& path,
                                const CachedFrame& frame) const
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    file.write(reinterpret_cast<const char*>(&kMagic), 4);
    file.write(reinterpret_cast<const char*>(&kVersion), 4);
    file.write(reinterpret_cast<const char*>(&frame.width), 4);
    file.write(reinterpret_cast<const char*>(&frame.height), 4);
    file.write(reinterpret_cast<const char*>(&frame.stride), 4);

    auto tierVal = static_cast<uint8_t>(frame.tier);
    file.write(reinterpret_cast<const char*>(&tierVal), 1);

    uint8_t flags = 0;
    if (frame.unpackedAlpha)      flags |= 0x01;
    if (frame.premultipliedAlpha) flags |= 0x02;
    if (frame.isKeyframe)         flags |= 0x04;
    file.write(reinterpret_cast<const char*>(&flags), 1);

    file.write(reinterpret_cast<const char*>(&frame.timestamp), 8);

    auto pixelSize = static_cast<uint64_t>(frame.pixels.size());
    file.write(reinterpret_cast<const char*>(&pixelSize), 8);
    file.write(reinterpret_cast<const char*>(frame.pixels.data()),
               static_cast<std::streamsize>(pixelSize));

    return file.good();
}

std::shared_ptr<CachedFrame> DiskFrameCache::readFrame(
    const std::filesystem::path& path, uint64_t mediaId) const
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;

    uint32_t magic{}, version{};
    file.read(reinterpret_cast<char*>(&magic), 4);
    file.read(reinterpret_cast<char*>(&version), 4);

    if (magic != kMagic || version != kVersion)
        return nullptr;

    auto frame = std::make_shared<CachedFrame>();
    frame->mediaId = mediaId;

    file.read(reinterpret_cast<char*>(&frame->width), 4);
    file.read(reinterpret_cast<char*>(&frame->height), 4);
    file.read(reinterpret_cast<char*>(&frame->stride), 4);

    uint8_t tierVal{}, flags{};
    file.read(reinterpret_cast<char*>(&tierVal), 1);
    file.read(reinterpret_cast<char*>(&flags), 1);

    frame->tier               = static_cast<ResolutionTier>(tierVal);
    frame->unpackedAlpha      = (flags & 0x01) != 0;
    frame->premultipliedAlpha = (flags & 0x02) != 0;
    frame->isKeyframe         = (flags & 0x04) != 0;

    file.read(reinterpret_cast<char*>(&frame->timestamp), 8);

    uint64_t pixelSize{};
    file.read(reinterpret_cast<char*>(&pixelSize), 8);

    // Sanity check: prevent absurd allocations from corrupt files.
    // Max sensible size: 8K × 8K × 4 bytes ≈ 256 MB.
    if (pixelSize > 256ULL * 1024 * 1024) {
        spdlog::warn("DiskFrameCache: corrupt frame file '{}' — pixel size {} bytes",
                     pathToUtf8(path), pixelSize);
        return nullptr;
    }

    frame->pixels.resize(static_cast<size_t>(pixelSize));
    file.read(reinterpret_cast<char*>(frame->pixels.data()),
              static_cast<std::streamsize>(pixelSize));

    if (!file) {
        spdlog::warn("DiskFrameCache: truncated frame file '{}'", pathToUtf8(path));
        return nullptr;
    }

    return frame;
}

// ─── Eviction ───────────────────────────────────────────────────────────────

void DiskFrameCache::evictMedia(uint64_t mediaId)
{
    auto hash = getPathHash(mediaId);
    if (hash.empty()) return;

    auto dir = m_cacheDir / "frames" / hash;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::lock_guard lock(m_mutex);
    // Remove from index
    size_t removed = 0;
    auto it = m_diskIndex.begin();
    while (it != m_diskIndex.end()) {
        if (it->pathHash == hash) {
            it = m_diskIndex.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    m_entryCount.store(m_diskIndex.size());
    spdlog::info("DiskFrameCache: evicted {} entries for media {}", removed, mediaId);
}

void DiskFrameCache::enforceBudget()
{
    namespace fs = std::filesystem;

    struct FileEntry {
        fs::path              path;
        uintmax_t             size;
        fs::file_time_type    mtime;
    };

    std::vector<FileEntry> files;
    std::error_code ec;
    auto framesDir = m_cacheDir / "frames";
    if (!fs::exists(framesDir, ec)) return;

    size_t totalSize = 0;
    for (auto& entry : fs::recursive_directory_iterator(framesDir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto sz = entry.file_size(ec);
        auto mt = entry.last_write_time(ec);
        files.push_back({entry.path(), sz, mt});
        totalSize += sz;
    }

    m_diskUsed.store(totalSize);
    if (totalSize <= m_budget) return;

    // Sort by mtime — oldest first (LRU eviction)
    std::sort(files.begin(), files.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.mtime < b.mtime;
              });

    size_t freed  = 0;
    size_t target = totalSize - m_budget + (m_budget / 10); // Free extra 10%

    for (const auto& f : files) {
        if (freed >= target) break;
        std::error_code rmEc;
        fs::remove(f.path, rmEc);
        if (!rmEc) freed += f.size;
    }

    // Clean up empty directories
    for (auto& entry : fs::directory_iterator(framesDir, ec)) {
        if (entry.is_directory()) {
            std::error_code emptyEc;
            if (fs::is_empty(entry.path(), emptyEc))
                fs::remove(entry.path(), emptyEc);
        }
    }

    // Rebuild index
    scanExistingCache();

    spdlog::info("DiskFrameCache: enforced budget — freed {:.1f} MB, now {:.1f}/{:.1f} GB",
                 freed / (1024.0 * 1024.0),
                 m_diskUsed.load() / (1024.0 * 1024.0 * 1024.0),
                 m_budget / (1024.0 * 1024.0 * 1024.0));
}

// ─── Startup scan ───────────────────────────────────────────────────────────

void DiskFrameCache::scanExistingCache()
{
    namespace fs = std::filesystem;

    std::error_code ec;
    auto framesDir = m_cacheDir / "frames";
    if (!fs::exists(framesDir, ec)) return;

    std::unordered_set<DiskKey, DiskKeyHash> newIndex;
    size_t totalSize = 0;
    size_t count     = 0;

    for (auto& hashDir : fs::directory_iterator(framesDir, ec)) {
        if (!hashDir.is_directory()) continue;
        std::string hash = pathToUtf8(hashDir.path().filename());

        for (auto& frameFile : fs::directory_iterator(hashDir.path(), ec)) {
            if (!frameFile.is_regular_file()) continue;
            std::string stem = pathToUtf8(frameFile.path().stem());

            // Parse "<frameNumber>_<tier>"
            auto underscorePos = stem.rfind('_');
            if (underscorePos == std::string::npos) continue;

            int64_t fn   = 0;
            uint8_t tier = 0;
            try {
                fn   = std::stoll(stem.substr(0, underscorePos));
                tier = static_cast<uint8_t>(std::stoi(stem.substr(underscorePos + 1)));
            } catch (...) {
                continue;
            }

            DiskKey dk{hash, fn, tier};
            newIndex.insert(dk);
            totalSize += frameFile.file_size(ec);
            ++count;
        }
    }

    // Swap under lock
    {
        std::lock_guard lock(m_mutex);
        m_diskIndex = std::move(newIndex);
    }
    m_diskUsed.store(totalSize);
    m_entryCount.store(count);
}

// ─── Configuration ──────────────────────────────────────────────────────────

void DiskFrameCache::setCacheDirectory(const std::filesystem::path& dir)
{
    {
        std::lock_guard lock(m_mutex);
        m_cacheDir = dir;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    scanExistingCache();
}

void DiskFrameCache::setBudget(size_t budgetBytes)
{
    m_budget = budgetBytes;
    if (m_diskUsed.load() > m_budget)
        enforceBudget();
}

} // namespace rt
