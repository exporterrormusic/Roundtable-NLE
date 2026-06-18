/*
 * AudioMixdown — Multi-track audio mixing to stereo WAV for export.
 *
 * Reads all audio clips from the timeline, applies per-track volume/pan/mute,
 * mixes to stereo interleaved float, and writes to WAV.
 * Can also encode to AAC via FFmpeg for final muxing.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rt {

// Forward declarations
class Timeline;

// ── Configuration ───────────────────────────────────────────────────────────

/// Audio codec for the mixdown output
enum class AudioCodec : uint8_t
{
    PCM_S16LE,   // 16-bit PCM (WAV)
    PCM_F32LE,   // 32-bit float PCM (WAV)
    AAC,         // AAC (for MP4/MOV muxing, or standalone .m4a)
    FLAC,        // Lossless
    MP3,         // MP3 (libmp3lame) — standalone .mp3
    Count
};

/// File extension (with leading dot) for a standalone audio export in `codec`.
[[nodiscard]] const char* audioCodecExtension(AudioCodec codec) noexcept;

struct AudioMixdownConfig
{
    uint32_t   sampleRate{48000};
    uint16_t   channels{2};         // Output channels (1=mono, 2=stereo)
    AudioCodec codec{AudioCodec::PCM_S16LE};
    int        bitrate{192000};     // Audio bitrate for lossy codecs (bits/sec)

    // Render range (in seconds). 0,0 = full timeline duration.
    double     startTime{0.0};
    double     endTime{0.0};

    // Master volume
    float      masterVolume{1.0f};
};

// ── Mix result ──────────────────────────────────────────────────────────────

struct MixdownResult
{
    std::vector<float> samples;     ///< Interleaved stereo float samples
    uint32_t           sampleRate{0};
    uint16_t           channels{0};
    int64_t            totalFrames{0};
    double             duration{0.0};

    [[nodiscard]] bool isValid() const noexcept { return !samples.empty(); }
};

// ── Progress callback ───────────────────────────────────────────────────────

/// Progress callback: (progress 0-1, status text)
using MixdownProgressFn = std::function<void(float, const std::string&)>;

// ═════════════════════════════════════════════════════════════════════════════

class AudioMixdown
{
public:
    AudioMixdown();
    ~AudioMixdown();

    AudioMixdown(const AudioMixdown&) = delete;
    AudioMixdown& operator=(const AudioMixdown&) = delete;

    // ── Mixing ──────────────────────────────────────────────────────────

    /// Mix all audio tracks from the timeline to interleaved float samples.
    /// \param timeline  The timeline to evaluate
    /// \param config    Mixdown configuration
    /// \param progress  Optional progress callback
    /// \return Mix result (empty on error)
    [[nodiscard]] MixdownResult mix(const Timeline& timeline,
                                    const AudioMixdownConfig& config = {},
                                    const MixdownProgressFn& progress = nullptr);

    // ── File output ─────────────────────────────────────────────────────

    /// Mix and write to a WAV file.
    bool mixToFile(const Timeline& timeline,
                   const std::filesystem::path& outputPath,
                   const AudioMixdownConfig& config = {},
                   const MixdownProgressFn& progress = nullptr);

    /// Write existing mix result to a WAV file.
    static bool writeWav(const MixdownResult& result,
                         const std::filesystem::path& outputPath);

    /// Write an existing mix result to a standalone audio file in the given
    /// codec/format.  PCM_S16LE goes through writeWav(); MP3/AAC/FLAC are
    /// encoded + muxed via FFmpeg (container chosen to match the codec).
    /// `bitrate` (bits/sec) applies to the lossy codecs (MP3/AAC) only.
    /// Returns false on error (e.g. the encoder isn't available in the build).
    static bool writeAudioFile(const MixdownResult& result,
                               const std::filesystem::path& outputPath,
                               AudioCodec codec,
                               int bitrate = 192000);

    /// Encode mix result to AAC (via FFmpeg). Returns encoded bytes.
    static std::vector<uint8_t> encodeAAC(const MixdownResult& result,
                                           int bitrate = 192000);

    // ── Queries ─────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// Estimate output file size in bytes.
    static size_t estimateFileSize(const AudioMixdownConfig& config, double durationSec);

private:
    /// Apply volume and pan to a mono/stereo source into the mix buffer.
    static void mixTrackInto(float* mixBuffer, int64_t mixFrames,
                              const float* sourceData, int64_t sourceFrames,
                              int64_t sourceOffset,
                              uint16_t sourceChannels, uint16_t mixChannels,
                              float volume, float pan, bool muted);

    std::string m_lastError;
};

} // namespace rt
