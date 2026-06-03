/*
 * Biquad — single-channel second-order IIR filter (Transposed Direct Form II).
 *
 * Coefficients follow Robert Bristow-Johnson's "Audio EQ Cookbook" and are
 * normalized so a0 == 1. One Biquad holds the state (z1, z2) for one channel,
 * so a stereo filter needs two Biquads.
 *
 * Header-only and trivially copyable — coefficient design is cheap enough to
 * run on the control thread whenever a parameter changes.
 */

#pragma once

#include <cmath>

namespace rt::audiofx {

struct Biquad
{
    // Filter shapes supported by the cookbook designer.
    enum class Type
    {
        Peaking,    ///< Bell — boost/cut around freq (uses gainDb)
        LowShelf,   ///< Shelf below freq (uses gainDb)
        HighShelf,  ///< Shelf above freq (uses gainDb)
        LowPass,    ///< 12 dB/oct low-pass (gainDb ignored)
        HighPass,   ///< 12 dB/oct high-pass (gainDb ignored)
        Notch       ///< Band-reject (gainDb ignored)
    };

    // Normalized coefficients (a0 folded in).
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    // Per-channel state.
    float z1{0.0f}, z2{0.0f};

    void reset() noexcept { z1 = z2 = 0.0f; }

    /// Configure as a pass-through (no effect on the signal).
    void setIdentity() noexcept
    {
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f; a1 = 0.0f; a2 = 0.0f;
    }

    /// Process one sample (Transposed Direct Form II — good float behaviour).
    [[nodiscard]] inline float processSample(float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    /// Design coefficients from the RBJ cookbook.
    /// \param freq    Centre / corner frequency in Hz.
    /// \param Q       Quality factor (bandwidth); higher = narrower.
    /// \param gainDb  Boost/cut in dB (peaking & shelves only).
    void setCoefficients(Type type, double sampleRate,
                         double freq, double Q, double gainDb) noexcept
    {
        // Guard against degenerate inputs that would produce NaNs.
        if (sampleRate <= 0.0 || freq <= 0.0 || Q <= 1e-4) { setIdentity(); return; }
        // Keep below Nyquist with a little margin.
        const double maxFreq = sampleRate * 0.49;
        if (freq > maxFreq) freq = maxFreq;

        const double A     = std::pow(10.0, gainDb / 40.0);  // sqrt of linear gain
        const double w0    = 2.0 * 3.14159265358979323846 * freq / sampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * Q);

        double nb0, nb1, nb2, na0, na1, na2;

        switch (type) {
        case Type::Peaking:
            nb0 = 1.0 + alpha * A;
            nb1 = -2.0 * cosw0;
            nb2 = 1.0 - alpha * A;
            na0 = 1.0 + alpha / A;
            na1 = -2.0 * cosw0;
            na2 = 1.0 - alpha / A;
            break;

        case Type::LowShelf: {
            const double sq = 2.0 * std::sqrt(A) * alpha;
            nb0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + sq);
            nb1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
            nb2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - sq);
            na0 =          (A + 1.0) + (A - 1.0) * cosw0 + sq;
            na1 = -2.0 *   ((A - 1.0) + (A + 1.0) * cosw0);
            na2 =          (A + 1.0) + (A - 1.0) * cosw0 - sq;
            break;
        }

        case Type::HighShelf: {
            const double sq = 2.0 * std::sqrt(A) * alpha;
            nb0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + sq);
            nb1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
            nb2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - sq);
            na0 =          (A + 1.0) - (A - 1.0) * cosw0 + sq;
            na1 =  2.0 *   ((A - 1.0) - (A + 1.0) * cosw0);
            na2 =          (A + 1.0) - (A - 1.0) * cosw0 - sq;
            break;
        }

        case Type::LowPass:
            nb0 = (1.0 - cosw0) / 2.0;
            nb1 =  1.0 - cosw0;
            nb2 = (1.0 - cosw0) / 2.0;
            na0 =  1.0 + alpha;
            na1 = -2.0 * cosw0;
            na2 =  1.0 - alpha;
            break;

        case Type::HighPass:
            nb0 =  (1.0 + cosw0) / 2.0;
            nb1 = -(1.0 + cosw0);
            nb2 =  (1.0 + cosw0) / 2.0;
            na0 =  1.0 + alpha;
            na1 = -2.0 * cosw0;
            na2 =  1.0 - alpha;
            break;

        case Type::Notch:
        default:
            nb0 =  1.0;
            nb1 = -2.0 * cosw0;
            nb2 =  1.0;
            na0 =  1.0 + alpha;
            na1 = -2.0 * cosw0;
            na2 =  1.0 - alpha;
            break;
        }

        const double inv = 1.0 / na0;
        b0 = static_cast<float>(nb0 * inv);
        b1 = static_cast<float>(nb1 * inv);
        b2 = static_cast<float>(nb2 * inv);
        a1 = static_cast<float>(na1 * inv);
        a2 = static_cast<float>(na2 * inv);
    }
};

} // namespace rt::audiofx
