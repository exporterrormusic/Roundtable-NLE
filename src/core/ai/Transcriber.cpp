/*
 * Transcriber.cpp — whisper.cpp integration implementation.
 *
 * When ROUNDTABLE_HAS_WHISPER is defined, uses whisper.cpp for transcription.
 * Otherwise, provides a compilable stub that returns empty results.
 */

#include "ai/Transcriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <urlmon.h>   // URLDownloadToFile
#pragma comment(lib, "urlmon.lib")
#endif

#ifdef ROUNDTABLE_HAS_WHISPER
#include <whisper.h>
#endif

#ifdef ROUNDTABLE_HAS_SNDFILE
#include "audio/AudioFile.h"
#endif

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Utility
// ═══════════════════════════════════════════════════════════════════════════

const char* whisperModelName(WhisperModelSize size) noexcept
{
    switch (size) {
    case WhisperModelSize::Tiny:    return "tiny";
    case WhisperModelSize::Base:    return "base";
    case WhisperModelSize::Small:   return "small";
    case WhisperModelSize::Medium:  return "medium";
    case WhisperModelSize::LargeV2: return "large-v2";
    case WhisperModelSize::LargeV3: return "large-v3";
    default:                        return "unknown";
    }
}

WhisperModelSize whisperModelFromName(const std::string& name) noexcept
{
    if (name == "tiny")     return WhisperModelSize::Tiny;
    if (name == "base")     return WhisperModelSize::Base;
    if (name == "small")    return WhisperModelSize::Small;
    if (name == "medium")   return WhisperModelSize::Medium;
    if (name == "large-v2") return WhisperModelSize::LargeV2;
    if (name == "large-v3") return WhisperModelSize::LargeV3;
    return WhisperModelSize::Base;
}

std::string TranscriptionResult::fullText() const
{
    std::string result;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) result += ' ';
        result += segments[i].text;
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Impl
// ═══════════════════════════════════════════════════════════════════════════

struct Transcriber::Impl
{
#ifdef ROUNDTABLE_HAS_WHISPER
    whisper_context* ctx{nullptr};
#endif
    WhisperModelSize modelSize{WhisperModelSize::Base};
    bool             loaded{false};
    std::atomic<bool> loading{false};
    std::atomic<bool> transcribing{false};
    std::atomic<bool> cancelRequested{false};
    bool             cudaAvailable{false};
    std::string      lastError;
    std::string      modelsDir{"models"};
    mutable std::mutex mu;
};

// ═══════════════════════════════════════════════════════════════════════════
//  Transcriber
// ═══════════════════════════════════════════════════════════════════════════

Transcriber::Transcriber()
    : m_impl(std::make_unique<Impl>())
{
    m_impl->modelsDir = "models";
#ifdef ROUNDTABLE_HAS_CUDA
    m_impl->cudaAvailable = true;
#endif
    spdlog::debug("Transcriber created (whisper: {})",
#ifdef ROUNDTABLE_HAS_WHISPER
                  "enabled"
#else
                  "stub"
#endif
    );
}

Transcriber::~Transcriber()
{
    unloadModel();
}

