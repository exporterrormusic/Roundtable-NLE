/*
 * AudioFile.cpp — audio file loading via libsndfile + FFmpeg fallback.
 */

#include "audio/AudioFile.h"
#include "PathUtils.h"
#include "decode/SharedFileIO.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// ── libsndfile ──────────────────────────────────────────────────────────────
#ifdef ROUNDTABLE_HAS_SNDFILE
#include <sndfile.h>
#endif

// ── FFmpeg fallback ─────────────────────────────────────────────────────────
#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}
#endif

// Shared-mode file open helpers (used by both FFmpeg and libsndfile
// backends, so the includes live outside the FFmpeg-only guard).
#include <cstdio>
#include <cerrno>
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <windows.h>
#endif

namespace rt {

namespace {

// Shared-mode file open — bypass MSVCRT on Windows (raw HANDLE + Win32
// ReadFile / SetFilePointerEx) so FILE_SHARE_DELETE is GUARANTEED to be
// honored. Earlier _fdopen-based approach worked in theory but at least
// one MSVCRT path strips the share state once the CRT wraps the
// descriptor, which is why the lock persisted after we thought we'd
// fixed it. The HANDLE we hold here is the only kernel object for the
// open and its share mask never changes.
//
// On POSIX this collapses to a regular FILE* — POSIX has no exclusive
// open by default, so there's nothing to fix there.


#ifdef ROUNDTABLE_HAS_SNDFILE
// libsndfile virtual IO bound to a raw Win32 HANDLE — bypasses MSVCRT
// entirely so FILE_SHARE_DELETE / FILE_SHARE_WRITE are kept on the
// kernel object for the lifetime of the open. (Reusing the same raw-
// HANDLE pattern as the FFmpeg path above.)

sf_count_t sfVioGetFilelen(void* ud) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(ud);
    LARGE_INTEGER sz;
    if (!::GetFileSizeEx(h, &sz)) return -1;
    return static_cast<sf_count_t>(sz.QuadPart);
#else
    FILE* fp = static_cast<FILE*>(ud);
    long cur = std::ftell(fp);
    std::fseek(fp, 0, SEEK_END);
    long end = std::ftell(fp);
    std::fseek(fp, cur, SEEK_SET);
    return end;
#endif
}
sf_count_t sfVioSeek(sf_count_t off, int whence, void* ud) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(ud);
    DWORD mode = FILE_BEGIN;
    switch (whence) {
        case SEEK_SET: mode = FILE_BEGIN;   break;
        case SEEK_CUR: mode = FILE_CURRENT; break;
        case SEEK_END: mode = FILE_END;     break;
        default: return -1;
    }
    LARGE_INTEGER liOff;    liOff.QuadPart    = off;
    LARGE_INTEGER liNewPos; liNewPos.QuadPart = 0;
    if (!::SetFilePointerEx(h, liOff, &liNewPos, mode)) return -1;
    return static_cast<sf_count_t>(liNewPos.QuadPart);
#else
    FILE* fp = static_cast<FILE*>(ud);
    if (std::fseek(fp, static_cast<long>(off), whence) != 0) return -1;
    return std::ftell(fp);
#endif
}
sf_count_t sfVioRead(void* ptr, sf_count_t count, void* ud) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(ud);
    DWORD got = 0;
    if (!::ReadFile(h, ptr, static_cast<DWORD>(count), &got, nullptr)) return -1;
    return static_cast<sf_count_t>(got);
#else
    FILE* fp = static_cast<FILE*>(ud);
    return static_cast<sf_count_t>(
        std::fread(ptr, 1, static_cast<size_t>(count), fp));
#endif
}
sf_count_t sfVioTell(void* ud) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(ud);
    LARGE_INTEGER zero; zero.QuadPart = 0;
    LARGE_INTEGER pos;  pos.QuadPart  = 0;
    if (!::SetFilePointerEx(h, zero, &pos, FILE_CURRENT)) return -1;
    return static_cast<sf_count_t>(pos.QuadPart);
#else
    FILE* fp = static_cast<FILE*>(ud);
    return std::ftell(fp);
#endif
}
#endif // ROUNDTABLE_HAS_SNDFILE


std::vector<float> resampleInterleavedLinear(const std::vector<float>& samples,
                                             uint16_t channels,
                                             uint32_t srcSampleRate,
                                             uint32_t dstSampleRate)
{
    if (samples.empty() || channels == 0 || srcSampleRate == 0 ||
        srcSampleRate == dstSampleRate) {
        return samples;
    }

    const int64_t srcFrames = static_cast<int64_t>(samples.size() / channels);
    if (srcFrames <= 0) {
        return {};
    }

    const double ratio = static_cast<double>(dstSampleRate) / srcSampleRate;
    const int64_t dstFrames = static_cast<int64_t>(std::ceil(srcFrames * ratio));
    std::vector<float> resampled(static_cast<size_t>(dstFrames * channels));

    for (int64_t d = 0; d < dstFrames; ++d) {
        const double srcPos = d / ratio;
        const int64_t s0 = static_cast<int64_t>(srcPos);
        const int64_t s1 = std::min(s0 + 1, srcFrames - 1);
        const float frac = static_cast<float>(srcPos - s0);

        for (uint16_t c = 0; c < channels; ++c) {
            const float v0 = samples[static_cast<size_t>(s0 * channels + c)];
            const float v1 = samples[static_cast<size_t>(s1 * channels + c)];
            resampled[static_cast<size_t>(d * channels + c)] = v0 + frac * (v1 - v0);
        }
    }

    return resampled;
}

