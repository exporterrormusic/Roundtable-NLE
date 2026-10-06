/*
 * AudioMixdown — Multi-track audio mixing to stereo WAV for export.
 *
 * Reads all audio clips from the timeline, applies per-track volume/pan/mute,
 * mixes to stereo interleaved float, and writes to WAV. AudioMixStream does
 * the mixing block by block; AudioMixdown::mix() collects a whole range.
 * Can also encode to AAC via FFmpeg for final muxing.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

// ── Streaming mix ───────────────────────────────────────────────────────────

/// Mixes the timeline's audio a block at a time. Each clip keeps its own
/// source reader and effect-chain state across blocks, so memory grows with
/// the block size, not with the export length, and the output is the same
/// whatever block sizes are used.
///
/// Everything needed from the timeline is copied at construction; the
/// timeline may change or go away afterwards.
class AudioMixStream
{
public:
    AudioMixStream(const Timeline& timeline, const AudioMixdownConfig& config);
    ~AudioMixStream();

    AudioMixStream(const AudioMixStream&) = delete;
    AudioMixStream& operator=(const AudioMixStream&) = delete;

    /// False when the render range is empty (see lastError()).
    [[nodiscard]] bool isValid() const noexcept { return m_totalFrames > 0; }

    [[nodiscard]] uint32_t sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] uint16_t channels() const noexcept { return m_channels; }
    [[nodiscard]] int64_t  totalFrames() const noexcept { return m_totalFrames; }
    [[nodiscard]] double   duration() const noexcept { return m_duration; }
    [[nodiscard]] int64_t  position() const noexcept { return m_position; }
    [[nodiscard]] size_t   audioTrackCount() const noexcept { return m_audioTracks; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// Mix the next `frames` frames into `out` (frames * channels() floats,
    /// overwritten). Returns the frames written: fewer than asked only at
    /// the end of the range, 0 once it is done.
    int64_t read(float* out, int64_t frames);

private:
    struct ClipSource;

    void mixClip(ClipSource& clip, float* out, int64_t blockFrames);

    std::vector<std::unique_ptr<ClipSource>> m_clips;   ///< track order, then clip order
    uint32_t    m_sampleRate{0};
    uint16_t    m_channels{0};
    int64_t     m_totalFrames{0};
    double      m_duration{0.0};
    int64_t     m_position{0};
    size_t      m_audioTracks{0};
    std::string m_lastError;
    std::vector<float> m_source;   ///< scratch: one clip's block of source frames
    std::vector<float> m_gains;    ///< scratch: one clip's block of fade gains
};

// ── Streaming file output ───────────────────────────────────────────────────

/// Writes interleaved float audio to a standalone file a block at a time:
/// the streaming form of AudioMixdown::writeAudioFile(), with the same
/// output for the same samples whatever the block sizes. PCM_S16LE writes a
/// WAV whose sizes finish() fills in; the other codecs encode and mux
/// through FFmpeg.
///
/// A writer destroyed before finish() closes the file as it stands; the
/// caller removes it.
class AudioFileWriter
{
public:
    AudioFileWriter();
    ~AudioFileWriter();

    AudioFileWriter(const AudioFileWriter&) = delete;
    AudioFileWriter& operator=(const AudioFileWriter&) = delete;

    /// `bitrate` (bits/sec) applies to the lossy codecs (MP3/AAC) only.
    bool open(const std::filesystem::path& outputPath, AudioCodec codec,
              uint32_t sampleRate, uint16_t channels, int bitrate = 192000);

    /// Append `frames` interleaved frames (frames * channels floats).
    bool write(const float* samples, int64_t frames);

    /// Flush, finalize the container and close. Returns false on any error.
    bool finish();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] int64_t framesWritten() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    struct WavOut;
    struct EncodedOut;

    std::unique_ptr<WavOut>     m_wav;
    std::unique_ptr<EncodedOut> m_encoded;
    std::string                 m_lastError;
};

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

    // ── Queries ─────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    /// Estimate output file size in bytes.
    static size_t estimateFileSize(const AudioMixdownConfig& config, double durationSec);

private:
    std::string m_lastError;
};

} // namespace rt
