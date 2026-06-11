/*
 * WaveformCache — background waveform peak generation for timeline display.
 *
 * Generates min/max peak data at multiple zoom levels and caches to disk
 * for fast reload. The timeline UI queries this cache to draw waveforms.
 *
 * Design:
 *   • computeAsync() launches a background thread to process an audio file
 *   • Multiple mip levels are generated (1 peak per N samples)
 *   • Results cached to disk as binary files (keyed by file hash + mod time)
 *   • getPeaks() returns the best-fit mip level for a given zoom
 *
 * Thread-safe: computation runs on a worker thread, results read from UI thread.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

/// A single peak pair (min + max) for waveform display
struct WaveformPeak
{
    float minVal{0.0f};
    float maxVal{0.0f};
};

/// One mip level of waveform data
struct WaveformMipLevel
{
    uint32_t               samplesPerPeak{256};   // How many source samples per peak
    uint16_t               channels{1};
    std::vector<WaveformPeak> peaks;              // Interleaved by channel

    /// Number of peaks per channel
    [[nodiscard]] size_t peaksPerChannel() const noexcept
    {
        return channels > 0 ? peaks.size() / channels : 0;
    }
};

/// Complete waveform data for one audio file (all mip levels)
struct WaveformData
{
    uint64_t    mediaId{0};
    std::string filePath;
    uint32_t    sampleRate{0};
    uint16_t    channels{0};
    int64_t     totalFrames{0};

    std::vector<WaveformMipLevel> mipLevels;

    /// Get the best mip level for a given samples-per-pixel ratio.
    /// Returns nullptr if no data available.
    [[nodiscard]] const WaveformMipLevel* bestLevel(uint32_t samplesPerPixel) const noexcept;
};

/// Status of waveform computation
enum class WaveformStatus : uint8_t
{
    NotStarted,
    Computing,
    Ready,
    Error
};

/// Progress callback: (framesProcessed, totalFrames)
using WaveformProgressCallback = std::function<void(int64_t, int64_t)>;

class WaveformCache
{
public:
    WaveformCache();
    ~WaveformCache();

    // Non-copyable
    WaveformCache(const WaveformCache&) = delete;
    WaveformCache& operator=(const WaveformCache&) = delete;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the directory for disk cache files. Default: system temp directory.
    void setCacheDirectory(const std::filesystem::path& dir);

    /// Get the cache directory.
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept;

    // ── Computation ─────────────────────────────────────────────────────

    /// Compute waveform data for an audio file. Non-blocking — runs on a
    /// background thread. Returns immediately.
    /// Call status() or waitUntilReady() to check completion.
    void computeAsync(uint64_t mediaId,
                      const std::filesystem::path& audioPath,
                      WaveformProgressCallback progress = nullptr);

    /// Compute waveform data from pre-loaded samples (synchronous).
    /// Useful for testing or when samples are already in memory.
    bool computeFromSamples(uint64_t mediaId,
                            const float* samples, int64_t frames,
                            uint16_t channels, uint32_t sampleRate);

    /// Get the computation status for a media ID.
    [[nodiscard]] WaveformStatus status(uint64_t mediaId) const;

    /// Wait until computation is complete. Returns false on timeout.
    bool waitUntilReady(uint64_t mediaId,
                        int timeoutMs = 30000) const;

    // ── Query ───────────────────────────────────────────────────────────

    /// Get waveform data for a media ID. Returns nullptr if not ready.
    [[nodiscard]] const WaveformData* get(uint64_t mediaId) const;

    /// Get peaks for a specific region at a given zoom level.
    /// Returns peaks for [startFrame, startFrame + numFrames) at approximately
    /// the given samples-per-pixel ratio.
    [[nodiscard]] std::vector<WaveformPeak> getPeaks(
        uint64_t mediaId, int64_t startFrame, int64_t numFrames,
        uint32_t samplesPerPixel, uint16_t channel = 0) const;

    // ── Cache management ────────────────────────────────────────────────

    /// Remove waveform data for a media ID (memory + disk).
    void remove(uint64_t mediaId);

    /// Clear all cached data.
    void clearAll();

    /// Try to load from disk cache. Returns true if found and loaded.
    bool loadFromDisk(uint64_t mediaId, const std::filesystem::path& audioPath);

    /// Save to disk cache. Returns true on success.
    bool saveToDisk(uint64_t mediaId) const;

    /// Get total memory usage of all cached waveform data.
    [[nodiscard]] size_t totalMemoryUsage() const;

private:
    /// Generate mip levels from raw samples
    static std::vector<WaveformMipLevel> generateMipLevels(
        const float* samples, int64_t frames,
        uint16_t channels);

    /// Compute a unique cache key for a file (hash of path + size + mtime)
    static std::string cacheKey(const std::filesystem::path& path);

    /// Disk cache file path for a given key
    [[nodiscard]] std::filesystem::path diskCachePath(const std::string& key) const;

    mutable std::mutex                                    m_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<WaveformData>>  m_cache;
    std::unordered_map<uint64_t, std::future<void>>       m_pending;
    std::unordered_map<uint64_t, WaveformStatus>          m_status;
    std::filesystem::path                                 m_cacheDir;
};

} // namespace rt