#ifdef ROUNDTABLE_HAS_FFMPEG
/// Resample an interleaved float buffer via swresample (windowed-sinc).
/// Same contract as resampleInterleavedLinear: output frame d corresponds to
/// source position d * srcRate / dstRate, length ≈ ceil(srcFrames * ratio).
/// The polyphase filter has a short warm-up transient at the start of the
/// buffer (zero-padded history) — callers that extract a sub-region must
/// decode with a pre-roll margin (kResampleMarginFrames) so the transient
/// never lands inside the extracted window.
std::vector<float> resampleInterleavedSwr(const std::vector<float>& samples,
                                          uint16_t channels,
                                          uint32_t srcSampleRate,
                                          uint32_t dstSampleRate)
{
    if (samples.empty() || channels == 0 || srcSampleRate == 0 ||
        srcSampleRate == dstSampleRate) {
        return samples;
    }

    const int64_t srcFrames = static_cast<int64_t>(samples.size() / channels);
    if (srcFrames <= 0) {
        return {};
    }

    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);

    SwrContext* ctx = nullptr;
    if (swr_alloc_set_opts2(&ctx,
                            &layout, AV_SAMPLE_FMT_FLT, static_cast<int>(dstSampleRate),
                            &layout, AV_SAMPLE_FMT_FLT, static_cast<int>(srcSampleRate),
                            0, nullptr) < 0 || !ctx || swr_init(ctx) < 0) {
        if (ctx) swr_free(&ctx);
        av_channel_layout_uninit(&layout);
        return resampleInterleavedLinear(samples, channels, srcSampleRate, dstSampleRate);
    }

    const int64_t maxOutFrames =
        av_rescale_rnd(srcFrames, dstSampleRate, srcSampleRate, AV_ROUND_UP) + 64;
    std::vector<float> out(static_cast<size_t>(maxOutFrames * channels));

    const uint8_t* inPlanes  = reinterpret_cast<const uint8_t*>(samples.data());
    uint8_t*       outPlanes = reinterpret_cast<uint8_t*>(out.data());
    int total = swr_convert(ctx, &outPlanes, static_cast<int>(maxOutFrames),
                            &inPlanes, static_cast<int>(srcFrames));
    if (total < 0) {
        swr_free(&ctx);
        av_channel_layout_uninit(&layout);
        return resampleInterleavedLinear(samples, channels, srcSampleRate, dstSampleRate);
    }

    // Drain the filter tail so the final frames are not silently dropped.
    if (total < maxOutFrames) {
        uint8_t* tail = reinterpret_cast<uint8_t*>(out.data() +
                            static_cast<size_t>(total) * channels);
        const int drained = swr_convert(ctx, &tail,
                                        static_cast<int>(maxOutFrames - total),
                                        nullptr, 0);
        if (drained > 0) total += drained;
    }

    swr_free(&ctx);
    av_channel_layout_uninit(&layout);

    // Preserve the linear resampler's length contract so the position math
    // in extractResampledRegion keeps lining up across chunk boundaries.
    const int64_t expected = static_cast<int64_t>(std::ceil(
        srcFrames * static_cast<double>(dstSampleRate) / srcSampleRate));
    out.resize(static_cast<size_t>(std::min<int64_t>(total, expected) * channels));
    return out;
}
#endif // ROUNDTABLE_HAS_FFMPEG

/// Source-frame pre/post-roll decoded around a resampled window so the
/// swr filter warm-up/tail transients fall outside the extracted region.
/// swresample's FIR spans a few dozen taps; 256 frames is comfortably past it.
constexpr int64_t kResampleMarginFrames = 256;

/// Rate-convert an interleaved float buffer.  swresample when available
/// (windowed-sinc — no HF roll-off/aliasing), linear interpolation otherwise.
std::vector<float> resampleInterleaved(const std::vector<float>& samples,
                                       uint16_t channels,
                                       uint32_t srcSampleRate,
                                       uint32_t dstSampleRate)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    return resampleInterleavedSwr(samples, channels, srcSampleRate, dstSampleRate);
#else
    return resampleInterleavedLinear(samples, channels, srcSampleRate, dstSampleRate);
#endif
}

