/*
 * ThumbnailGenerator.cpp — Background thumbnail generation implementation.
 */

#include "media/ThumbnailGenerator.h"
#include "media/MediaPool.h"

#include <spdlog/spdlog.h>

// Private stbi implementation for ThumbnailGenerator (all formats, file-local)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction / Destruction
// ═════════════════════════════════════════════════════════════════════════════

ThumbnailGenerator::ThumbnailGenerator(size_t threadCount,
                                       uint32_t defaultWidth,
                                       uint32_t defaultHeight)
    : m_defaultWidth(defaultWidth)
    , m_defaultHeight(defaultHeight)
{
    // Start worker threads
    size_t count = std::max(size_t(1), threadCount);
    m_threads.reserve(count);
    for (size_t i = 0; i < count; ++i)
        m_threads.emplace_back(&ThumbnailGenerator::workerLoop, this);

    spdlog::debug("ThumbnailGenerator: started {} worker threads", count);
}

ThumbnailGenerator::~ThumbnailGenerator()
{
    // Signal shutdown
    {
        std::lock_guard lock(m_queueMutex);
        m_shutdown = true;
    }
    m_queueCV.notify_all();

    // Join all threads
    for (auto& t : m_threads)
    {
        if (t.joinable())
            t.join();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Configuration
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGenerator::setDefaultSize(uint32_t width, uint32_t height) noexcept
{
    m_defaultWidth  = width > 0 ? width : 160;
    m_defaultHeight = height > 0 ? height : 120;
}

void ThumbnailGenerator::setMediaPool(MediaPool* pool) noexcept
{
    m_pool = pool;
}

void ThumbnailGenerator::setCacheDirectory(const std::filesystem::path& dir)
{
    m_cacheDir = dir;
    if (!dir.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Request thumbnails
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGenerator::requestThumbnail(const std::filesystem::path& filePath,
                                           ThumbnailCallback callback,
                                           uint32_t maxWidth)
{
    if (maxWidth == 0) maxWidth = m_defaultWidth;

    // Check cache first
    auto cached = getCached(filePath, maxWidth);
    if (cached)
    {
        if (callback) callback(filePath, cached);
        return;
    }

    // Queue the request
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push({filePath, callback, maxWidth});
    }
    m_queueCV.notify_one();
}

void ThumbnailGenerator::requestBatch(const std::vector<std::filesystem::path>& files,
                                       ThumbnailCallback callback,
                                       uint32_t maxWidth)
{
    for (const auto& f : files)
        requestThumbnail(f, callback, maxWidth);
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::generateSync(
    const std::filesystem::path& filePath, uint32_t maxWidth)
{
    if (maxWidth == 0) maxWidth = m_defaultWidth;

    // Check cache
    auto cached = getCached(filePath, maxWidth);
    if (cached) return cached;

    // Generate
    auto thumb = generateThumbnail(filePath, maxWidth);

    // Stamp with file mtime so the cache can detect stale entries later
    if (thumb)
    {
        std::error_code ec;
        thumb->mtime = std::filesystem::last_write_time(filePath, ec);
    }

    // Cache it
    if (thumb && thumb->valid)
    {
        std::string pathStr = resolveCanonicalPath(filePath);
        cachePut({pathStr, maxWidth}, thumb);
    }

    return thumb;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Cache management
// ═════════════════════════════════════════════════════════════════════════════

bool ThumbnailGenerator::isCached(const std::filesystem::path& filePath,
                                   uint32_t maxWidth) const
{
    if (maxWidth == 0) maxWidth = m_defaultWidth;

    std::string pathStr = resolveCanonicalPath(filePath);

    // Read current file mtime for staleness check
    std::filesystem::file_time_type currentMtime{};
    {
        std::error_code ec;
        currentMtime = std::filesystem::last_write_time(filePath, ec);
        if (ec) currentMtime = std::filesystem::file_time_type{};
    }

    std::lock_guard lock(m_cacheMutex);
    auto it = m_cache.find({pathStr, maxWidth});
    if (it != m_cache.end())
    {
        // File was overwritten since we cached it — treat as not cached
        if (currentMtime != std::filesystem::file_time_type{} &&
            currentMtime != it->second->mtime)
        {
            m_cache.erase(it);
            return false;
        }
        return true;
    }
    return false;
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::getCached(
    const std::filesystem::path& filePath, uint32_t maxWidth) const
{
    if (maxWidth == 0) maxWidth = m_defaultWidth;

    std::string pathStr = resolveCanonicalPath(filePath);

    // Read current file mtime for staleness check
    std::filesystem::file_time_type currentMtime{};
    {
        std::error_code ec;
        currentMtime = std::filesystem::last_write_time(filePath, ec);
        if (ec) currentMtime = std::filesystem::file_time_type{};
    }

    std::lock_guard lock(m_cacheMutex);
    auto it = m_cache.find({pathStr, maxWidth});
    if (it != m_cache.end())
    {
        // If the file's mtime on disk differs from when we cached it,
        // the file was overwritten — evict and pretend we don't have it.
        if (currentMtime != std::filesystem::file_time_type{} &&
            currentMtime != it->second->mtime)
        {
            m_cache.erase(it);
            return nullptr;
        }
        return it->second;
    }
    return nullptr;
}

void ThumbnailGenerator::clearCache()
{
    std::lock_guard lock(m_cacheMutex);
    m_cache.clear();
}

void ThumbnailGenerator::evictPath(const std::filesystem::path& filePath)
{
    std::string pathStr = resolveCanonicalPath(filePath);
    std::lock_guard lock(m_cacheMutex);
    // Erase all cache entries whose key path matches (handles multiple
    // maxWidth variants — 160px icon, 400px scrub preview, etc.).
    for (auto it = m_cache.begin(); it != m_cache.end(); ) {
        if (it->first.path == pathStr)
            it = m_cache.erase(it);
        else
            ++it;
    }
}

void ThumbnailGenerator::clearAllCaches()
{
    clearCache();

    // Also clear disk cache
    if (!m_cacheDir.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(m_cacheDir, ec);
        std::filesystem::create_directories(m_cacheDir, ec);
    }
}

size_t ThumbnailGenerator::cacheCount() const
{
    std::lock_guard lock(m_cacheMutex);
    return m_cache.size();
}

size_t ThumbnailGenerator::cacheMemoryUsed() const
{
    std::lock_guard lock(m_cacheMutex);
    size_t total = 0;
    for (const auto& [key, thumb] : m_cache)
        total += thumb->memoryUsage();
    return total;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Pending requests
// ═════════════════════════════════════════════════════════════════════════════

size_t ThumbnailGenerator::pendingCount() const
{
    std::lock_guard lock(m_queueMutex);
    return m_queue.size();
}

void ThumbnailGenerator::cancelAll()
{
    std::lock_guard lock(m_queueMutex);
    std::queue<Request>().swap(m_queue);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Media type detection
// ═════════════════════════════════════════════════════════════════════════════

MediaType ThumbnailGenerator::detectMediaType(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    // Lowercase the extension
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (isVideoExtension(ext)) return MediaType::Video;
    if (isImageExtension(ext)) return MediaType::Image;
    if (isAudioExtension(ext)) return MediaType::Audio;

    // Check for Spine files
    if (ext == ".skel" || ext == ".json" || ext == ".atlas")
        return MediaType::Spine;

    return MediaType::Unknown;
}

bool ThumbnailGenerator::isVideoExtension(const std::string& ext)
{
    static const char* exts[] = {
        ".mp4", ".mkv", ".avi", ".mov", ".webm", ".wmv", ".flv",
        ".m4v", ".ts", ".mts", ".m2ts", ".mpg", ".mpeg", ".ogv",
        ".mxf"   // ProRes/DNxHD masters (e.g. Wells ProRes 4444)
    };
    for (const char* e : exts)
        if (ext == e) return true;
    return false;
}

bool ThumbnailGenerator::isImageExtension(const std::string& ext)
{
    static const char* exts[] = {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".webp",
        ".tiff", ".tif", ".psd", ".exr", ".hdr"
    };
    for (const char* e : exts)
        if (ext == e) return true;
    return false;
}

bool ThumbnailGenerator::isAudioExtension(const std::string& ext)
{
    static const char* exts[] = {
        ".wav", ".mp3", ".flac", ".ogg", ".aac", ".wma", ".m4a",
        ".aiff", ".aif", ".opus"
    };
    for (const char* e : exts)
        if (ext == e) return true;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Worker thread
// ═════════════════════════════════════════════════════════════════════════════

void ThumbnailGenerator::workerLoop()
{
    while (true)
    {
        Request req;

        // Wait for work
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCV.wait(lock, [this]() {
                return m_shutdown || !m_queue.empty();
            });

            if (m_shutdown && m_queue.empty())
                return;

            req = std::move(m_queue.front());
            m_queue.pop();
        }

        // Generate the thumbnail
        auto thumb = generateThumbnail(req.path, req.maxWidth);

        // Stamp with file mtime so the cache can detect stale entries later
        if (thumb)
        {
            std::error_code ec;
            thumb->mtime = std::filesystem::last_write_time(req.path, ec);
        }

        // Cache it (even if invalid, to avoid repeated generation attempts)
        if (thumb)
        {
            std::string pathStr = resolveCanonicalPath(req.path);
            cachePut({pathStr, req.maxWidth}, thumb);
        }

        // Invoke callback
        if (req.callback)
        {
            try
            {
                req.callback(req.path, thumb);
            }
            catch (const std::exception& e)
            {
                spdlog::error("ThumbnailGenerator: callback exception: {}", e.what());
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Thumbnail generation
// ═════════════════════════════════════════════════════════════════════════════

std::shared_ptr<Thumbnail> ThumbnailGenerator::generateThumbnail(
    const std::filesystem::path& path, uint32_t maxWidth)
{
    MediaType type = detectMediaType(path);

    switch (type)
    {
    case MediaType::Video:
        return generateVideoThumbnail(path, maxWidth);
    case MediaType::Image:
        return generateImageThumbnail(path, maxWidth);
    case MediaType::Audio:
        return generateAudioThumbnail(path, maxWidth);
    case MediaType::Spine:
    case MediaType::Unknown:
    default:
        return generatePlaceholder(type, maxWidth);
    }
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::generateVideoThumbnail(
    const std::filesystem::path& path, uint32_t maxWidth)
{
    // Try to use MediaPool if available for shared decoder access
    if (m_pool)
    {
        auto handle = m_pool->open(path);
        if (handle != InvalidMedia)
        {
            const auto* info = m_pool->getInfo(handle);
            int64_t midFrame = info ? info->frameCount / 2 : 0;
            auto frame = m_pool->getFrame(handle, midFrame, ResolutionTier::Quarter);
            m_pool->release(handle);

            if (frame && frame->ensurePixels())
            {
                auto thumb = std::make_shared<Thumbnail>();
                thumb->width      = frame->width;
                thumb->height     = frame->height;
                thumb->stride     = frame->stride;
                thumb->pixels     = frame->pixels;
                thumb->sourcePath = path;
                thumb->type       = MediaType::Video;
                thumb->valid      = true;
                return thumb;
            }
        }
    }

    // Fallback: generate placeholder
    return generatePlaceholder(MediaType::Video, maxWidth);
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::generateImageThumbnail(
    const std::filesystem::path& path, uint32_t maxWidth)
{
    uint32_t targetW = (maxWidth > 0) ? maxWidth : m_defaultWidth;

    // Resolve the file path — try direct first, then search asset directories
    // (mirrors MediaPool::open() fallback for relative/filename-only paths).
    namespace fs = std::filesystem;
    fs::path resolvedPath = path;
    {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            fs::path filename = path.filename();
            const fs::path searchDirs[] = {
                fs::path("assets") / "backgrounds",
                fs::path("assets") / "characters",
                fs::path("assets") / "videos",
                fs::path("assets"),
            };
            for (const auto& dir : searchDirs) {
                fs::path candidate = dir / filename;
                if (fs::exists(candidate, ec)) {
                    resolvedPath = candidate;
                    break;
                }
            }
        }
    }

    // Read file into memory first (std::ifstream handles Windows wide paths)
    std::ifstream file(resolvedPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        spdlog::warn("ThumbnailGenerator: cannot open image '{}' (resolved: '{}')",
                     path.string(), resolvedPath.string());
        return generatePlaceholder(MediaType::Image, maxWidth);
    }
    auto fileSize = file.tellg();
    if (fileSize <= 0) {
        spdlog::warn("ThumbnailGenerator: empty image file '{}'", path.string());
        return generatePlaceholder(MediaType::Image, maxWidth);
    }
    file.seekg(0);
    std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
    file.close();

    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load_from_memory(
        fileData.data(), static_cast<int>(fileData.size()),
        &w, &h, &channels, 4);

    if (!data || w <= 0 || h <= 0) {
        const char* reason = stbi_failure_reason();
        spdlog::warn("ThumbnailGenerator: stbi decode failed for '{}': {}",
                     path.string(), reason ? reason : "unknown");
        if (data) stbi_image_free(data);
        return generatePlaceholder(MediaType::Image, maxWidth);
    }

    uint32_t targetH = static_cast<uint32_t>(h * (static_cast<double>(targetW) / w));
    if (targetH == 0) targetH = 1;
    uint32_t outW = (static_cast<uint32_t>(w) <= targetW) ? static_cast<uint32_t>(w) : targetW;
    uint32_t outH = (static_cast<uint32_t>(w) <= targetW) ? static_cast<uint32_t>(h) : targetH;

    auto thumb = std::make_shared<Thumbnail>();
    thumb->width  = outW;
    thumb->height = outH;
    thumb->stride = outW * 4;
    thumb->pixels.resize(outW * outH * 4);
    thumb->sourcePath = path;
    thumb->type  = MediaType::Image;
    thumb->valid = true;

    for (uint32_t y = 0; y < outH; ++y) {
        uint32_t srcY = y * static_cast<uint32_t>(h) / outH;
        for (uint32_t x = 0; x < outW; ++x) {
            uint32_t srcX = x * static_cast<uint32_t>(w) / outW;
            const uint8_t* src = data + (srcY * w + srcX) * 4;
            uint8_t* dst = thumb->pixels.data() + (y * outW + x) * 4;
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
        }
    }
    stbi_image_free(data);
    spdlog::info("ThumbnailGenerator: image '{}' loaded OK ({}x{} -> {}x{})",
                path.filename().string(), w, h, outW, outH);
    return thumb;
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::generateAudioThumbnail(
    const std::filesystem::path& /*path*/, uint32_t maxWidth)
{
    return generatePlaceholder(MediaType::Audio, maxWidth);
}

std::shared_ptr<Thumbnail> ThumbnailGenerator::generatePlaceholder(
    MediaType type, uint32_t maxWidth)
{
    uint32_t w = maxWidth > 0 ? maxWidth : m_defaultWidth;
    uint32_t h = m_defaultHeight;

    // Maintain a reasonable aspect ratio
    if (w > 0 && h > 0)
    {
        double aspect = static_cast<double>(m_defaultWidth) / m_defaultHeight;
        h = static_cast<uint32_t>(w / aspect);
        if (h == 0) h = 1;
    }

    auto thumb = std::make_shared<Thumbnail>();
    thumb->width      = w;
    thumb->height     = h;
    thumb->stride     = w * 4;
    thumb->pixels.resize(w * h * 4);
    thumb->type       = type;
    thumb->valid      = false;  // Placeholder — not a real thumbnail

    // Fill with a color based on media type
    uint8_t r = 60, g = 60, b = 80, a = 255;
    switch (type)
    {
    case MediaType::Video: r = 40; g = 60; b = 100; break;
    case MediaType::Image: r = 50; g = 80; b = 50;  break;
    case MediaType::Audio: r = 80; g = 50; b = 80;  break;
    case MediaType::Spine: r = 80; g = 70; b = 40;  break;
    default: break;
    }

    uint8_t color[4] = {b, g, r, a};
    for (size_t i = 0; i < w * h; ++i)
    {
        std::copy_n(std::begin(color), 4, thumb->pixels.data() + i * 4);
    }

    return thumb;
}

void ThumbnailGenerator::cachePut(const ThumbnailKey& key, std::shared_ptr<Thumbnail> thumb)
{
    std::lock_guard lock(m_cacheMutex);
    m_cache[key] = std::move(thumb);
}

std::string ThumbnailGenerator::resolveCanonicalPath(
    const std::filesystem::path& filePath) const
{
    std::error_code ec;
    auto canonical = std::filesystem::canonical(filePath, ec);
    return ec ? filePath.string() : canonical.string();
}

} // namespace rt

