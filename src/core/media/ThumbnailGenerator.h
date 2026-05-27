/*
 * ThumbnailGenerator — Background thumbnail generation for the Project Bin.
 *
 * Generates small preview images (thumbnails) from media files.
 * Uses a thread pool so thumbnail generation never blocks the UI.
 *
 * Supports:
 *   - Video files: extract a frame near the midpoint, scale down
 *   - Image files: load and scale down
 *   - Audio files: generate a mini-waveform image
 *   - Spine characters: placeholder icon
 *
 * Thumbnail storage:
 *   - In-memory LRU cache for fast retrieval
 *   - On-disk cache (JPEG) for persistence across sessions
 *
 * Thread model:
 *   - Requests are queued and processed by a configurable thread pool
 *   - Completion callbacks are invoked on the requesting thread (if Qt, via signal)
 *   - All public methods are thread-safe
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <queue>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace rt {

class MediaPool;

/// Type of media for thumbnail generation strategy
enum class MediaType : uint8_t
{
    Video,
    Image,
    Audio,
    Spine,
    Unknown
};

/// Result of a thumbnail generation request
struct Thumbnail
{
    uint32_t              width{0};
    uint32_t              height{0};
    uint32_t              stride{0};     ///< Bytes per row
    std::vector<uint8_t>  pixels;        ///< BGRA pixel data
    std::filesystem::path sourcePath;    ///< Original source file
    MediaType             type{MediaType::Unknown};
    bool                  valid{false};  ///< True if generation succeeded

    [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
    [[nodiscard]] size_t memoryUsage() const noexcept
    {
        return pixels.size() + sizeof(*this);
    }
};

/// Unique key for a thumbnail in the cache
struct ThumbnailKey
{
    std::string path;       ///< Canonical path string
    uint32_t    maxWidth;   ///< Requested max width

    bool operator==(const ThumbnailKey& o) const noexcept
    {
        return path == o.path && maxWidth == o.maxWidth;
    }
};

struct ThumbnailKeyHash
{
    size_t operator()(const ThumbnailKey& k) const noexcept
    {
        size_t h = std::hash<std::string>{}(k.path);
        h ^= std::hash<uint32_t>{}(k.maxWidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

/// Callback signature for async thumbnail completion
using ThumbnailCallback = std::function<void(const std::filesystem::path& path,
                                              std::shared_ptr<Thumbnail> thumb)>;

class ThumbnailGenerator
{
public:
    /// Create a generator with specified thread count and thumbnail size.
    explicit ThumbnailGenerator(size_t threadCount = 2,
                                 uint32_t defaultWidth = 160,
                                 uint32_t defaultHeight = 120);
    ~ThumbnailGenerator();

    // Non-copyable
    ThumbnailGenerator(const ThumbnailGenerator&) = delete;
    ThumbnailGenerator& operator=(const ThumbnailGenerator&) = delete;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the default thumbnail dimensions.
    void setDefaultSize(uint32_t width, uint32_t height) noexcept;
    [[nodiscard]] uint32_t defaultWidth()  const noexcept { return m_defaultWidth; }
    [[nodiscard]] uint32_t defaultHeight() const noexcept { return m_defaultHeight; }

    /// Set an optional MediaPool for video decoding.
    void setMediaPool(MediaPool* pool) noexcept;

    /// Set disk cache directory. If not set, disk caching is disabled.
    void setCacheDirectory(const std::filesystem::path& dir);
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept { return m_cacheDir; }

    // ── Request thumbnails ──────────────────────────────────────────────

    /// Request a thumbnail asynchronously. Calls `callback` when ready.
    /// If the thumbnail is already cached, callback may be called synchronously.
    void requestThumbnail(const std::filesystem::path& filePath,
                          ThumbnailCallback callback,
                          uint32_t maxWidth = 0);

    /// Request thumbnails for multiple files at once.
    void requestBatch(const std::vector<std::filesystem::path>& files,
                      ThumbnailCallback callback,
                      uint32_t maxWidth = 0);

    /// Generate a thumbnail synchronously (blocks until done).
    [[nodiscard]] std::shared_ptr<Thumbnail> generateSync(
        const std::filesystem::path& filePath,
        uint32_t maxWidth = 0);

    // ── Cache management ────────────────────────────────────────────────

    /// Check if a thumbnail is cached in memory.
    [[nodiscard]] bool isCached(const std::filesystem::path& filePath,
                                uint32_t maxWidth = 0) const;

    /// Get a cached thumbnail (nullptr if not cached).
    [[nodiscard]] std::shared_ptr<Thumbnail> getCached(
        const std::filesystem::path& filePath,
        uint32_t maxWidth = 0) const;

    /// Clear the in-memory cache.
    void clearCache();

    /// Evict all cache entries for a specific file path.
    /// Call this when a file's content changes on disk so the next
    /// requestThumbnail() call regenerates from the new file bytes.
    void evictPath(const std::filesystem::path& filePath);

    /// Clear both in-memory and disk caches.
    void clearAllCaches();

    /// Number of cached thumbnails.
    [[nodiscard]] size_t cacheCount() const;

    /// Total memory used by cached thumbnails.
    [[nodiscard]] size_t cacheMemoryUsed() const;

    // ── Pending requests ────────────────────────────────────────────────

    /// Number of pending (queued) thumbnail requests.
    [[nodiscard]] size_t pendingCount() const;

    /// Cancel all pending requests.
    void cancelAll();

    // ── Utilities ───────────────────────────────────────────────────────

    /// Detect media type from file extension.
    [[nodiscard]] static MediaType detectMediaType(const std::filesystem::path& path);

    /// Supported video extensions.
    [[nodiscard]] static bool isVideoExtension(const std::string& ext);

    /// Supported image extensions.
    [[nodiscard]] static bool isImageExtension(const std::string& ext);

    /// Supported audio extensions.
    [[nodiscard]] static bool isAudioExtension(const std::string& ext);

private:
    struct Request
    {
        std::filesystem::path path;
        ThumbnailCallback     callback;
        uint32_t              maxWidth;
    };

    void workerLoop();
    std::shared_ptr<Thumbnail> generateThumbnail(const std::filesystem::path& path,
                                                   uint32_t maxWidth);
    std::shared_ptr<Thumbnail> generateVideoThumbnail(const std::filesystem::path& path,
                                                       uint32_t maxWidth);
    std::shared_ptr<Thumbnail> generateImageThumbnail(const std::filesystem::path& path,
                                                       uint32_t maxWidth);
    std::shared_ptr<Thumbnail> generateAudioThumbnail(const std::filesystem::path& path,
                                                       uint32_t maxWidth);
    std::shared_ptr<Thumbnail> generatePlaceholder(MediaType type,
                                                     uint32_t maxWidth);

    void cachePut(const ThumbnailKey& key, std::shared_ptr<Thumbnail> thumb);

    [[nodiscard]] std::string resolveCanonicalPath(const std::filesystem::path& filePath) const;

    // Thread pool
    std::vector<std::thread>  m_threads;
    std::queue<Request>       m_queue;
    mutable std::mutex        m_queueMutex;
    std::condition_variable   m_queueCV;
    std::atomic<bool>         m_shutdown{false};

    // In-memory cache
    mutable std::mutex m_cacheMutex;
    std::unordered_map<ThumbnailKey, std::shared_ptr<Thumbnail>, ThumbnailKeyHash> m_cache;

    // Configuration
    uint32_t              m_defaultWidth{160};
    uint32_t              m_defaultHeight{120};
    MediaPool*            m_pool{nullptr};
    std::filesystem::path m_cacheDir;
};

} // namespace rt
