/*
 * AudioMixdown.cpp — Multi-track audio mixing for export.
 */

#include "AudioMixdown.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/Transition.h"
#include "audio/AudioFile.h"
#include "audio/AudioRegionReader.h"
#include "audiofx/FxChain.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace rt {

const char* audioCodecExtension(AudioCodec codec) noexcept
{
    switch (codec) {
        case AudioCodec::PCM_S16LE:
        case AudioCodec::PCM_F32LE: return ".wav";
        case AudioCodec::AAC:       return ".m4a";
        case AudioCodec::FLAC:      return ".flac";
        case AudioCodec::MP3:       return ".mp3";
        default:                    return ".wav";
    }
}

AudioMixdown::AudioMixdown() = default;
AudioMixdown::~AudioMixdown() = default;

// ── AudioMixStream ──────────────────────────────────────────────────────────

/// One clip's contribution: timing and gain staging copied from the
/// timeline, plus the source reader and effect state carried between blocks.
struct AudioMixStream::ClipSource
{
    /// A cross-dissolve touching this clip: a fade-out when the clip is its
    /// left side, a fade-in when it is the right side.
    struct TransitionFade {
        double tStartSec;
        double tEndSec;
        bool   isLeft;  // true = fade-out, false = fade-in
    };

    std::string mediaPath;
    int         audioStreamIndex{-1};

    int64_t mixStart{0};       ///< first output frame, from the range start
    int64_t mixFrames{0};      ///< frames requested from the source region
    int64_t sourceStart{0};    ///< region start, output-rate domain
    double  readStartSec{0.0};
    double  clipStartSec{0.0};
    double  clipEndSec{0.0};
    double  fadeInSec{0.0};
    double  fadeOutSec{0.0};
    std::vector<TransitionFade> transFades;
    std::vector<EffectType>     channelFills;   ///< enabled fill effects, stack order
    audiofx::FxChain fx;
    bool    hasFx{false};
    float   volume{1.0f};      ///< clip volume x master volume
    float   pan{0.0f};

    // Opened when the clip's first block comes up; released once it is done
    // so a long timeline never holds every source open at once.
    bool     started{false};
    bool     done{false};
    uint16_t sourceChannels{0};
    std::unique_ptr<AudioFile>         file;
    std::unique_ptr<AudioRegionReader> reader;

    void release()
    {
        done = true;
        reader.reset();   // reads from `file`: goes first
        file.reset();
        fx = {};
    }
};

