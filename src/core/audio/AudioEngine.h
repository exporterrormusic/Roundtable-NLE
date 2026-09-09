/*
 * AudioEngine — real-time audio playback via PortAudio (WASAPI).
 *
 * Features:
 *   • Real-time mixer: renders all active audio clips per callback
 *   • Per-track volume, pan, mute, solo
 *   • Sample-accurate scrubbing (short burst on seek)
 *   • WASAPI exclusive-mode support for low-latency playback
 *   • Master output metering (peak + RMS)
 *   • Drives the AVSyncClock — audio callback is the master time source
 *
 * The engine runs its own real-time thread via PortAudio callback. No
 * allocations or locks in the audio path — uses lock-free queues for
 * transport commands.
 *
 * Thread-safety: configure methods must be called from the main thread.
 * The audio callback runs on a dedicated real-time thread.
 */

#pragma once

#include "effects/Effect.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/TimeStretch.h"

// Forward declaration for PortAudio callback type
struct PaStreamCallbackTimeInfo;

namespace rt {

// Forward declarations
class AVSyncClock;
class AudioFile;

struct AudioSourceView
{
    std::shared_ptr<const std::vector<float>> buffer;
    const float* samples{nullptr};
    int64_t totalFrames{0};
    int64_t startFrame{0};
    uint32_t channels{2};
    uint32_t sampleRate{48000};
};

class AudioSampleProvider
{
public:
    virtual ~AudioSampleProvider() = default;
    [[nodiscard]] virtual AudioSourceView currentView() const = 0;
};

/// Audio device information
struct AudioDeviceInfo
{
    int         index{-1};
    std::string name;
    int         maxOutputChannels{0};
    double      defaultSampleRate{0.0};
    bool        isDefault{false};
};

/// Audio engine configuration
struct AudioEngineConfig
{
    uint32_t sampleRate{48000};
    uint32_t channels{2};          // Output channels (stereo)
    uint32_t framesPerBuffer{512}; // Callback buffer size (latency = frames / sampleRate)
    int      deviceIndex{-1};      // -1 = default device
    bool     exclusiveMode{false}; // WASAPI exclusive mode
};

/// Per-track audio source for the mixer
struct AudioTrackSource
{
    uint64_t            trackId{0};
    std::shared_ptr<AudioSampleProvider> sampleProvider;
    std::shared_ptr<const std::vector<float>> sampleBuffer;
    const float*        samples{nullptr};   // Interleaved float samples
    int64_t             totalFrames{0};     // Total frames in the buffer
    int64_t             startFrame{0};      // Frame offset into the timeline
    uint32_t            channels{2};
    uint32_t            sampleRate{48000};
    float               volume{1.0f};
    float               pan{0.0f};          // -1.0 left, 0.0 center, 1.0 right
    bool                muted{false};
    bool                solo{false};
    bool                maintainPitch{true};  ///< Preserve pitch at non-1x speed
    double              clipSpeed{1.0};       ///< Per-clip speed multiplier (from Speed/Duration)

    /// Channel-routing effects applied once when the source snapshot is published.
    std::vector<EffectType> audioEffects;

    /// Fade envelope (optional). Called with normalized position [0, 1].
    std::function<float(float)> fadeEnvelope;
};

/// Peak meter data
struct AudioMeter
{
    float peakL{0.0f};
    float peakR{0.0f};
    float rmsL{0.0f};
    float rmsR{0.0f};
};

/// Diagnostics collected without logging from the real-time callback.
struct AudioCallbackStats
{
    uint64_t callbackCount{0};
    uint64_t outputUnderflows{0};
    uint64_t outputOverflows{0};
    uint64_t callbacksOverBudget{0};
    uint64_t maxCallbackMicros{0};
};

/// Transport state
enum class TransportState : uint8_t
{
    Stopped,
    Playing,
    Paused,
    Scrubbing
};

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // Non-copyable
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ── Initialization ──────────────────────────────────────────────────

    /// Initialize PortAudio. Must be called before any other method.
    bool initialize(const AudioEngineConfig& config = {});

    /// Shutdown and release all resources.
    void shutdown();

    /// Is the engine initialized?
    [[nodiscard]] bool isInitialized() const noexcept;

    /// Enumerate available audio output devices.
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerateDevices() const;

    // ── Transport ───────────────────────────────────────────────────────

    /// Start playback from current position.
    void play();

    /// Pause playback (keeps position).
    void pause();

    /// Stop playback (resets position to 0).
    void stop();

    /// Seek to a specific sample frame position.
    void seekToFrame(int64_t frame);

    /// Scrub: play a short burst of audio at the given position.
    void scrub(int64_t frame, int64_t durationFrames = 2048);

    /// Get current transport state.
    [[nodiscard]] TransportState transportState() const noexcept;

    /// Get current playback position in sample frames.
    [[nodiscard]] int64_t currentFrame() const noexcept;

    /// Get current playback position in seconds.
    [[nodiscard]] double currentTimeSeconds() const noexcept;

    // ── Mixer sources ───────────────────────────────────────────────────

