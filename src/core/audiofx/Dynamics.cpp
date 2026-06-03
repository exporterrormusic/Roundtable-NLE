#include "audiofx/Dynamics.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

namespace {

/// Soft-knee compression static curve. Given the input level (dB) and a
/// downward compressor with threshold T, ratio R, knee width W, returns the
/// gain to apply (dB, <= 0).
[[nodiscard]] float compressorGainDb(float xDb, float T, float R, float W) noexcept
{
    const float over = xDb - T;
    if (W > 0.0f && 2.0f * std::fabs(over) <= W) {
        // Quadratic interpolation through the knee.
        const float t = over + W * 0.5f;
        const float yDb = xDb + (1.0f / R - 1.0f) * (t * t) / (2.0f * W);
        return yDb - xDb;
    }
    if (over <= 0.0f) return 0.0f;          // below threshold: no reduction
    const float yDb = T + over / R;         // above the knee: full ratio
    return yDb - xDb;                        // negative
}

/// Downward-expander / gate static curve. Below threshold the signal is
/// pushed down by (ratio-1); attenuation is clamped to `rangeDb`.
[[nodiscard]] float gateGainDb(float xDb, float T, float R, float rangeDb) noexcept
{
    if (xDb >= T) return 0.0f;
    const float gain = (xDb - T) * (R - 1.0f);   // negative
    return std::max(gain, rangeDb);
}

/// Peak-envelope detector: instant attack to new peaks, exponential release.
/// Keeps the detected level on the signal envelope instead of the raw sample.
inline float detectPeak(float& env, float peak, float relCoef) noexcept
{
    env = peak > env ? peak : relCoef * env + (1.0f - relCoef) * peak;
    return env;
}

/// Branched (decoupled) smoothing of the gain-control signal in dB.
/// More reduction attacks fast; recovery releases slowly.
inline void smoothGain(float& gs, float targetDb, float attCoef, float relCoef) noexcept
{
    if (targetDb < gs)  // need more reduction -> attack
        gs = attCoef * gs + (1.0f - attCoef) * targetDb;
    else                // recovering -> release
        gs = relCoef * gs + (1.0f - relCoef) * targetDb;
}

} // namespace

Dynamics::Dynamics() : AudioProcessor("Dynamics")
{
    recomputeCoefs();
}

void Dynamics::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    recomputeCoefs();
    reset();
}

void Dynamics::reset() noexcept
{
    m_gateGsDb = m_compGsDb = m_limGsDb = 0.0f;
    m_gateEnv = m_compEnv = m_limEnv = 0.0f;
    m_meterGate.store(0.0f, std::memory_order_relaxed);
    m_meterComp.store(0.0f, std::memory_order_relaxed);
    m_meterLim.store(0.0f, std::memory_order_relaxed);
}

void Dynamics::recomputeCoefs()
{
    m_gateAtt = timeConstantCoef(m_gate.attackMs  * 0.001f, m_sampleRate);
    m_gateRel = timeConstantCoef(m_gate.releaseMs * 0.001f, m_sampleRate);
    m_compAtt = timeConstantCoef(m_comp.attackMs  * 0.001f, m_sampleRate);
    m_compRel = timeConstantCoef(m_comp.releaseMs * 0.001f, m_sampleRate);
    // Limiter attacks effectively instantly; only release is tunable.
    m_limAtt  = 0.0f;
    m_limRel  = timeConstantCoef(m_lim.releaseMs * 0.001f, m_sampleRate);
}

void Dynamics::setGate(const GateSettings& g)       { m_gate = g; recomputeCoefs(); }
void Dynamics::setCompressor(const CompSettings& c) { m_comp = c; recomputeCoefs(); }
void Dynamics::setLimiter(const LimiterSettings& l) { m_lim = l;  recomputeCoefs(); }