AudioMixStream::AudioMixStream(const Timeline& timeline, const AudioMixdownConfig& config)
    : m_sampleRate(config.sampleRate)
    , m_channels(config.channels)
{
    if (config.sampleRate == 0 || config.channels == 0) {
        m_lastError = "AudioMixdown: Invalid output format";
        return;
    }

    // Determine render range
    const double startSec = config.startTime;
    double endSec = config.endTime;
    if (endSec <= startSec) {
        endSec = ticksToSeconds(timeline.duration());
    }
    if (endSec <= startSec) {
        m_lastError = "AudioMixdown: Empty timeline";
        return;
    }

    m_duration    = endSec - startSec;
    m_totalFrames = static_cast<int64_t>(m_duration * config.sampleRate);

    // Check for solo tracks
    bool hasSolo = false;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (track && track->type() == TrackType::Audio) {
            ++m_audioTracks;
            if (track->isSoloed()) hasSolo = true;
        }
    }

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track || track->type() != TrackType::Audio) continue;

        bool muted = track->isMuted();
        if (hasSolo && !track->isSoloed()) muted = true;
        if (muted) continue;

        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* clip = track->clip(ci);
            if (!clip || !clip->isAudio()) continue;

            // Get clip timing relative to render range
            const double clipStartSec = ticksToSeconds(clip->timelineIn());
            const double clipEndSec   = ticksToSeconds(clip->timelineOut());

            // Skip clips outside render range (with extension margins — see below)
            if (clipEndSec <= startSec || clipStartSec >= endSec) continue;

            // Cast to AudioClip to get media path and properties
            const auto* aclip = dynamic_cast<const AudioClip*>(clip);
            if (!aclip || aclip->mediaPath().empty()) continue;

            auto source = std::make_unique<ClipSource>();

            // ── Compute transition extensions for crossfades ────────────
            // Cross-dissolve transitions straddle the edit point: the left
            // clip needs to be read past its natural end, and the right clip
            // before its natural start. Without this, the overlap needed for
            // a crossfade doesn't exist and the transition collapses to
            // fade-out-to-silence then fade-in.
            const uint64_t clipId = clip->id();
            double extBeforeSec = 0.0;
            double extAfterSec  = 0.0;

            for (size_t tri = 0; tri < track->transitionCount(); ++tri) {
                const Transition* trans = track->transition(tri);
                if (!trans) continue;
                int64_t tStart, tEnd;
                trans->getRange(tStart, tEnd);
                const int64_t tDur = tEnd - tStart;
                if (tDur <= 0) continue;

                if (trans->leftClipId == clipId) {
                    // Fade-out: clip extends past its natural end
                    const double after = ticksToSeconds(tEnd - clip->timelineOut());
                    if (after > extAfterSec) extAfterSec = after;
                    source->transFades.push_back({ticksToSeconds(tStart), ticksToSeconds(tEnd), true});
                }
                if (trans->rightClipId == clipId) {
                    // Fade-in: clip extends before its natural start
                    const double before = ticksToSeconds(clip->timelineIn() - tStart);
                    if (before > extBeforeSec) extBeforeSec = before;
                    source->transFades.push_back({ticksToSeconds(tStart), ticksToSeconds(tEnd), false});
                }
            }

            // ── Extended read range ─────────────────────────────────────
            // Extend the timeline region by transition pre/post-roll, then
            // clamp to the render range and source file boundaries.
            const double readStartSec = std::max(clipStartSec - extBeforeSec, startSec);
            const double readEndSec   = std::min(clipEndSec   + extAfterSec,  endSec);
            if (readEndSec <= readStartSec) continue;

            source->mixStart  = static_cast<int64_t>((readStartSec - startSec) * config.sampleRate);
            source->mixFrames = static_cast<int64_t>((readEndSec - readStartSec) * config.sampleRate);
            if (source->mixFrames <= 0) continue;

            // Compute source read offset, backing up by the extension
            double sourceStartSec = ticksToSeconds(clip->sourceIn());
            sourceStartSec += (readStartSec - clipStartSec);
            if (sourceStartSec < 0.0) {
                // Not enough source before sourceIn; shift the timeline
                // region forward to stay aligned with available source.
                // The first 'deficit' seconds of the timeline region will
                // have no source data — this is handled naturally by the
                // fade envelope (pre-clip-start samples get gain=0).
                sourceStartSec = 0.0;
            }

            // The region is read in the OUTPUT sample-rate domain (windowed-
            // sinc conversion when the source rate differs; nearest-neighbor
            // mapping aliased every 44.1 kHz source in a 48 kHz export —
            // cleanup audit §5.5).
            source->sourceStart = std::max<int64_t>(0, static_cast<int64_t>(
                sourceStartSec * config.sampleRate));

            // Open on the clip's chosen audio stream (-1 = auto/best) so the
            // export uses the SAME stream the user picked for preview.
            source->mediaPath        = aclip->mediaPath();
            source->audioStreamIndex = aclip->audioStreamIndex();

            // Channel fill effects (applied to stereo sources only).
            const auto& fxStack = clip->effects();
            for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                const auto& fx = fxStack.effect(ei);
                if (!fx.isEnabled()) continue;
                if (fx.effectType() == EffectType::FillLeftWithRight ||
                    fx.effectType() == EffectType::FillRightWithLeft)
                    source->channelFills.push_back(fx.effectType());
            }

            // Per-clip audio FX chain (EQ / dynamics), state kept across blocks.
            if (aclip->audioFx().isActive()) {
                source->fx    = aclip->audioFx().clone();
                source->hasFx = true;
            }

            source->readStartSec = readStartSec;
            source->clipStartSec = clipStartSec;
            source->clipEndSec   = clipEndSec;
            source->fadeInSec    = ticksToSeconds(aclip->fadeInDuration());
            source->fadeOutSec   = ticksToSeconds(aclip->fadeOutDuration());

            // ── Volume and pan ──────────────────────────────────────────
            source->volume = aclip->volume().evaluate(clip->timelineIn()) * config.masterVolume;
            source->pan    = aclip->pan().evaluate(clip->timelineIn());

            m_clips.push_back(std::move(source));
        }
    }
}

