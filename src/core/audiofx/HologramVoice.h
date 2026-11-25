/*
 * HologramVoice — the "converter mask" voice: the clean voice blended with a
 * noise-driven vocoder copy of itself, so it sounds whispery and synthetic
 * while staying clear.
 *
 * The defaults reproduce Sin (NIKKE): measured against a long sample of her
 * lines, her voice is the dry voice plus an equal-level noise vocoder layer
 * (28 bands, 120 Hz envelope smoothing). Running a dry voice through these
 * defaults lands on her harmonic-to-noise and spectral-balance numbers.
 *
 * How it works: the voice and a white-noise carrier are split into the same
 * log-spaced bands; each noise band is flattened to a unit envelope and then
 * given the voice band's envelope. The bands summed are the "converter"
 * layer. A slow level match keeps that layer as loud as the voice, so Mix is
 * a true balance at any Bands / Smoothing setting.
 *
 * The band filters are 4th-order Butterworth band-passes (four sections,
 * double precision) — the exact filters of the reference renders the owner
 * approved. Cheaper filters were tried and audibly differ: one band-pass
 * section, or a high-pass + low-pass pair, leaks so much that low harmonics
 * fill every band and the voice drowns in noise.
 */

#pragma once

#include "audiofx/Biquad.h"
#include "audiofx/ParamProcessor.h"

#include <array>
#include <cstdint>
#include <vector>

namespace rt::audiofx {

class HologramVoice final : public ParamProcessor
{
public:
    enum Param : int
    {
        Mix = 0,      ///< 0–100 %: 0 = clean voice, 50 = equal (Sin), 100 = layer only
        Bands,        ///< vocoder bands, 8–40
        Smoothing,    ///< Hz: envelope follower cutoff (lower = smoother/softer)
        Brightness,   ///< dB: high shelf at 4 kHz on the layer
        Output,       ///< dB
        Count
    };

    static constexpr int kMaxBands = 40;

    HologramVoice();

    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

    /// One second-order section in double precision (Transposed Direct Form II).
    struct Section
    {
        double b0{1.0}, b1{0.0}, b2{0.0}, a1{0.0}, a2{0.0};
        double z1{0.0}, z2{0.0};
        void reset() noexcept { z1 = z2 = 0.0; }
        [[nodiscard]] double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /// 4th-order Butterworth band-pass [lo, hi] Hz as four sections, 0 dB at
    /// the band centre (same design as scipy.signal.butter(4, [lo, hi],
    /// "bandpass")). Exposed for tests.
    static std::array<Section, 4> designBandPass(double lo, double hi, double sampleRate);

protected:
    void onParamsChanged() override;

private:
    /// One band of one signal: the band-pass and the envelope follower on
    /// its rectified output.
    struct BandFilter
    {
        std::array<Section, 4> bp{};
        Biquad env;
        void reset() noexcept { for (auto& s : bp) s.reset(); env.reset(); }
        [[nodiscard]] double band(double x) noexcept
        {
            for (auto& s : bp) x = s.process(x);
            return x;
        }
    };

    [[nodiscard]] float nextGaussian() noexcept;

    std::vector<BandFilter> m_carrier;   ///< kMaxBands (shared by every channel)
    std::vector<BandFilter> m_voice;     ///< channels * kMaxBands
    std::vector<Biquad>     m_shelf;     ///< per channel, on the layer
    std::vector<double>     m_powIn;     ///< per channel, smoothed input power
    std::vector<double>     m_powLayer;  ///< per channel, smoothed layer power

    uint32_t m_noise{0x9E3779B9u};
    float m_spareGaussian{0.0f};
    bool  m_hasSpare{false};
    int   m_bandCount{28};
    float m_dryGain{0.5f};
    float m_wetGain{0.5f};
    float m_out{1.0f};
    double m_powCoef{0.0};   ///< double: a 3 s follower's per-sample step is ~7e-6
    bool  m_prepared{false};
};

} // namespace rt::audiofx