bool Transcriber::loadModel(WhisperModelSize size, const TranscribeProgressFn& progress)
{
    if (m_impl->loading.load()) return false;

    m_impl->loading.store(true);
    m_impl->lastError.clear();

    if (progress)
        progress(0.0f, std::string("Loading Whisper ") + whisperModelName(size) + " model...");

#ifdef ROUNDTABLE_HAS_WHISPER
    // Unload previous
    if (m_impl->ctx) {
        whisper_free(m_impl->ctx);
        m_impl->ctx = nullptr;
    }

    const std::string modelName = whisperModelName(size);
    const std::string modelFile = m_impl->modelsDir + "/ggml-" + modelName + ".bin";

    // If model not found locally, try downloading from HuggingFace
    if (!std::filesystem::exists(modelFile)) {
        spdlog::info("Transcriber: Model not found at '{}' — attempting download",
                     modelFile);

        if (progress)
            progress(5.0f, std::string("Downloading Whisper ") + modelName + " model (this may take a while)...");

        // Create the models directory
        std::error_code ec;
        std::filesystem::create_directories(m_impl->modelsDir, ec);

        // Download from HuggingFace whisper.cpp repo
        const std::string url = std::string("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-")
                              + modelName + ".bin";

        bool downloadOk = false;
#ifdef _WIN32
        std::wstring wurl(url.begin(), url.end());
        std::wstring wdest(modelFile.begin(), modelFile.end());
        HRESULT hr = URLDownloadToFileW(nullptr, wurl.c_str(), wdest.c_str(), 0, nullptr);
        downloadOk = (hr == S_OK);
        if (!downloadOk) {
            m_impl->lastError = "Download failed (HRESULT: " + std::to_string(hr) + ")";
            spdlog::error("Transcriber: {}", m_impl->lastError);
        }
#else
        m_impl->lastError = "Auto-download only supported on Windows. "
                            "Please manually download the model from:\n  " + url;
        spdlog::error("Transcriber: {}", m_impl->lastError);
#endif

        if (!downloadOk) {
            m_impl->loading.store(false);
            if (progress)
                progress(0.0f, m_impl->lastError);
            return false;
        }

        spdlog::info("Transcriber: Model downloaded to '{}'", modelFile);
        if (progress)
            progress(50.0f, "Model downloaded, loading...");
    }

    // Verify the file exists (should after download or was already there)
    if (!std::filesystem::exists(modelFile)) {
        m_impl->lastError = "Model file not found: " + modelFile;
        spdlog::error("Transcriber: {}", m_impl->lastError);
        m_impl->loading.store(false);
        if (progress)
            progress(0.0f, m_impl->lastError);
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
#ifdef ROUNDTABLE_HAS_CUDA
    cparams.use_gpu = true;
#else
    cparams.use_gpu = false;
#endif

    // DTW (cross-attention) token-level timestamps: far more accurate than the
    // heuristic t0/t1 — the heuristic's token ENDS run long before silence,
    // which pushed AudioSync clip out-points late.  The alignment-heads preset
    // must match the model, so map it from the requested size; if init fails
    // with DTW (e.g. a custom/quantized model the preset doesn't fit), retry
    // without so transcription still works on the heuristic timestamps.
    cparams.dtw_token_timestamps = true;
    switch (size) {
    case WhisperModelSize::Tiny:    cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY;     break;
    case WhisperModelSize::Base:    cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE;     break;
    case WhisperModelSize::Small:   cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL;    break;
    case WhisperModelSize::Medium:  cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM;   break;
    case WhisperModelSize::LargeV2: cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V2; break;
    case WhisperModelSize::LargeV3: cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3; break;
    }

    m_impl->ctx = whisper_init_from_file_with_params(modelFile.c_str(), cparams);
    if (!m_impl->ctx) {
        spdlog::warn("Transcriber: init with DTW timestamps failed for '{}' — retrying without",
                     modelFile);
        cparams.dtw_token_timestamps = false;
        cparams.dtw_aheads_preset    = WHISPER_AHEADS_NONE;
        m_impl->ctx = whisper_init_from_file_with_params(modelFile.c_str(), cparams);
    }
    if (!m_impl->ctx) {
        m_impl->lastError = "Failed to load whisper model: " + modelFile;
        spdlog::error("Transcriber: {}", m_impl->lastError);
        m_impl->loading.store(false);
        return false;
    }

    m_impl->modelSize = size;
    m_impl->loaded = true;
    m_impl->loading.store(false);
    spdlog::info("Transcriber: Loaded model '{}' (GPU: {})",
                 whisperModelName(size), m_impl->cudaAvailable ? "yes" : "no");
    if (progress)
        progress(100.0f, "Model loaded successfully");
    return true;

#else
    // Stub: pretend model is loaded
    m_impl->modelSize = size;
    m_impl->loaded = true;
    m_impl->loading.store(false);
    spdlog::info("Transcriber (stub): Model '{}' marked as loaded", whisperModelName(size));
    if (progress)
        progress(100.0f, "Model loaded (stub mode)");
    return true;
#endif
}

void Transcriber::setModelsDirectory(const std::string& dir)
{
    m_impl->modelsDir = dir;
}

const std::string& Transcriber::modelsDirectory() const noexcept
{
    return m_impl->modelsDir;
}

void Transcriber::unloadModel()
{
#ifdef ROUNDTABLE_HAS_WHISPER
    if (m_impl->ctx) {
        whisper_free(m_impl->ctx);
        m_impl->ctx = nullptr;
    }
#endif
    m_impl->loaded = false;
    spdlog::debug("Transcriber: Model unloaded");
}

bool Transcriber::isModelLoaded() const noexcept { return m_impl->loaded; }
bool Transcriber::isLoading() const noexcept { return m_impl->loading.load(); }
WhisperModelSize Transcriber::currentModel() const noexcept { return m_impl->modelSize; }
bool Transcriber::isCudaAvailable() const noexcept { return m_impl->cudaAvailable; }
const std::string& Transcriber::lastError() const noexcept { return m_impl->lastError; }
bool Transcriber::isTranscribing() const noexcept { return m_impl->transcribing.load(); }

TranscriptionResult Transcriber::transcribe(
    const std::string& audioPath,
    const std::string& language,
    const TranscribeProgressFn& progress)
{
    if (!m_impl->loaded) {
        loadModel(m_impl->modelSize, progress);
        if (!m_impl->loaded) return {};
    }

    if (!std::filesystem::exists(audioPath)) {
        m_impl->lastError = "Audio file not found: " + audioPath;
        spdlog::error("Transcriber: {}", m_impl->lastError);
        return {};
    }

    m_impl->transcribing.store(true);
    m_impl->cancelRequested.store(false);

    if (progress)
        progress(0.0f, "Starting transcription...");

    TranscriptionResult result;

#ifdef ROUNDTABLE_HAS_WHISPER
    // Load audio file using AudioFile, resample to 16kHz mono for whisper
    std::vector<float> samples;
    {
#ifdef ROUNDTABLE_HAS_SNDFILE
        AudioFile audioFile;
        if (!audioFile.open(audioPath)) {
            m_impl->lastError = "Failed to open audio file: " + audioPath;
            spdlog::error("Transcriber: {}", m_impl->lastError);
            m_impl->transcribing.store(false);
            return {};
        }

        if (progress)
            progress(5.0f, "Loading and resampling audio...");

        // Read all samples resampled to 16kHz
        auto resampled = audioFile.readAllResampled(16000);
        if (resampled.empty()) {
            m_impl->lastError = "Failed to read audio samples from: " + audioPath;
            spdlog::error("Transcriber: {}", m_impl->lastError);
            m_impl->transcribing.store(false);
            return {};
        }

        // Mix down to mono if multi-channel
        auto channels = audioFile.info().channels;
        if (channels > 1) {
            size_t monoFrames = resampled.size() / channels;
            samples.resize(monoFrames);
            for (size_t i = 0; i < monoFrames; ++i) {
                float sum = 0.0f;
                for (uint16_t ch = 0; ch < channels; ++ch)
                    sum += resampled[i * channels + ch];
                samples[i] = sum / static_cast<float>(channels);
            }
        } else {
            samples = std::move(resampled);
        }

        result.duration = audioFile.info().duration;

        spdlog::info("Transcriber: Loaded {} mono samples ({:.1f}s) from '{}'",
                     samples.size(), result.duration, audioPath);
#else
        m_impl->lastError = "Audio file loading requires libsndfile (ROUNDTABLE_HAS_SNDFILE)";
        spdlog::error("Transcriber: {}", m_impl->lastError);
        m_impl->transcribing.store(false);
        return {};
#endif
    }

    if (progress)
        progress(10.0f, "Running whisper transcription...");

    // Configure whisper parameters
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.translate        = false;
    wparams.no_timestamps    = false;
    wparams.token_timestamps = true;
    wparams.max_len          = 0; // No max segment length
    wparams.suppress_nst     = true; // no "(sighs)"-style non-speech tokens —
                                     // they became junk words that skewed the
                                     // script alignment's clip boundaries

    // Wire whisper's built-in progress callback so the bar advances smoothly.
    // Whisper reports 0-100 int; we map that to 10-95 float range
    // (0-10 reserved for audio loading, 95-100 for post-processing).
    struct ProgressCtx {
        const TranscribeProgressFn* fn;
    };
    ProgressCtx progressCtx{&progress};

    if (progress) {
        wparams.progress_callback = [](struct whisper_context* /*ctx*/,
                                       struct whisper_state* /*state*/,
                                       int pct, void* user_data) {
            auto* pctx = static_cast<ProgressCtx*>(user_data);
            if (pctx->fn && *pctx->fn) {
                float mapped = 10.0f + static_cast<float>(pct) * 0.85f; // 10..95
                (*pctx->fn)(mapped,
                    "Transcribing... " + std::to_string(static_cast<int>(mapped)) + "%");
            }
        };
        wparams.progress_callback_user_data = &progressCtx;
    }

    if (!language.empty())
        wparams.language = language.c_str();

    if (whisper_full(m_impl->ctx, wparams, samples.data(),
                     static_cast<int>(samples.size())) != 0) {
        m_impl->lastError = "Whisper transcription failed";
        m_impl->transcribing.store(false);
        return {};
    }

    int nSegments = whisper_full_n_segments(m_impl->ctx);
    int64_t prevDtw = -1;   // emit time of the previous token (global to the file)
    for (int i = 0; i < nSegments; ++i) {
        TranscriptionSegment seg;
        seg.id    = i;
        seg.text  = whisper_full_get_segment_text(m_impl->ctx, i);
        seg.start = whisper_full_get_segment_t0(m_impl->ctx, i) / 100.0;
        seg.end   = whisper_full_get_segment_t1(m_impl->ctx, i) / 100.0;

        // Merge whisper's sub-word TOKENS into whole words (a token whose text
        // starts with a space begins a new word; punctuation/continuations
        // attach to the current one).  Raw tokens made poor "words": a script
        // word like "consider" arrived as "cons"+"ider", which could never
        // text-match during alignment and depressed clip confidence.
        //
        // Timestamps prefer DTW when computed (t_dtw = the moment the token was
        // emitted, ≈ its end — accurate) over the heuristic t0/t1, whose ends
        // run long before silence.  A token's start is its heuristic t0 bounded
        // to [previous token's emit, own end] so gaps between words survive but
        // gross early/late starts cannot.
        WordSegment cur;
        bool haveCur = false;
        auto flushWord = [&]() {
            if (haveCur && !cur.word.empty()) {
                if (cur.end <= cur.start) cur.end = cur.start + 0.01;
                seg.words.push_back(std::move(cur));
            }
            cur = WordSegment{};
            haveCur = false;
        };
        int nTokens = whisper_full_n_tokens(m_impl->ctx, i);
        for (int j = 0; j < nTokens; ++j) {
            auto td = whisper_full_get_token_data(m_impl->ctx, i, j);
            const char* tokenText = whisper_full_get_token_text(m_impl->ctx, i, j);
            if (!tokenText || tokenText[0] == '\0' || tokenText[0] == '[')
                continue; // special tokens
            double t0 = td.t0 / 100.0;
            double t1 = td.t1 / 100.0;
            if (td.t_dtw >= 0) {
                t1 = td.t_dtw / 100.0;
                const double lo = (prevDtw >= 0) ? prevDtw / 100.0 : seg.start;
                t0 = std::min(std::max(t0, lo), t1);
                prevDtw = td.t_dtw;
            }
            if (!haveCur || tokenText[0] == ' ') {
                flushWord();
                cur.word        = tokenText;
                cur.start       = t0;
                cur.end         = t1;
                cur.probability = td.p;
                haveCur = true;
            } else {
                cur.word += tokenText;
                cur.end   = std::max(cur.end, t1);
                cur.probability = std::min(cur.probability, td.p);
            }
        }
        flushWord();

        // Semantic split into smaller pieces
        semanticSplit(seg, result.segments);
    }

    if (progress)
        progress(96.0f, "Processing segments...");

    result.language = language.empty() ? "auto" : language;
#else
    // Stub: return empty result
    spdlog::info("Transcriber (stub): transcribe('{}') — returning empty result", audioPath);
    result.language = language.empty() ? "en" : language;
    result.duration = 0.0;
#endif

    // Re-index segments
    for (size_t i = 0; i < result.segments.size(); ++i)
        result.segments[i].id = static_cast<int>(i);

    m_impl->transcribing.store(false);

    if (progress)
        progress(100.0f, "Transcription complete");

    spdlog::info("Transcriber: Transcribed {} segments from '{}'",
                 result.segments.size(), audioPath);
    return result;
}

std::future<TranscriptionResult> Transcriber::transcribeAsync(
    const std::string& audioPath,
    const std::string& language,
    const TranscribeProgressFn& progress)
{
    return std::async(std::launch::async, [this, audioPath, language, progress]() {
        return transcribe(audioPath, language, progress);
    });
}

void Transcriber::cancelAsync()
{
    m_impl->cancelRequested.store(true);
}

// ─── Semantic splitting ─────────────────────────────────────────────────────

static void flushSegment(std::vector<WordSegment>&& words,
                         std::vector<TranscriptionSegment>& output)
{
    if (words.empty()) return;

    TranscriptionSegment sub;
    sub.id    = 0; // Re-indexed later
    sub.start = words.front().start;
    sub.end   = words.back().end;
    sub.words = std::move(words);

    // Build text from words
    for (const auto& w : sub.words)
        sub.text += w.word;

    // Trim leading/trailing whitespace
    while (!sub.text.empty() && std::isspace(static_cast<unsigned char>(sub.text.front())))
        sub.text.erase(sub.text.begin());
    while (!sub.text.empty() && std::isspace(static_cast<unsigned char>(sub.text.back())))
        sub.text.pop_back();

    output.push_back(std::move(sub));
}

void Transcriber::semanticSplit(const TranscriptionSegment& input,
                                std::vector<TranscriptionSegment>& output) const
{
    if (input.words.empty()) {
        output.push_back(input);
        return;
    }

    std::vector<WordSegment> currentWords;

    for (size_t i = 0; i < input.words.size(); ++i) {
        currentWords.push_back(input.words[i]);

        const auto& word = input.words[i];
        std::string text = word.word;

        // Check for sentence-ending punctuation
        bool hasPunc = false;
        for (char c : text) {
            if (c == '.' || c == '!' || c == '?') {
                hasPunc = true;
                break;
            }
        }

        bool isLast = (i == input.words.size() - 1);
        if (!isLast) {
            double gap = input.words[i + 1].start - word.end;

            // Split if: punctuation + decent gap (0.6s), or huge gap alone (2.5s)
            if ((hasPunc && gap > 0.6) || gap > 2.5) {
                flushSegment(std::move(currentWords), output);
                currentWords.clear();
            }
        } else {
            // End of segment — flush remaining words
            flushSegment(std::move(currentWords), output);
            currentWords.clear();
        }
    }
}

} // namespace rt
