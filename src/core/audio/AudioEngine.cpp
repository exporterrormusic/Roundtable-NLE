/*
 * AudioEngine.cpp — real-time audio playback via PortAudio (WASAPI).
 */

#include "audio/AudioEngine.h"
#include "playback/AVSyncClock.h"
#include "audio/TimeStretch.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef ROUNDTABLE_HAS_PORTAUDIO
#include <portaudio.h>
#endif

namespace rt {

namespace {

inline void computePan(float pan, uint32_t channels, float& panL, float& panR)
{
    if (channels == 1) {
        const float theta = (pan + 1.0f) * 0.25f * 3.14159265f;
        panL = std::cos(theta);
        panR = std::sin(theta);
    } else {
        panL = std::min(1.0f, 1.0f - pan);
        panR = std::min(1.0f, 1.0f + pan);
    }
}

AudioSourceView resolveAudioSourceView(const AudioTrackSource& src)
{
    if (src.sampleProvider) {
        return src.sampleProvider->currentView();
    }

    AudioSourceView view;
    view.samples = src.samples;
    view.totalFrames = src.totalFrames;
    view.startFrame = src.startFrame;
    view.channels = src.channels;
    view.sampleRate = src.sampleRate;
    return view;
}

} // namespace

// ─── Pimpl ──────────────────────────────────────────────────────────────────

struct AudioEngine::Impl
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    PaStream* stream{nullptr};
#endif
    bool paInitialized{false};
};

// ─── Constructor / Destructor ───────────────────────────────────────────────

AudioEngine::AudioEngine()
    : m_impl(std::make_unique<Impl>())
{
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

// ─── Initialization ─────────────────────────────────────────────────────────

bool AudioEngine::initialize(const AudioEngineConfig& config)
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (m_impl->paInitialized) {
        spdlog::warn("AudioEngine: already initialized");
        return true;
    }

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        m_lastError = std::string("PortAudio init failed: ") + Pa_GetErrorText(err);
        spdlog::error("AudioEngine: {}", m_lastError);
        return false;
    }
    m_impl->paInitialized = true;
    m_config = config;

    // Resolve device
    int deviceIdx = config.deviceIndex;
    if (deviceIdx < 0) {
        deviceIdx = Pa_GetDefaultOutputDevice();
        if (deviceIdx == paNoDevice) {
            m_lastError = "No default audio output device";
            spdlog::error("AudioEngine: {}", m_lastError);
            return false;
        }
    }

    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(deviceIdx);
    if (!devInfo) {
        m_lastError = "Invalid audio device index";
        spdlog::error("AudioEngine: {}", m_lastError);
        return false;
    }

    spdlog::info("AudioEngine: using device '{}' ({} Hz, {} ch)",
                 devInfo->name, config.sampleRate, config.channels);

    // Set up output parameters
    PaStreamParameters outParams{};
    outParams.device           = deviceIdx;
    outParams.channelCount     = static_cast<int>(config.channels);
    outParams.sampleFormat     = paFloat32;
    outParams.suggestedLatency = devInfo->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    // Try WASAPI exclusive mode if requested
#ifdef PA_USE_WASAPI
    PaWasapiStreamInfo wasapiInfo{};
    if (config.exclusiveMode) {
        wasapiInfo.size            = sizeof(PaWasapiStreamInfo);
        wasapiInfo.hostApiType     = paWASAPI;
        wasapiInfo.version         = 1;
        wasapiInfo.flags           = paWinWasapiExclusive;
        outParams.hostApiSpecificStreamInfo = &wasapiInfo;
        spdlog::info("AudioEngine: WASAPI exclusive mode enabled");
    }
