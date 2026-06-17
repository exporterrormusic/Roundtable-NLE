/*
 * SegmentRenderCache.cpp — see SegmentRenderCache.h for the contract.
 *
 * Two-level store: an in-memory LRU (the hot path) backed by an optional disk
 * cache (enableDiskCache) so pre-rendered segments survive an app restart.
 * Disk keys are (tick, tier, configHash) — content-addressed, so a file is
 * valid in any session whose config hashes equal; an edit changes the hash and
 * thus the filename, so a stale frame can never be served.  Writes go through a
 * background thread (write-behind); reads are synchronous on an in-memory miss.
 */

#include "cache/SegmentRenderCache.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace rt {

namespace {
template <typename T>
void writePod(std::ofstream& os, const T& v)
{
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool readPod(std::ifstream& is, T& v)
{
    return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(T)));
}
} // namespace

SegmentRenderCache::SegmentRenderCache(size_t budgetBytes)
    : m_budgetBytes(budgetBytes)
{
}

SegmentRenderCache::~SegmentRenderCache()
{
    if (m_writerRunning.exchange(false)) {
        m_writerCv.notify_all();
        if (m_writer.joinable()) m_writer.join();
    }
}

// ── In-memory store ─────────────────────────────────────────────────────────

void SegmentRenderCache::dropEntry_locked(Map::iterator it)
{
    m_curBytes -= it->second.bytes;
    m_lru.erase(it->second.lruIt);
    m_entries.erase(it);
}

void SegmentRenderCache::evictToFit_locked()
{
    // Evict least-recently-used (LRU tail) until within budget.  Never evict
    // the very last entry below budget — if a single frame exceeds the whole
    // budget we still keep it (the alternative is never caching at all).
    while (m_curBytes > m_budgetBytes && m_lru.size() > 1) {
        const Key victim = m_lru.back();
        auto it = m_entries.find(victim);
        if (it == m_entries.end()) { m_lru.pop_back(); continue; }  // defensive
        dropEntry_locked(it);
    }
}

void SegmentRenderCache::insertMemory_locked(const Key& key, uint64_t configHash,
                                             std::shared_ptr<CachedFrame> frame)
{
    const size_t bytes = frame->memoryUsage();
    if (auto existing = m_entries.find(key); existing != m_entries.end())
        dropEntry_locked(existing);

    m_lru.push_front(key);
    Entry e;
    e.configHash = configHash;
    e.frame      = std::move(frame);
    e.bytes      = bytes;
    e.lruIt      = m_lru.begin();
    m_entries.emplace(key, std::move(e));
    m_curBytes += bytes;

    evictToFit_locked();
}

void SegmentRenderCache::put(int64_t tick, ResolutionTier tier,
                             uint64_t configHash,
                             std::shared_ptr<CachedFrame> frame)
{
    if (!frame) return;
    const Key key{tick, static_cast<uint8_t>(tier)};
    {
        std::lock_guard lk(m_mtx);
        insertMemory_locked(key, configHash, frame);
    }
    if (m_diskEnabled)
        enqueueDiskWrite(DiskKey{tick, static_cast<uint8_t>(tier), configHash},
                         std::move(frame));
}

std::shared_ptr<CachedFrame> SegmentRenderCache::get(int64_t tick,
                                                     ResolutionTier tier,
                                                     uint64_t configHash)
{
    const Key key{tick, static_cast<uint8_t>(tier)};
    {
        std::lock_guard lk(m_mtx);
        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            if (it->second.configHash == configHash) {
                m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);  // MRU
                return it->second.frame;
            }
            dropEntry_locked(it);   // stale memory entry — fall through to disk
        }
        if (!m_diskEnabled) return nullptr;
        if (m_diskIndex.find(DiskKey{tick, static_cast<uint8_t>(tier), configHash})
                == m_diskIndex.end())
            return nullptr;
    }

    // In-memory miss but present on disk — read it (IO outside the lock) and
    // promote into memory so subsequent hits are fast.
    const DiskKey dk{tick, static_cast<uint8_t>(tier), configHash};
    auto frame = readFrameFromDisk(diskPathFor(dk), dk);

    std::lock_guard lk(m_mtx);
    if (!frame) {
        m_diskIndex.erase(dk);  // corrupt / vanished — forget it
        return nullptr;
    }
    insertMemory_locked(key, configHash, frame);
    return frame;
}

bool SegmentRenderCache::hasFresh(int64_t tick, ResolutionTier tier,
                                  uint64_t configHash) const
{
    const Key key{tick, static_cast<uint8_t>(tier)};
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    if (it != m_entries.end() && it->second.configHash == configHash)
        return true;
    return m_diskEnabled
        && m_diskIndex.find(DiskKey{tick, static_cast<uint8_t>(tier), configHash})
               != m_diskIndex.end();
}

void SegmentRenderCache::clear()
{
    // In-memory only: disk entries persist intentionally (that's the point).
    std::lock_guard lk(m_mtx);
    m_entries.clear();
    m_lru.clear();
    m_curBytes = 0;
}

