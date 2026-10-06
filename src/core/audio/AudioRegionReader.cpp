/*
 * AudioRegionReader.cpp — block-wise readRegionResampled().
 *
 * Mirrors AudioFile::readRegionResampled step for step: decode the source
 * span AudioFile::resampleSourceSpan() names, rate-convert it, drop the
 * frames before the region start, cap the converted length at
 * ceil(sourceFrames * ratio), and deliver at most numFrames. The difference
 * is that decoding and conversion run sequentially, one block at a time.
 */

#include "audio/AudioRegionReader.h"
#include "audio/AudioFile.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace rt {

namespace {
/// Smallest source read; keeps per-call decode overhead low for small blocks.
constexpr int64_t kMinSourceChunkFrames = 4096;
} // namespace

struct AudioRegionReader::Impl
{
    AudioFile* file{nullptr};
    uint16_t   channels{0};
    uint32_t   sourceRate{0};
    uint32_t   targetRate{0};

    int64_t spanStart{0};       ///< first source frame decoded
    int64_t spanEnd{0};         ///< source frame decoding stops at
    int64_t sourcePos{0};       ///< next source frame to decode
    bool    sourceDone{false};
    bool    finished{false};    ///< nothing more will be produced

    int64_t skip{0};            ///< converted frames still to drop before the region
    int64_t remaining{0};       ///< region frames still to deliver
    int64_t produced{0};        ///< converted frames produced (skipped ones included)
    int64_t cap{std::numeric_limits<int64_t>::max()};  ///< converted-length contract

    std::vector<float> pending; ///< converted, not yet delivered
    size_t             pendingHead{0};
    std::vector<float> sourceChunk;
    std::vector<float> converted;

#ifdef ROUNDTABLE_HAS_FFMPEG
    SwrContext* swr{nullptr};
#endif

    ~Impl()
    {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (swr) swr_free(&swr);
#endif
    }

    [[nodiscard]] int64_t pendingFrames() const noexcept
    {
        return static_cast<int64_t>((pending.size() - pendingHead) / channels);
    }

    void emit(const float* data, int64_t frames)
    {
        frames = std::min(frames, std::max<int64_t>(0, cap - produced));
        if (frames <= 0) return;
        produced += frames;
        const int64_t drop = std::min(skip, frames);
        skip -= drop;
        pending.insert(pending.end(),
                       data + static_cast<size_t>(drop) * channels,
                       data + static_cast<size_t>(frames) * channels);
    }

    void feed(const float* data, int64_t frames)
    {
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (swr) {
            const int capacity = swr_get_out_samples(swr, static_cast<int>(frames));
            if (capacity <= 0) return;
            converted.resize(static_cast<size_t>(capacity) * channels);
            const uint8_t* in = reinterpret_cast<const uint8_t*>(data);
            uint8_t* out = reinterpret_cast<uint8_t*>(converted.data());
            const int got = swr_convert(swr, &out, capacity, &in, static_cast<int>(frames));
            if (got > 0) emit(converted.data(), got);
            return;
        }
#endif
        emit(data, frames);
    }

    void finishSource()
    {
        sourceDone = true;
#ifdef ROUNDTABLE_HAS_FFMPEG
        if (!swr) return;
        // The one-shot resampler's length contract, from the frames read.
        const int64_t sourceFrames = sourcePos - spanStart;
        cap = static_cast<int64_t>(std::ceil(
            sourceFrames * static_cast<double>(targetRate) / sourceRate));
        if (produced > cap) {
            // Defensive: never deliver past the contract.
            const int64_t excess = std::min(produced - cap, pendingFrames());
            pending.resize(pending.size() - static_cast<size_t>(excess) * channels);
            produced -= excess;
        }
        // Drain the filter tail.
        for (;;) {
            const int capacity = std::max(256, swr_get_out_samples(swr, 0));
            converted.resize(static_cast<size_t>(capacity) * channels);
            uint8_t* out = reinterpret_cast<uint8_t*>(converted.data());
            const int got = swr_convert(swr, &out, capacity, nullptr, 0);
            if (got <= 0) break;
            emit(converted.data(), got);
        }
#endif
    }