#endif

    err = Pa_OpenStream(&m_impl->stream,
                        nullptr,        // no input
                        &outParams,
                        config.sampleRate,
                        config.framesPerBuffer,
                        paClipOff,      // don't clip output
                        &AudioEngine::paCallback,
                        this);

    if (err != paNoError) {
        m_lastError = std::string("PortAudio open stream failed: ") + Pa_GetErrorText(err);
        spdlog::error("AudioEngine: {}", m_lastError);
        Pa_Terminate();
        m_impl->paInitialized = false;
        return false;
    }

    spdlog::info("AudioEngine: initialized ({} Hz, {} ch, {} frames/buffer)",
                 config.sampleRate, config.channels, config.framesPerBuffer);
    return true;
#else
    (void)config;
    m_lastError = "PortAudio not available (ROUNDTABLE_HAS_PORTAUDIO not defined)";
    spdlog::warn("AudioEngine: {}", m_lastError);
    return false;
#endif
}

void AudioEngine::shutdown()
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (m_impl->stream) {
        Pa_StopStream(m_impl->stream);
        Pa_CloseStream(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->paInitialized) {
        Pa_Terminate();
        m_impl->paInitialized = false;
    }
#endif
    m_state.store(TransportState::Stopped);
    m_playPosition.store(0);
    spdlog::info("AudioEngine: shutdown");
}

bool AudioEngine::isInitialized() const noexcept
{
    return m_impl && m_impl->paInitialized;
}

// ─── Device enumeration ────────────────────────────────────────────────────

std::vector<AudioDeviceInfo> AudioEngine::enumerateDevices() const
{
    std::vector<AudioDeviceInfo> devices;

#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (!m_impl->paInitialized) return devices;

    const int defaultOut = Pa_GetDefaultOutputDevice();
    const int count      = Pa_GetDeviceCount();

    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;

        AudioDeviceInfo dev;
        dev.index             = i;
        dev.name              = info->name;
        dev.maxOutputChannels = info->maxOutputChannels;
        dev.defaultSampleRate = info->defaultSampleRate;
        dev.isDefault         = (i == defaultOut);
        devices.push_back(std::move(dev));
    }
#endif

    return devices;
}

// ─── Transport ──────────────────────────────────────────────────────────────

void AudioEngine::play()
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (!m_impl->stream) return;

    if (m_state.load() != TransportState::Playing) {
        // Stop any active stream FIRST, BEFORE changing state.
        // This prevents a rogue audio callback from firing with
        // state=Playing while the old stream is still active — which
        // would advance m_playPosition and the sync clock, then
        // clock->reset() below would undo the clock advance but NOT
        // the position advance, creating a persistent ~10ms A/V offset.
        if (Pa_IsStreamActive(m_impl->stream) || !Pa_IsStreamStopped(m_impl->stream)) {
            Pa_StopStream(m_impl->stream);
        }

        // Now safe to change state — stream is stopped, no callbacks.
        m_state.store(TransportState::Playing);

        PaError err = Pa_StartStream(m_impl->stream);
        if (err != paNoError) {
            spdlog::error("AudioEngine: play Pa_StartStream failed: {}",
                          Pa_GetErrorText(err));
            m_state.store(TransportState::Paused);
        } else {
            // Start the sync clock AFTER the audio stream is active.
            // This prevents the 50-100ms Pa_StopStream + Pa_StartStream
            // gap from letting wall-clock extrapolation run video ahead
            // of audio.  Re-anchor the clock at the current position so
            // the extrapolation origin matches this moment, not the
            // earlier reset() call.
            auto* clock = m_syncClock.load();
            if (clock) {
                const int64_t tick = (m_config.sampleRate > 0)
                    ? (m_playPosition.load() * 48000)
                      / static_cast<int64_t>(m_config.sampleRate)
                    : 0;
                clock->reset(tick);
                clock->setRunning(true);
            }
        }
    }
#endif
}

void AudioEngine::pause()
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (!m_impl->stream) return;

    m_state.store(TransportState::Paused);

    auto* clock = m_syncClock.load();
    if (clock) clock->setRunning(false);

    Pa_StopStream(m_impl->stream);
#endif
}