void SegmentRenderCache::invalidateRange(int64_t fromTick, int64_t toTick)
{
    std::lock_guard lk(m_mtx);
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (it->first.tick >= fromTick && it->first.tick <= toTick) {
            const auto cur = it++;
            dropEntry_locked(cur);
        } else {
            ++it;
        }
    }
}

void SegmentRenderCache::setBudgetBytes(size_t budgetBytes)
{
    std::lock_guard lk(m_mtx);
    m_budgetBytes = budgetBytes;
    evictToFit_locked();
}

size_t SegmentRenderCache::sizeBytes() const
{
    std::lock_guard lk(m_mtx);
    return m_curBytes;
}

size_t SegmentRenderCache::count() const
{
    std::lock_guard lk(m_mtx);
    return m_entries.size();
}

// ── Disk persistence ────────────────────────────────────────────────────────

void SegmentRenderCache::enableDiskCache(const std::filesystem::path& dir,
                                         size_t budgetBytes)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        spdlog::warn("SegmentRenderCache: cannot create disk dir '{}': {}",
                     rt::pathToUtf8(dir), ec.message());
        return;
    }
    {
        std::lock_guard lk(m_mtx);
        m_diskDir     = dir;
        m_diskBudget  = budgetBytes;
        m_diskEnabled = true;
    }
    scanDiskCache();
    if (!m_writerRunning.exchange(true))
        m_writer = std::thread(&SegmentRenderCache::diskWriterLoop, this);

    spdlog::info("SegmentRenderCache: disk cache '{}' ({} MB), {} existing frame(s)",
                 rt::pathToUtf8(dir), budgetBytes / (1024 * 1024), m_diskIndex.size());
}

std::filesystem::path SegmentRenderCache::diskPathFor(const DiskKey& k) const
{
    char name[80];
    std::snprintf(name, sizeof(name), "seg_%u_%lld_%016llx.rtsc",
                  static_cast<unsigned>(k.tier),
                  static_cast<long long>(k.tick),
                  static_cast<unsigned long long>(k.configHash));
    return m_diskDir / name;
}