void Dynamics::loadVoicePreset()
{
    GateSettings g;
    g.enabled = true; g.thresholdDb = -45.0f; g.ratio = 3.0f;
    g.rangeDb = -40.0f; g.attackMs = 1.0f; g.releaseMs = 120.0f;
    setGate(g);

    CompSettings c;
    c.enabled = true; c.thresholdDb = -20.0f; c.ratio = 3.0f; c.kneeDb = 8.0f;
    c.makeupDb = 4.0f; c.attackMs = 12.0f; c.releaseMs = 150.0f;
    setCompressor(c);

    LimiterSettings l;
    l.enabled = true; l.ceilingDb = -0.5f; l.releaseMs = 50.0f;
    setLimiter(l);
}

bool Dynamics::isActive() const noexcept
{
    return m_enabled && (m_gate.enabled || m_comp.enabled || m_lim.enabled);
}

std::unique_ptr<AudioProcessor> Dynamics::clone() const
{
    auto c = std::make_unique<Dynamics>();
    c->setEnabled(m_enabled);
    c->setGate(m_gate);
    c->setCompressor(m_comp);
    c->setLimiter(m_lim);
    return c;
}

float Dynamics::framePeak(const float* frame, int ch) const noexcept
{
    float peak = 0.0f;
    for (int c = 0; c < ch; ++c)
        peak = std::max(peak, std::fabs(frame[c]));
    return peak;
}

void Dynamics::process(float* data, int frames) noexcept
{
    if (!isActive() || frames <= 0) return;

    const int ch = m_channels;

    const bool  gateOn = m_gate.enabled;
    const float gT = m_gate.thresholdDb, gR = std::max(1.0f, m_gate.ratio), gRange = std::min(0.0f, m_gate.rangeDb);

    const bool  compOn = m_comp.enabled;
    const float cT = m_comp.thresholdDb, cR = std::max(1.0f, m_comp.ratio);
    const float cKnee = std::max(0.0f, m_comp.kneeDb), cMakeup = dbToLinear(m_comp.makeupDb);

    const bool  limOn = m_lim.enabled;
    const float lCeil = m_lim.ceilingDb;

    // Track the deepest reduction over the block for metering.
    float meterGate = 0.0f, meterComp = 0.0f, meterLim = 0.0f;

    for (int f = 0; f < frames; ++f) {
        float* frame = data + static_cast<size_t>(f) * ch;

        // ── Gate ──
        if (gateOn) {
            const float lvlDb = linearToDb(detectPeak(m_gateEnv, framePeak(frame, ch), m_gateRel));
            const float target = gateGainDb(lvlDb, gT, gR, gRange);
            smoothGain(m_gateGsDb, target, m_gateAtt, m_gateRel);
            const float gain = dbToLinear(m_gateGsDb);
            for (int c = 0; c < ch; ++c) frame[c] *= gain;
            meterGate = std::min(meterGate, m_gateGsDb);
        }

        // ── Compressor (detect on gated signal) ──
        if (compOn) {
            const float lvlDb = linearToDb(detectPeak(m_compEnv, framePeak(frame, ch), m_compRel));
            const float target = compressorGainDb(lvlDb, cT, cR, cKnee);
            smoothGain(m_compGsDb, target, m_compAtt, m_compRel);
            const float gain = dbToLinear(m_compGsDb) * cMakeup;
            for (int c = 0; c < ch; ++c) frame[c] *= gain;
            meterComp = std::min(meterComp, m_compGsDb);
        }

        // ── Limiter (brick wall, instant attack) ──
        if (limOn) {
            const float lvlDb = linearToDb(detectPeak(m_limEnv, framePeak(frame, ch), m_limRel));
            const float target = std::min(0.0f, lCeil - lvlDb);  // <= 0 when over ceiling
            smoothGain(m_limGsDb, target, m_limAtt, m_limRel);
            const float gain = dbToLinear(m_limGsDb);
            for (int c = 0; c < ch; ++c) frame[c] *= gain;
            meterLim = std::min(meterLim, m_limGsDb);
        }
    }

    m_meterGate.store(meterGate, std::memory_order_relaxed);
    m_meterComp.store(meterComp, std::memory_order_relaxed);
    m_meterLim.store(meterLim, std::memory_order_relaxed);
}

} // namespace rt::audiofx