/// Extract the requested region from a resampled buffer.
/// `srcStart` is the source-frame position that was decoded, and `srcSampleRate`/`targetSampleRate`
/// define the resampling ratio. Returns the number of frames written to `outSamples`.
int64_t extractResampledRegion(std::vector<float>& resampled,
                               uint16_t channels,
                               int64_t srcStart,
                               uint32_t srcSampleRate,
                               uint32_t targetSampleRate,
                               int64_t startFrame,
                               int64_t numFrames,
                               std::vector<float>& outSamples)
{
    const int64_t resampledRegionStart = static_cast<int64_t>(
        std::llround(srcStart * static_cast<double>(targetSampleRate) / srcSampleRate));
    const int64_t offsetFrames = std::max<int64_t>(0, startFrame - resampledRegionStart);
    const int64_t availableFrames = static_cast<int64_t>(resampled.size() / channels);
    if (offsetFrames >= availableFrames) {
        outSamples.clear();
        return 0;
    }

    const int64_t framesToCopy = std::min<int64_t>(numFrames, availableFrames - offsetFrames);
    const size_t sampleOffset = static_cast<size_t>(offsetFrames * channels);
    const size_t sampleCount = static_cast<size_t>(framesToCopy * channels);
    outSamples.resize(sampleCount);
    std::memcpy(outSamples.data(), resampled.data() + sampleOffset,
                sampleCount * sizeof(float));
    return framesToCopy;
}

#ifdef ROUNDTABLE_HAS_FFMPEG
/// Convert a decoded AVFrame to interleaved float samples via swr_convert,
/// appending the result to `outBuffer`. Returns number of frames converted.
int convertAndAppendFrame(SwrContext* swrCtx, AVFrame* decodedFrame,
                          uint16_t channels, std::vector<float>& outBuffer)
{
    const int outSamples = swr_get_out_samples(swrCtx, decodedFrame->nb_samples);
    std::vector<float> tmp(static_cast<size_t>(outSamples * channels));
    uint8_t* outBuf = reinterpret_cast<uint8_t*>(tmp.data());

    const int converted = swr_convert(swrCtx, &outBuf, outSamples,
                                      const_cast<const uint8_t**>(decodedFrame->extended_data),
                                      decodedFrame->nb_samples);
    if (converted > 0) {
        tmp.resize(static_cast<size_t>(converted * channels));
        outBuffer.insert(outBuffer.end(), tmp.begin(), tmp.end());
    }
    av_frame_unref(decodedFrame);
    return converted;
}
#endif

} // namespace

// ─── Pimpl ──────────────────────────────────────────────────────────────────

struct AudioFile::Impl
{
#ifdef ROUNDTABLE_HAS_SNDFILE
    SNDFILE*  sndFile{nullptr};
    SF_INFO   sfInfo{};
    // libsndfile doesn't take ownership of the user data we pass to
    // sf_open_virtual, so we own + close it alongside sndFile.
    // On Windows this is a raw HANDLE; on POSIX a FILE*. Stored as
    // void* so the struct doesn't need to know which platform.
    void*     sndFileFP{nullptr};
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
    AVFormatContext*  fmtCtx{nullptr};
    AVCodecContext*   codecCtx{nullptr};
    int               audioStreamIdx{-1};
    SwrContext*       swrCtx{nullptr};

    // Custom AVIO so the source file is opened with full Windows share
    // access (read | write | delete). See VideoDecoderInit.cpp for the
    // full rationale — same fix, parallel decoder. avioFile holds a
    // raw HANDLE on Windows / FILE* on POSIX.
    AVIOContext*      avioCtx{nullptr};
    void*             avioFile{nullptr};
#endif
};

// ─── Constructor / Destructor ───────────────────────────────────────────────

AudioFile::AudioFile()
    : m_impl(std::make_unique<Impl>())
{
}

AudioFile::~AudioFile()
{
    close();
}

// ─── Open ───────────────────────────────────────────────────────────────────

bool AudioFile::open(const std::string& utf8Path)
{
    return open(utf8ToPath(utf8Path));
}

bool AudioFile::open(const std::filesystem::path& path)
{
    std::lock_guard lock(m_mutex);
    close();

    m_path = path;

    if (!std::filesystem::exists(path)) {
        m_lastError = "File not found: " + pathToUtf8(path);
        spdlog::error("AudioFile: {}", m_lastError);
        return false;
    }

    // Try libsndfile first
#ifdef ROUNDTABLE_HAS_SNDFILE
    if (openSndfile(path)) {
        m_isOpen  = true;
        m_backend = AudioBackend::Sndfile;
        spdlog::info("AudioFile: opened '{}' via libsndfile ({} ch, {} Hz, {} frames)",
                     pathToUtf8(path.filename()), m_info.channels,
                     m_info.sampleRate, m_info.frames);
        return true;
    }
#endif

    // Fallback to FFmpeg
#ifdef ROUNDTABLE_HAS_FFMPEG
    if (openFFmpeg(path)) {
        m_isOpen  = true;
        m_backend = AudioBackend::FFmpeg;
        spdlog::info("AudioFile: opened '{}' via FFmpeg ({} ch, {} Hz, {} frames)",
                     pathToUtf8(path.filename()), m_info.channels,
                     m_info.sampleRate, m_info.frames);
        return true;
    }
#endif

    m_lastError = "No audio backend could open: " + pathToUtf8(path);
    spdlog::error("AudioFile: {}", m_lastError);
    return false;
}

// ─── Close ──────────────────────────────────────────────────────────────────