    void fill(int64_t neededFrames)
    {
        if (sourceDone) {
            finished = true;
            return;
        }
        const double ratio = static_cast<double>(sourceRate) / targetRate;
        const int64_t wanted = std::max<int64_t>(
            kMinSourceChunkFrames,
            static_cast<int64_t>(std::ceil((neededFrames + skip) * ratio)) + 32);
        const int64_t frames = std::min(wanted, spanEnd - sourcePos);
        if (frames <= 0) {
            finishSource();
            return;
        }
        const int64_t got = file->readRegion(sourcePos, frames, sourceChunk);
        if (got > 0) {
            sourcePos += got;
            feed(sourceChunk.data(), got);
        }
        if (got < frames || sourcePos >= spanEnd) finishSource();
    }
};

AudioRegionReader::AudioRegionReader() = default;
AudioRegionReader::~AudioRegionReader() = default;

uint16_t AudioRegionReader::channels() const noexcept
{
    return m_impl ? m_impl->channels : 0;
}

bool AudioRegionReader::begin(AudioFile& file, int64_t startFrame, int64_t numFrames,
                              uint32_t targetSampleRate)
{
    m_impl.reset();
    const AudioFileInfo& info = file.info();
    if (!file.isOpen() || startFrame < 0 || numFrames <= 0 || targetSampleRate == 0 ||
        info.sampleRate == 0 || info.channels == 0) {
        return false;
    }

    const auto span = AudioFile::resampleSourceSpan(
        startFrame, numFrames, info.sampleRate, targetSampleRate, info.frames);
    if (span.end <= span.start) return false;

    auto impl = std::make_unique<Impl>();
    impl->file       = &file;
    impl->channels   = info.channels;
    impl->sourceRate = info.sampleRate;
    impl->targetRate = targetSampleRate;
    impl->spanStart  = span.start;
    impl->spanEnd    = span.end;
    impl->sourcePos  = span.start;
    impl->remaining  = numFrames;

    // Where the converted span starts in the target domain (as in
    // extractResampledRegion): the frames before startFrame are dropped.
    const int64_t convertedStart = static_cast<int64_t>(std::llround(
        span.start * static_cast<double>(targetSampleRate) / info.sampleRate));
    impl->skip = std::max<int64_t>(0, startFrame - convertedStart);

    if (info.sampleRate != targetSampleRate) {
        bool resamplerReady = false;
#ifdef ROUNDTABLE_HAS_FFMPEG
        AVChannelLayout layout;
        av_channel_layout_default(&layout, info.channels);
        if (swr_alloc_set_opts2(&impl->swr,
                                &layout, AV_SAMPLE_FMT_FLT, static_cast<int>(targetSampleRate),
                                &layout, AV_SAMPLE_FMT_FLT, static_cast<int>(info.sampleRate),
                                0, nullptr) >= 0 &&
            impl->swr && swr_init(impl->swr) >= 0) {
            resamplerReady = true;
        } else if (impl->swr) {
            swr_free(&impl->swr);
        }
        av_channel_layout_uninit(&layout);
#endif
        if (!resamplerReady) {
            // No streaming resampler: read the region in one go, the way
            // readRegionResampled itself falls back.
            std::vector<float> all;
            if (file.readRegionResampled(startFrame, numFrames, targetSampleRate, all) <= 0)
                return false;
            impl->pending = std::move(all);
            impl->skip = 0;
            impl->sourceDone = true;
        }
    }

    m_impl = std::move(impl);
    return true;
}

int64_t AudioRegionReader::read(int64_t maxFrames, std::vector<float>& out)
{
    out.clear();
    if (!m_impl || maxFrames <= 0) return 0;
    Impl& impl = *m_impl;
    if (impl.remaining <= 0) return 0;

    const int64_t wanted = std::min(maxFrames, impl.remaining);
    while (impl.pendingFrames() < wanted && !impl.finished)
        impl.fill(wanted - impl.pendingFrames());

    const int64_t frames = std::min(wanted, impl.pendingFrames());
    if (frames <= 0) return 0;

    const auto first = impl.pending.begin() + static_cast<std::ptrdiff_t>(impl.pendingHead);
    const size_t count = static_cast<size_t>(frames) * impl.channels;
    out.assign(first, first + static_cast<std::ptrdiff_t>(count));
    impl.pendingHead += count;
    impl.remaining -= frames;

    if (impl.pendingHead == impl.pending.size()) {
        impl.pending.clear();
        impl.pendingHead = 0;
    } else if (impl.pendingHead * 2 > impl.pending.size()) {
        impl.pending.erase(impl.pending.begin(),
                           impl.pending.begin() + static_cast<std::ptrdiff_t>(impl.pendingHead));
        impl.pendingHead = 0;
    }
    return frames;
}

} // namespace rt
