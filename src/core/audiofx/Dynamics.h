/*
 * Dynamics — unified Gate → Compressor → Limiter (Premiere "Dynamics" effect).
 *
 * Premiere bundles its dynamics processing into a single effect rather than
 * three separate plugins; this mirrors that. Three stereo-linked stages run in
 * series, each independently toggleable:
 *
 *   1. Gate       — downward expander; attenuates signal below a threshold
 *                   (noise floor / room tone cleanup).
 *   2. Compressor — soft-knee feed-forward compressor with makeup gain.
 *   3. Limiter    — brick-wall ceiling with fast attack (peak safety).
 *
 * Each stage uses log-domain, branched attack/release smoothing of the gain
 * control signal (Reiss/Giannoulis "decoupled smoothing"), which is the
 * standard well-behaved design. Detection is stereo-linked: the loudest
 * channel drives the gain so the stereo image is preserved.
 *
 * Gain-reduction (in dB, <= 0) is exposed per stage for UI metering.
 */

#pragma once

#include "audiofx/AudioProcessor.h"

#include <atomic>

namespace rt::audiofx {

class Dynamics final : public AudioProcessor
{
public:
    struct GateSettings
    {
        bool  enabled{false};
        float thresholdDb{-40.0f};  ///< below this, attenuation kicks in
        float ratio{4.0f};          ///< downward-expansion ratio (>=1)
        float rangeDb{-60.0f};      ///< max attenuation (floor), <= 0
        float attackMs{1.0f};
        float releaseMs{100.0f};
    };

    struct CompSettings
    {
        bool  enabled{false};
        float thresholdDb{-18.0f};
        float ratio{3.0f};          ///< >=1; 1 = no compression
        float kneeDb{6.0f};         ///< soft-knee width (0 = hard knee)
        float makeupDb{0.0f};
        float attackMs{10.0f};
        float releaseMs{120.0f};
    };

    struct LimiterSettings
    {
        bool  enabled{false};
        float ceilingDb{-0.3f};     ///< brick-wall output ceiling
        float releaseMs{50.0f};
    };

    Dynamics();

    // ── AudioProcessor ──────────────────────────────────────────────────
    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

    // ── Settings (control thread) ───────────────────────────────────────
    void setGate(const GateSettings& g);
    void setCompressor(const CompSettings& c);
    void setLimiter(const LimiterSettings& l);

    [[nodiscard]] const GateSettings&    gate()       const noexcept { return m_gate; }
    [[nodiscard]] const CompSettings&    compressor() const noexcept { return m_comp; }
    [[nodiscard]] const LimiterSettings& limiter()    const noexcept { return m_lim; }

    /// Gentle broadcast-voice starting point.
    void loadVoicePreset();

    // ── Metering (audio thread writes, UI reads) ────────────────────────
    [[nodiscard]] float gateReductionDb()  const noexcept { return m_meterGate.load(std::memory_order_relaxed); }
    [[nodiscard]] float compReductionDb()  const noexcept { return m_meterComp.load(std::memory_order_relaxed); }
    [[nodiscard]] float limiterReductionDb() const noexcept { return m_meterLim.load(std::memory_order_relaxed); }

private:
    void recomputeCoefs();
    /// Peak across channels for one frame (stereo-linked detection).
    [[nodiscard]] float framePeak(const float* frame, int ch) const noexcept;

    GateSettings    m_gate;
    CompSettings    m_comp;
    LimiterSettings m_lim;

    // Smoothed gain-control state (dB, <= 0) carried across frames.
    float m_gateGsDb{0.0f};
    float m_compGsDb{0.0f};
    float m_limGsDb{0.0f};

    // Per-stage peak-envelope detectors (linear) — instant attack, exponential
    // release. Detecting on the envelope rather than the raw sample stops the
    // gain stages from chattering on every waveform zero-crossing.
    float m_gateEnv{0.0f};
    float m_compEnv{0.0f};
    float m_limEnv{0.0f};

    // Precomputed smoothing coefficients.
    float m_gateAtt{0.0f}, m_gateRel{0.0f};
    float m_compAtt{0.0f}, m_compRel{0.0f};
    float m_limAtt{0.0f},  m_limRel{0.0f};

    std::atomic<float> m_meterGate{0.0f};
    std::atomic<float> m_meterComp{0.0f};
    std::atomic<float> m_meterLim{0.0f};
};

} // namespace rt::audiofx