void AudioFile::close()
{
    // Note: caller holds m_mutex (or this is called from destructor)

#ifdef ROUNDTABLE_HAS_SNDFILE
    if (m_impl->sndFile) {
        sf_close(m_impl->sndFile);
        m_impl->sndFile = nullptr;
        m_impl->sfInfo  = {};
    }
    // Close the shared-mode handle that backed sf_open_virtual.
    if (m_impl->sndFileFP) {
        closeSharedHandle(static_cast<SharedFileHandle>(m_impl->sndFileFP));
        m_impl->sndFileFP = nullptr;
    }
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
    if (m_impl->swrCtx) {
        swr_free(&m_impl->swrCtx);
    }
    if (m_impl->codecCtx) {
        avcodec_free_context(&m_impl->codecCtx);
    }
    if (m_impl->fmtCtx) {
        avformat_close_input(&m_impl->fmtCtx);
    }
    // Tear down custom AVIO + shared FILE. AVFMT_FLAG_CUSTOM_IO means
    // avformat_close_input does NOT free our AVIOContext.
    if (m_impl->avioCtx) {
        av_freep(&m_impl->avioCtx->buffer);
        AVIOContext* tmp = m_impl->avioCtx;
        avio_context_free(&tmp);
        m_impl->avioCtx = nullptr;
    }
    if (m_impl->avioFile) {
        closeSharedHandle(static_cast<SharedFileHandle>(m_impl->avioFile));
        m_impl->avioFile = nullptr;
    }
    m_impl->audioStreamIdx = -1;
#endif

    m_info    = {};
    m_backend = AudioBackend::None;
    m_isOpen  = false;
}

// ─── Getters ────────────────────────────────────────────────────────────────

bool AudioFile::isOpen() const noexcept { return m_isOpen; }

const AudioFileInfo& AudioFile::info() const noexcept { return m_info; }

AudioBackend AudioFile::backend() const noexcept { return m_backend; }

const std::string& AudioFile::lastError() const noexcept { return m_lastError; }

const std::filesystem::path& AudioFile::filePath() const noexcept { return m_path; }

// ─── Read all samples ───────────────────────────────────────────────────────

std::vector<float> AudioFile::readAll()
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return {};

    std::vector<float> samples;

#ifdef ROUNDTABLE_HAS_SNDFILE
    if (m_backend == AudioBackend::Sndfile && m_impl->sndFile) {
        const auto totalSamples = m_info.totalSamples();
        samples.resize(static_cast<size_t>(totalSamples));

        sf_seek(m_impl->sndFile, 0, SEEK_SET);
        const auto read = sf_readf_float(m_impl->sndFile, samples.data(), m_info.frames);
        if (read != m_info.frames) {
            spdlog::warn("AudioFile: expected {} frames, read {}", m_info.frames, read);
            samples.resize(static_cast<size_t>(read * m_info.channels));
        }
        return samples;
    }
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
    if (m_backend == AudioBackend::FFmpeg) {
        return readAllFFmpeg();
    }
#endif

    return samples;
}

// ─── Read region ────────────────────────────────────────────────────────────

int64_t AudioFile::readRegion(int64_t startFrame, int64_t numFrames,
                              std::vector<float>& outSamples)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return 0;

    // Clamp
    if (startFrame < 0) startFrame = 0;
    if (startFrame >= m_info.frames) return 0;
    numFrames = std::min(numFrames, m_info.frames - startFrame);

    outSamples.resize(static_cast<size_t>(numFrames * m_info.channels));

#ifdef ROUNDTABLE_HAS_SNDFILE
    if (m_backend == AudioBackend::Sndfile && m_impl->sndFile) {
        sf_seek(m_impl->sndFile, startFrame, SEEK_SET);
        const auto read = sf_readf_float(m_impl->sndFile, outSamples.data(), numFrames);
        if (read < numFrames) {
            outSamples.resize(static_cast<size_t>(read * m_info.channels));
        }
        return read;
    }
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
    if (m_backend == AudioBackend::FFmpeg) {
        return readRegionFFmpeg(startFrame, numFrames, outSamples);
    }
#endif

    return 0;
}