void AudioEngine::stop()
{
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (!m_impl->stream) return;

    m_state.store(TransportState::Stopped);

    // Stop the stream FIRST so the callback can't advance m_playPosition
    // after we reset it below.
    Pa_StopStream(m_impl->stream);

    m_playPosition.store(0);

    // Reset all time-stretchers
    m_stretchers.clear();

    auto* clock = m_syncClock.load();
    if (clock) {
        clock->setRunning(false);
        clock->reset(0);
    }
#endif
}

void AudioEngine::seekToFrame(int64_t frame)
{
    m_playPosition.store(frame);
    m_seekGeneration.fetch_add(1, std::memory_order_release);

    // Signal audio thread to reset stretchers (thread-safe)
    m_resetStretchers.store(true, std::memory_order_release);

    auto* clock = m_syncClock.load();
    if (clock) {
        // Convert sample frame to TimeTick (48000 ticks/sec)
        const int64_t tick = (m_config.sampleRate > 0)
            ? (frame * 48000) / static_cast<int64_t>(m_config.sampleRate)
            : 0;
        clock->reset(tick);
    }
}

void AudioEngine::scrub(int64_t frame, int64_t durationFrames)
{
    m_playPosition.store(frame);
    m_scrubEnd.store(frame + durationFrames);
    m_seekGeneration.fetch_add(1, std::memory_order_release);
    m_state.store(TransportState::Scrubbing);

#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if (m_impl->stream && !Pa_IsStreamActive(m_impl->stream)) {
        Pa_StartStream(m_impl->stream);
    }
#endif
}

TransportState AudioEngine::transportState() const noexcept
{
    return m_state.load();
}

int64_t AudioEngine::currentFrame() const noexcept
{
    return m_playPosition.load();
}

double AudioEngine::currentTimeSeconds() const noexcept
{
    return (m_config.sampleRate > 0)
        ? static_cast<double>(m_playPosition.load()) / m_config.sampleRate
        : 0.0;
}

// ─── Mixer sources ──────────────────────────────────────────────────────────

void AudioEngine::setTrackSources(std::vector<AudioTrackSource> sources)
{
    std::lock_guard lock(m_sourcesMutex);
    m_sources = std::move(sources);
}

void AudioEngine::clearTrackSources()
{
    std::lock_guard lock(m_sourcesMutex);
    m_sources.clear();
}

void AudioEngine::updateSourceLevels(uint64_t trackId, float volume, float pan, bool muted)
{
    std::lock_guard lock(m_sourcesMutex);
    for (auto& src : m_sources) {
        if (src.trackId == trackId) {
            src.volume = volume;
            src.pan    = pan;
            src.muted  = muted;
        }
    }
}

bool AudioEngine::hasTrackSources() const
{
    std::lock_guard lock(m_sourcesMutex);
    return !m_sources.empty();
}

void AudioEngine::resetStretchers()
{
    m_resetStretchers.store(true, std::memory_order_release);
}

void AudioEngine::setMasterVolume(float vol) noexcept
{
    m_masterVolume.store(vol);
}

float AudioEngine::masterVolume() const noexcept
{
    return m_masterVolume.load();
}

// ─── Metering ───────────────────────────────────────────────────────────────

AudioMeter AudioEngine::meter() const noexcept
{
    return {
        m_peakL.load(), m_peakR.load(),
        m_rmsL.load(),  m_rmsR.load()
    };
}

// ─── Sync clock ─────────────────────────────────────────────────────────────

void AudioEngine::setSyncClock(AVSyncClock* clock) noexcept
{
    m_syncClock.store(clock);
}

void AudioEngine::setPlaybackSpeed(double speed) noexcept
{
    m_playbackSpeedFixed.store(static_cast<int64_t>(speed * 1000.0));
    // Signal audio thread to reset stretchers (thread-safe)
    m_resetStretchers.store(true, std::memory_order_release);
}

double AudioEngine::playbackSpeed() const noexcept
{
    return static_cast<double>(m_playbackSpeedFixed.load()) / 1000.0;
}

// ─── Configuration ──────────────────────────────────────────────────────────