    /// Prepare and publish the active audio sources as one immutable mixer snapshot.
    /// Called from the main thread when playback begins or composition changes.
    void setTrackSources(std::vector<AudioTrackSource> sources);

    /// Clear all track sources.
    void clearTrackSources();

    /// Live-update volume/pan/mute for a single source identified by trackId
    /// (== clipId as set by AudioPlaybackService). No-op if no match.
    /// Thread-safe; avoids rebuilding the source list during scrubbing.
    void updateSourceLevels(uint64_t trackId, float volume, float pan, bool muted);

    /// Check whether any track sources are loaded.
    [[nodiscard]] bool hasTrackSources() const;

    /// Clear all time-stretcher instances (call when switching audio contexts).
    void resetStretchers();

    /// Set master volume (0.0 to 1.0+, >1.0 = boost).
    void setMasterVolume(float vol) noexcept;

    /// Get master volume.
    [[nodiscard]] float masterVolume() const noexcept;

    // ── Metering ────────────────────────────────────────────────────────

    /// Get current peak meter values (updated every callback).
    [[nodiscard]] AudioMeter meter() const noexcept;

    /// Get/reset real-time callback timing and device-underrun diagnostics.
    [[nodiscard]] AudioCallbackStats callbackStats() const noexcept;
    void resetCallbackStats() noexcept;

    // ── Sync clock ──────────────────────────────────────────────────────

    /// Set playback speed for the audio mixer (negative = reverse).
    /// Uses WSOLA time-stretch to preserve pitch at any speed.
    void setPlaybackSpeed(double speed);

    /// Get current playback speed.
    [[nodiscard]] double playbackSpeed() const noexcept;

    /// Attach an AV sync clock. The audio callback drives the clock.
    void setSyncClock(AVSyncClock* clock) noexcept;

    /// Get the current sync clock pointer.
    [[nodiscard]] AVSyncClock* syncClock() const noexcept { return m_syncClock.load(); }

    // ── Configuration ───────────────────────────────────────────────────

    /// Get the current configuration.
    [[nodiscard]] const AudioEngineConfig& config() const noexcept;

    /// Get sample rate.
    [[nodiscard]] uint32_t sampleRate() const noexcept;

    /// Get the last error message.
    [[nodiscard]] const std::string& lastError() const noexcept;

private:
    friend struct AudioEngineTestAccess;

    /// PortAudio stream callback (static → member)
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const ::PaStreamCallbackTimeInfo* timeInfo,
                          unsigned long statusFlags,
                          void* userData);

    /// Instance callback
    int onAudioCallback(float* output, unsigned long frameCount,
                        unsigned long statusFlags);

    /// Mix one source into the output buffer (speed-aware: handles reverse + fast)
    void mixSource(const AudioTrackSource& src, TimeStretch* stretcher,
                   float* output,
                   unsigned long frameCount, int64_t playPos,
                   double speed, bool hasSolo,
                   float volume, float pan, bool muted);

    struct MixerSnapshot;
    void publishTrackSourcesLocked(std::vector<AudioTrackSource> sources);
    [[nodiscard]] std::vector<AudioTrackSource> copyTrackSourcesLocked() const;
    void recordCallbackStats(std::chrono::steady_clock::time_point started,
                             unsigned long frameCount,
                             unsigned long statusFlags) noexcept;

    struct Impl;
    std::unique_ptr<Impl>     m_impl;
    AudioEngineConfig         m_config;
    std::string               m_lastError;

    // Atomics for lock-free audio thread communication
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<int64_t>        m_playPosition{0};
    std::atomic<float>          m_masterVolume{1.0f};
    std::atomic<int64_t>        m_scrubEnd{0};
    std::atomic<int64_t>        m_playbackSpeedFixed{1000};  // speed * 1000 (fixed-point)
    std::atomic<uint32_t>       m_seekGeneration{0};         // incremented on seek/scrub

    // Metering (written by audio thread, read by UI)
    std::atomic<float>          m_peakL{0.0f};
    std::atomic<float>          m_peakR{0.0f};
    std::atomic<float>          m_rmsL{0.0f};
    std::atomic<float>          m_rmsR{0.0f};
    std::atomic<uint64_t>       m_callbackCount{0};
    std::atomic<uint64_t>       m_outputUnderflows{0};
    std::atomic<uint64_t>       m_outputOverflows{0};
    std::atomic<uint64_t>       m_callbacksOverBudget{0};
    std::atomic<uint64_t>       m_maxCallbackMicros{0};

    // Sync clock (set from main thread, read by audio thread)
    std::atomic<AVSyncClock*>   m_syncClock{nullptr};

    // Prepared mixer state is swapped atomically.
    // Publishers use this mutex; the audio callback never takes it.
    mutable std::mutex m_snapshotUpdateMutex;
    std::shared_ptr<MixerSnapshot> m_activeMixerSnapshot;
    std::atomic<MixerSnapshot*> m_callbackMixerSnapshot{nullptr};
    std::atomic<uint32_t> m_callbackReaders{0};
    std::vector<std::shared_ptr<MixerSnapshot>> m_retiredSnapshots;
};

} // namespace rt