AudioMixStream::~AudioMixStream() = default;

int64_t AudioMixStream::read(float* out, int64_t frames)
{
    const int64_t blockFrames = std::min(frames, m_totalFrames - m_position);
    if (!out || blockFrames <= 0) return 0;

    std::fill(out, out + static_cast<size_t>(blockFrames) * m_channels, 0.0f);
    // Clips sum in track order, then clip order, every block: each output
    // sample accumulates in the same order whatever the block size.
    for (auto& clip : m_clips) {
        if (clip->done || clip->mixStart >= m_position + blockFrames) continue;
        mixClip(*clip, out, blockFrames);
    }
    m_position += blockFrames;
    return blockFrames;
}

void AudioMixStream::mixClip(ClipSource& clip, float* out, int64_t blockFrames)
{
    if (!clip.started) {
        clip.started = true;
        auto file = std::make_unique<AudioFile>();
        if (!file->open(clip.mediaPath, clip.audioStreamIndex)) {
            spdlog::warn("AudioMixdown: Cannot open '{}' (audio stream {})",
                         clip.mediaPath, clip.audioStreamIndex);
            clip.release();
            return;
        }
        auto reader = std::make_unique<AudioRegionReader>();
        if (!reader->begin(*file, clip.sourceStart, clip.mixFrames, m_sampleRate)) {
            clip.release();
            return;
        }
        clip.sourceChannels = reader->channels();
        if (clip.hasFx) clip.fx.prepare(m_sampleRate, clip.sourceChannels);
        clip.file   = std::move(file);
        clip.reader = std::move(reader);
    }

    // The part of the clip's region that falls in this block. Blocks come in
    // order, so `first` is exactly where the previous block stopped reading.
    const int64_t limit = std::min(clip.mixFrames, m_totalFrames - clip.mixStart);
    const int64_t first = std::max<int64_t>(0, m_position - clip.mixStart);
    const int64_t last  = std::min(limit, m_position + blockFrames - clip.mixStart);
    if (last <= first) {
        if (first >= limit) clip.release();
        return;
    }

    const int64_t wanted = last - first;
    const int64_t framesRead = clip.reader->read(wanted, m_source);
    if (framesRead <= 0) {
        clip.release();
        return;
    }

    const uint16_t srcCh = clip.sourceChannels;
    float* src = m_source.data();

    // ── Apply audio effects (channel fill) on stereo ────────────────────
    if (srcCh == 2) {
        for (const EffectType fill : clip.channelFills) {
            for (int64_t f = 0; f < framesRead; ++f) {
                float* frame = src + f * 2;
                if (fill == EffectType::FillLeftWithRight) frame[0] = frame[1];
                else                                       frame[1] = frame[0];
            }
        }
    }

    // ── Per-clip audio FX chain (EQ / dynamics) ─────────────────────────
    // Samples are already in the output sample-rate domain.
    if (clip.hasFx) clip.fx.process(src, static_cast<int>(framesRead));

    // ── Per-sample fade envelope ────────────────────────────────────────
    // Combines: per-clip fade in/out + transition crossfades.
    // Applied multiplicatively to each source sample before mixing.
    m_gains.resize(static_cast<size_t>(framesRead));
    for (int64_t i = 0; i < framesRead; ++i) {
        // Timeline position of this output sample
        const int64_t f = first + i;
        const double tSec = clip.readStartSec +
            static_cast<double>(f) / static_cast<double>(m_sampleRate);
        float gain = 1.0f;

        // ── Per-clip fade in ────────────────────────────────────────────
        if (clip.fadeInSec > 0.0) {
            const double fadePos = tSec - clip.clipStartSec;
            if (fadePos < 0.0) {
                gain = 0.0f;  // pre-roll before clip (right-side transition)
            } else if (fadePos < clip.fadeInSec) {
                gain *= static_cast<float>(fadePos / clip.fadeInSec);
            }
        }

        // ── Per-clip fade out ───────────────────────────────────────────
        if (clip.fadeOutSec > 0.0) {
            const double fadePos = clip.clipEndSec - tSec;
            if (fadePos < 0.0) {
                gain = 0.0f;  // post-roll after clip (left-side transition)
            } else if (fadePos < clip.fadeOutSec) {
                gain *= static_cast<float>(fadePos / clip.fadeOutSec);
            }
        }

        // ── Transition crossfades ───────────────────────────────────────
        for (const auto& tf : clip.transFades) {
            if (tSec >= tf.tStartSec && tSec < tf.tEndSec) {
                const double dur = tf.tEndSec - tf.tStartSec;
                const double t = (tSec - tf.tStartSec) / dur;
                if (tf.isLeft) {
                    // Fade-out: 1.0 → 0.0
                    gain *= static_cast<float>(1.0 - std::clamp(t, 0.0, 1.0));
                } else {
                    // Fade-in: 0.0 → 1.0
                    gain *= static_cast<float>(std::clamp(t, 0.0, 1.0));
                }
            }
        }

        m_gains[static_cast<size_t>(i)] = gain;
    }

    // ── Mix into the block ──────────────────────────────────────────────
    const uint16_t channels = m_channels;
    float* dst = out + static_cast<size_t>(clip.mixStart + first - m_position) * channels;
    const float volume = clip.volume;

    // Compute stereo pan gains
    const float panL = std::min(1.0f, 1.0f - clip.pan);
    const float panR = std::min(1.0f, 1.0f + clip.pan);

    if (srcCh == channels) {
        // Fast path: same channel count (rate already matches — the reader
        // delivers output-rate samples)
        if (channels == 2) {
            for (int64_t f = 0; f < framesRead; ++f) {
                const int64_t i = f * 2;
                const float g = m_gains[static_cast<size_t>(f)] * volume;
                dst[i]     += src[i]     * g * panL;
                dst[i + 1] += src[i + 1] * g * panR;
            }
        } else {
            for (int64_t f = 0; f < framesRead; ++f) {
                const int64_t s = f * static_cast<int64_t>(channels);
                const float g = m_gains[static_cast<size_t>(f)] * volume;
                for (uint16_t ch = 0; ch < channels; ++ch)
                    dst[s + ch] += src[s + ch] * g;
            }
        }
    } else {
        // General path: channel mapping (rate already matches)
        for (int64_t f = 0; f < framesRead; ++f) {
            const float g = m_gains[static_cast<size_t>(f)] * volume;
            for (uint16_t ch = 0; ch < channels; ++ch) {
                const uint16_t srcChan = (ch < srcCh) ? ch : 0;
                dst[f * channels + ch] += src[f * srcCh + srcChan] * g;
            }
        }
    }

    // A short read means the source ran out: the clip is done.
    if (framesRead < wanted || first + framesRead >= limit) clip.release();
}