int64_t AudioFile::readRegionResampled(int64_t startFrame, int64_t numFrames,
                                       uint32_t targetSampleRate,
                                       std::vector<float>& outSamples)
{
    std::vector<float> region;
    AudioFileInfo infoSnapshot;

    {
        std::lock_guard lock(m_mutex);
        if (!m_isOpen || startFrame < 0 || numFrames <= 0 ||
            targetSampleRate == 0 || m_info.sampleRate == 0) {
            outSamples.clear();
            return 0;
        }

        infoSnapshot = m_info;

#ifdef ROUNDTABLE_HAS_SNDFILE
        if (m_backend == AudioBackend::Sndfile && m_impl->sndFile) {
            const double invRatio = static_cast<double>(m_info.sampleRate) / targetSampleRate;
            // Margin keeps the swr filter warm-up/tail outside the window.
            int64_t srcStart = static_cast<int64_t>(std::floor(startFrame * invRatio))
                               - kResampleMarginFrames;
            int64_t srcEnd = static_cast<int64_t>(std::ceil((startFrame + numFrames) * invRatio))
                             + 1 + kResampleMarginFrames;
            srcStart = std::max<int64_t>(0, srcStart);
            srcEnd = std::min<int64_t>(m_info.frames, srcEnd);
            if (srcEnd <= srcStart) {
                outSamples.clear();
                return 0;
            }

            region.resize(static_cast<size_t>((srcEnd - srcStart) * m_info.channels));
            sf_seek(m_impl->sndFile, srcStart, SEEK_SET);
            const int64_t read = sf_readf_float(m_impl->sndFile, region.data(), srcEnd - srcStart);
            if (read <= 0) {
                outSamples.clear();
                return 0;
            }
            region.resize(static_cast<size_t>(read * m_info.channels));

            auto resampled = resampleInterleaved(region, m_info.channels,
                                                       m_info.sampleRate, targetSampleRate);
            return extractResampledRegion(resampled, m_info.channels, srcStart,
                                          m_info.sampleRate, targetSampleRate,
                                          startFrame, numFrames, outSamples);
        }
#endif

#ifdef ROUNDTABLE_HAS_FFMPEG
        if (m_backend == AudioBackend::FFmpeg) {
            const double invRatio = static_cast<double>(m_info.sampleRate) / targetSampleRate;
            // Margin keeps the swr filter warm-up/tail outside the window.
            int64_t srcStart = static_cast<int64_t>(std::floor(startFrame * invRatio))
                               - kResampleMarginFrames;
            int64_t srcEnd = static_cast<int64_t>(std::ceil((startFrame + numFrames) * invRatio))
                             + 1 + kResampleMarginFrames;
            srcStart = std::max<int64_t>(0, srcStart);
            srcEnd = std::min<int64_t>(m_info.frames, srcEnd);
            if (srcEnd <= srcStart) {
                outSamples.clear();
                return 0;
            }

            const int64_t read = readRegionFFmpeg(srcStart, srcEnd - srcStart, region);
            if (read <= 0 || region.empty()) {
                outSamples.clear();
                return 0;
            }
            region.resize(static_cast<size_t>(read * m_info.channels));

            auto resampled = resampleInterleaved(region, m_info.channels,
                                                       m_info.sampleRate, targetSampleRate);
            return extractResampledRegion(resampled, m_info.channels, srcStart,
                                          m_info.sampleRate, targetSampleRate,
                                          startFrame, numFrames, outSamples);
        }
#endif
    }

    if (targetSampleRate == infoSnapshot.sampleRate) {
        return readRegion(startFrame, numFrames, outSamples);
    }

    outSamples.clear();
    return 0;
}

size_t AudioFile::buildPeakEnvelopeResampled(uint32_t targetSampleRate,
                                             int64_t windowFrames,
                                             std::vector<float>& outPeaks,
                                             int64_t chunkFrames)
{
    AudioFileInfo infoSnapshot;

    {
        std::lock_guard lock(m_mutex);
        if (!m_isOpen || targetSampleRate == 0 || windowFrames <= 0 ||
            m_info.sampleRate == 0 || m_info.channels == 0 || m_info.frames <= 0) {
            outPeaks.clear();
            return 0;
        }

        infoSnapshot = m_info;
    }

    const int64_t totalTargetFrames = std::max<int64_t>(
        1,
        static_cast<int64_t>(std::llround(
            static_cast<double>(infoSnapshot.frames) * targetSampleRate /
            infoSnapshot.sampleRate)));
    const size_t peakCount = static_cast<size_t>(
        (totalTargetFrames + windowFrames - 1) / windowFrames);
    outPeaks.assign(peakCount, 0.0f);

    if (chunkFrames <= 0) {
        chunkFrames = std::max<int64_t>(windowFrames, windowFrames * 512);
    }
    chunkFrames = std::max<int64_t>(windowFrames, chunkFrames);
    chunkFrames = ((chunkFrames + windowFrames - 1) / windowFrames) * windowFrames;

    std::vector<float> chunk;
    for (int64_t chunkStart = 0; chunkStart < totalTargetFrames;
         chunkStart += chunkFrames) {
        const int64_t requestedFrames =
            std::min<int64_t>(chunkFrames, totalTargetFrames - chunkStart);
        const int64_t framesRead = readRegionResampled(
            chunkStart, requestedFrames, targetSampleRate, chunk);
        if (framesRead <= 0 || chunk.empty()) {
            break;
        }

        const int64_t safeFramesRead = std::min<int64_t>(framesRead, requestedFrames);
        for (int64_t localFrame = 0; localFrame < safeFramesRead; ++localFrame) {
            const size_t peakIndex = static_cast<size_t>(
                (chunkStart + localFrame) / windowFrames);
            float maxVal = outPeaks[peakIndex];
            const size_t sampleBase = static_cast<size_t>(localFrame * infoSnapshot.channels);
            for (uint16_t channel = 0; channel < infoSnapshot.channels; ++channel) {
                maxVal = std::max(maxVal,
                                  std::abs(chunk[sampleBase + channel]));
            }
            outPeaks[peakIndex] = maxVal;
        }
    }

    return outPeaks.size();
}

// ─── Read + resample ────────────────────────────────────────────────────────

std::vector<float> AudioFile::readAllResampled(uint32_t targetSampleRate)
{
    auto samples = readAll();
    if (samples.empty() || m_info.sampleRate == targetSampleRate) {
        return samples;
    }

    return resampleInterleaved(samples, m_info.channels,
                                     m_info.sampleRate, targetSampleRate);
}