const AudioEngineConfig& AudioEngine::config() const noexcept
{
    return m_config;
}

uint32_t AudioEngine::sampleRate() const noexcept
{
    return m_config.sampleRate;
}

const std::string& AudioEngine::lastError() const noexcept
{
    return m_lastError;
}

// ─── PortAudio callback (static) ────────────────────────────────────────────

int AudioEngine::paCallback(const void* /*input*/, void* output,
                             unsigned long frameCount,
                             const ::PaStreamCallbackTimeInfo* /*timeInfo*/,
                             unsigned long /*statusFlags*/,
                             void* userData)
{
    auto* engine = static_cast<AudioEngine*>(userData);
    auto* out    = static_cast<float*>(output);
    return engine->onAudioCallback(out, frameCount);
}

// ─── Audio callback (instance) ──────────────────────────────────────────────

int AudioEngine::onAudioCallback(float* output, unsigned long frameCount)
{
    const auto channels = m_config.channels;
    const auto totalSamples = frameCount * channels;

    // Zero output first
    std::memset(output, 0, totalSamples * sizeof(float));

    const auto state = m_state.load();
    const auto seekGen = m_seekGeneration.load(std::memory_order_acquire);

    if (state == TransportState::Stopped || state == TransportState::Paused) {
        // Output silence but keep the stream alive (paContinue).
        // The explicit Pa_StopStream() calls in stop()/pause() handle
        // the actual stream lifecycle.  Returning paComplete here would
        // permanently deactivate the stream, preventing Pa_StartStream
        // from working on the next play().
#ifdef ROUNDTABLE_HAS_PORTAUDIO
        return paContinue;
#else
        return 0;
#endif
    }

    const int64_t playPos = m_playPosition.load();

    // Scrub mode: stop after burst
    if (state == TransportState::Scrubbing) {
        const int64_t scrubEnd = m_scrubEnd.load();
        if (playPos >= scrubEnd) {
            m_state.store(TransportState::Paused);
            // Return paContinue so the stream stays alive for the next scrub.
#ifdef ROUNDTABLE_HAS_PORTAUDIO
            return paContinue;
#else
            return 0;
#endif
        }
    }

    // Lock-free copy of sources (we hold the lock briefly)
    std::vector<AudioTrackSource> sources;
    {
        std::lock_guard lock(m_sourcesMutex);
        sources = m_sources;
    }

    // Check for solo tracks
    bool hasSolo = false;
    for (const auto& src : sources) {
        if (src.solo) { hasSolo = true; break; }
    }

    // Read current playback speed (fixed-point * 1000 for lock-free)
    const double speed = static_cast<double>(
        m_playbackSpeedFixed.load(std::memory_order_relaxed)) / 1000.0;

    // Reset stretchers if signaled by UI thread (thread-safe: done on audio thread)
    if (m_resetStretchers.exchange(false, std::memory_order_acquire)) {
        for (auto& [id, ts] : m_stretchers)
            ts.reset();
    }

    // Mix all active sources.
    for (const auto& src : sources) {
        mixSource(src, output, frameCount, playPos, speed, hasSolo);
    }

    // Apply master volume
    const float masterVol = m_masterVolume.load();
    if (masterVol != 1.0f) {
        for (unsigned long i = 0; i < totalSamples; ++i) {
            output[i] *= masterVol;
        }
    }

    // Update metering
    float peakL = 0.0f, peakR = 0.0f;
    float sumL  = 0.0f, sumR  = 0.0f;

    for (unsigned long f = 0; f < frameCount; ++f) {
        const float l = (channels >= 1) ? output[f * channels]     : 0.0f;
        const float r = (channels >= 2) ? output[f * channels + 1] : l;

        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
        sumL += l * l;
        sumR += r * r;
    }

    m_peakL.store(peakL);
    m_peakR.store(peakR);
    m_rmsL.store(std::sqrt(sumL / static_cast<float>(frameCount)));
    m_rmsR.store(std::sqrt(sumR / static_cast<float>(frameCount)));

    // Advance play position by speed-adjusted frame count.
    // For 2x speed, we skip ahead 2 audio frames per callback frame;
    // for -1x (reverse), we move backwards.
    // Guard with seek generation: if a seek/scrub happened during this
    // callback, discard our advance so the new seek position is preserved.
    if (m_seekGeneration.load(std::memory_order_acquire) == seekGen) {
        const int64_t speedAdv = m_playbackSpeedFixed.load(std::memory_order_relaxed);
        const int64_t advanceDelta = (static_cast<int64_t>(frameCount) * speedAdv) / 1000;
        const int64_t newPos = playPos + advanceDelta;
        m_playPosition.store(std::max<int64_t>(0, newPos));

        // Only advance the sync clock during playback.  During scrub the
        // m_playPosition advance above is needed (to detect scrubEnd),
        // but the master clock must stay pinned to the scrub position
        // set by seekInternal().  Otherwise each scrub burst drifts the
        // clock ~2048 samples ahead, and for slow .mp4 decodes the
        // drift can exceed 100ms — producing audible A/V desync when
        // playback resumes.
        if (state == TransportState::Playing) {
            auto* clock = m_syncClock.load();
            if (clock) {
                clock->advance(static_cast<int64_t>(frameCount), m_config.sampleRate);
            }
        }
    }

#ifdef ROUNDTABLE_HAS_PORTAUDIO
    return paContinue;
#else
    return 0;
#endif
}