// ── AudioMixdown ────────────────────────────────────────────────────────────

MixdownResult AudioMixdown::mix(const Timeline& timeline,
                                 const AudioMixdownConfig& config,
                                 const MixdownProgressFn& progress)
{
    MixdownResult result;

    AudioMixStream stream(timeline, config);
    if (!stream.isValid()) {
        if (!stream.lastError().empty()) m_lastError = stream.lastError();
        return result;
    }

    const int64_t totalFrames = stream.totalFrames();
    result.sampleRate  = stream.sampleRate();
    result.channels    = stream.channels();
    result.totalFrames = totalFrames;
    result.duration    = stream.duration();
    result.samples.resize(static_cast<size_t>(totalFrames) * result.channels, 0.0f);

    if (progress) progress(0.0f, "Mixing audio...");

    constexpr int64_t kBlockFrames = 1 << 16;   // ~1.4 s at 48 kHz
    for (int64_t pos = 0; pos < totalFrames;) {
        const int64_t framesMixed = stream.read(
            result.samples.data() + static_cast<size_t>(pos) * result.channels,
            std::min(kBlockFrames, totalFrames - pos));
        if (framesMixed <= 0) break;
        pos += framesMixed;
        if (progress)
            progress(static_cast<float>(pos) / static_cast<float>(totalFrames), "Mixing audio...");
    }

    if (progress) progress(1.0f, "Mix complete");
    spdlog::info("AudioMixdown: Mixed {} tracks, {} frames, {:.1f}s",
                 stream.audioTrackCount(), totalFrames, result.duration);
    return result;
}

bool AudioMixdown::mixToFile(const Timeline& timeline,
                              const std::filesystem::path& outputPath,
                              const AudioMixdownConfig& config,
                              const MixdownProgressFn& progress)
{
    auto result = mix(timeline, config, progress);
    if (!result.isValid()) return false;
    return writeWav(result, outputPath);
}

