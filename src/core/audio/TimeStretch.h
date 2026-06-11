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

    // Scratch buffer for feeding SoundTouch and receiving output
    static constexpr int kBlockSize = 512;
};

} // namespace rt