bool SegmentRenderCache::parseDiskName(const std::string& filename, DiskKey& out)
{
    // Format: "seg_<tier>_<tick>_<hash16hex>.rtsc"
    constexpr std::string_view kPrefix = "seg_";
    constexpr std::string_view kSuffix = ".rtsc";
    if (filename.size() <= kPrefix.size() + kSuffix.size()) return false;
    if (filename.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        return false;

    const char* p   = filename.data() + kPrefix.size();
    const char* end = filename.data() + filename.size() - kSuffix.size();

    auto nextSep = [&](const char* from) -> const char* {
        for (const char* c = from; c < end; ++c) if (*c == '_') return c;
        return nullptr;
    };
    const char* sep1 = nextSep(p);
    if (!sep1) return false;
    const char* sep2 = nextSep(sep1 + 1);
    if (!sep2) return false;

    unsigned tier = 0; int64_t tick = 0; uint64_t hash = 0;
    if (std::from_chars(p, sep1, tier).ec != std::errc{}) return false;
    if (std::from_chars(sep1 + 1, sep2, tick).ec != std::errc{}) return false;
    if (std::from_chars(sep2 + 1, end, hash, 16).ec != std::errc{}) return false;

    out.tier       = static_cast<uint8_t>(tier);
    out.tick       = tick;
    out.configHash = hash;
    return true;
}

bool SegmentRenderCache::writeFrameToDisk(const std::filesystem::path& path,
                                          const DiskKey& k,
                                          const CachedFrame& frame) const
{
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;

    const uint32_t width  = frame.width;
    const uint32_t height = frame.height;
    const uint32_t stride = frame.stride ? frame.stride : frame.width * 4;
    const uint8_t  flags  = static_cast<uint8_t>((frame.unpackedAlpha ? 1 : 0)
                                               | (frame.premultipliedAlpha ? 2 : 0));
    const uint64_t sz     = frame.pixels.size();

    writePod(os, kDiskMagic);
    writePod(os, kDiskVersion);
    writePod(os, k.tick);
    writePod(os, k.tier);
    writePod(os, k.configHash);
    writePod(os, width);
    writePod(os, height);
    writePod(os, stride);
    writePod(os, flags);
    writePod(os, sz);
    os.write(reinterpret_cast<const char*>(frame.pixels.data()),
             static_cast<std::streamsize>(sz));
    return static_cast<bool>(os);
}

std::shared_ptr<CachedFrame> SegmentRenderCache::readFrameFromDisk(
    const std::filesystem::path& path, const DiskKey& k) const
{
    std::ifstream is(path, std::ios::binary);
    if (!is) return nullptr;

    uint32_t magic = 0, version = 0;
    if (!readPod(is, magic) || magic != kDiskMagic) return nullptr;
    if (!readPod(is, version) || version != kDiskVersion) return nullptr;

    int64_t  tick = 0; uint8_t tier = 0; uint64_t hash = 0;
    uint32_t width = 0, height = 0, stride = 0; uint8_t flags = 0; uint64_t sz = 0;
    if (!readPod(is, tick) || !readPod(is, tier) || !readPod(is, hash)) return nullptr;
    if (!readPod(is, width) || !readPod(is, height) || !readPod(is, stride)) return nullptr;
    if (!readPod(is, flags) || !readPod(is, sz)) return nullptr;

    // Defensive: the file must describe exactly the key we asked for, and a
    // sane pixel count (guards against a truncated/corrupt or colliding file).
    if (tick != k.tick || tier != k.tier || hash != k.configHash) return nullptr;
    if (sz == 0 || sz > static_cast<uint64_t>(width) * height * 4 * 2) return nullptr;

    auto frame = std::make_shared<CachedFrame>();
    frame->width              = width;
    frame->height             = height;
    frame->stride             = stride;
    frame->tier               = static_cast<ResolutionTier>(tier);
    frame->unpackedAlpha      = (flags & 1) != 0;
    frame->premultipliedAlpha = (flags & 2) != 0;
    frame->pixels.resize(static_cast<size_t>(sz));
    if (!is.read(reinterpret_cast<char*>(frame->pixels.data()),
                 static_cast<std::streamsize>(sz)))
        return nullptr;
    return frame;
}

void SegmentRenderCache::enqueueDiskWrite(const DiskKey& k,
                                          std::shared_ptr<CachedFrame> frame)
{
    if (!frame) return;
    {
        std::lock_guard lk(m_mtx);
        if (m_diskIndex.find(k) != m_diskIndex.end()) return;  // already persisted
    }
    std::lock_guard wl(m_writerMtx);
    // Bound pinned RAM: drop the oldest queued write under sustained pressure
    // (it'll just be re-rendered on demand, like Premiere's media cache).
    while (m_writeQueue.size() >= kMaxWriteQueue) m_writeQueue.pop_front();
    m_writeQueue.push_back(PendingWrite{k, std::move(frame)});
    m_writerCv.notify_one();
}

void SegmentRenderCache::diskWriterLoop()
{
    while (true) {
        PendingWrite job;
        {
            std::unique_lock wl(m_writerMtx);
            m_writerCv.wait(wl, [this] {
                return !m_writerRunning.load() || !m_writeQueue.empty();
            });
            if (!m_writerRunning.load() && m_writeQueue.empty()) return;
            if (m_writeQueue.empty()) continue;
            job = std::move(m_writeQueue.front());
            m_writeQueue.pop_front();
        }
        if (!job.frame) continue;

        const auto path = diskPathFor(job.key);
        if (!writeFrameToDisk(path, job.key, *job.frame)) {
            spdlog::debug("SegmentRenderCache: disk write failed for {}",
                          rt::pathToUtf8(path));
            continue;
        }
        std::error_code ec;
        const auto fsz = std::filesystem::file_size(path, ec);
        {
            std::lock_guard lk(m_mtx);
            m_diskIndex.insert(job.key);
        }
        if (!ec) m_diskUsed.fetch_add(static_cast<size_t>(fsz));
        enforceDiskBudget();
    }
}

void SegmentRenderCache::scanDiskCache()
{
    std::error_code ec;
    size_t used = 0;
    std::lock_guard lk(m_mtx);
    m_diskIndex.clear();
    for (std::filesystem::directory_iterator it(m_diskDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".rtsc") continue;
        DiskKey k;
        if (!parseDiskName(it->path().filename().string(), k)) continue;
        m_diskIndex.insert(k);
        std::error_code sec;
        used += static_cast<size_t>(it->file_size(sec));
    }
    m_diskUsed.store(used);
}

void SegmentRenderCache::enforceDiskBudget()
{
    if (m_diskUsed.load() <= m_diskBudget) return;

    struct E { std::filesystem::path path; DiskKey key; uintmax_t size;
               std::filesystem::file_time_type mtime; };
    std::vector<E> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_diskDir, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".rtsc") continue;
        DiskKey k;
        if (!parseDiskName(it->path().filename().string(), k)) continue;
        std::error_code sec, tec;
        files.push_back({it->path(), k, it->file_size(sec),
                         std::filesystem::last_write_time(it->path(), tec)});
    }
    std::sort(files.begin(), files.end(),
              [](const E& a, const E& b) { return a.mtime < b.mtime; });  // oldest first

    size_t used = m_diskUsed.load();
    for (const auto& f : files) {
        if (used <= m_diskBudget) break;
        std::error_code rec;
        if (std::filesystem::remove(f.path, rec)) {
            used -= std::min<size_t>(used, static_cast<size_t>(f.size));
            std::lock_guard lk(m_mtx);
            m_diskIndex.erase(f.key);
        }
    }
    m_diskUsed.store(used);
}

} // namespace rt