// ─── libsndfile backend ─────────────────────────────────────────────────────

bool AudioFile::openSndfile([[maybe_unused]] const std::filesystem::path& path)
{
#ifdef ROUNDTABLE_HAS_SNDFILE
    m_impl->sfInfo = {};

    // Open the file ourselves in shared mode (read|write|delete) and
    // hand libsndfile a virtual IO bound to that FILE*. sf_open() would
    // open via default fopen and re-lock the file on Windows, defeating
    // the un-lock work done for the FFmpeg path.
    SharedFileHandle sndHandle = openSharedReadHandle(path, "AudioFile");
    if (!sndHandle) {
        m_lastError = "Cannot open audio file (shared mode): " + pathToUtf8(path);
        return false;
    }
    m_impl->sndFileFP = static_cast<void*>(sndHandle);
    spdlog::debug("AudioFile/sndfile: shared-mode open '{}' (raw HANDLE)", pathToUtf8(path));

    SF_VIRTUAL_IO vio;
    vio.get_filelen = &sfVioGetFilelen;
    vio.seek        = &sfVioSeek;
    vio.read        = &sfVioRead;
    vio.write       = nullptr;
    vio.tell        = &sfVioTell;

    m_impl->sndFile = sf_open_virtual(&vio, SFM_READ, &m_impl->sfInfo,
                                      m_impl->sndFileFP);

    if (!m_impl->sndFile) {
        m_lastError = sf_strerror(nullptr);
        closeSharedHandle(static_cast<SharedFileHandle>(m_impl->sndFileFP));
        m_impl->sndFileFP = nullptr;
        return false;
    }

    m_info.sampleRate = static_cast<uint32_t>(m_impl->sfInfo.samplerate);
    m_info.channels   = static_cast<uint16_t>(m_impl->sfInfo.channels);
    m_info.frames     = m_impl->sfInfo.frames;
    m_info.duration   = (m_info.sampleRate > 0)
                            ? static_cast<double>(m_info.frames) / m_info.sampleRate
                            : 0.0;

    // Determine format string
    const int majorFmt = m_impl->sfInfo.format & SF_FORMAT_TYPEMASK;
    switch (majorFmt) {
        case SF_FORMAT_WAV:  m_info.format = "WAV";  break;
        case SF_FORMAT_AIFF: m_info.format = "AIFF"; break;
        case SF_FORMAT_FLAC: m_info.format = "FLAC"; break;
        case SF_FORMAT_OGG:  m_info.format = "OGG";  break;
        case SF_FORMAT_AU:   m_info.format = "AU";   break;
        case SF_FORMAT_RAW:  m_info.format = "RAW";  break;
        case SF_FORMAT_W64:  m_info.format = "W64";  break;
        case SF_FORMAT_CAF:  m_info.format = "CAF";  break;
        default:             m_info.format = "Unknown"; break;
    }

    // Determine codec / bit depth
    const int subFmt = m_impl->sfInfo.format & SF_FORMAT_SUBMASK;
    switch (subFmt) {
        case SF_FORMAT_PCM_S8:  m_info.codec = "PCM 8-bit";   m_info.bitDepth = 8;  break;
        case SF_FORMAT_PCM_16:  m_info.codec = "PCM 16-bit";  m_info.bitDepth = 16; break;
        case SF_FORMAT_PCM_24:  m_info.codec = "PCM 24-bit";  m_info.bitDepth = 24; break;
        case SF_FORMAT_PCM_32:  m_info.codec = "PCM 32-bit";  m_info.bitDepth = 32; break;
        case SF_FORMAT_FLOAT:   m_info.codec = "Float 32-bit"; m_info.bitDepth = 32; break;
        case SF_FORMAT_DOUBLE:  m_info.codec = "Float 64-bit"; m_info.bitDepth = 64; break;
        default:                m_info.codec = "Unknown";      m_info.bitDepth = 0;  break;
    }

    return true;
#else
    return false;
#endif
}

// ─── FFmpeg backend ─────────────────────────────────────────────────────────

