/*
 * Transcriber — whisper.cpp integration for audio transcription.
 *
 * Provides GPU-accelerated speech-to-text with word-level timestamps.
 * Supports multiple model sizes (tiny → large-v3, including turbo).
 * Runs transcription on a dedicated thread — never blocks UI.
 *
 * When ROUNDTABLE_HAS_WHISPER is defined, uses whisper.cpp directly.
 * Otherwise, provides a stub that returns empty results.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

// ─── Data types ─────────────────────────────────────────────────────────────

/// A single word with precise timestamps.
struct WordSegment
{
    std::string word;
    double      start{0.0};       // seconds
    double      end{0.0};         // seconds
    float       probability{1.0f};
};

/// A sentence/phrase-level segment with optional word breakdown.
struct TranscriptionSegment
{
    int                      id{0};
    std::string              text;
    double                   start{0.0};  // seconds
    double                   end{0.0};    // seconds
    std::vector<WordSegment> words;
    std::string              character;   // Assigned after matching
};

enum class TranscriptionStatus { NotStarted, Success, NoSpeech, Cancelled, Unavailable, Failed };
enum class TranscriptionErrorCode
{
    None, BackendUnavailable, ModelUnavailable, AudioNotFound,
    AudioDecodeFailed, InferenceFailed, Cancelled
};

struct TranscriptionError
{
    TranscriptionErrorCode code{TranscriptionErrorCode::None};
    std::string message;
};

struct TranscriberCapabilities
{
    bool available{false};
    bool segmentTiming{false};
    bool wordTiming{false};
    bool confidence{false};
    bool languageDetection{false};
    bool diarization{false};
    bool gpu{false};
    bool cancellation{false};
};

/// Complete transcription result.
struct TranscriptionResult
{
    std::vector<TranscriptionSegment> segments;
    std::string                       language;
    double                            duration{0.0}; // Total audio duration
    TranscriptionStatus               status{TranscriptionStatus::NotStarted};
    TranscriptionError                error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == TranscriptionStatus::Success
            || status == TranscriptionStatus::NoSpeech;
    }

    /// Concatenated full text of all segments.
    [[nodiscard]] std::string fullText() const;
};

// ─── Model sizes ────────────────────────────────────────────────────────────

enum class WhisperModelSize
{
    Tiny,
    Base,
    Small,
    Medium,
    LargeV3Turbo,
    LargeV2,
    LargeV3,
    Count
};

inline constexpr WhisperModelSize kDefaultWhisperModel = WhisperModelSize::Small;

[[nodiscard]] const char* whisperModelName(WhisperModelSize size) noexcept;
[[nodiscard]] WhisperModelSize whisperModelFromName(const std::string& name) noexcept;

// ─── Callbacks ──────────────────────────────────────────────────────────────

/// Progress callback: (progress_percent 0-100, status_text)
using TranscribeProgressFn = std::function<void(float, const std::string&)>;

/// Completion callback on success.
using TranscribeCompleteFn = std::function<void(TranscriptionResult)>;

/// Error callback on failure.
using TranscribeErrorFn = std::function<void(const std::string&)>;

// ─── Transcriber class ──────────────────────────────────────────────────────

class Transcriber
{
public:
    Transcriber();
    ~Transcriber();

    // Non-copyable
    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;

    // ── Model management ────────────────────────────────────────────────

    /// Set the directory where whisper models are stored (default: "models/"
    /// relative to CWD).  Set this before calling loadModel() to point
    /// at a writable user data directory for auto-downloaded models.
    void setModelsDirectory(const std::string& dir);

    /// Get the current models directory.
    [[nodiscard]] const std::string& modelsDirectory() const noexcept;

    /// Load a whisper model. Downloads from HuggingFace if not cached.
    /// \param size    Model size to load.
    /// \param progress  Optional progress callback.
    /// \return true on success, false on failure.
    bool loadModel(WhisperModelSize size = kDefaultWhisperModel,
                   const TranscribeProgressFn& progress = nullptr);

    /// Unload the current model, freeing memory.
    void unloadModel();

    /// True if a model is loaded and ready.
    [[nodiscard]] bool isModelLoaded() const noexcept;

    /// True if the model is currently loading.
    [[nodiscard]] bool isLoading() const noexcept;

    /// Current loaded model size.
    [[nodiscard]] WhisperModelSize currentModel() const noexcept;

    /// Enable Silero VAD to skip silence while preserving original word offsets.
    /// Enabled by default; falls back to full-audio transcription if unavailable.
    void setVadEnabled(bool enabled) noexcept;

    /// True when VAD is requested for subsequent transcriptions.
    [[nodiscard]] bool isVadEnabled() const noexcept;

    // ── Synchronous transcription ───────────────────────────────────────

    /// Transcribe an audio file. Blocks until complete.
    /// \param audioPath  Path to audio file (.wav, .mp3, .flac, etc.)
    /// \param language   ISO language code ("en", "ja", etc.), empty for auto-detect
    /// \param progress   Optional progress callback
    /// \return Transcription result (empty on failure)
    [[nodiscard]] TranscriptionResult transcribe(
        const std::string& audioPath,
        const std::string& language = "",
        const TranscribeProgressFn& progress = nullptr);

    // ── Asynchronous transcription ──────────────────────────────────────

    /// Transcribe asynchronously on a background thread.
    /// Returns a future that will hold the result.
    [[nodiscard]] std::future<TranscriptionResult> transcribeAsync(
        const std::string& audioPath,
        const std::string& language = "",
        const TranscribeProgressFn& progress = nullptr);

    /// Cancel any running async transcription.
    void cancelAsync();

    /// True if async transcription is in progress.
    [[nodiscard]] bool isTranscribing() const noexcept;

    // ── Device info ─────────────────────────────────────────────────────

    /// True if CUDA acceleration is available.
    [[nodiscard]] bool isCudaAvailable() const noexcept;

    /// Features provided by the compiled transcription backend.
    [[nodiscard]] TranscriberCapabilities capabilities() const noexcept;

    /// Last error message.
    [[nodiscard]] const std::string& lastError() const noexcept;

private:
    /// Semantic split of a large segment based on punctuation/timing gaps.
    void semanticSplit(const TranscriptionSegment& input,
                       std::vector<TranscriptionSegment>& output) const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace rt
