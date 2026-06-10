/*
 * WaveformCache.cpp — background waveform peak generation and disk cache.
 */

#include "media/WaveformCache.h"
#include "PathUtils.h"
#include "media/AudioFile.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>

namespace rt {

// ─── Mip level sizes (samples per peak) ─────────────────────────────────────
// Level 0: 256 samples/peak  (zoomed-in view)
// Level 1: 1024 samples/peak
// Level 2: 4096 samples/peak
// Level 3: 16384 samples/peak (overview)
static constexpr uint32_t kMipSizes[] = { 256, 1024, 4096, 16384 };
static constexpr size_t   kNumMipLevels = 4;

// Disk cache magic + version
static constexpr uint32_t kWaveformMagic   = 0x57464D43; // "WFMC"
static constexpr uint32_t kWaveformVersion = 1;

// FNV-1a hash constants
static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
static constexpr uint64_t kFnvPrime       = 1099511628211ULL;

// ─── WaveformData::bestLevel ────────────────────────────────────────────────

const WaveformMipLevel* WaveformData::bestLevel(uint32_t samplesPerPixel) const noexcept
{
    if (mipLevels.empty()) return nullptr;

    // Find the mip level with samplesPerPeak <= samplesPerPixel
    // (so we have enough detail). Pick the coarsest that still fits.
    const WaveformMipLevel* best = &mipLevels[0];
    for (const auto& level : mipLevels) {
        if (level.samplesPerPeak <= samplesPerPixel) {
            best = &level;
        }
    }
    return best;
}

// ─── Constructor / Destructor ───────────────────────────────────────────────

WaveformCache::WaveformCache()
{
    // Default cache directory: system temp
    m_cacheDir = std::filesystem::temp_directory_path() / "roundtable_waveforms";
}

WaveformCache::~WaveformCache()
{
    // Wait for any pending computations
    std::lock_guard lock(m_mutex);
    for (auto& [id, future] : m_pending) {
        if (future.valid()) {
            future.wait();
        }
    }
}

// ─── Configuration ──────────────────────────────────────────────────────────

void WaveformCache::setCacheDirectory(const std::filesystem::path& dir)
{
    std::lock_guard lock(m_mutex);
    m_cacheDir = dir;
}

const std::filesystem::path& WaveformCache::cacheDirectory() const noexcept
{
    return m_cacheDir;
}

// ─── Async computation ──────────────────────────────────────────────────────

void WaveformCache::computeAsync(uint64_t mediaId,
                                  const std::filesystem::path& audioPath,
                                  WaveformProgressCallback progress)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_status.count(mediaId) && m_status[mediaId] == WaveformStatus::Computing) {
            spdlog::warn("WaveformCache: already computing for media {}", mediaId);
            return;
        }
        m_status[mediaId] = WaveformStatus::Computing;
    }

    // Launch background thread
    auto future = std::async(std::launch::async,
        [this, mediaId, audioPath, progress = std::move(progress)]()
    {
        spdlog::info("WaveformCache: computing waveform for '{}'",
                     pathToUtf8(audioPath.filename()));

        AudioFile audioFile;
        if (!audioFile.open(audioPath)) {
            spdlog::error("WaveformCache: failed to open '{}'", pathToUtf8(audioPath));
            std::lock_guard lock(m_mutex);
            m_status[mediaId] = WaveformStatus::Error;
            return;
        }

        const auto& info = audioFile.info();
        auto samples = audioFile.readAll();
        if (samples.empty()) {
            spdlog::error("WaveformCache: no samples in '{}'", pathToUtf8(audioPath));
            std::lock_guard lock(m_mutex);
            m_status[mediaId] = WaveformStatus::Error;
            return;
        }

        if (progress) progress(info.frames / 2, info.frames);

        // Generate mip levels
        auto mipLevels = generateMipLevels(
            samples.data(), info.frames, info.channels);

        if (progress) progress(info.frames, info.frames);

        // Store result
        auto data = std::make_shared<WaveformData>();
        data->mediaId     = mediaId;
        data->filePath    = pathToUtf8(audioPath);
        data->sampleRate  = info.sampleRate;
        data->channels    = info.channels;
        data->totalFrames = info.frames;
        data->mipLevels   = std::move(mipLevels);

        {
            std::lock_guard lock(m_mutex);
            m_cache[mediaId]  = std::move(data);
            m_status[mediaId] = WaveformStatus::Ready;
        }

        // Save to disk cache
        saveToDisk(mediaId);

        spdlog::info("WaveformCache: done for media {} ({} frames)",
                     mediaId, info.frames);
    });

    std::lock_guard lock(m_mutex);
    m_pending[mediaId] = std::move(future);
}

// ─── Synchronous computation from samples ───────────────────────────────────

