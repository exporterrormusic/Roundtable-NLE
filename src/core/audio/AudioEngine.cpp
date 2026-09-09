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
#include <limits>

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
    view.buffer = src.sampleBuffer;
    view.samples = src.sampleBuffer ? src.sampleBuffer->data() : src.samples;
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

struct AudioEngine::MixerSnapshot
{
    struct RealtimeLevels
    {
        std::atomic<float> volume{1.0f};
        std::atomic<float> pan{0.0f};
        std::atomic<bool> muted{false};
    };

    struct PreparedSource
    {
        AudioTrackSource source;
        std::unique_ptr<RealtimeLevels> levels;
        std::unique_ptr<TimeStretch> stretcher;
    };

    std::vector<PreparedSource> sources;
    double playbackSpeed{1.0};
};

// ─── Constructor / Destructor ───────────────────────────────────────────────

AudioEngine::AudioEngine()
    : m_impl(std::make_unique<Impl>())
{
    m_activeMixerSnapshot = std::make_shared<MixerSnapshot>();
    m_callbackMixerSnapshot.store(m_activeMixerSnapshot.get(),
                                  std::memory_order_release);
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
    resetCallbackStats();
    resetStretchers();

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
    {
        std::lock_guard lock(m_snapshotUpdateMutex);
        m_retiredSnapshots.clear();
    }
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

    resetStretchers();

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

    resetStretchers();

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
    resetStretchers();
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
    std::lock_guard lock(m_snapshotUpdateMutex);
    publishTrackSourcesLocked(std::move(sources));
}

void AudioEngine::clearTrackSources()
{
    setTrackSources({});
}

void AudioEngine::updateSourceLevels(uint64_t trackId, float volume, float pan, bool muted)
{
    std::lock_guard lock(m_snapshotUpdateMutex);
    const auto& snapshot = m_activeMixerSnapshot;
    if (!snapshot) return;

    for (auto& prepared : snapshot->sources) {
        if (prepared.source.trackId == trackId && prepared.levels) {
            prepared.levels->volume.store(volume, std::memory_order_relaxed);
            prepared.levels->pan.store(pan, std::memory_order_relaxed);
            prepared.levels->muted.store(muted, std::memory_order_release);
        }
    }
}

bool AudioEngine::hasTrackSources() const
{
    std::lock_guard lock(m_snapshotUpdateMutex);
    const auto& snapshot = m_activeMixerSnapshot;
    return snapshot && !snapshot->sources.empty();
}

void AudioEngine::resetStretchers()
{
    std::lock_guard lock(m_snapshotUpdateMutex);
    publishTrackSourcesLocked(copyTrackSourcesLocked());
}