// static
bool AudioMixdown::writeWav(const MixdownResult& result,
                             const std::filesystem::path& outputPath)
{
    if (!result.isValid()) return false;

    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open()) {
        spdlog::error("AudioMixdown: Cannot open {} for writing", pathToUtf8(outputPath));
        return false;
    }

    // Write WAV header (PCM 16-bit)
    uint16_t channels    = result.channels;
    uint32_t sampleRate  = result.sampleRate;
    uint16_t bitsPerSamp = 16;
    uint32_t byteRate    = sampleRate * channels * (bitsPerSamp / 8);
    uint16_t blockAlign  = channels * (bitsPerSamp / 8);
    uint32_t dataSize    = static_cast<uint32_t>(result.totalFrames * channels * (bitsPerSamp / 8));
    uint32_t fileSize    = 36 + dataSize;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&fileSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    uint32_t fmtSize = 16;
    out.write(reinterpret_cast<const char*>(&fmtSize), 4);
    uint16_t audioFmt = 1; // PCM
    out.write(reinterpret_cast<const char*>(&audioFmt), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sampleRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSamp), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataSize), 4);

    // Convert float samples to 16-bit and write
    for (int64_t i = 0; i < result.totalFrames * channels; ++i) {
        float sample = std::clamp(result.samples[static_cast<size_t>(i)], -1.0f, 1.0f);
        int16_t s16 = static_cast<int16_t>(sample * 32767.0f);
        out.write(reinterpret_cast<const char*>(&s16), 2);
    }

    spdlog::info("AudioMixdown: Wrote {} ({:.1f}s, {} Hz, {} ch)",
                 pathToUtf8(outputPath), result.duration, sampleRate, channels);
    return true;
}