bool WaveformCache::computeFromSamples(uint64_t mediaId,
                                        const float* samples, int64_t frames,
                                        uint16_t channels, uint32_t sampleRate)
{
    if (!samples || frames <= 0 || channels == 0) return false;

    auto mipLevels = generateMipLevels(samples, frames, channels);

    auto data = std::make_shared<WaveformData>();
    data->mediaId     = mediaId;
    data->sampleRate  = sampleRate;
    data->channels    = channels;
    data->totalFrames = frames;
    data->mipLevels   = std::move(mipLevels);

    std::lock_guard lock(m_mutex);
    m_cache[mediaId]  = std::move(data);
    m_status[mediaId] = WaveformStatus::Ready;
    return true;
}

// ─── Status ─────────────────────────────────────────────────────────────────

WaveformStatus WaveformCache::status(uint64_t mediaId) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_status.find(mediaId);
    return (it != m_status.end()) ? it->second : WaveformStatus::NotStarted;
}

bool WaveformCache::waitUntilReady(uint64_t mediaId, int timeoutMs) const
{
    // Check if already ready
    {
        std::lock_guard lock(m_mutex);
        auto it = m_status.find(mediaId);
        if (it != m_status.end() && it->second == WaveformStatus::Ready)
            return true;
        if (it != m_status.end() && it->second == WaveformStatus::Error)
            return false;
    }

    // Poll until ready or timeout
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(m_mutex);
        auto it = m_status.find(mediaId);
        if (it != m_status.end()) {
            if (it->second == WaveformStatus::Ready) return true;
            if (it->second == WaveformStatus::Error) return false;
        }
    }
    return false;
}

// ─── Query ──────────────────────────────────────────────────────────────────

const WaveformData* WaveformCache::get(uint64_t mediaId) const
{
    std::lock_guard lock(m_mutex);
    auto it = m_cache.find(mediaId);
    return (it != m_cache.end()) ? it->second.get() : nullptr;
}

std::vector<WaveformPeak> WaveformCache::getPeaks(
    uint64_t mediaId, int64_t startFrame, int64_t numFrames,
    uint32_t samplesPerPixel, uint16_t channel) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_cache.find(mediaId);
    if (it == m_cache.end()) return {};

    const auto& data = *it->second;
    const auto* level = data.bestLevel(samplesPerPixel);
    if (!level) return {};

    if (channel >= level->channels) return {};

    const auto spp = level->samplesPerPeak;
    const auto peaksPerCh = level->peaksPerChannel();

    // Convert frame range to peak range
    const auto startPeak = static_cast<size_t>(startFrame / spp);
    const auto endPeak   = static_cast<size_t>((startFrame + numFrames + spp - 1) / spp);

    const auto clampedStart = std::min(startPeak, peaksPerCh);
    const auto clampedEnd   = std::min(endPeak, peaksPerCh);

    std::vector<WaveformPeak> result;
    result.reserve(clampedEnd - clampedStart);

    for (size_t p = clampedStart; p < clampedEnd; ++p) {
        result.push_back(level->peaks[p * level->channels + channel]);
    }

    return result;
}

// ─── Cache management ───────────────────────────────────────────────────────

void WaveformCache::remove(uint64_t mediaId)
{
    std::lock_guard lock(m_mutex);
    m_cache.erase(mediaId);
    m_status.erase(mediaId);
    m_pending.erase(mediaId);
}

void WaveformCache::clearAll()
{
    std::lock_guard lock(m_mutex);

    for (auto& [id, future] : m_pending) {
        if (future.valid()) future.wait();
    }

    m_cache.clear();
    m_status.clear();
    m_pending.clear();
}

size_t WaveformCache::totalMemoryUsage() const
{
    std::lock_guard lock(m_mutex);
    size_t total = 0;
    for (const auto& [id, data] : m_cache) {
        for (const auto& level : data->mipLevels) {
            total += level.peaks.size() * sizeof(WaveformPeak);
        }
    }
    return total;
}

// ─── Disk cache ─────────────────────────────────────────────────────────────

bool WaveformCache::loadFromDisk(uint64_t mediaId,
                                  const std::filesystem::path& audioPath)
{
    const auto key  = cacheKey(audioPath);
    const auto path = diskCachePath(key);

    if (!std::filesystem::exists(path)) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    // Read header
    uint32_t magic{}, version{};
    file.read(reinterpret_cast<char*>(&magic), 4);
    file.read(reinterpret_cast<char*>(&version), 4);

    if (magic != kWaveformMagic || version != kWaveformVersion) {
        spdlog::warn("WaveformCache: invalid cache file '{}'", pathToUtf8(path));
        return false;
    }

    auto data = std::make_shared<WaveformData>();
    data->mediaId = mediaId;
    data->filePath = pathToUtf8(audioPath);

    file.read(reinterpret_cast<char*>(&data->sampleRate), 4);
    file.read(reinterpret_cast<char*>(&data->channels), 2);
    file.read(reinterpret_cast<char*>(&data->totalFrames), 8);

    uint32_t numLevels{};
    file.read(reinterpret_cast<char*>(&numLevels), 4);

    data->mipLevels.resize(numLevels);
    for (uint32_t i = 0; i < numLevels; ++i) {
        auto& level = data->mipLevels[i];
        file.read(reinterpret_cast<char*>(&level.samplesPerPeak), 4);
        file.read(reinterpret_cast<char*>(&level.channels), 2);

        uint64_t peakCount{};
        file.read(reinterpret_cast<char*>(&peakCount), 8);

        level.peaks.resize(static_cast<size_t>(peakCount));
        file.read(reinterpret_cast<char*>(level.peaks.data()),
                  static_cast<std::streamsize>(peakCount * sizeof(WaveformPeak)));
    }

    if (!file) {
        spdlog::warn("WaveformCache: truncated cache file '{}'", pathToUtf8(path));
        return false;
    }

    std::lock_guard lock(m_mutex);
    m_cache[mediaId]  = std::move(data);
    m_status[mediaId] = WaveformStatus::Ready;

    spdlog::info("WaveformCache: loaded from disk cache for media {}", mediaId);
    return true;
}

