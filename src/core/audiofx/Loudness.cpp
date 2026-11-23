#include "audiofx/Loudness.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

namespace {
constexpr double kPi = 3.14159265358979323846;

double lufsOf(double meanSquare)
{
    return -0.691 + 10.0 * std::log10(std::max(meanSquare, 1e-30));
}
} // namespace

LoudnessMeter::LoudnessMeter(double sampleRate, int channels)
    : m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0)
    , m_channels(std::max(1, channels))
{
    const double fs = m_sampleRate;
    {   // Stage 1: high shelf (+4 dB above ~1.7 kHz, head acoustics).
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        m_shelf = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0,
                   (Vh - Vb * K / Q + K * K) / a0, 2.0 * (K * K - 1.0) / a0,
                   (1.0 - K / Q + K * K) / a0};
    }
    {   // Stage 2: high pass (RLB weighting).
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;
        m_highPass = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    }
    m_shelfState.assign(static_cast<size_t>(m_channels), State{});
    m_hpState.assign(static_cast<size_t>(m_channels), State{});
    m_stepEnergy.assign(static_cast<size_t>(m_channels), 0.0);
    m_stepFrames = std::max<int64_t>(1, static_cast<int64_t>(std::llround(fs * 0.1)));
}

void LoudnessMeter::add(const float* interleaved, int64_t frames)
{
    if (!interleaved || frames <= 0) return;
    const int ch = m_channels;
    for (int64_t f = 0; f < frames; ++f) {
        for (int c = 0; c < ch; ++c) {
            double x = interleaved[static_cast<size_t>(f) * ch + c];
            auto run = [](const Stage& s, State& st, double in) {
                const double y = s.b0 * in + st.z1;
                st.z1 = s.b1 * in - s.a1 * y + st.z2;
                st.z2 = s.b2 * in - s.a2 * y;
                return y;
            };
            x = run(m_shelf, m_shelfState[static_cast<size_t>(c)], x);
            x = run(m_highPass, m_hpState[static_cast<size_t>(c)], x);
            m_stepEnergy[static_cast<size_t>(c)] += x * x;
        }
        if (++m_inStep == m_stepFrames) {
            double weighted = 0.0;
            for (int c = 0; c < ch; ++c) {
                const double g = c < 3 ? 1.0 : 1.41;
                weighted += g * m_stepEnergy[static_cast<size_t>(c)];
                m_stepEnergy[static_cast<size_t>(c)] = 0.0;
            }
            m_steps.push_back(weighted);
            m_inStep = 0;
            // A 400 ms block = the last four 100 ms steps.
            if (m_steps.size() >= 4) {
                const size_t n = m_steps.size();
                const double sum = m_steps[n - 1] + m_steps[n - 2] + m_steps[n - 3] + m_steps[n - 4];
                m_blocks.push_back(sum / static_cast<double>(4 * m_stepFrames));
            }
        }
    }
}

std::optional<double> LoudnessMeter::integrated() const
{
    double absSum = 0.0;
    size_t absCount = 0;
    for (double z : m_blocks)
        if (lufsOf(z) > -70.0) { absSum += z; ++absCount; }
    if (absCount == 0) return std::nullopt;

    const double relativeGate = lufsOf(absSum / static_cast<double>(absCount)) - 10.0;
    double sum = 0.0;
    size_t count = 0;
    for (double z : m_blocks) {
        const double l = lufsOf(z);
        if (l > -70.0 && l > relativeGate) { sum += z; ++count; }
    }
    if (count == 0) return std::nullopt;
    return lufsOf(sum / static_cast<double>(count));
}

std::optional<double> integratedLoudness(const float* interleaved, int64_t frames,
                                         int channels, double sampleRate)
{
    LoudnessMeter meter(sampleRate, channels);
    meter.add(interleaved, frames);
    return meter.integrated();
}

} // namespace rt::audiofx