// ─── Mix one source ─────────────────────────────────────────────────────────

void AudioEngine::mixSource(const AudioTrackSource& src, float* output,
                             unsigned long frameCount, int64_t playPos,
                             double speed, bool hasSolo)
{
    // Mute/solo logic
    if (src.muted) return;
    if (hasSolo && !src.solo) return;

    const AudioSourceView view = resolveAudioSourceView(src);
    if (!view.samples || view.totalFrames <= 0) return;

    // Apply audio effects (channel fill) to a local copy if needed
    std::vector<float> fxBuf;
    const float* mixSamples = view.samples;
    if (!src.audioEffects.empty() && view.channels == 2) {
        const size_t totalSamples = static_cast<size_t>(view.totalFrames) * view.channels;
        fxBuf.assign(view.samples, view.samples + totalSamples);
        for (auto fxType : src.audioEffects) {
            if (fxType == EffectType::FillLeftWithRight) {
                for (size_t s = 0; s < totalSamples; s += 2)
                    fxBuf[s] = fxBuf[s + 1];
            } else if (fxType == EffectType::FillRightWithLeft) {
                for (size_t s = 0; s < totalSamples; s += 2)
                    fxBuf[s + 1] = fxBuf[s];
            }
        }
        mixSamples = fxBuf.data();
    }

    // Create a modified view pointing to the (possibly effect-processed) samples
    AudioSourceView effView = view;
    effView.samples = mixSamples;

    const auto outCh = m_config.channels;

    // Combine global shuttle speed with per-clip speed.
    // Shuttle speed controls the transport (JKL), clip speed is from
    // the Speed/Duration dialog.  The effective speed is the product.
    const double clipSpd   = src.clipSpeed;
    const double effSpeed  = speed * clipSpd;
    const double absEff    = std::abs(effSpeed);

    // At exactly 1x effective speed, use direct copy (no time-stretch needed)
    if (absEff > 0.999 && absEff < 1.001 && effSpeed > 0.0) {
        for (unsigned long f = 0; f < frameCount; ++f) {
            const int64_t timelineFrame = playPos + static_cast<int64_t>(f);
            const int64_t srcFrame = timelineFrame - effView.startFrame;

            if (srcFrame < 0 || srcFrame >= effView.totalFrames) continue;

            float vol = src.volume;
            if (src.fadeEnvelope && effView.totalFrames > 0) {
                const float normalizedPos = static_cast<float>(srcFrame)
                                          / static_cast<float>(effView.totalFrames);
                vol *= src.fadeEnvelope(normalizedPos);
            }

            const auto srcIdx = static_cast<size_t>(srcFrame * effView.channels);

            if (effView.channels == 1) {
                const float sample = effView.samples[srcIdx] * vol;
                float panL, panR;
                computePan(src.pan, 1, panL, panR);
                if (outCh >= 1) output[f * outCh]     += sample * panL;
                if (outCh >= 2) output[f * outCh + 1] += sample * panR;
            } else if (effView.channels == 2) {
                const float sL = effView.samples[srcIdx]     * vol;
                const float sR = effView.samples[srcIdx + 1] * vol;
                float panL, panR;
                computePan(src.pan, 2, panL, panR);
                if (outCh >= 1) output[f * outCh]     += sL * panL;
                if (outCh >= 2) output[f * outCh + 1] += sR * panR;
            } else {
                for (uint32_t c = 0; c < effView.channels && c < outCh; ++c)
                    output[f * outCh + c] += effView.samples[srcIdx + c] * vol;
            }
        }
        return;
    }

    // Non-1x effective speed: use SoundTouch for pitch-preserved playback,
    // or simple sample-skipping if maintainPitch is false.
    if (src.maintainPitch) {
        // Compute source offset before creating a stretcher.
        // Skip clips that are completely out of range to avoid
        // initializing a SoundTouch instance with a bad read position.
        const int64_t srcStart = playPos - effView.startFrame;
        if (srcStart >= effView.totalFrames)
            return;  // clip is fully past

        // Get or create a per-track TimeStretch instance.
        auto it = m_stretchers.find(src.trackId);
        if (it == m_stretchers.end()) {
            // Don't allocate a stretcher for a clip that hasn't started yet
            if (srcStart < 0)
                return;
            it = m_stretchers.emplace(src.trackId,
                TimeStretch(effView.channels, m_config.sampleRate)).first;
        }
        auto& ts = it->second;
        ts.setSpeed(effSpeed);

        // Source start is just the linear offset into the source buffer.
        // SoundTouch handles consuming source samples at the correct rate
        // based on the tempo setting — we must NOT pre-multiply by clipSpeed
        // or the audio gets double-sped.
        ts.process(effView.samples, effView.totalFrames, srcStart,
                   output, frameCount,
                   src.volume, src.pan, outCh, effView.channels,
                   src.fadeEnvelope, effView.totalFrames);
    } else {
        // No pitch compensation — simple sample-skipping (pitch shifts naturally)
        float panL, panR;
        computePan(src.pan, effView.channels, panL, panR);

        for (unsigned long f = 0; f < frameCount; ++f) {
            const int64_t timelineFrame = playPos + static_cast<int64_t>(f);
            // Map timeline frame to source frame using clip speed
            const int64_t srcFrame = static_cast<int64_t>(
                static_cast<double>(timelineFrame - effView.startFrame) * std::abs(clipSpd));

            if (srcFrame < 0 || srcFrame >= effView.totalFrames) continue;

            float vol = src.volume;
            if (src.fadeEnvelope && effView.totalFrames > 0) {
                const float normPos = static_cast<float>(srcFrame)
                                    / static_cast<float>(effView.totalFrames);
                vol *= src.fadeEnvelope(normPos);
            }

            const auto si = static_cast<size_t>(srcFrame * effView.channels);
            const auto oi = static_cast<size_t>(f * outCh);

            if (effView.channels == 1) {
                const float s = effView.samples[si] * vol;
                if (outCh >= 1) output[oi]     += s * panL;
                if (outCh >= 2) output[oi + 1] += s * panR;
            } else if (effView.channels == 2) {
                if (outCh >= 1) output[oi]     += effView.samples[si]     * vol * panL;
                if (outCh >= 2) output[oi + 1] += effView.samples[si + 1] * vol * panR;
            } else {
                for (uint32_t c = 0; c < effView.channels && c < outCh; ++c)
                    output[oi + c] += effView.samples[si + c] * vol;
            }
        }
    }
}

} // namespace rt

