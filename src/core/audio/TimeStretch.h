/*
 * TimeStretch.h — SoundTouch-based real-time time-stretching with pitch preservation.
 *
 * Wraps the SoundTouch library for high-quality pitch-preserving playback
 * at any speed (shuttle, reverse, slow-motion). Used by AudioEngine.
 *
 * Thread-safety: each instance is used exclusively by the audio callback
 * thread — no locking required.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace soundtouch { class SoundTouch; }

namespace rt {

class TimeStretch
{
public:
    explicit TimeStretch(uint32_t channels = 2, uint32_t sampleRate = 48000);
    ~TimeStretch();

    // Movable, not copyable
    TimeStretch(TimeStretch&& other) noexcept;
    TimeStretch& operator=(TimeStretch&& other) noexcept;
    TimeStretch(const TimeStretch&) = delete;
    TimeStretch& operator=(const TimeStretch&) = delete;

    /// Reset internal state (call on seek / source change).
    void reset();

    /// Set the time-stretch ratio. Negative = reverse playback.
    void setSpeed(double speed);

    /// Produce outFrames of pitch-preserved output into an additive buffer.
    /// `srcStart` is the buffer frame the playhead says this call's first
    /// output sample belongs to.  The first call starts reading there; later
    /// calls only compare it with where the stretcher's own output has got
    /// to, and restart from it when the two differ by more than
    /// kResyncSeconds (a playhead jump nothing reset us for, or drift).
    /// Returns the number of source frames logically consumed.
    int64_t process(const float* src, int64_t srcFrames,
                    int64_t srcStart,
                    float* output, unsigned long outFrames,
                    float volume, float pan, uint32_t outChannels,
                    uint32_t srcChannels,
                    const std::function<float(float)>& fadeEnvelope,
                    int64_t totalSrcFrames);

private:
    std::unique_ptr<soundtouch::SoundTouch> m_st;
    uint32_t m_channels;
    uint32_t m_sampleRate;
    double   m_speed{1.0};
    double   m_readPos{0.0};
    bool     m_initialized{false};

public:
    /// Buffer frame the next output sample corresponds to: the anchor plus
    /// output produced since x tempo (sign follows direction).  Unlike
    /// m_readPos (the FEED head, a block or more ahead inside SoundTouch's
    /// FIFO) this tracks what is actually being heard.
    [[nodiscard]] double outputSourcePosition() const noexcept;

    /// Re-anchor threshold.  Well above per-callback rounding (< 1 frame)
    /// and SoundTouch's constant internal latency, well below what is
    /// audible as A/V offset.
    static constexpr double kResyncSeconds = 0.020;

private:
    double   m_anchorPos{0.0};       ///< srcStart the current run began at
    int64_t  m_outSinceAnchor{0};    ///< output frames produced since then

    static constexpr int kBlockSize = 512;
    // Allocated and warmed when the mixer snapshot is prepared, never from
    // the real-time callback.
    std::vector<float> m_feedBuffer;
    std::vector<float> m_receiveBuffer;
};

} // namespace rt
