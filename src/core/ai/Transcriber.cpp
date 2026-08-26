/*
 * Transcriber.cpp — whisper.cpp integration implementation.
 *
 * When ROUNDTABLE_HAS_WHISPER is defined, uses whisper.cpp for transcription.
 * Otherwise, provides a compilable stub that returns empty results.
 */

#include "ai/Transcriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
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

namespace {

constexpr const char* kVadModelFileName = "ggml-silero-v6.2.0.bin";
constexpr const char* kVadModelUrl =
    "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin";
constexpr double kVadCentisecondsToSamples = 160.0; // 16 kHz / 100

#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
std::string base64Encode(const std::string& input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    uint32_t accumulator = 0;
    int bits = -6;
    for (const unsigned char byte : input) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 0) {
            output.push_back(alphabet[(accumulator >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6)
        output.push_back(alphabet[((accumulator << 8) >> (bits + 8)) & 0x3f]);
    while (output.size() % 4)
        output.push_back('=');
    return output;
}

std::string base64Decode(const std::string& input)
{
    static constexpr unsigned char invalid = 0xff;
    static const auto table = [] {
        std::array<unsigned char, 256> result{};
        result.fill(invalid);
        const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < alphabet.size(); ++i)
            result[static_cast<unsigned char>(alphabet[i])] = static_cast<unsigned char>(i);
        return result;
    }();

    std::string output;
    uint32_t accumulator = 0;
    int bits = -8;
    for (const unsigned char c : input) {
        if (c == '=') break;
        if (table[c] == invalid) continue;
        accumulator = (accumulator << 6) | table[c];
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos
            ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return fields;
}

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::wstring quoteWindowsArg(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(c);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
#endif
#endif

bool downloadFile(const std::string& url, const std::string& destination,
                  std::string& error)
{
#ifdef _WIN32
    std::wstring wurl(url.begin(), url.end());
    std::wstring wdest(destination.begin(), destination.end());
    const HRESULT hr = URLDownloadToFileW(nullptr, wurl.c_str(), wdest.c_str(), 0, nullptr);
    if (hr == S_OK)
        return true;
    error = "Download failed (HRESULT: " + std::to_string(hr) + ")";
#else
    error = "Auto-download is only supported on Windows. Please download: " + url;
#endif
    return false;
}

} // namespace

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
    case WhisperModelSize::LargeV3Turbo: return "large-v3-turbo";
    case WhisperModelSize::LargeV2: return "large-v2";
    case WhisperModelSize::LargeV3: return "large-v3";
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    case WhisperModelSize::CrisperWhisper2Large: return "crisperwhisper-2-large-personal";
#endif
    default:                        return "unknown";
    }
}

WhisperModelSize whisperModelFromName(const std::string& name) noexcept
{
    if (name == "tiny")     return WhisperModelSize::Tiny;
    if (name == "base")     return WhisperModelSize::Base;
    if (name == "small")    return WhisperModelSize::Small;
    if (name == "medium")   return WhisperModelSize::Medium;
    if (name == "large-v3-turbo") return WhisperModelSize::LargeV3Turbo;
    if (name == "large-v2") return WhisperModelSize::LargeV2;
    if (name == "large-v3") return WhisperModelSize::LargeV3;
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    if (name == "crisperwhisper-2-large-personal")
        return WhisperModelSize::CrisperWhisper2Large;
#endif
    return kDefaultWhisperModel;
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
#if defined(ROUNDTABLE_HAS_CRISPERWHISPER) && defined(_WIN32)
    HANDLE crisperProcess{nullptr};
    HANDLE crisperStdin{nullptr};
    HANDLE crisperStdout{nullptr};
    std::string crisperReadBuffer;

    void stopCrisper()
    {
        if (crisperStdin) {
            CloseHandle(crisperStdin);
            crisperStdin = nullptr;
        }
        if (crisperProcess) {
            if (WaitForSingleObject(crisperProcess, 500) == WAIT_TIMEOUT)
                TerminateProcess(crisperProcess, 1);
            CloseHandle(crisperProcess);
            crisperProcess = nullptr;
        }
        if (crisperStdout) {
            CloseHandle(crisperStdout);
            crisperStdout = nullptr;
        }
        crisperReadBuffer.clear();
    }

    bool readCrisperLine(std::string& line)
    {
        while (true) {
            const size_t newline = crisperReadBuffer.find('\n');
            if (newline != std::string::npos) {
                line = crisperReadBuffer.substr(0, newline);
                crisperReadBuffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }

            if (cancelRequested.load(std::memory_order_acquire)) {
                lastError = "Transcription cancelled";
                stopCrisper();
                return false;
            }

            DWORD available = 0;
            if (!crisperStdout ||
                !PeekNamedPipe(crisperStdout, nullptr, 0, nullptr, &available, nullptr)) {
                lastError = "CrisperWhisper worker pipe closed unexpectedly";
                stopCrisper();
                return false;
            }
            if (available > 0) {
                char buffer[4096];
                DWORD read = 0;
                const DWORD amount = std::min<DWORD>(available, sizeof(buffer));
                if (!ReadFile(crisperStdout, buffer, amount, &read, nullptr) || read == 0) {
                    lastError = "Failed reading from the CrisperWhisper worker";
                    stopCrisper();
                    return false;
                }
                crisperReadBuffer.append(buffer, read);
                continue;
            }

            DWORD exitCode = STILL_ACTIVE;
            if (!crisperProcess || !GetExitCodeProcess(crisperProcess, &exitCode) ||
                exitCode != STILL_ACTIVE) {
                lastError = "CrisperWhisper worker exited before returning a result";
                stopCrisper();
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    bool startCrisper()
    {
        stopCrisper();

        if (!std::filesystem::exists(ROUNDTABLE_CRISPERWHISPER_WORKER_PATH)) {
            lastError = std::string("CrisperWhisper worker script not found: ")
                + ROUNDTABLE_CRISPERWHISPER_WORKER_PATH;
            return false;
        }

        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE childStdoutWrite = nullptr;
        HANDLE childStdinRead = nullptr;
        HANDLE childStderr = nullptr;
        if (!CreatePipe(&crisperStdout, &childStdoutWrite, &security, 0) ||
            !SetHandleInformation(crisperStdout, HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(&childStdinRead, &crisperStdin, &security, 0) ||
            !SetHandleInformation(crisperStdin, HANDLE_FLAG_INHERIT, 0)) {
            lastError = "Unable to create CrisperWhisper worker pipes";
            if (childStdoutWrite) CloseHandle(childStdoutWrite);
            if (childStdinRead) CloseHandle(childStdinRead);
            stopCrisper();
            return false;
        }

        wchar_t pythonBuffer[32768]{};
        const DWORD pythonLength = GetEnvironmentVariableW(
            L"ROUNDTABLE_CRISPERWHISPER_PYTHON", pythonBuffer,
            static_cast<DWORD>(std::size(pythonBuffer)));
        std::wstring python;
        if (pythonLength > 0 && pythonLength < std::size(pythonBuffer)) {
            python.assign(pythonBuffer, pythonLength);
        } else if (std::filesystem::exists(ROUNDTABLE_CRISPERWHISPER_PYTHON_PATH)) {
            python = utf8ToWide(ROUNDTABLE_CRISPERWHISPER_PYTHON_PATH);
        } else {
            python = L"python";
        }
        const std::wstring worker = utf8ToWide(ROUNDTABLE_CRISPERWHISPER_WORKER_PATH);
        std::wstring command = quoteWindowsArg(python) + L" -u " + quoteWindowsArg(worker);

        childStderr = CreateFileW(L"NUL", GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childStdinRead;
        startup.hStdOutput = childStdoutWrite;
        startup.hStdError = childStderr != INVALID_HANDLE_VALUE
            ? childStderr : childStdoutWrite;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process);
        CloseHandle(childStdinRead);
        CloseHandle(childStdoutWrite);
        if (childStderr && childStderr != INVALID_HANDLE_VALUE)
            CloseHandle(childStderr);
        if (!created) {
            lastError = "Unable to start Python for CrisperWhisper (Windows error "
                + std::to_string(GetLastError()) + ")";
            stopCrisper();
            return false;
        }
        CloseHandle(process.hThread);
        crisperProcess = process.hProcess;

        while (true) {
            std::string line;
            if (!readCrisperLine(line)) return false;
            const auto fields = splitTabs(line);
            if (!fields.empty() && fields[0] == "READY") return true;
            if (!fields.empty() && fields[0] == "ERROR") {
                lastError = fields.size() >= 2
                    ? base64Decode(fields[1])
                    : "CrisperWhisper worker failed during startup";
                stopCrisper();
                return false;
            }
            // Ignore incidental third-party stdout produced while importing or
            // downloading; protocol records are the only authoritative lines.
        }
    }

    bool sendCrisperRequest(const std::string& audioPath, const std::string& language)
    {
        const std::string request = "TRANSCRIBE\t" + base64Encode(audioPath)
            + "\t" + base64Encode(language) + "\n";
        DWORD written = 0;
        if (!crisperStdin ||
            !WriteFile(crisperStdin, request.data(), static_cast<DWORD>(request.size()),
                       &written, nullptr) || written != request.size()) {
            lastError = "Unable to send audio to the CrisperWhisper worker";
            stopCrisper();
            return false;
        }
        return true;
    }
#endif
    WhisperModelSize modelSize{kDefaultWhisperModel};
    bool             loaded{false};
    std::atomic<bool> loading{false};
    std::atomic<bool> transcribing{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> vadEnabled{true};
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
#ifdef ROUNDTABLE_HAS_WHISPER_CUDA
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

#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    if (size == WhisperModelSize::CrisperWhisper2Large) {
        m_impl->cancelRequested.store(false);
        if (progress)
            progress(0.0f, "Starting CrisperWhisper 2 Large (first use downloads the model)...");
#ifdef ROUNDTABLE_HAS_WHISPER
        if (m_impl->ctx) {
            whisper_free(m_impl->ctx);
            m_impl->ctx = nullptr;
        }
#endif
#ifdef _WIN32
        if (!m_impl->startCrisper()) {
            m_impl->loaded = false;
            m_impl->loading.store(false);
            if (progress) progress(0.0f, m_impl->lastError);
            spdlog::error("Transcriber: {}", m_impl->lastError);
            return false;
        }
        m_impl->modelSize = size;
        m_impl->loaded = true;
        m_impl->loading.store(false);
        if (progress) progress(100.0f, "CrisperWhisper 2 Large ready");
        spdlog::info("Transcriber: CrisperWhisper 2 Large worker ready");
        return true;
#else
        m_impl->lastError = "The personal CrisperWhisper adapter currently supports Windows only";
        m_impl->loaded = false;
        m_impl->loading.store(false);
        if (progress) progress(0.0f, m_impl->lastError);
        return false;
#endif
    }
#endif

    if (progress)
        progress(0.0f, std::string("Loading Whisper ") + whisperModelName(size) + " model...");

#ifdef ROUNDTABLE_HAS_WHISPER
#if defined(ROUNDTABLE_HAS_CRISPERWHISPER) && defined(_WIN32)
    m_impl->stopCrisper();
#endif
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

        const bool downloadOk = downloadFile(url, modelFile, m_impl->lastError);
        if (!downloadOk)
            spdlog::error("Transcriber: {}", m_impl->lastError);

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

    // VAD is deliberately optional: a missing network connection must not make
    // the main Whisper model unusable. The transcription path falls back to
    // processing the full audio when this tiny companion model is unavailable.
    const std::string vadModelFile = m_impl->modelsDir + "/" + kVadModelFileName;
    if (m_impl->vadEnabled.load() && !std::filesystem::exists(vadModelFile)) {
        if (progress)
            progress(55.0f, "Downloading Silero voice-activity model...");
        std::string vadError;
        if (downloadFile(kVadModelUrl, vadModelFile, vadError)) {
            spdlog::info("Transcriber: VAD model downloaded to '{}'", vadModelFile);
        } else {
            spdlog::warn("Transcriber: VAD model unavailable ({}); full-audio fallback will be used",
                         vadError);
        }
    }

    whisper_context_params cparams = whisper_context_default_params();
#ifdef ROUNDTABLE_HAS_WHISPER_CUDA
    cparams.use_gpu = true;
#else
    cparams.use_gpu = false;
#endif
    // whisper.cpp 1.8 enables Flash Attention by default, but it cannot expose
    // the cross-attention weights required by DTW. Timing accuracy matters more
    // than this optimization for caption and clip alignment.
    cparams.flash_attn = false;

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
    case WhisperModelSize::LargeV3Turbo:
        cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;
        break;
    case WhisperModelSize::LargeV2: cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V2; break;
    case WhisperModelSize::LargeV3: cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3; break;
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    case WhisperModelSize::CrisperWhisper2Large: break; // handled above
#endif
    case WhisperModelSize::Count: break;
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
    m_impl->lastError = "Whisper transcription backend is not available in this build";
    m_impl->loaded = false;
    m_impl->loading.store(false);
    spdlog::error("Transcriber: {}", m_impl->lastError);
    if (progress)
        progress(0.0f, m_impl->lastError);
    return false;
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
#if defined(ROUNDTABLE_HAS_CRISPERWHISPER) && defined(_WIN32)
    m_impl->stopCrisper();
#endif
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
void Transcriber::setVadEnabled(bool enabled) noexcept { m_impl->vadEnabled.store(enabled); }
bool Transcriber::isVadEnabled() const noexcept { return m_impl->vadEnabled.load(); }
bool Transcriber::isCudaAvailable() const noexcept { return m_impl->cudaAvailable; }
TranscriberCapabilities Transcriber::capabilities() const noexcept
{
    TranscriberCapabilities result;
#if defined(ROUNDTABLE_HAS_WHISPER) || defined(ROUNDTABLE_HAS_CRISPERWHISPER)
    result.available = true;
    result.segmentTiming = true;
    result.wordTiming = true;
    result.confidence = true;
    result.languageDetection = true;
    result.gpu = m_impl->cudaAvailable
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
        || m_impl->modelSize == WhisperModelSize::CrisperWhisper2Large
#endif
        ;
    result.cancellation = true;
#endif
    return result;
}
const std::string& Transcriber::lastError() const noexcept { return m_impl->lastError; }
bool Transcriber::isTranscribing() const noexcept { return m_impl->transcribing.load(); }

TranscriptionResult Transcriber::transcribe(
    const std::string& audioPath,
    const std::string& language,
    const TranscribeProgressFn& progress)
{
    m_impl->lastError.clear();
    auto failure = [&](TranscriptionStatus status, TranscriptionErrorCode code) {
        TranscriptionResult failed;
        failed.status = status;
        failed.error = {code, m_impl->lastError};
        return failed;
    };

    if (!capabilities().available) {
        m_impl->lastError = "Whisper transcription backend is not available in this build";
        return failure(TranscriptionStatus::Unavailable,
                       TranscriptionErrorCode::BackendUnavailable);
    }

    if (!m_impl->loaded) {
        loadModel(m_impl->modelSize, progress);
        if (!m_impl->loaded)
            return failure(TranscriptionStatus::Unavailable,
                           TranscriptionErrorCode::ModelUnavailable);
    }

    if (!std::filesystem::exists(audioPath)) {
        m_impl->lastError = "Audio file not found: " + audioPath;
        spdlog::error("Transcriber: {}", m_impl->lastError);
        return failure(TranscriptionStatus::Failed,
                       TranscriptionErrorCode::AudioNotFound);
    }

    m_impl->transcribing.store(true);
    m_impl->cancelRequested.store(false);

    if (progress)
        progress(0.0f, "Starting transcription...");

    TranscriptionResult result;

#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    if (m_impl->modelSize == WhisperModelSize::CrisperWhisper2Large) {
#ifdef _WIN32
        if (progress)
            progress(10.0f, "Running CrisperWhisper 2 Large...");
        if (!m_impl->crisperProcess && !m_impl->startCrisper()) {
            m_impl->transcribing.store(false);
            return failure(TranscriptionStatus::Unavailable,
                           TranscriptionErrorCode::BackendUnavailable);
        }
        if (!m_impl->sendCrisperRequest(audioPath, language)) {
            m_impl->transcribing.store(false);
            return failure(TranscriptionStatus::Failed,
                           TranscriptionErrorCode::InferenceFailed);
        }

        TranscriptionSegment complete;
        bool receivedBegin = false;
        bool receivedEnd = false;
        while (!receivedEnd) {
            std::string line;
            if (!m_impl->readCrisperLine(line)) {
                const bool cancelled = m_impl->cancelRequested.load(std::memory_order_acquire);
                m_impl->transcribing.store(false);
                return failure(cancelled ? TranscriptionStatus::Cancelled
                                         : TranscriptionStatus::Failed,
                               cancelled ? TranscriptionErrorCode::Cancelled
                                         : TranscriptionErrorCode::InferenceFailed);
            }

            const auto fields = splitTabs(line);
            if (fields.empty()) continue;
            if (fields[0] == "ERROR") {
                m_impl->lastError = fields.size() >= 2
                    ? base64Decode(fields[1]) : "CrisperWhisper transcription failed";
                m_impl->transcribing.store(false);
                return failure(TranscriptionStatus::Failed,
                               TranscriptionErrorCode::InferenceFailed);
            }
            if (fields[0] == "BEGIN" && fields.size() >= 3) {
                result.language = base64Decode(fields[1]);
                try {
                    result.duration = std::stod(fields[2]);
                } catch (...) {
                    result.duration = 0.0;
                }
                receivedBegin = true;
                if (progress) progress(95.0f, "Receiving word-level timestamps...");
            } else if (fields[0] == "WORD" && fields.size() >= 4 && receivedBegin) {
                WordSegment word;
                try {
                    word.start = std::stod(fields[1]);
                    word.end = std::stod(fields[2]);
                } catch (...) {
                    continue;
                }
                word.word = base64Decode(fields[3]);
                if (!complete.words.empty() && !word.word.empty() && word.word.front() != ' ')
                    word.word.insert(word.word.begin(), ' ');
                word.probability = 1.0f;
                complete.words.push_back(std::move(word));
            } else if (fields[0] == "END" && receivedBegin) {
                receivedEnd = true;
            }
        }

        if (!complete.words.empty()) {
            complete.start = complete.words.front().start;
            complete.end = complete.words.back().end;
            for (const auto& word : complete.words)
                complete.text += word.word;
            semanticSplit(complete, result.segments);
        }
#else
        m_impl->lastError = "The personal CrisperWhisper adapter currently supports Windows only";
        m_impl->transcribing.store(false);
        return failure(TranscriptionStatus::Unavailable,
                       TranscriptionErrorCode::BackendUnavailable);
#endif
    } else
#endif
    {
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
            return failure(TranscriptionStatus::Failed,
                           TranscriptionErrorCode::AudioDecodeFailed);
        }

        if (progress)
            progress(5.0f, "Loading and resampling audio...");

        // Read all samples resampled to 16kHz
        auto resampled = audioFile.readAllResampled(16000);
        if (resampled.empty()) {
            m_impl->lastError = "Failed to read audio samples from: " + audioPath;
            spdlog::error("Transcriber: {}", m_impl->lastError);
            m_impl->transcribing.store(false);
            return failure(TranscriptionStatus::Failed,
                           TranscriptionErrorCode::AudioDecodeFailed);
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
        return failure(TranscriptionStatus::Unavailable,
                       TranscriptionErrorCode::BackendUnavailable);
#endif
    }

    if (m_impl->cancelRequested.load(std::memory_order_acquire)) {
        m_impl->lastError = "Transcription cancelled";
        m_impl->transcribing.store(false);
        return failure(TranscriptionStatus::Cancelled,
                       TranscriptionErrorCode::Cancelled);
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
    wparams.abort_callback = [](void* userData) {
        return static_cast<std::atomic<bool>*>(userData)->load(
            std::memory_order_acquire);
    };
    wparams.abort_callback_user_data = &m_impl->cancelRequested;
    wparams.suppress_nst     = true; // no "(sighs)"-style non-speech tokens —
                                     // they became junk words that skewed the
                                     // script alignment's clip boundaries

    // Whisper reports 0-100 for each call. VAD can produce multiple calls, so
    // the callback's base/span are updated for each original-audio range.
    struct ProgressCtx {
        const TranscribeProgressFn* fn;
        float base{10.0f};
        float span{85.0f};
    };
    ProgressCtx progressCtx{&progress};

    if (progress) {
        wparams.progress_callback = [](struct whisper_context* /*ctx*/,
                                       struct whisper_state* /*state*/,
                                       int pct, void* user_data) {
            auto* pctx = static_cast<ProgressCtx*>(user_data);
            if (pctx->fn && *pctx->fn) {
                const float mapped = pctx->base + pctx->span * static_cast<float>(pct) / 100.0f;
                (*pctx->fn)(mapped,
                    "Transcribing... " + std::to_string(static_cast<int>(mapped)) + "%");
            }
        };
        wparams.progress_callback_user_data = &progressCtx;
    }

    if (!language.empty())
        wparams.language = language.c_str();

    // Extract the just-completed whisper call, adding the original-audio offset
    // when the call covered one VAD speech range. This avoids whisper.cpp's
    // built-in VAD path, which remaps segment times but not token/word times.
    auto appendWhisperSegments = [&](double timeOffset) {
        const int nSegments = whisper_full_n_segments(m_impl->ctx);
        int64_t prevDtw = -1;
        for (int i = 0; i < nSegments; ++i) {
            TranscriptionSegment seg;
            seg.id    = static_cast<int>(result.segments.size());
            seg.text  = whisper_full_get_segment_text(m_impl->ctx, i);
            seg.start = timeOffset + whisper_full_get_segment_t0(m_impl->ctx, i) / 100.0;
            seg.end   = timeOffset + whisper_full_get_segment_t1(m_impl->ctx, i) / 100.0;

            // Merge whisper sub-word tokens into whole words. DTW supplies the
            // end time; the range offset keeps every word on the source clock.
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

            const int nTokens = whisper_full_n_tokens(m_impl->ctx, i);
            for (int j = 0; j < nTokens; ++j) {
                const auto td = whisper_full_get_token_data(m_impl->ctx, i, j);
                const char* tokenText = whisper_full_get_token_text(m_impl->ctx, i, j);
                if (!tokenText || tokenText[0] == '\0' || tokenText[0] == '[')
                    continue;

                double t0 = timeOffset + td.t0 / 100.0;
                double t1 = timeOffset + td.t1 / 100.0;
                if (td.t_dtw >= 0) {
                    t1 = timeOffset + td.t_dtw / 100.0;
                    const double lo = prevDtw >= 0
                        ? timeOffset + prevDtw / 100.0
                        : seg.start;
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
                    cur.end = std::max(cur.end, t1);
                    cur.probability = std::min(cur.probability, td.p);
                }
            }
            flushWord();
            semanticSplit(seg, result.segments);
        }
    };

    struct SpeechRange {
        int startSample;
        int endSample;
    };
    std::vector<SpeechRange> speechRanges;
    bool vadSucceeded = false;
    const std::string vadModelFile = m_impl->modelsDir + "/" + kVadModelFileName;

    if (m_impl->vadEnabled.load() && std::filesystem::exists(vadModelFile)) {
        if (progress)
            progress(10.0f, "Detecting speech regions...");

        whisper_vad_context_params vadContextParams = whisper_vad_default_context_params();
#ifdef ROUNDTABLE_HAS_WHISPER_CUDA
        vadContextParams.use_gpu = true;
#else
        vadContextParams.use_gpu = false;
#endif
        whisper_vad_context* vadContext =
            whisper_vad_init_from_file_with_params(vadModelFile.c_str(), vadContextParams);
        if (vadContext) {
            whisper_vad_params vadParams = whisper_vad_default_params();
            whisper_vad_segments* vadSegments = whisper_vad_segments_from_samples(
                vadContext, vadParams, samples.data(), static_cast<int>(samples.size()));
            if (vadSegments) {
                vadSucceeded = true;
                const int count = whisper_vad_segments_n_segments(vadSegments);
                speechRanges.reserve(static_cast<size_t>(count));
                const int sampleCount = static_cast<int>(samples.size());
                for (int i = 0; i < count; ++i) {
                    const auto start = static_cast<int>(std::llround(
                        whisper_vad_segments_get_segment_t0(vadSegments, i)
                        * kVadCentisecondsToSamples));
                    const auto end = static_cast<int>(std::llround(
                        whisper_vad_segments_get_segment_t1(vadSegments, i)
                        * kVadCentisecondsToSamples));
                    const int clampedStart = std::clamp(start, 0, sampleCount);
                    const int clampedEnd = std::clamp(end, clampedStart, sampleCount);
                    if (clampedEnd > clampedStart)
                        speechRanges.push_back({clampedStart, clampedEnd});
                }
                whisper_vad_free_segments(vadSegments);
            } else {
                spdlog::warn("Transcriber: VAD detection failed; transcribing full audio");
            }
            whisper_vad_free(vadContext);
        } else {
            spdlog::warn("Transcriber: Could not load VAD model '{}'; transcribing full audio",
                         vadModelFile);
        }
    }

    bool transcriptionOk = true;
    if (vadSucceeded) {
        int64_t totalSpeechSamples = 0;
        for (const auto& range : speechRanges)
            totalSpeechSamples += range.endSample - range.startSample;

        int64_t completedSamples = 0;
        for (const auto& range : speechRanges) {
            if (m_impl->cancelRequested.load(std::memory_order_acquire)) {
                transcriptionOk = false;
                break;
            }
            const int rangeSamples = range.endSample - range.startSample;
            progressCtx.base = 10.0f + 85.0f * static_cast<float>(completedSamples)
                                           / static_cast<float>(totalSpeechSamples);
            progressCtx.span = 85.0f * static_cast<float>(rangeSamples)
                                      / static_cast<float>(totalSpeechSamples);
            if (whisper_full(m_impl->ctx, wparams, samples.data() + range.startSample,
                             rangeSamples) != 0) {
                transcriptionOk = false;
                break;
            }
            appendWhisperSegments(static_cast<double>(range.startSample) / 16000.0);
            completedSamples += rangeSamples;
        }
        spdlog::info("Transcriber: VAD selected {} speech range(s), {:.1f}s of {:.1f}s",
                     speechRanges.size(), static_cast<double>(totalSpeechSamples) / 16000.0,
                     result.duration);
    } else {
        progressCtx.base = 10.0f;
        progressCtx.span = 85.0f;
        transcriptionOk = whisper_full(m_impl->ctx, wparams, samples.data(),
                                       static_cast<int>(samples.size())) == 0;
        if (transcriptionOk)
            appendWhisperSegments(0.0);
    }

    if (!transcriptionOk) {
        const bool cancelled = m_impl->cancelRequested.load(std::memory_order_acquire);
        m_impl->lastError = cancelled
            ? "Transcription cancelled"
            : "Whisper transcription failed";
        m_impl->transcribing.store(false);
        return failure(cancelled ? TranscriptionStatus::Cancelled
                                 : TranscriptionStatus::Failed,
                       cancelled ? TranscriptionErrorCode::Cancelled
                                 : TranscriptionErrorCode::InferenceFailed);
    }

    if (progress)
        progress(96.0f, "Processing segments...");

    result.language = language.empty() ? "auto" : language;
#else
    // Stub: return empty result
    spdlog::info("Transcriber (stub): transcribe('{}') — returning empty result", audioPath);
    m_impl->lastError = "Whisper transcription backend is not available in this build";
    m_impl->transcribing.store(false);
    return failure(TranscriptionStatus::Unavailable,
                   TranscriptionErrorCode::BackendUnavailable);
#endif
    }

    // Re-index segments
    for (size_t i = 0; i < result.segments.size(); ++i)
        result.segments[i].id = static_cast<int>(i);

    m_impl->transcribing.store(false);
    result.status = result.segments.empty()
        ? TranscriptionStatus::NoSpeech
        : TranscriptionStatus::Success;

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