std::vector<AudioTrackSource> AudioEngine::copyTrackSourcesLocked() const
{
    std::vector<AudioTrackSource> sources;
    const auto& snapshot = m_activeMixerSnapshot;
    if (!snapshot) return sources;

    sources.reserve(snapshot->sources.size());
    for (const auto& prepared : snapshot->sources) {
        auto source = prepared.source;
        if (prepared.levels) {
            source.volume = prepared.levels->volume.load(std::memory_order_relaxed);
            source.pan = prepared.levels->pan.load(std::memory_order_relaxed);
            source.muted = prepared.levels->muted.load(std::memory_order_acquire);
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

void AudioEngine::publishTrackSourcesLocked(std::vector<AudioTrackSource> sources)
{
    auto next = std::make_shared<MixerSnapshot>();
    next->sources.reserve(sources.size());
    const double playbackSpeed = static_cast<double>(
        m_playbackSpeedFixed.load(std::memory_order_relaxed)) / 1000.0;
    next->playbackSpeed = playbackSpeed;

    for (auto& source : sources) {
        AudioSourceView view = resolveAudioSourceView(source);
        if (view.samples && view.totalFrames > 0 && view.channels > 0) {
            source.sampleBuffer = std::move(view.buffer);
            source.samples = view.samples;
            source.totalFrames = view.totalFrames;
            source.startFrame = view.startFrame;
            source.channels = view.channels;
            source.sampleRate = view.sampleRate;
        }
        source.sampleProvider.reset();

        const bool validStereoSize = source.totalFrames > 0
            && static_cast<uint64_t>(source.totalFrames)
                <= std::numeric_limits<size_t>::max() / 2;
        if (!source.audioEffects.empty() && source.samples
            && validStereoSize && source.channels == 2) {
            const size_t sampleCount = static_cast<size_t>(source.totalFrames) * 2;
            auto processed = std::make_shared<std::vector<float>>(
                source.samples, source.samples + sampleCount);
            for (const auto effect : source.audioEffects) {
                if (effect == EffectType::FillLeftWithRight) {
                    for (size_t i = 0; i < sampleCount; i += 2)
                        (*processed)[i] = (*processed)[i + 1];
                } else if (effect == EffectType::FillRightWithLeft) {
                    for (size_t i = 0; i < sampleCount; i += 2)
                        (*processed)[i + 1] = (*processed)[i];
                }
            }
            source.sampleBuffer = std::move(processed);
            source.samples = source.sampleBuffer->data();
        }
        source.audioEffects.clear();

        MixerSnapshot::PreparedSource prepared;
        prepared.source = std::move(source);
        prepared.levels = std::make_unique<MixerSnapshot::RealtimeLevels>();
        prepared.levels->volume.store(prepared.source.volume, std::memory_order_relaxed);
        prepared.levels->pan.store(prepared.source.pan, std::memory_order_relaxed);
        prepared.levels->muted.store(prepared.source.muted, std::memory_order_relaxed);
        const double effectiveSpeed =
            playbackSpeed * prepared.source.clipSpeed;
        const bool needsStretcher = effectiveSpeed <= 0.0
            || std::abs(effectiveSpeed - 1.0) >= 0.001;
        if (prepared.source.maintainPitch && prepared.source.channels > 0
            && needsStretcher) {
            prepared.stretcher = std::make_unique<TimeStretch>(
                prepared.source.channels, m_config.sampleRate);
            prepared.stretcher->setSpeed(effectiveSpeed);
        }
        next->sources.push_back(std::move(prepared));
    }

    auto previous = std::move(m_activeMixerSnapshot);
    m_activeMixerSnapshot = std::move(next);
    m_callbackMixerSnapshot.store(m_activeMixerSnapshot.get(),
                                  std::memory_order_seq_cst);
    if (previous)
        m_retiredSnapshots.push_back(std::move(previous));

    if (m_callbackReaders.load(std::memory_order_seq_cst) == 0)
        m_retiredSnapshots.clear();
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

AudioCallbackStats AudioEngine::callbackStats() const noexcept
{
    return {
        m_callbackCount.load(std::memory_order_relaxed),
        m_outputUnderflows.load(std::memory_order_relaxed),
        m_outputOverflows.load(std::memory_order_relaxed),
        m_callbacksOverBudget.load(std::memory_order_relaxed),
        m_maxCallbackMicros.load(std::memory_order_relaxed)
    };
}

void AudioEngine::resetCallbackStats() noexcept
{
    m_callbackCount.store(0, std::memory_order_relaxed);
    m_outputUnderflows.store(0, std::memory_order_relaxed);
    m_outputOverflows.store(0, std::memory_order_relaxed);
    m_callbacksOverBudget.store(0, std::memory_order_relaxed);
    m_maxCallbackMicros.store(0, std::memory_order_relaxed);
}

// ─── Sync clock ─────────────────────────────────────────────────────────────

void AudioEngine::setSyncClock(AVSyncClock* clock) noexcept
{
    m_syncClock.store(clock);
}

void AudioEngine::setPlaybackSpeed(double speed)
{
    const int64_t fixedSpeed = static_cast<int64_t>(speed * 1000.0);
    if (m_playbackSpeedFixed.exchange(fixedSpeed) == fixedSpeed)
        return;
    resetStretchers();
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

void AudioEngine::recordCallbackStats(
    std::chrono::steady_clock::time_point started,
    unsigned long frameCount,
    unsigned long statusFlags) noexcept
{
    const auto elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());

    m_callbackCount.fetch_add(1, std::memory_order_relaxed);
#ifdef ROUNDTABLE_HAS_PORTAUDIO
    if ((statusFlags & paOutputUnderflow) != 0)
        m_outputUnderflows.fetch_add(1, std::memory_order_relaxed);
    if ((statusFlags & paOutputOverflow) != 0)
        m_outputOverflows.fetch_add(1, std::memory_order_relaxed);
#else
    (void)statusFlags;
#endif

    const uint64_t budgetMicros = m_config.sampleRate > 0
        ? (static_cast<uint64_t>(frameCount) * 1000000ull) / m_config.sampleRate
        : 0;
    if (budgetMicros > 0 && elapsed > budgetMicros)
        m_callbacksOverBudget.fetch_add(1, std::memory_order_relaxed);

    uint64_t previousMax = m_maxCallbackMicros.load(std::memory_order_relaxed);
    while (elapsed > previousMax
           && !m_maxCallbackMicros.compare_exchange_weak(
               previousMax, elapsed, std::memory_order_relaxed)) {
    }
}

// ─── PortAudio callback (static) ────────────────────────────────────────────

int AudioEngine::paCallback(const void* /*input*/, void* output,
                             unsigned long frameCount,
                             const ::PaStreamCallbackTimeInfo* /*timeInfo*/,
                             unsigned long statusFlags,
                             void* userData)
{
    auto* engine = static_cast<AudioEngine*>(userData);
    auto* out    = static_cast<float*>(output);
    return engine->onAudioCallback(out, frameCount, statusFlags);
}

// ─── Audio callback (instance) ──────────────────────────────────────────────

int AudioEngine::onAudioCallback(float* output, unsigned long frameCount,
                                 unsigned long statusFlags)
{
    const auto callbackStarted = std::chrono::steady_clock::now();
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
        recordCallbackStats(callbackStarted, frameCount, statusFlags);
        return paContinue;
#else
        recordCallbackStats(callbackStarted, frameCount, statusFlags);
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
            recordCallbackStats(callbackStarted, frameCount, statusFlags);
            return paContinue;
#else
            recordCallbackStats(callbackStarted, frameCount, statusFlags);
            return 0;
#endif
        }
    }

    m_callbackReaders.fetch_add(1, std::memory_order_seq_cst);
    struct SnapshotReadGuard {
        std::atomic<uint32_t>& readers;
        ~SnapshotReadGuard()
        {
            readers.fetch_sub(1, std::memory_order_seq_cst);
        }
    } snapshotReadGuard{m_callbackReaders};
    MixerSnapshot* snapshot =
        m_callbackMixerSnapshot.load(std::memory_order_seq_cst);

    // Check for solo tracks
    bool hasSolo = false;
    if (snapshot) {
        for (const auto& prepared : snapshot->sources) {
            if (prepared.source.solo) { hasSolo = true; break; }
        }
    }

    // Mode and speed belong to the same immutable mixer snapshot.
    const double speed = snapshot ? snapshot->playbackSpeed : 1.0;

    // Mix all active sources.
    if (snapshot) {
        for (const auto& prepared : snapshot->sources) {
            const float volume = prepared.levels
                ? prepared.levels->volume.load(std::memory_order_relaxed)
                : prepared.source.volume;
            const float pan = prepared.levels
                ? prepared.levels->pan.load(std::memory_order_relaxed)
                : prepared.source.pan;
            const bool muted = prepared.levels
                ? prepared.levels->muted.load(std::memory_order_acquire)
                : prepared.source.muted;
            mixSource(prepared.source, prepared.stretcher.get(), output,
                      frameCount, playPos, speed, hasSolo,
                      volume, pan, muted);
        }
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
        const int64_t speedAdv = static_cast<int64_t>(std::llround(speed * 1000.0));
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

    recordCallbackStats(callbackStarted, frameCount, statusFlags);

#ifdef ROUNDTABLE_HAS_PORTAUDIO
    return paContinue;
#else
    return 0;
#endif
}

// ─── Mix one source ─────────────────────────────────────────────────────────

void AudioEngine::mixSource(const AudioTrackSource& src, TimeStretch* stretcher,
                             float* output,
                             unsigned long frameCount, int64_t playPos,
                             double speed, bool hasSolo,
                             float volume, float pan, bool muted)
{
    // Mute/solo logic
    if (muted) return;
    if (hasSolo && !src.solo) return;

    AudioSourceView view;
    view.samples = src.samples;
    view.totalFrames = src.totalFrames;
    view.startFrame = src.startFrame;
    view.channels = src.channels;
    view.sampleRate = src.sampleRate;
    if (!view.samples || view.totalFrames <= 0) return;

    const AudioSourceView& effView = view;

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

            float vol = volume;
            if (src.fadeEnvelope && effView.totalFrames > 0) {
                const float normalizedPos = static_cast<float>(srcFrame)
                                          / static_cast<float>(effView.totalFrames);
                vol *= src.fadeEnvelope(normalizedPos);
            }

            const auto srcIdx = static_cast<size_t>(srcFrame * effView.channels);

            if (effView.channels == 1) {
                const float sample = effView.samples[srcIdx] * vol;
                float panL, panR;
                computePan(pan, 1, panL, panR);
                if (outCh >= 1) output[f * outCh]     += sample * panL;
                if (outCh >= 2) output[f * outCh + 1] += sample * panR;
            } else if (effView.channels == 2) {
                const float sL = effView.samples[srcIdx]     * vol;
                const float sR = effView.samples[srcIdx + 1] * vol;
                float panL, panR;
                computePan(pan, 2, panL, panR);
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
        // Skip clips outside the prepared stretcher's playable range.
        const int64_t srcStart = playPos - effView.startFrame;
        if (srcStart >= effView.totalFrames)
            return;  // clip is fully past

        if (!stretcher || srcStart < 0)
            return;
        auto& ts = *stretcher;

        // Source start is just the linear offset into the source buffer.
        // SoundTouch handles consuming source samples at the correct rate
        // based on the tempo setting — we must NOT pre-multiply by clipSpeed
        // or the audio gets double-sped.
        ts.process(effView.samples, effView.totalFrames, srcStart,
                   output, frameCount,
                   volume, pan, outCh, effView.channels,
                   src.fadeEnvelope, effView.totalFrames);
    } else {
        // No pitch compensation — simple sample-skipping (pitch shifts naturally)
        float panL, panR;
        computePan(pan, effView.channels, panL, panR);

        for (unsigned long f = 0; f < frameCount; ++f) {
            const int64_t timelineFrame = playPos + static_cast<int64_t>(f);
            // Map timeline frame to source frame using clip speed
            const int64_t srcFrame = static_cast<int64_t>(
                static_cast<double>(timelineFrame - effView.startFrame) * std::abs(clipSpd));

            if (srcFrame < 0 || srcFrame >= effView.totalFrames) continue;

            float vol = volume;
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
