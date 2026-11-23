#include "audiofx/Distortion.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

namespace {

const char* const kModeNames[] = {"Soft Clip", "Hard Clip", "Tube", "Bit Crush"};

const ParamSpec kDistortionSpecs[] = {
    {"Type",          0.0f,   0.0f,     3.0f, 1.0f,   0, "", ParamKind::Choice, kModeNames, 4},
    {"Drive",        12.0f,   0.0f,    48.0f, 0.5f,   1, " dB"},
    {"Tone",       8000.0f, 500.0f, 20000.0f, 100.0f, 0, " Hz"},
    {"Bit Depth",     8.0f,   2.0f,    16.0f, 1.0f,   0, " bits"},
    {"Downsample",    1.0f,   1.0f,    32.0f, 1.0f,   0, "x"},
    {"Mix",         100.0f,   0.0f,   100.0f, 1.0f,   0, " %"},
    {"Output",       -6.0f, -24.0f,    12.0f, 0.5f,   1, " dB"},
};
static_assert(sizeof(kDistortionSpecs) / sizeof(kDistortionSpecs[0]) == Distortion::Count);

} // namespace

Distortion::Distortion()
    : ParamProcessor("Distortion", kDistortionSpecs, Count)
{
    onParamsChanged();
}

void Distortion::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    m_tone.assign(static_cast<size_t>(m_channels), Biquad{});
    m_dcBlock.assign(static_cast<size_t>(m_channels), Biquad{});
    m_held.assign(static_cast<size_t>(m_channels), 0.0f);
    m_prepared = true;
    onParamsChanged();
    reset();
}

void Distortion::reset() noexcept
{
    for (auto& b : m_tone) b.reset();
    for (auto& b : m_dcBlock) b.reset();
    std::fill(m_held.begin(), m_held.end(), 0.0f);
    m_holdCounter = 0;
}

void Distortion::onParamsChanged()
{
    m_mode       = std::clamp(static_cast<int>(param(Type)), 0, ModeCount - 1);
    m_drive      = dbToLinear(param(Drive));
    m_levels     = std::pow(2.0f, param(Bits) - 1.0f);
    m_downsample = std::max(1, static_cast<int>(param(Downsample)));
    m_mix        = param(Mix) / 100.0f;
    m_out        = dbToLinear(param(Output));
    for (auto& b : m_tone)
        b.setCoefficients(Biquad::Type::LowPass, m_sampleRate, param(Tone), 0.707, 0.0);
    for (auto& b : m_dcBlock)
        b.setCoefficients(Biquad::Type::HighPass, m_sampleRate, 15.0, 0.707, 0.0);
}

bool Distortion::isActive() const noexcept
{
    return m_mix > 0.0f || std::fabs(m_out - 1.0f) > 1e-4f;
}

void Distortion::process(float* data, int frames) noexcept
{
    if (!m_prepared || frames <= 0) return;
    const int ch = m_channels;

    for (int f = 0; f < frames; ++f) {
        float* frame = data + static_cast<size_t>(f) * ch;
        const bool takeSample = (m_holdCounter == 0);
        if (++m_holdCounter >= m_downsample) m_holdCounter = 0;

        for (int c = 0; c < ch; ++c) {
            const float x = frame[c];
            const float z = x * m_drive;
            float y;
            switch (m_mode) {
            case HardClip:
                y = std::clamp(z, -1.0f, 1.0f);
                break;
            case Tube: {
                // Asymmetric bias: positive half saturates later than the
                // negative half, which adds even harmonics.
                constexpr float kBias = 0.25f;
                y = std::tanh(z + kBias) - std::tanh(kBias);
                break;
            }
            case BitCrush: {
                if (takeSample) {
                    const float q = std::clamp(z, -1.0f, 1.0f);
                    m_held[static_cast<size_t>(c)] = std::round(q * m_levels) / m_levels;
                }
                y = m_held[static_cast<size_t>(c)];
                break;
            }
            case SoftClip:
            default:
                y = std::tanh(z);
                break;
            }
            y = m_dcBlock[static_cast<size_t>(c)].processSample(
                m_tone[static_cast<size_t>(c)].processSample(y));
            frame[c] = m_out * (m_mix * y + (1.0f - m_mix) * x);
        }
    }
}

std::unique_ptr<AudioProcessor> Distortion::clone() const
{
    return cloneAs<Distortion>();
}

} // namespace rt::audiofx
