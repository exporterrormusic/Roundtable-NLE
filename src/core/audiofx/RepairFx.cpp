#include "audiofx/RepairFx.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

// ── DeEsser ─────────────────────────────────────────────────────────────────

namespace {
const ParamSpec kDeEsserSpecs[] = {
    {"Frequency",     6000.0f, 3000.0f, 12000.0f, 100.0f, 0, " Hz"},
    {"Threshold",      -30.0f,  -60.0f,     0.0f,   0.5f, 1, " dB"},
    {"Max Reduction",    8.0f,    0.0f,    24.0f,   0.5f, 1, " dB"},
};
static_assert(sizeof(kDeEsserSpecs) / sizeof(kDeEsserSpecs[0]) == DeEsser::Count);
} // namespace

DeEsser::DeEsser() : ParamProcessor("DeEsser", kDeEsserSpecs, Count) { onParamsChanged(); }

void DeEsser::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = std::max(1, channels);
    m_band.assign(static_cast<size_t>(m_channels), Biquad{});
    m_shelf.assign(static_cast<size_t>(m_channels), Biquad{});
    m_prepared = true;
    onParamsChanged();
    reset();
}

void DeEsser::reset() noexcept
{
    for (auto& b : m_band) b.reset();
    for (auto& b : m_shelf) { b.reset(); b.setIdentity(); }
    m_env = 0.0f;
    m_appliedDb = 0.0f;
    m_sinceUpdate = 0;
}

void DeEsser::onParamsChanged()
{
    m_threshold = dbToLinear(param(Threshold));
    m_floor = dbToLinear(-param(MaxReduction));
    m_attack = timeConstantCoef(0.001f, m_sampleRate);
    m_release = timeConstantCoef(0.060f, m_sampleRate);
    for (auto& b : m_band)
        b.setCoefficients(Biquad::Type::HighPass, m_sampleRate, param(Frequency), 0.707, 0.0);
}

bool DeEsser::isActive() const noexcept { return param(MaxReduction) > 0.0f; }

void DeEsser::process(float* data, int frames) noexcept
{
    if (!m_prepared) return;
    constexpr int kUpdateFrames = 16;   // shelf redesign cadence (sample-indexed)
    const int ch = m_channels;
    for (int f = 0; f < frames; ++f) {
        float* fr = data + static_cast<size_t>(f) * ch;
        float peak = 0.0f;
        for (int c = 0; c < ch; ++c)
            peak = std::max(peak, std::fabs(m_band[static_cast<size_t>(c)].processSample(fr[c])));
        const float coef = peak > m_env ? m_attack : m_release;
        m_env = coef * m_env + (1.0f - coef) * peak;

        if (++m_sinceUpdate >= kUpdateFrames) {
            m_sinceUpdate = 0;
            float g = 1.0f;
            if (m_env > m_threshold) g = std::max(m_floor, m_threshold / m_env);
            const float db = linearToDb(g);
            if (std::fabs(db - m_appliedDb) > 0.05f) {
                m_appliedDb = db;
                for (auto& sh : m_shelf) {
                    if (db > -0.01f) sh.setIdentity();
                    else sh.setCoefficients(Biquad::Type::HighShelf, m_sampleRate,
                                            param(Frequency), 0.707, db);
                }
            }
        }
        for (int c = 0; c < ch; ++c)
            fr[c] = m_shelf[static_cast<size_t>(c)].processSample(fr[c]);
    }
}

std::unique_ptr<AudioProcessor> DeEsser::clone() const { return cloneAs<DeEsser>(); }

// ── DeHum ───────────────────────────────────────────────────────────────────

namespace {
const char* const kMainsNames[] = {"50 Hz", "60 Hz"};
const ParamSpec kDeHumSpecs[] = {
    {"Frequency", 1.0f, 0.0f, 1.0f, 1.0f, 0, "", ParamKind::Choice, kMainsNames, 2},
    {"Harmonics", 4.0f, 1.0f, 8.0f, 1.0f, 0, ""},
    {"Amount",  100.0f, 0.0f, 100.0f, 1.0f, 0, " %"},
};
static_assert(sizeof(kDeHumSpecs) / sizeof(kDeHumSpecs[0]) == DeHum::Count);
} // namespace

DeHum::DeHum() : ParamProcessor("DeHum", kDeHumSpecs, Count) { onParamsChanged(); }

void DeHum::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = std::max(1, channels);
    m_notch.assign(static_cast<size_t>(m_channels), {});
    m_prepared = true;
    onParamsChanged();
    reset();
}

void DeHum::reset() noexcept
{
    for (auto& set : m_notch)
        for (auto& b : set) b.reset();
}

void DeHum::onParamsChanged()
{
    const double mains = param(MainsFrequency) > 0.5f ? 60.0 : 50.0;
    m_count = std::clamp(static_cast<int>(param(Harmonics)), 1, kMaxHarmonics);
    m_mix = param(Depth) / 100.0f;
    for (auto& set : m_notch)
        for (int h = 0; h < kMaxHarmonics; ++h)
            set[static_cast<size_t>(h)].setCoefficients(
                Biquad::Type::Notch, m_sampleRate, mains * (h + 1), 30.0, 0.0);
}

bool DeHum::isActive() const noexcept { return m_mix > 0.0f; }

void DeHum::process(float* data, int frames) noexcept
{
    if (!m_prepared) return;
    const int ch = m_channels;
    for (int f = 0; f < frames; ++f) {
        float* fr = data + static_cast<size_t>(f) * ch;
        for (int c = 0; c < ch; ++c) {
            auto& set = m_notch[static_cast<size_t>(c)];
            float y = fr[c];
            for (int h = 0; h < m_count; ++h) y = set[static_cast<size_t>(h)].processSample(y);
            fr[c] = fr[c] + m_mix * (y - fr[c]);
        }
    }
}

std::unique_ptr<AudioProcessor> DeHum::clone() const { return cloneAs<DeHum>(); }

// ── StereoWidth ─────────────────────────────────────────────────────────────

namespace {
const ParamSpec kWidthSpecs[] = {
    {"Width", 100.0f, 0.0f, 200.0f, 1.0f, 0, " %"},
};
} // namespace

StereoWidth::StereoWidth() : ParamProcessor("StereoWidth", kWidthSpecs, Count) {}

void StereoWidth::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = std::max(1, channels);
}

bool StereoWidth::isActive() const noexcept { return std::fabs(param(Width) - 100.0f) > 0.01f; }

void StereoWidth::process(float* data, int frames) noexcept
{
    if (m_channels < 2) return;
    const float w = param(Width) / 100.0f;
    const int ch = m_channels;
    for (int f = 0; f < frames; ++f) {
        float* fr = data + static_cast<size_t>(f) * ch;
        const float mid = 0.5f * (fr[0] + fr[1]);
        const float side = 0.5f * (fr[0] - fr[1]) * w;
        fr[0] = mid + side;
        fr[1] = mid - side;
    }
}

std::unique_ptr<AudioProcessor> StereoWidth::clone() const { return cloneAs<StereoWidth>(); }

} // namespace rt::audiofx
