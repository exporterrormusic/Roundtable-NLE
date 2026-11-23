#include "audiofx/Delay.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

namespace {

const ParamSpec kDelaySpecs[] = {
    {"Delay Time",  350.0f,   1.0f,  2000.0f, 1.0f,   0, " ms"},
    {"Feedback",     35.0f,   0.0f,    95.0f, 1.0f,   0, " %"},
    {"Ping-Pong",     0.0f,   0.0f,     1.0f, 1.0f,   0, "", ParamKind::Toggle},
    {"Low Cut",      80.0f,  20.0f,  2000.0f, 5.0f,   0, " Hz"},
    {"High Cut",   6000.0f, 500.0f, 20000.0f, 100.0f, 0, " Hz"},
    {"Saturation",    0.0f,   0.0f,   100.0f, 1.0f,   0, " %"},
    {"Dry",         100.0f,   0.0f,   100.0f, 1.0f,   0, " %"},
    {"Wet",          35.0f,   0.0f,   100.0f, 1.0f,   0, " %"},
};
static_assert(sizeof(kDelaySpecs) / sizeof(kDelaySpecs[0]) == Delay::Count);

} // namespace

Delay::Delay()
    : ParamProcessor("Delay", kDelaySpecs, Count)
{
    onParamsChanged();
}

void Delay::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    m_size = static_cast<int>(std::ceil(kMaxTimeMs * 0.001 * m_sampleRate)) + 2;
    m_buf.assign(static_cast<size_t>(m_channels), std::vector<float>(static_cast<size_t>(m_size), 0.0f));
    m_lowCut.assign(static_cast<size_t>(m_channels), Biquad{});
    m_highCut.assign(static_cast<size_t>(m_channels), Biquad{});
    m_prepared = true;
    onParamsChanged();
    reset();
}

void Delay::reset() noexcept
{
    for (auto& b : m_buf) std::fill(b.begin(), b.end(), 0.0f);
    for (auto& b : m_lowCut) b.reset();
    for (auto& b : m_highCut) b.reset();
    m_w = 0;
}

void Delay::onParamsChanged()
{
    m_delay = std::clamp(static_cast<int>(param(Time) * 0.001 * m_sampleRate), 1, std::max(1, m_size - 1));
    m_feedback = param(Feedback) / 100.0f;
    m_pingPong = param(PingPong) > 0.5f;
    m_satDrive = 1.0f + 3.0f * param(Saturation) / 100.0f;
    m_dry = param(Dry) / 100.0f;
    m_wet = param(Wet) / 100.0f;
    for (auto& b : m_lowCut)
        b.setCoefficients(Biquad::Type::HighPass, m_sampleRate, param(LowCut), 0.707, 0.0);
    for (auto& b : m_highCut)
        b.setCoefficients(Biquad::Type::LowPass, m_sampleRate, param(HighCut), 0.707, 0.0);
}

bool Delay::isActive() const noexcept
{
    return m_wet > 0.0f || m_dry < 0.999f;
}

void Delay::process(float* data, int frames) noexcept
{
    if (!m_prepared || frames <= 0) return;
    const int ch = m_channels;
    const bool pingPong = m_pingPong && ch >= 2;
    const bool saturate = m_satDrive > 1.001f;

    for (int f = 0; f < frames; ++f) {
        float* frame = data + static_cast<size_t>(f) * ch;
        int r = m_w - m_delay;
        if (r < 0) r += m_size;
        const size_t rs = static_cast<size_t>(r);
        const size_t ws = static_cast<size_t>(m_w);

        // Feedback signal for each line: filtered (and saturated) repeat.
        auto feedbackOf = [&](int c, float delayed) {
            float s = m_highCut[static_cast<size_t>(c)].processSample(
                m_lowCut[static_cast<size_t>(c)].processSample(delayed));
            if (saturate) s = std::tanh(s * m_satDrive) / m_satDrive;
            return s;
        };
        auto store = [&](int c, float v) {
            if (!(std::fabs(v) < 1e6f)) v = 0.0f;   // NaN / runaway guard
            m_buf[static_cast<size_t>(c)][ws] = v;
        };

        if (pingPong) {
            const float dl = m_buf[0][rs];
            const float dr = m_buf[1][rs];
            const float fl = feedbackOf(0, dl);
            const float fr = feedbackOf(1, dr);
            const float mono = 0.5f * (frame[0] + frame[1]);
            store(0, mono + m_feedback * fr);   // input enters left
            store(1, m_feedback * fl);          // left repeat bounces right
            frame[0] = m_dry * frame[0] + m_wet * dl;
            frame[1] = m_dry * frame[1] + m_wet * dr;
            for (int c = 2; c < ch; ++c) {
                const float d = m_buf[static_cast<size_t>(c)][rs];
                store(c, frame[c] + m_feedback * feedbackOf(c, d));
                frame[c] = m_dry * frame[c] + m_wet * d;
            }
        } else {
            for (int c = 0; c < ch; ++c) {
                const float d = m_buf[static_cast<size_t>(c)][rs];
                store(c, frame[c] + m_feedback * feedbackOf(c, d));
                frame[c] = m_dry * frame[c] + m_wet * d;
            }
        }
        if (++m_w >= m_size) m_w = 0;
    }
}

std::unique_ptr<AudioProcessor> Delay::clone() const
{
    return cloneAs<Delay>();
}

} // namespace rt::audiofx
