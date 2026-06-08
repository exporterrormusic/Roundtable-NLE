/*
 * AudioFile — audio file I/O via libsndfile.
 *
 * Loads audio from WAV, AIFF, AU, W64, CAF, RAW formats and provides:
 *   • Interleaved float sample data (normalized -1.0 to 1.0)
 *   • Stream info (sample rate, channels, duration, frame count)
 *   • On-demand region reading for large files
 *
 * Falls back to FFmpeg for formats not supported by libsndfile (MP3, OGG, FLAC
 * when external libs are not compiled in, or audio tracks in video containers).
 *
 * Thread-safe: all public methods are guarded by a mutex.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

/// Information about an audio file
struct AudioFileInfo
{
    uint32_t    sampleRate{0};
    uint16_t    channels{0};
    int64_t     frames{0};       // Total sample frames (one frame = N channel samples)
    double      duration{0.0};   // Duration in seconds
    std::string codec;           // e.g. "PCM 16-bit", "Float 32-bit"
    std::string format;          // e.g. "WAV", "AIFF", "FLAC"
    uint32_t    bitDepth{0};

    /// Total sample count (frames * channels)
    [[nodiscard]] int64_t totalSamples() const noexcept { return frames * channels; }

    /// Memory required for all samples as float (bytes)
    [[nodiscard]] size_t memorySizeFloat() const noexcept
    {
        return static_cast<size_t>(totalSamples()) * sizeof(float);
    }
};

/// Backend used to decode the audio file
enum class AudioBackend : uint8_t
{
    None,
    Sndfile,    // libsndfile
    FFmpeg,     // FFmpeg (for MP3, OGG, video containers, etc.)
};

class AudioFile
{
public:
    AudioFile();
    ~AudioFile();

    // Non-copyable
    AudioFile(const AudioFile&) = delete;
    AudioFile& operator=(const AudioFile&) = delete;

    /// Open an audio file. Returns true on success.
    /// Tries libsndfile first, then falls back to FFmpeg.
    bool open(const std::filesystem::path& path);

    /// Open from a UTF-8-encoded path string. The std::filesystem::path(string)
    /// constructor decodes via the ANSI codepage on Windows and mangles non-CP_ACP
    /// characters (e.g. U+FF5C in yt-dlp names); route through utf8ToPath() instead.
    bool open(const std::string& utf8Path);

    /// Close and release all resources.
    void close();

    /// Is a file currently open?
    [[nodiscard]] bool isOpen() const noexcept;

    /// Get file information.
    [[nodiscard]] const AudioFileInfo& info() const noexcept;

    /// Which backend decoded this file?
    [[nodiscard]] AudioBackend backend() const noexcept;

    /// Read ALL samples into interleaved float buffer.
    /// Returns empty vector on failure. For large files, prefer readRegion().
    [[nodiscard]] std::vector<float> readAll();

    /// Read a region of sample frames [startFrame, startFrame + numFrames).
    /// Output: interleaved float samples (numFrames * channels).
    /// Returns number of frames actually read.
    int64_t readRegion(int64_t startFrame, int64_t numFrames,
                       std::vector<float>& outSamples);

    /// Read all samples and resample to targetSampleRate if different.
    /// Simple linear interpolation resampler for preview quality.
    [[nodiscard]] std::vector<float> readAllResampled(uint32_t targetSampleRate);

    /// Read a region expressed in the target sample-rate domain and resample
    /// if needed. Output is interleaved float samples.
    int64_t readRegionResampled(int64_t startFrame, int64_t numFrames,
                                uint32_t targetSampleRate,
                                std::vector<float>& outSamples);

    /// Build a max-peak envelope for the full file in the target sample-rate
    /// domain without materializing the entire decoded file in memory.
    size_t buildPeakEnvelopeResampled(uint32_t targetSampleRate,
                                      int64_t windowFrames,
                                      std::vector<float>& outPeaks,
                                      int64_t chunkFrames = 0);

    /// Get the last error message.
    [[nodiscard]] const std::string& lastError() const noexcept;

    /// Get the file path.
    [[nodiscard]] const std::filesystem::path& filePath() const noexcept;

private:
    bool openSndfile(const std::filesystem::path& path);
    bool openFFmpeg(const std::filesystem::path& path);

#ifdef ROUNDTABLE_HAS_FFMPEG
    std::vector<float> readAllFFmpeg();
    int64_t readRegionFFmpeg(int64_t startFrame, int64_t numFrames,
                             std::vector<float>& outSamples);
#endif

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    AudioFileInfo         m_info;
    AudioBackend          m_backend{AudioBackend::None};
    std::filesystem::path m_path;
    std::string           m_lastError;
    bool                  m_isOpen{false};
    mutable std::mutex    m_mutex;
};

} // namespace rt