// static
bool AudioMixdown::writeAudioFile(const MixdownResult& result,
                                  const std::filesystem::path& outputPath,
                                  AudioCodec codec, int bitrate)
{
    if (!result.isValid()) return false;

    // WAV → the existing PCM-16 writer (no encoder/muxer needed).
    if (codec == AudioCodec::PCM_S16LE)
        return writeWav(result, outputPath);

    // Map our format → FFmpeg encoder id, container short-name, and the
    // sample format the encoder expects.  Hard-coding the sample format avoids
    // the deprecated AVCodec::sample_fmts query and is correct for these codecs.
    AVCodecID      codecId   = AV_CODEC_ID_AAC;
    const char*    fmtName   = "ipod";              // .m4a (MP4/AAC)
    AVSampleFormat sampleFmt = AV_SAMPLE_FMT_FLTP;
    bool           usesBitrate = true;
    switch (codec) {
        case AudioCodec::MP3:  codecId = AV_CODEC_ID_MP3;  fmtName = "mp3";  sampleFmt = AV_SAMPLE_FMT_FLTP; usesBitrate = true;  break;
        case AudioCodec::AAC:  codecId = AV_CODEC_ID_AAC;  fmtName = "ipod"; sampleFmt = AV_SAMPLE_FMT_FLTP; usesBitrate = true;  break;
        case AudioCodec::FLAC: codecId = AV_CODEC_ID_FLAC; fmtName = "flac"; sampleFmt = AV_SAMPLE_FMT_S16;  usesBitrate = false; break;
        case AudioCodec::PCM_F32LE: codecId = AV_CODEC_ID_PCM_F32LE; fmtName = "wav"; sampleFmt = AV_SAMPLE_FMT_FLT; usesBitrate = false; break;
        default: break;
    }

    const std::string outUtf8 = pathToUtf8(outputPath);

    const AVCodec* enc = avcodec_find_encoder(codecId);
    if (!enc) {
        spdlog::error("AudioMixdown: audio encoder not available in this build "
                      "(codec id {})", static_cast<int>(codecId));
        return false;
    }

    AVFormatContext* fmt = nullptr;
    avformat_alloc_output_context2(&fmt, nullptr, fmtName, outUtf8.c_str());
    if (!fmt) {
        spdlog::error("AudioMixdown: failed to allocate output context for {}", outUtf8);
        return false;
    }

    AVStream* st = avformat_new_stream(fmt, nullptr);
    AVCodecContext* ctx = st ? avcodec_alloc_context3(enc) : nullptr;
    if (!st || !ctx) {
        spdlog::error("AudioMixdown: failed to create audio stream/encoder context");
        if (ctx) avcodec_free_context(&ctx);
        avformat_free_context(fmt);
        return false;
    }

    ctx->sample_fmt  = sampleFmt;
    ctx->sample_rate = static_cast<int>(result.sampleRate);
    av_channel_layout_default(&ctx->ch_layout, result.channels);
    ctx->time_base   = {1, ctx->sample_rate};
    if (usesBitrate)
        ctx->bit_rate = bitrate > 0 ? bitrate : 192000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Local cleanup helper for the failure paths below.
    auto teardown = [&](SwrContext** swr, AVFrame** frame, AVPacket** pkt) {
        if (pkt && *pkt)     av_packet_free(pkt);
        if (frame && *frame) av_frame_free(frame);
        if (swr && *swr)     swr_free(swr);
        avcodec_free_context(&ctx);
        if (fmt) {
            if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE))
                avio_closep(&fmt->pb);
            avformat_free_context(fmt);
        }
    };

    if (avcodec_open2(ctx, enc, nullptr) < 0) {
        spdlog::error("AudioMixdown: failed to open audio encoder");
        teardown(nullptr, nullptr, nullptr);
        return false;
    }
    avcodec_parameters_from_context(st->codecpar, ctx);
    st->time_base = ctx->time_base;

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, outUtf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            spdlog::error("AudioMixdown: failed to open output file: {}", outUtf8);
            teardown(nullptr, nullptr, nullptr);
            return false;
        }
    }
    if (avformat_write_header(fmt, nullptr) < 0) {
        spdlog::error("AudioMixdown: failed to write container header for {}", outUtf8);
        teardown(nullptr, nullptr, nullptr);
        return false;
    }

    // Resample interleaved float (mix buffer) → the encoder's sample format.
    SwrContext* swr = nullptr;
    AVChannelLayout srcLayout{};
    av_channel_layout_default(&srcLayout, result.channels);
    swr_alloc_set_opts2(&swr,
                        &ctx->ch_layout, sampleFmt,        ctx->sample_rate,
                        &srcLayout,      AV_SAMPLE_FMT_FLT, static_cast<int>(result.sampleRate),
                        0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        spdlog::error("AudioMixdown: failed to init resampler");
        teardown(&swr, nullptr, nullptr);
        return false;
    }

    const int frameSize = ctx->frame_size > 0 ? ctx->frame_size : 4096;

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        spdlog::error("AudioMixdown: failed to allocate audio frame");
        teardown(&swr, &frame, nullptr);
        return false;
    }
    frame->format      = sampleFmt;
    frame->sample_rate = ctx->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    frame->nb_samples  = frameSize;
    if (av_frame_get_buffer(frame, 0) < 0) {
        spdlog::error("AudioMixdown: failed to allocate audio frame buffer");
        teardown(&swr, &frame, nullptr);
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    const float* pcm = result.samples.data();
    const int64_t totalFrames = result.totalFrames;
    int64_t samplesRead = 0;
    int64_t pts = 0;
    bool writeError = false;

    auto drain = [&]() {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            pkt->stream_index = st->index;
            av_packet_rescale_ts(pkt, ctx->time_base, st->time_base);
            if (av_interleaved_write_frame(fmt, pkt) < 0) writeError = true;
            av_packet_unref(pkt);
        }
    };

    while (samplesRead < totalFrames && !writeError) {
        const int64_t remaining = totalFrames - samplesRead;
        const int     chunk     = static_cast<int>(std::min<int64_t>(remaining, frameSize));

        const uint8_t* srcData[] = {
            reinterpret_cast<const uint8_t*>(pcm + samplesRead * result.channels)
        };
        av_frame_make_writable(frame);
        frame->nb_samples = chunk;
        swr_convert(swr, frame->data, chunk, srcData, chunk);

        frame->pts = pts;
        pts += chunk;

        if (avcodec_send_frame(ctx, frame) < 0) { writeError = true; break; }
        drain();
        samplesRead += chunk;
    }

    // Flush the encoder.
    avcodec_send_frame(ctx, nullptr);
    drain();

    if (!writeError)
        av_write_trailer(fmt);

    teardown(&swr, &frame, &pkt);

    if (writeError) {
        spdlog::error("AudioMixdown: error writing encoded audio to {}", outUtf8);
        return false;
    }

    spdlog::info("AudioMixdown: wrote {} ({:.1f}s, codec {})",
                 outUtf8, result.duration, static_cast<int>(codec));
    return true;
}

