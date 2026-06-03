#include "audiofx/ParametricEQ.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

ParametricEQ::ParametricEQ() : AudioProcessor("ParametricEQ")
{
    prepare(m_sampleRate, m_channels);
}

Biquad& ParametricEQ::filt(int channel, int stage) noexcept
{
    return m_filters[static_cast<size_t>(channel) * kStages + static_cast<size_t>(stage)];
}

void ParametricEQ::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    m_filters.assign(static_cast<size_t>(m_channels) * kStages, Biquad{});

    recomputeHighPass();
    recomputeLowPass();
    for (int i = 0; i < kMaxBands; ++i)
        recomputeBand(i);
    reset();
}

void ParametricEQ::reset() noexcept
{
    for (auto& f : m_filters) f.reset();
}

bool ParametricEQ::isActive() const noexcept
{
    if (!m_enabled) return false;
    if (m_hpfOn || m_lpfOn) return true;
    if (std::fabs(m_outputGainDb) > 1e-3f) return true;
    for (const auto& b : m_bands)
        if (b.enabled && (b.type == Biquad::Type::LowPass ||
                          b.type == Biquad::Type::HighPass ||
                          b.type == Biquad::Type::Notch ||
                          std::fabs(b.gainDb) > 1e-3f))
            return true;
    return false;
}

std::unique_ptr<AudioProcessor> ParametricEQ::clone() const
{
    auto c = std::make_unique<ParametricEQ>();
    c->setEnabled(m_enabled);
    c->m_bands = m_bands;
    c->m_hpfOn = m_hpfOn; c->m_hpfFreq = m_hpfFreq; c->m_hpfQ = m_hpfQ;
    c->m_lpfOn = m_lpfOn; c->m_lpfFreq = m_lpfFreq; c->m_lpfQ = m_lpfQ;
    c->setOutputGainDb(m_outputGainDb);
    // Coefficients are rebuilt on the clone's next prepare().
    return c;
}

void ParametricEQ::recomputeHighPass()
{
    for (int c = 0; c < m_channels; ++c) {
        Biquad& f = filt(c, kHpfStage);
        if (m_hpfOn) f.setCoefficients(Biquad::Type::HighPass, m_sampleRate, m_hpfFreq, m_hpfQ, 0.0);
        else         f.setIdentity();
    }
}

void ParametricEQ::recomputeLowPass()
{
    for (int c = 0; c < m_channels; ++c) {
        Biquad& f = filt(c, kLpfStage);
        if (m_lpfOn) f.setCoefficients(Biquad::Type::LowPass, m_sampleRate, m_lpfFreq, m_lpfQ, 0.0);
        else         f.setIdentity();
    }
}

void ParametricEQ::recomputeBand(int i)
{
    const Band& b = m_bands[static_cast<size_t>(i)];
    for (int c = 0; c < m_channels; ++c) {
        Biquad& f = filt(c, kBandStage + i);
        if (b.enabled) f.setCoefficients(b.type, m_sampleRate, b.freqHz, b.q, b.gainDb);
        else           f.setIdentity();
    }
}

void ParametricEQ::setHighPass(bool on, float freqHz, float q)
{
    m_hpfOn = on; m_hpfFreq = freqHz; m_hpfQ = q;
    recomputeHighPass();
}

void ParametricEQ::setLowPass(bool on, float freqHz, float q)
{
    m_lpfOn = on; m_lpfFreq = freqHz; m_lpfQ = q;
    recomputeLowPass();
}

void ParametricEQ::setBand(int i, const Band& b)
{
    if (i < 0 || i >= kMaxBands) return;
    m_bands[static_cast<size_t>(i)] = b;
    recomputeBand(i);
}

void ParametricEQ::setBandEnabled(int i, bool on)
{
    if (i < 0 || i >= kMaxBands) return;
    m_bands[static_cast<size_t>(i)].enabled = on;
    recomputeBand(i);
}

void ParametricEQ::setOutputGainDb(float db)
{
    m_outputGainDb  = db;
    m_outputGainLin = dbToLinear(db);
}

void ParametricEQ::loadVoicePreset()
{
    setHighPass(true, 80.0f, 0.707f);   // remove rumble / plosives
    setLowPass(false, 18000.0f);

    Band presence;
    presence.enabled = true;
    presence.type    = Biquad::Type::Peaking;
    presence.freqHz  = 3500.0f;         // intelligibility / presence
    presence.gainDb  = 3.0f;
    presence.q       = 1.2f;
    setBand(0, presence);

    Band warmth;
    warmth.enabled = true;
    warmth.type    = Biquad::Type::Peaking;
    warmth.freqHz  = 250.0f;            // tame muddiness
    warmth.gainDb  = -2.0f;
    warmth.q       = 1.0f;
    setBand(1, warmth);

    for (int i = 2; i < kMaxBands; ++i)
        setBandEnabled(i, false);

    setOutputGainDb(0.0f);
}

void ParametricEQ::process(float* data, int frames) noexcept
{
    if (!m_enabled || !isActive() || frames <= 0) return;

    const int ch = m_channels;
    const float outGain = m_outputGainLin;

    for (int f = 0; f < frames; ++f) {
        float* frame = data + static_cast<size_t>(f) * ch;
        for (int c = 0; c < ch; ++c) {
            float x = frame[c];
            Biquad* block = &m_filters[static_cast<size_t>(c) * kStages];
            // HPF -> bands -> LPF, in series.
            x = block[kHpfStage].processSample(x);
            for (int b = 0; b < kMaxBands; ++b)
                x = block[kBandStage + b].processSample(x);
            x = block[kLpfStage].processSample(x);
            frame[c] = x * outGain;
        }
    }
}

} // namespace rt::audiofx
