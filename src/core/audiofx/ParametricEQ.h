/*
 * ParametricEQ — multi-band parametric equalizer (Premiere "Parametric EQ").
 *
 * Layout, signal order top-to-bottom:
 *   • High-pass filter  (optional, low-cut)
 *   • Up to kMaxBands peaking / shelf bands
 *   • Low-pass filter   (optional, high-cut)
 *   • Output gain trim
 *
 * Each band is a cookbook Biquad replicated per channel. Band parameters are
 * edited on the control thread; coefficients recompute immediately (cheap).
 * process() only reads the precomputed coefficients, so it is RT-safe.
 */

#pragma once

#include "audiofx/AudioProcessor.h"
#include "audiofx/Biquad.h"

#include <array>
#include <vector>

namespace rt::audiofx {

class ParametricEQ final : public AudioProcessor
{
public:
    static constexpr int kMaxBands = 8;

    /// One tunable peaking/shelf band.
    struct Band
    {
        bool         enabled{false};
        Biquad::Type type{Biquad::Type::Peaking};
        float        freqHz{1000.0f};
        float        gainDb{0.0f};
        float        q{1.0f};
    };

    ParametricEQ();

    // ── AudioProcessor ──────────────────────────────────────────────────
    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

    // ── High-pass / low-pass shelves ────────────────────────────────────
    void setHighPass(bool on, float freqHz, float q = 0.707f);
    void setLowPass(bool on, float freqHz, float q = 0.707f);

    [[nodiscard]] bool  highPassOn()   const noexcept { return m_hpfOn; }
    [[nodiscard]] float highPassFreq() const noexcept { return m_hpfFreq; }
    [[nodiscard]] float highPassQ()    const noexcept { return m_hpfQ; }
    [[nodiscard]] bool  lowPassOn()    const noexcept { return m_lpfOn; }
    [[nodiscard]] float lowPassFreq()  const noexcept { return m_lpfFreq; }
    [[nodiscard]] float lowPassQ()     const noexcept { return m_lpfQ; }

    // ── Bands ───────────────────────────────────────────────────────────
    [[nodiscard]] int  bandCount() const noexcept { return kMaxBands; }
    [[nodiscard]] const Band& band(int i) const { return m_bands[static_cast<size_t>(i)]; }
    void setBand(int i, const Band& b);
    void setBandEnabled(int i, bool on);

    /// Output gain trim applied after all bands.
    void setOutputGainDb(float db);
    [[nodiscard]] float outputGainDb() const noexcept { return m_outputGainDb; }

    /// Premiere-style starting point: gentle low-cut + presence bell, flat.
    void loadVoicePreset();

private:
    void recomputeBand(int i);
    void recomputeHighPass();
    void recomputeLowPass();
    /// Index into the per-channel filter block for a given stage.
    [[nodiscard]] Biquad& filt(int channel, int stage) noexcept;

    // Stage layout per channel: [HPF][band0..band7][LPF]
    static constexpr int kHpfStage  = 0;
    static constexpr int kBandStage = 1;                       // first band
    static constexpr int kLpfStage  = kBandStage + kMaxBands;  // = 9
    static constexpr int kStages    = kLpfStage + 1;           // = 10

    std::array<Band, kMaxBands> m_bands{};

    bool  m_hpfOn{false};
    float m_hpfFreq{80.0f};
    float m_hpfQ{0.707f};
    bool  m_lpfOn{false};
    float m_lpfFreq{18000.0f};
    float m_lpfQ{0.707f};

    float m_outputGainDb{0.0f};
    float m_outputGainLin{1.0f};

    // channels * kStages biquads, indexed [channel*kStages + stage].
    std::vector<Biquad> m_filters;
};

} // namespace rt::audiofx
