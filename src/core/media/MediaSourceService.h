#pragma once

#include "engine/EngineContracts.h"
#include "media/AudioFile.h"
#include "media/MediaPool.h"
#include "media/ThumbnailGenerator.h"
#include "PathUtils.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rt {

struct MediaSourceInfo
{
    MediaHandle handle{InvalidMedia};
    std::filesystem::path path;
    uint32_t width{0};
    uint32_t height{0};
    double fps{0.0};
    double durationSeconds{0.0};
    int64_t frameCount{0};
    std::string codecName;
    std::string containerFormat;
    bool hasAudio{false};
    bool hasVideo{false};
    bool hasAlpha{false};
    bool packedAlpha{false};
    bool isVariableFrameRate{false};
};

enum class MediaPixelFormatPreference : uint8_t
{
    Any,
    Bgra,
    NativeGpu
};

enum class MediaAlphaMode : uint8_t
{
    Unknown,
    Straight,
    Premultiplied,
    Packed
};

struct MediaSourceOpenRequest
{
    std::filesystem::path path;
    RenderRequestType type{RenderRequestType::Still};
    bool allowAsyncOpen{false};
};

struct MediaSourceOpenResult
{
    MediaHandle handle{InvalidMedia};
    std::filesystem::path path;
    RenderRequestType type{RenderRequestType::Still};
    bool asyncQueued{false};
    bool failed{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle != InvalidMedia;
    }
};

struct MediaFrameRequest
{
    MediaHandle handle{InvalidMedia};
    int64_t frameNumber{0};
    RenderRequestType type{RenderRequestType::Still};
    RenderQuality quality{RenderQuality::Auto};
    RenderExactness exactness{RenderExactness::ExactRequired};
    ResolutionTier tier{ResolutionTier::Full};
    MediaPixelFormatPreference pixelFormat{MediaPixelFormatPreference::Any};
    MediaAlphaMode alphaMode{MediaAlphaMode::Unknown};
    bool preferNativeGpuFormat{false};
    const char* caller{nullptr};
};

struct MediaCacheKey
{
    MediaHandle source{InvalidMedia};
    int64_t frameNumber{0};
    RenderRequestType type{RenderRequestType::Still};
    RenderQuality quality{RenderQuality::Auto};
    RenderExactness exactness{RenderExactness::ExactRequired};
    ResolutionTier tier{ResolutionTier::Full};
    MediaPixelFormatPreference pixelFormat{MediaPixelFormatPreference::Any};
    MediaAlphaMode alphaMode{MediaAlphaMode::Unknown};

    [[nodiscard]] bool operator==(const MediaCacheKey& other) const noexcept
    {
        return source == other.source &&
               frameNumber == other.frameNumber &&
               type == other.type &&
               quality == other.quality &&
               exactness == other.exactness &&
               tier == other.tier &&
               pixelFormat == other.pixelFormat &&
               alphaMode == other.alphaMode;
    }
};

struct MediaFrameResult
{
    MediaCacheKey key;
    std::shared_ptr<CachedFrame> frame;
    bool blockingDecodeAllowed{false};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(frame);
    }
};

struct MediaThumbnailRequest
{
    std::filesystem::path path;
    uint32_t maxWidth{0};
    RenderRequestType type{RenderRequestType::Thumbnail};
};

struct MediaWaveformRequest
{
    std::filesystem::path path;
    uint64_t mediaId{0};
    uint32_t sampleRate{48000};
    int64_t framesPerPeak{480};
};

struct MediaWaveformEnvelope
{
    std::vector<float> peaks;
    size_t peakCount{0};
    uint16_t channels{0};

    [[nodiscard]] bool valid() const noexcept
    {
        return peakCount > 0 && !peaks.empty();
    }
};


/// Decode request options derived from MediaFrameRequest.
struct FrameRequestOptions
{
    ResolutionTier tier{ResolutionTier::Full};
    bool exactDecode{false};
    bool preferNativeGpuFormat{false};
};

class MediaSourceService
{
public:
    MediaSourceService() = default;

    /// Take ownership of a MediaPool.  Caller must not retain the raw pointer.
    explicit MediaSourceService(std::unique_ptr<MediaPool> pool) noexcept
        : m_ownedPool(std::move(pool)), m_pool(m_ownedPool.get())
    {
    }

    /// Wrap an externally-owned pool (transitional - prefer the owning constructor).
    explicit MediaSourceService(MediaPool* pool) noexcept
        : m_pool(pool)
    {
    }

    void bind(MediaPool* pool) noexcept
    {
        m_ownedPool.reset();
        m_pool = pool;
    }

    /// Transfer ownership of an existing pool into this service.
    void takeOwnership(std::unique_ptr<MediaPool> pool) noexcept
    {
        m_ownedPool = std::move(pool);
        m_pool = m_ownedPool.get();
    }

    [[nodiscard]] MediaPool* mediaPool() const noexcept
    {
        return m_pool;
    }

    [[nodiscard]] bool isValid(MediaHandle handle) const
    {
        return m_pool && m_pool->isValid(handle);
    }

    [[nodiscard]] std::filesystem::path pathFor(MediaHandle handle) const
    {
        return isValid(handle) ? m_pool->getPath(handle) : std::filesystem::path{};
    }