bool WaveformCache::saveToDisk(uint64_t mediaId) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_cache.find(mediaId);
    if (it == m_cache.end()) return false;

    const auto& data = *it->second;

    // Create cache directory
    std::error_code ec;
    std::filesystem::create_directories(m_cacheDir, ec);
    if (ec) {
        spdlog::warn("WaveformCache: could not create cache dir: {}", ec.message());
        return false;
    }

    const auto key  = cacheKey(std::filesystem::path(data.filePath));
    const auto path = diskCachePath(key);

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        spdlog::warn("WaveformCache: could not open cache file for writing");
        return false;
    }

    // Header
    file.write(reinterpret_cast<const char*>(&kWaveformMagic), 4);
    file.write(reinterpret_cast<const char*>(&kWaveformVersion), 4);
    file.write(reinterpret_cast<const char*>(&data.sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&data.channels), 2);
    file.write(reinterpret_cast<const char*>(&data.totalFrames), 8);

    const auto numLevels = static_cast<uint32_t>(data.mipLevels.size());
    file.write(reinterpret_cast<const char*>(&numLevels), 4);

    for (const auto& level : data.mipLevels) {
        file.write(reinterpret_cast<const char*>(&level.samplesPerPeak), 4);
        file.write(reinterpret_cast<const char*>(&level.channels), 2);

        const auto peakCount = static_cast<uint64_t>(level.peaks.size());
        file.write(reinterpret_cast<const char*>(&peakCount), 8);
        file.write(reinterpret_cast<const char*>(level.peaks.data()),
                   static_cast<std::streamsize>(peakCount * sizeof(WaveformPeak)));
    }

    spdlog::info("WaveformCache: saved to disk for media {}", mediaId);
    return true;
}

// ─── Generate mip levels ────────────────────────────────────────────────────

std::vector<WaveformMipLevel> WaveformCache::generateMipLevels(
    const float* samples, int64_t frames, uint16_t channels)
{
    std::vector<WaveformMipLevel> levels;
    levels.reserve(kNumMipLevels);

    for (size_t mip = 0; mip < kNumMipLevels; ++mip) {
        const uint32_t spp = kMipSizes[mip];
        const auto numPeaks = static_cast<size_t>((frames + spp - 1) / spp);

        WaveformMipLevel level;
        level.samplesPerPeak = spp;
        level.channels       = channels;
        level.peaks.resize(numPeaks * channels);

        for (size_t p = 0; p < numPeaks; ++p) {
            const auto startFrame = static_cast<int64_t>(p * spp);
            const auto endFrame   = std::min(startFrame + static_cast<int64_t>(spp), frames);

            for (uint16_t ch = 0; ch < channels; ++ch) {
                float minVal =  1.0f;
                float maxVal = -1.0f;

                for (int64_t f = startFrame; f < endFrame; ++f) {
                    const float s = samples[f * channels + ch];
                    minVal = std::min(minVal, s);
                    maxVal = std::max(maxVal, s);
                }

                level.peaks[p * channels + ch] = { minVal, maxVal };
            }
        }

        levels.push_back(std::move(level));
    }

    return levels;
}

// ─── Cache key generation ───────────────────────────────────────────────────

std::string WaveformCache::cacheKey(const std::filesystem::path& path)
{
    // Simple hash based on path + file size + last write time
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    const auto modTime  = std::filesystem::last_write_time(path, ec);

    uint64_t hash = kFnvOffsetBasis;
    const auto pathStr = pathToUtf8(path);
    for (char c : pathStr) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kFnvPrime;
    }
    hash ^= fileSize;
    hash *= kFnvPrime;

    const auto modCount = modTime.time_since_epoch().count();
    hash ^= static_cast<uint64_t>(modCount);
    hash *= kFnvPrime;

    // Convert to hex string
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

std::filesystem::path WaveformCache::diskCachePath(const std::string& key) const
{
    return m_cacheDir / (key + ".wfcache");
}

} // namespace rt