// static
std::vector<uint8_t> AudioMixdown::encodeAAC(const MixdownResult& result, int bitrate)
{
    std::vector<uint8_t> output;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        spdlog::error("AudioMixdown: AAC encoder not found");
        return output;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        spdlog::error("AudioMixdown: Failed to alloc AAC context");
        return output;
    }

    ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    ctx->sample_rate = static_cast<int>(result.sampleRate);
    ctx->bit_rate    = bitrate > 0 ? bitrate : 192000;
    av_channel_layout_default(&ctx->ch_layout, result.channels);
    ctx->time_base   = {1, ctx->sample_rate};

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        spdlog::error("AudioMixdown: Failed to open AAC encoder");
        avcodec_free_context(&ctx);
        return output;
    }

    const int frameSize = ctx->frame_size;

    // SwrContext: interleaved float (FLT) → planar float (FLTP)
    SwrContext* swr = nullptr;
    AVChannelLayout srcLayout{}, dstLayout{};
    av_channel_layout_default(&srcLayout, result.channels);
    av_channel_layout_default(&dstLayout, result.channels);
    swr_alloc_set_opts2(&swr,
                        &dstLayout, AV_SAMPLE_FMT_FLTP, ctx->sample_rate,
                        &srcLayout, AV_SAMPLE_FMT_FLT,  static_cast<int>(result.sampleRate),
                        0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        spdlog::error("AudioMixdown: Failed to init SwrContext");
        avcodec_free_context(&ctx);
        if (swr) swr_free(&swr);
        return output;
    }

    AVFrame* frame = av_frame_alloc();
    frame->format      = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = ctx->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    frame->nb_samples  = frameSize;
    av_frame_get_buffer(frame, 0);

    AVPacket* pkt = av_packet_alloc();

    const float* pcm         = result.samples.data();
    const int64_t totalFrames = static_cast<int64_t>(result.totalFrames);
    int64_t samplesRead = 0;
    int64_t pts = 0;

    // Reserve rough estimate
    output.reserve(static_cast<size_t>(result.duration * (bitrate > 0 ? bitrate : 192000) / 8));

    auto collectPackets = [&]() {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            output.insert(output.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };

    while (samplesRead < totalFrames) {
        int64_t remaining = totalFrames - samplesRead;
        int chunk = static_cast<int>(std::min<int64_t>(remaining, frameSize));

        const uint8_t* srcData[] = {
            reinterpret_cast<const uint8_t*>(pcm + samplesRead * result.channels)
        };
        av_frame_make_writable(frame);
        frame->nb_samples = chunk;
        swr_convert(swr, frame->data, chunk, srcData, chunk);

        frame->pts = pts;
        pts += chunk;

        if (avcodec_send_frame(ctx, frame) < 0) {
            spdlog::error("AudioMixdown: Error sending frame to AAC encoder");
            break;
        }
        collectPackets();
        samplesRead += chunk;
    }

    // Flush encoder
    avcodec_send_frame(ctx, nullptr);
    collectPackets();

    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    avcodec_free_context(&ctx);

    spdlog::info("AudioMixdown: Encoded {} AAC bytes from {:.1f}s audio",
                 output.size(), result.duration);
    return output;
}

// static
size_t AudioMixdown::estimateFileSize(const AudioMixdownConfig& config, double durationSec)
{
    switch (config.codec) {
        case AudioCodec::PCM_S16LE:
            return static_cast<size_t>(durationSec * config.sampleRate * config.channels * 2) + 44;
        case AudioCodec::PCM_F32LE:
            return static_cast<size_t>(durationSec * config.sampleRate * config.channels * 4) + 44;
        case AudioCodec::AAC:
        case AudioCodec::MP3:
            return static_cast<size_t>(durationSec * config.bitrate / 8);
        case AudioCodec::FLAC:
            // FLAC is ~60% of PCM
            return static_cast<size_t>(durationSec * config.sampleRate * config.channels * 2 * 0.6);
        default:
            return 0;
    }
}

} // namespace rt
