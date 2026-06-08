/*
 * AudioMixdown.cpp — Multi-track audio mixing for export.
 */

#include "AudioMixdown.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "media/AudioFile.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
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

AudioMixdown::AudioMixdown() = default;
AudioMixdown::~AudioMixdown() = default;

MixdownResult AudioMixdown::mix(const Timeline& timeline,
                                 const AudioMixdownConfig& config,
                                 const MixdownProgressFn& progress)
{
    MixdownResult result;

    // Determine render range
    double startSec = config.startTime;
    double endSec   = config.endTime;
    if (endSec <= startSec) {
        endSec = ticksToSeconds(timeline.duration());
    }
    if (endSec <= startSec) {
        m_lastError = "AudioMixdown: Empty timeline";
        return result;
    }

    double duration = endSec - startSec;
    int64_t totalFrames = static_cast<int64_t>(duration * config.sampleRate);

    result.sampleRate  = config.sampleRate;
    result.channels    = config.channels;
    result.totalFrames = totalFrames;
    result.duration    = duration;
    result.samples.resize(static_cast<size_t>(totalFrames) * config.channels, 0.0f);

    if (progress) progress(0.0f, "Mixing audio...");

    // Check for solo tracks
    bool hasSolo = false;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (track && track->type() == TrackType::Audio && track->isSoloed()) {
            hasSolo = true;
            break;
        }
    }

    // Process each audio track
    int tracksProcessed = 0;
    int totalAudioTracks = 0;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track || track->type() != TrackType::Audio) continue;
        ++totalAudioTracks;
    }

    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track || track->type() != TrackType::Audio) continue;

        bool muted = track->isMuted();
        if (hasSolo && !track->isSoloed()) muted = true;

        // Process each audio clip in this track
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            const Clip* clip = track->clip(ci);
            if (!clip || clip->clipType() != ClipType::Audio) continue;

            // Get clip timing relative to render range
            double clipStart = ticksToSeconds(clip->timelineIn());
            double clipEnd   = ticksToSeconds(clip->timelineOut());

            // Skip clips outside render range
            if (clipEnd <= startSec || clipStart >= endSec) continue;

            // Calculate overlap
            double overlapStart = std::max(clipStart, startSec);
            double overlapEnd   = std::min(clipEnd, endSec);

            int64_t mixStartFrame = static_cast<int64_t>((overlapStart - startSec) * config.sampleRate);
            int64_t mixFrameCount = static_cast<int64_t>((overlapEnd - overlapStart) * config.sampleRate);

            if (mixFrameCount <= 0) continue;
            if (muted) continue;

            // Cast to AudioClip to get media path and properties
            const auto* aclip = dynamic_cast<const AudioClip*>(clip);
            if (!aclip || aclip->mediaPath().empty()) continue;

            // Open the audio file
            AudioFile audioFile;
            if (!audioFile.open(aclip->mediaPath())) {
                spdlog::warn("AudioMixdown: Cannot open '{}'", aclip->mediaPath());
                continue;
            }

            // Determine source offset: how far into the source file this clip starts
            double sourceStartSec = ticksToSeconds(clip->sourceIn());
            // Adjust if the render range starts partway through the clip
            sourceStartSec += (overlapStart - clipStart);

            uint32_t srcRate = audioFile.info().sampleRate;
            uint16_t srcCh   = audioFile.info().channels;

            int64_t srcStartFrame = static_cast<int64_t>(sourceStartSec * srcRate);
            // How many source frames to read (account for sample rate difference)
            int64_t srcFrameCount = static_cast<int64_t>(
                static_cast<double>(mixFrameCount) * srcRate / config.sampleRate);

            if (srcStartFrame < 0) srcStartFrame = 0;
            if (srcFrameCount <= 0) continue;

            // Read the audio region
            std::vector<float> srcSamples;
            int64_t framesRead = audioFile.readRegion(srcStartFrame, srcFrameCount, srcSamples);
            if (framesRead <= 0) continue;

            // ── Apply audio effects (channel fill) on stereo ────────────
            if (srcCh == 2) {
                const auto& fxStack = clip->effects();
                for (size_t ei = 0; ei < fxStack.effectCount(); ++ei) {
                    const auto& fx = fxStack.effect(ei);
                    if (!fx.isEnabled()) continue;
                    if (fx.effectType() == EffectType::FillLeftWithRight) {
                        for (int64_t s = 0; s < static_cast<int64_t>(srcSamples.size()); s += 2)
                            srcSamples[static_cast<size_t>(s)] = srcSamples[static_cast<size_t>(s + 1)];
                    } else if (fx.effectType() == EffectType::FillRightWithLeft) {
                        for (int64_t s = 0; s < static_cast<int64_t>(srcSamples.size()); s += 2)
                            srcSamples[static_cast<size_t>(s + 1)] = srcSamples[static_cast<size_t>(s)];
                    }
                }
            }

            // ── Per-clip audio FX chain (EQ / dynamics) ─────────────────
            // Process on a CLONE so the live clip's filter/envelope state is
            // never touched by the offline render. Runs at the source rate /
            // channel count, before volume/pan and any resampling.
            if (aclip->audioFx().isActive()) {
                auto chain = aclip->audioFx().clone();
                chain.prepare(srcRate, srcCh);
                chain.process(srcSamples.data(), static_cast<int>(framesRead));
            }

            // Get volume (evaluate at clip start; for full keyframe support
            // we'd evaluate per-sample, but this gives correct static volume)
            float volume = aclip->volume().evaluate(clip->timelineIn()) * config.masterVolume;
            float clipPan = aclip->pan().evaluate(clip->timelineIn());

            // Mix into output buffer
            float* dst = result.samples.data() + mixStartFrame * config.channels;
            const float* src = srcSamples.data();

            // Determine how many frames to actually mix (clamped to what we read
            // and adjusted for potential sample rate difference)
            int64_t framesToMix = std::min(mixFrameCount,
                                           static_cast<int64_t>(framesRead * config.sampleRate / srcRate));
            framesToMix = std::min(framesToMix, totalFrames - mixStartFrame);

            // Compute stereo pan gains
            const float panL = std::min(1.0f, 1.0f - clipPan);
            const float panR = std::min(1.0f, 1.0f + clipPan);

            if (srcRate == config.sampleRate && srcCh == config.channels) {
                // Fast path: same rate, same channels — add with volume + pan
                if (config.channels == 2) {
                    for (int64_t f = 0; f < framesToMix; ++f) {
                        int64_t si = f * 2;
                        int64_t di = f * 2;
                        if (si + 1 < static_cast<int64_t>(srcSamples.size())) {
                            dst[di]     += src[si]     * volume * panL;
                            dst[di + 1] += src[si + 1] * volume * panR;
                        }
                    }
                } else {
                    for (int64_t s = 0; s < framesToMix * config.channels; ++s) {
                        if (s < static_cast<int64_t>(srcSamples.size()))
                            dst[s] += src[s] * volume;
                    }
                }
            } else {
                // General path: simple nearest-neighbour resampling + channel mapping
                for (int64_t f = 0; f < framesToMix; ++f) {
                    int64_t srcIdx = static_cast<int64_t>(
                        static_cast<double>(f) * srcRate / config.sampleRate);
                    if (srcIdx >= framesRead) break;

                    for (uint16_t ch = 0; ch < config.channels; ++ch) {
                        uint16_t srcChan = (ch < srcCh) ? ch : 0; // mono→stereo: duplicate
                        int64_t si = srcIdx * srcCh + srcChan;
                        int64_t di = f * config.channels + ch;
                        if (si < static_cast<int64_t>(srcSamples.size()) &&
                            (mixStartFrame * config.channels + di) <
                                static_cast<int64_t>(result.samples.size())) {
                            dst[di] += src[si] * volume;
                        }
                    }
                }
            }
        }

        ++tracksProcessed;
        if (progress && totalAudioTracks > 0) {
            progress(static_cast<float>(tracksProcessed) / totalAudioTracks,
                     "Mixing track " + std::to_string(tracksProcessed) + "/" +
                     std::to_string(totalAudioTracks));
        }
    }

    if (progress) progress(1.0f, "Mix complete");
    spdlog::info("AudioMixdown: Mixed {} tracks, {} frames, {:.1f}s",
                 totalAudioTracks, totalFrames, duration);
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
            return static_cast<size_t>(durationSec * config.bitrate / 8);
        case AudioCodec::FLAC:
            // FLAC is ~60% of PCM
            return static_cast<size_t>(durationSec * config.sampleRate * config.channels * 2 * 0.6);
        default:
            return 0;
    }
}