bool AudioFile::openFFmpeg([[maybe_unused]] const std::filesystem::path& path)
{
#ifdef ROUNDTABLE_HAS_FFMPEG
    // Shared-mode open + custom AVIO (see header for rationale).
    SharedFileHandle sh = openSharedReadHandle(path, "AudioFile");
    if (!sh) {
        m_lastError = "Cannot open audio file (shared mode): " + pathToUtf8(path);
        return false;
    }
    m_impl->avioFile = static_cast<void*>(sh);
    spdlog::debug("AudioFile/FFmpeg: shared-mode open '{}' (raw HANDLE)", pathToUtf8(path));

    auto* buf = static_cast<uint8_t*>(av_malloc(kSharedAvioBufSize));
    if (!buf) {
        closeSharedHandle(sh);
        m_impl->avioFile = nullptr;
        m_lastError = "av_malloc(AVIO buffer) failed";
        return false;
    }

    m_impl->avioCtx = avio_alloc_context(buf, kSharedAvioBufSize,
                                         /*write_flag=*/0,
                                         /*opaque=*/static_cast<void*>(sh),
                                         &sharedAvioRead,
                                         /*write_packet=*/nullptr,
                                         &sharedAvioSeek);
    if (!m_impl->avioCtx) {
        av_free(buf);
        closeSharedHandle(sh);
        m_impl->avioFile = nullptr;
        m_lastError = "avio_alloc_context failed";
        return false;
    }

    m_impl->fmtCtx = avformat_alloc_context();
    if (!m_impl->fmtCtx) {
        av_freep(&m_impl->avioCtx->buffer);
        avio_context_free(&m_impl->avioCtx);
        closeSharedHandle(sh);
        m_impl->avioFile = nullptr;
        m_lastError = "avformat_alloc_context failed";
        return false;
    }
    m_impl->fmtCtx->pb     = m_impl->avioCtx;
    m_impl->fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    int ret = avformat_open_input(&m_impl->fmtCtx, pathToUtf8(path).c_str(),
                                  nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        m_lastError = std::string("FFmpeg open error: ") + errbuf;
        // avformat_open_input frees fmtCtx on failure; our AVIO + handle
        // are independent and still need teardown.
        if (m_impl->avioCtx) {
            av_freep(&m_impl->avioCtx->buffer);
            avio_context_free(&m_impl->avioCtx);
        }
        closeSharedHandle(sh);
        m_impl->avioFile = nullptr;
        return false;
    }

    ret = avformat_find_stream_info(m_impl->fmtCtx, nullptr);
    if (ret < 0) {
        m_lastError = "FFmpeg: could not find stream info";
        avformat_close_input(&m_impl->fmtCtx);
        return false;
    }

    // Find best audio stream
    m_impl->audioStreamIdx = av_find_best_stream(
        m_impl->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_impl->audioStreamIdx < 0) {
        m_lastError = "FFmpeg: no audio stream found";
        avformat_close_input(&m_impl->fmtCtx);
        return false;
    }

    const auto* stream  = m_impl->fmtCtx->streams[m_impl->audioStreamIdx];
    const auto* codecPar = stream->codecpar;

    // Find and open decoder
    const auto* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) {
        m_lastError = "FFmpeg: unsupported audio codec";
        avformat_close_input(&m_impl->fmtCtx);
        return false;
    }

    m_impl->codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_impl->codecCtx, codecPar);

    ret = avcodec_open2(m_impl->codecCtx, codec, nullptr);
    if (ret < 0) {
        m_lastError = "FFmpeg: could not open audio codec";
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->fmtCtx);
        return false;
    }

    // Set up resampler to float interleaved output
    AVChannelLayout outLayout{};
    if (codecPar->ch_layout.nb_channels == 1) {
        outLayout = AV_CHANNEL_LAYOUT_MONO;
    } else {
        outLayout = AV_CHANNEL_LAYOUT_STEREO;
    }

    ret = swr_alloc_set_opts2(&m_impl->swrCtx,
                              &outLayout, AV_SAMPLE_FMT_FLT,
                              codecPar->sample_rate,
                              &codecPar->ch_layout,
                              static_cast<AVSampleFormat>(codecPar->format),
                              codecPar->sample_rate,
                              0, nullptr);
    if (ret < 0 || !m_impl->swrCtx) {
        m_lastError = "FFmpeg: could not create resampler";
        avcodec_free_context(&m_impl->codecCtx);
        avformat_close_input(&m_impl->fmtCtx);
        return false;
    }

    swr_init(m_impl->swrCtx);

    // Fill info
    m_info.sampleRate = static_cast<uint32_t>(codecPar->sample_rate);
    m_info.channels   = static_cast<uint16_t>(outLayout.nb_channels);
    m_info.bitDepth   = static_cast<uint32_t>(codecPar->bits_per_raw_sample);
    if (m_info.bitDepth == 0)
        m_info.bitDepth = static_cast<uint32_t>(codecPar->bits_per_coded_sample);

    // Estimate frame count from duration
    if (stream->duration > 0 && stream->time_base.den > 0) {
        const double durationSec = static_cast<double>(stream->duration)
                                 * stream->time_base.num / stream->time_base.den;
        m_info.duration = durationSec;
        m_info.frames   = static_cast<int64_t>(durationSec * m_info.sampleRate);
    } else if (m_impl->fmtCtx->duration > 0) {
        m_info.duration = static_cast<double>(m_impl->fmtCtx->duration) / AV_TIME_BASE;
        m_info.frames   = static_cast<int64_t>(m_info.duration * m_info.sampleRate);
    }

    m_info.codec  = avcodec_get_name(codecPar->codec_id);
    m_info.format = m_impl->fmtCtx->iformat->name;

    return true;
#else
    return false;
#endif
}

// ─── FFmpeg: read all samples ───────────────────────────────────────────────