    [[nodiscard]] std::optional<MediaSourceInfo> sourceInfo(MediaHandle handle) const
    {
        if (!isValid(handle)) {
            return std::nullopt;
        }

        const auto* info = m_pool->getInfo(handle);
        if (!info) {
            return std::nullopt;
        }

        MediaSourceInfo source;
        source.handle = handle;
        source.path = m_pool->getPath(handle);
        source.width = info->width;
        source.height = info->height;
        source.fps = info->fps;
        source.durationSeconds = info->duration;
        source.frameCount = info->frameCount;
        source.codecName = info->codecName;
        source.containerFormat = info->containerFormat;
        source.hasAudio = info->hasAudio;
        source.hasVideo = info->videoStreamIndex >= 0;
        source.hasAlpha = info->hasAlpha;
        source.packedAlpha = info->packedAlpha;
        source.isVariableFrameRate = info->isVFR;
        return source;
    }

    [[nodiscard]] uint64_t sourceIdForPath(const std::filesystem::path& path) const
    {
        if (path.empty()) {
            return 0;
        }

        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        const std::string key = ec ? pathToUtf8(path.lexically_normal())
                                   : pathToUtf8(canonical.lexically_normal());
        return static_cast<uint64_t>(std::hash<std::string>{}(key));
    }

    [[nodiscard]] uint64_t sourceIdForHandle(MediaHandle handle) const
    {
        return sourceIdForPath(pathFor(handle));
    }

    [[nodiscard]] MediaSourceOpenResult openSource(const MediaSourceOpenRequest& request) const
    {
        MediaSourceOpenResult result;
        result.path = request.path;
        result.type = request.type;

        if (!m_pool || request.path.empty()) {
            result.failed = true;
            return result;
        }

        if (request.allowAsyncOpen && request.type == RenderRequestType::Playback &&
            !m_pool->isPathOpen(request.path)) {
            m_pool->openAsync(request.path);
            result.asyncQueued = true;
            return result;
        }

        result.handle = m_pool->open(request.path);
        result.failed = result.handle == InvalidMedia;
        return result;
    }

    [[nodiscard]] MediaCacheKey cacheKeyFor(const MediaFrameRequest& request) const noexcept
    {
        return MediaCacheKey{
            request.handle,
            request.frameNumber,
            request.type,
            request.quality,
            request.exactness,
            request.tier,
            request.pixelFormat,
            request.alphaMode
        };
    }

    [[nodiscard]] FrameRequestOptions frameOptionsFor(const MediaFrameRequest& request) const noexcept
    {
        const bool exactDecode = request.exactness == RenderExactness::ExactRequired &&
            request.type != RenderRequestType::Playback;
        return FrameRequestOptions{
            request.tier,
            exactDecode,
            request.preferNativeGpuFormat ||
                request.pixelFormat == MediaPixelFormatPreference::NativeGpu
        };
    }

    [[nodiscard]] bool allowsBlockingDecode(const MediaFrameRequest& request) const noexcept
    {
        return request.type != RenderRequestType::Playback &&
               request.exactness == RenderExactness::ExactRequired;
    }

    [[nodiscard]] MediaFrameResult requestFrame(const MediaFrameRequest& request) const
    {
        MediaFrameResult result;
        result.key = cacheKeyFor(request);
        result.blockingDecodeAllowed = allowsBlockingDecode(request);

        if (!m_pool || !isValid(request.handle)) {
            return result;
        }

        const auto options = frameOptionsFor(request);
        result.frame = result.blockingDecodeAllowed
            ? m_pool->getFrame(request.handle, request.frameNumber, options.tier, !options.exactDecode)
            : m_pool->tryGetFrame(request.handle, request.frameNumber, options.tier);
        return result;
    }

    [[nodiscard]] bool isFrameCached(const MediaFrameRequest& request) const
    {
        return m_pool && isValid(request.handle) &&
               m_pool->isFrameCached(request.handle, request.frameNumber,
                                     frameOptionsFor(request).tier);
    }

    void schedulePrefetch(const MediaFrameRequest& request,
                          int count,
                          bool urgent) const
    {
        if (!m_pool || !isValid(request.handle)) {
            return;
        }
        m_pool->schedulePrefetch(request.handle, request.frameNumber,
                                 count, urgent, frameOptionsFor(request).tier);
    }

    void startLoopPreDecode(const MediaFrameRequest& request,
                            int64_t priority = 0) const
    {
        if (!m_pool || !isValid(request.handle)) {
            return;
        }
        m_pool->startLoopPreDecode(request.handle, request.tier, priority);
    }

    [[nodiscard]] std::shared_ptr<Thumbnail> requestThumbnailSync(
        const MediaThumbnailRequest& request,
        ThumbnailGenerator* generator = nullptr) const
    {
        if (request.path.empty()) {
            return nullptr;
        }

        if (generator) {
            if (m_pool) {
                generator->setMediaPool(m_pool);
            }
            return generator->generateSync(request.path, request.maxWidth);
        }

        ThumbnailGenerator localGenerator(1, request.maxWidth > 0 ? request.maxWidth : 160, 120);
        if (m_pool) {
            localGenerator.setMediaPool(m_pool);
        }
        return localGenerator.generateSync(request.path, request.maxWidth);
    }

    [[nodiscard]] MediaWaveformEnvelope buildWaveformEnvelope(
        const MediaWaveformRequest& request) const
    {
        MediaWaveformEnvelope envelope;
        if (request.path.empty() || request.sampleRate == 0 || request.framesPerPeak <= 0) {
            return envelope;
        }

        AudioFile file;
        if (!file.open(request.path)) {
            return envelope;
        }

        envelope.peakCount = file.buildPeakEnvelopeResampled(
            request.sampleRate, request.framesPerPeak, envelope.peaks);
        envelope.channels = file.info().channels;
        return envelope;
    }

private:
    std::unique_ptr<MediaPool> m_ownedPool;
    MediaPool* m_pool{nullptr};
};

} // namespace rt