// static
void AudioMixdown::mixTrackInto(float* mixBuffer, int64_t mixFrames,
                                 const float* sourceData, int64_t sourceFrames,
                                 int64_t sourceOffset,
                                 uint16_t sourceChannels, uint16_t mixChannels,
                                 float volume, float pan, bool muted)
{
    if (muted || !sourceData || !mixBuffer) return;

    // Calculate stereo pan coefficients (constant-power pan law)
    float panAngle = (pan + 1.0f) * 0.25f * 3.14159265f; // 0 to pi/2
    float gainL = volume * std::cos(panAngle);
    float gainR = volume * std::sin(panAngle);

    int64_t framesToMix = std::min(mixFrames, sourceFrames - sourceOffset);
    if (framesToMix <= 0) return;

    for (int64_t f = 0; f < framesToMix; ++f) {
        int64_t srcIdx = (sourceOffset + f) * sourceChannels;
        int64_t mixIdx = f * mixChannels;

        float srcL = sourceData[srcIdx];
        float srcR = sourceChannels > 1 ? sourceData[srcIdx + 1] : srcL;

        if (mixChannels >= 2) {
            mixBuffer[mixIdx]     += srcL * gainL;
            mixBuffer[mixIdx + 1] += srcR * gainR;
        } else {
            mixBuffer[mixIdx] += (srcL + srcR) * 0.5f * volume;
        }
    }
}

} // namespace rt