#ifdef ROUNDTABLE_HAS_FFMPEG
std::vector<float> AudioFile::readAllFFmpeg()
{
    if (!m_impl->fmtCtx || !m_impl->codecCtx) return {};

    // Seek to beginning
    av_seek_frame(m_impl->fmtCtx, m_impl->audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_impl->codecCtx);

    std::vector<float> allSamples;
    allSamples.reserve(static_cast<size_t>(m_info.frames * m_info.channels));

    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();

    while (av_read_frame(m_impl->fmtCtx, packet) >= 0) {
        if (packet->stream_index != m_impl->audioStreamIdx) {
            av_packet_unref(packet);
            continue;
        }

        int ret = avcodec_send_packet(m_impl->codecCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) continue;

        while (avcodec_receive_frame(m_impl->codecCtx, frame) == 0) {
            convertAndAppendFrame(m_impl->swrCtx, frame, m_info.channels, allSamples);
        }
    }

    // Flush decoder
    avcodec_send_packet(m_impl->codecCtx, nullptr);
    while (avcodec_receive_frame(m_impl->codecCtx, frame) == 0) {
        convertAndAppendFrame(m_impl->swrCtx, frame, m_info.channels, allSamples);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    // Update actual frame count
    m_info.frames = static_cast<int64_t>(allSamples.size()) / m_info.channels;
    m_info.duration = (m_info.sampleRate > 0)
                        ? static_cast<double>(m_info.frames) / m_info.sampleRate
                        : 0.0;

    return allSamples;
}

int64_t AudioFile::readRegionFFmpeg(int64_t startFrame, int64_t numFrames,
                                    std::vector<float>& outSamples)
{
    if (!m_impl->fmtCtx || !m_impl->codecCtx || m_impl->audioStreamIdx < 0 ||
        numFrames <= 0) {
        outSamples.clear();
        return 0;
    }

    AVStream* stream = m_impl->fmtCtx->streams[m_impl->audioStreamIdx];
    const AVRational sampleTimeBase{1, static_cast<int>(std::max<uint32_t>(1, m_info.sampleRate))};
    const int64_t endFrame = startFrame + numFrames;
    const int64_t seekTarget = av_rescale_q(startFrame, sampleTimeBase, stream->time_base);

    av_seek_frame(m_impl->fmtCtx, m_impl->audioStreamIdx, seekTarget, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_impl->codecCtx);
    swr_close(m_impl->swrCtx);
    swr_init(m_impl->swrCtx);

    outSamples.assign(static_cast<size_t>(numFrames * m_info.channels), 0.0f);

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t framesWritten = 0;
    int64_t fallbackFramePos = startFrame;

    auto consumeFrame = [&](AVFrame* decodedFrame) {
        int64_t frameStart = fallbackFramePos;
        if (decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
            frameStart = av_rescale_q(decodedFrame->best_effort_timestamp,
                                      stream->time_base, sampleTimeBase);
        }

        const int outFrameCapacity = swr_get_out_samples(m_impl->swrCtx, decodedFrame->nb_samples);
        std::vector<float> tmp(static_cast<size_t>(outFrameCapacity * m_info.channels));
        uint8_t* outBuf = reinterpret_cast<uint8_t*>(tmp.data());

        const int converted = swr_convert(m_impl->swrCtx,
                                          &outBuf, outFrameCapacity,
                                          const_cast<const uint8_t**>(decodedFrame->extended_data),
                                          decodedFrame->nb_samples);
        if (converted <= 0) {
            av_frame_unref(decodedFrame);
            return;
        }

        tmp.resize(static_cast<size_t>(converted * m_info.channels));
        const int64_t frameEnd = frameStart + converted;
        fallbackFramePos = frameEnd;

        const int64_t copyStart = std::max<int64_t>(startFrame, frameStart);
        const int64_t copyEnd = std::min<int64_t>(endFrame, frameEnd);
        if (copyEnd > copyStart) {
            const int64_t localStart = copyStart - frameStart;
            const int64_t dstStart = copyStart - startFrame;
            const int64_t framesToCopy = copyEnd - copyStart;
            const size_t srcSampleOffset = static_cast<size_t>(localStart * m_info.channels);
            const size_t dstSampleOffset = static_cast<size_t>(dstStart * m_info.channels);
            const size_t sampleCount = static_cast<size_t>(framesToCopy * m_info.channels);
            std::memcpy(outSamples.data() + dstSampleOffset,
                        tmp.data() + srcSampleOffset,
                        sampleCount * sizeof(float));
            framesWritten = std::max<int64_t>(framesWritten, dstStart + framesToCopy);
        }

        av_frame_unref(decodedFrame);
    };

    while (framesWritten < numFrames && av_read_frame(m_impl->fmtCtx, packet) >= 0) {
        if (packet->stream_index != m_impl->audioStreamIdx) {
            av_packet_unref(packet);
            continue;
        }

        const int ret = avcodec_send_packet(m_impl->codecCtx, packet);
        av_packet_unref(packet);
        if (ret < 0) {
            continue;
        }

        while (avcodec_receive_frame(m_impl->codecCtx, frame) == 0) {
            consumeFrame(frame);
            if (framesWritten >= numFrames) {
                break;
            }
        }
    }

    avcodec_send_packet(m_impl->codecCtx, nullptr);
    while (framesWritten < numFrames && avcodec_receive_frame(m_impl->codecCtx, frame) == 0) {
        consumeFrame(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&packet);

    outSamples.resize(static_cast<size_t>(framesWritten * m_info.channels));
    return framesWritten;
}
#endif

} // namespace rt

