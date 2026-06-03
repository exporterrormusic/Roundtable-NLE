/*
 * AudioProcessor — base interface for real-time audio DSP processors.
 *
 * Every processor in the audiofx module (ParametricEQ, Dynamics, …) follows
 * the same host contract used by real DAW plugins:
 *
 *   prepare(sampleRate, channels)  — control thread, may allocate / recompute
 *   process(data, frames)          — audio thread, NO allocation, NO locks
 *   reset()                        — clear filter / envelope state
 *
 * Audio is interleaved 32-bit float: `data` holds frames * channels samples,
 * laid out [f0c0, f0c1, …, f1c0, …]. Processing is in place.
 *
 * Parameter changes (set* methods) happen on the control thread. They may
 * recompute coefficients but must never resize buffers that process() reads —
 * structural changes (channel count, band count) go through prepare().
 */

#pragma once

#include <cmath>
#include <memory>

namespace rt::audiofx {

// ── dB / linear helpers ──────────────────────────────────────────────────────

/// Convert decibels to a linear amplitude multiplier.
[[nodiscard]] inline float dbToLinear(float db) noexcept
{
    return std::pow(10.0f, db * 0.05f);
}

/// Convert a linear amplitude to decibels. Floored at -180 dB to avoid -inf.
[[nodiscard]] inline float linearToDb(float lin) noexcept
{
    const float a = std::fabs(lin);
    return a > 1e-9f ? 20.0f * std::log10(a) : -180.0f;
}

/// One-pole smoothing coefficient for a given time constant (seconds).
/// env = coef*env + (1-coef)*target reaches ~63% of target in `seconds`.
[[nodiscard]] inline float timeConstantCoef(float seconds, double sampleRate) noexcept
{
    if (seconds <= 0.0f) return 0.0f;  // instantaneous
    return static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
}

// ── Base class ───────────────────────────────────────────────────────────────

class AudioProcessor
{
public:
    virtual ~AudioProcessor() = default;

    /// Configure for a sample rate / channel layout. Control thread only.
    /// May allocate and recompute coefficients. Implicitly resets state.
    virtual void prepare(double sampleRate, int channels) = 0;

    /// Clear internal state (filter histories, envelopes). Audio-thread safe.
    virtual void reset() noexcept = 0;

    /// Process `frames` of interleaved float audio in place.
    /// Audio-thread safe: no allocation, no locks. When disabled or inactive
    /// the processor must leave `data` untouched.
    virtual void process(float* data, int frames) noexcept = 0;

    /// Whether the processor currently alters the signal. Lets a host skip
    /// processors that are at unity (e.g. an EQ with all bands flat).
    [[nodiscard]] virtual bool isActive() const noexcept = 0;

    /// Deep copy of settings (NOT runtime state — clones start un-prepared).
    /// Lets a host snapshot a chain for an offline render without touching the
    /// live processor's filter / envelope state.
    [[nodiscard]] virtual std::unique_ptr<AudioProcessor> clone() const = 0;

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool e) noexcept { m_enabled = e; }

    /// Stable identifier used for serialization / factory creation.
    [[nodiscard]] const char* typeName() const noexcept { return m_typeName; }

protected:
    explicit AudioProcessor(const char* typeName) noexcept : m_typeName(typeName) {}

    const char* m_typeName;
    bool        m_enabled{true};
    double      m_sampleRate{48000.0};
    int         m_channels{2};
};

} // namespace rt::audiofx
