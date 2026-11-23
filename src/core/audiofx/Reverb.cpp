#include "audiofx/Reverb.h"

#include <algorithm>
#include <cmath>

namespace rt::audiofx {

namespace {

const ParamSpec kReverbSpecs[] = {
    {"Room Size",         50.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Decay",              1.2f,   0.1f,     4.0f, 0.05f, 2, " s"},
    {"Pre-Delay",         10.0f,   0.0f,   200.0f, 1.0f,  0, " ms"},
    {"Damping",           50.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Diffusion",         70.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Early Reflections", 30.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Width",            100.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Low Cut",          100.0f,  20.0f,  1000.0f, 5.0f,  0, " Hz"},
    {"High Cut",        8000.0f, 1000.0f, 20000.0f, 100.0f, 0, " Hz"},
    {"Dry",              100.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
    {"Wet",               30.0f,   0.0f,   100.0f, 1.0f,  0, " %"},
};
static_assert(sizeof(kReverbSpecs) / sizeof(kReverbSpecs[0]) == Reverb::Count);

// Mutually prime-ish FDN line lengths (ms) at room scale 1.
constexpr float kLineMs[8]     = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f};
// Schroeder input diffusers (samples at 48 kHz).
constexpr float kApSamples[4]  = {142.0f, 107.0f, 379.0f, 277.0f};
// Early reflection taps (ms at room scale 1) and their gains.
constexpr float kTapMs[8]      = {7.0f, 11.3f, 17.1f, 23.9f, 31.3f, 41.7f, 53.1f, 67.3f};
constexpr float kTapGain[8]    = {0.84f, 0.72f, 0.66f, 0.55f, 0.48f, 0.40f, 0.33f, 0.27f};

constexpr float kMaxRoomScale = 1.5f;
constexpr float kMaxPreDelayMs = 200.0f;

/// In-place 8-point fast Walsh–Hadamard transform, normalised (orthogonal).
inline void hadamard8(float* v) noexcept
{
    for (int h = 1; h < 8; h <<= 1)
        for (int i = 0; i < 8; i += h << 1)
            for (int j = i; j < i + h; ++j) {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
    constexpr float kNorm = 0.35355339059f;   // 1 / sqrt(8)
    for (int i = 0; i < 8; ++i) v[i] *= kNorm;
}

} // namespace

Reverb::Reverb()
    : ParamProcessor("Reverb", kReverbSpecs, Count)
{
    onParamsChanged();
}

void Reverb::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    const double sr = m_sampleRate;

    const int maxPre = static_cast<int>(std::ceil(kMaxPreDelayMs * 0.001 * sr));
    const int maxTap = static_cast<int>(std::ceil(kTapMs[7] * kMaxRoomScale * 0.001 * sr));
    m_input.resize(maxPre + maxTap + 2);
    for (int i = 0; i < kDiffusers; ++i)
        m_ap[i].resize(static_cast<int>(std::ceil(kApSamples[i] * sr / 48000.0)) + 2);
    for (int i = 0; i < kLines; ++i)
        m_line[i].resize(static_cast<int>(std::ceil(kLineMs[i] * kMaxRoomScale * 0.001 * sr)) + 2);

    m_prepared = true;
    onParamsChanged();
    reset();
}

void Reverb::reset() noexcept
{
    m_input.clear();
    for (auto& r : m_ap) r.clear();
    for (auto& r : m_line) r.clear();
    m_lp.fill(0.0f);
    for (auto& b : m_lowCut) b.reset();
    for (auto& b : m_highCut) b.reset();
}

void Reverb::onParamsChanged()
{
    const double sr = m_sampleRate;
    const float roomScale = 0.5f + param(RoomSize) / 100.0f;   // 0.5 .. 1.5
    const float decay = std::max(0.1f, param(Decay));

    m_preDelay = static_cast<int>(param(PreDelay) * 0.001 * sr);

    float gainSq = 0.0f;
    for (int i = 0; i < kLines; ++i) {
        m_lineLen[i] = std::max(1, static_cast<int>(kLineMs[i] * roomScale * 0.001 * sr));
        // Loop gain so the line decays 60 dB over `decay` seconds.
        const float g = std::pow(10.0f, -3.0f * static_cast<float>(m_lineLen[i] / sr) / decay);
        m_lineGain[i] = g;
        gainSq += g * g;
    }
    // Normalise the tail level so long decays don't get much louder than short
    // ones (steady-state energy of a loop with gain g is 1 / (1 - g²)).
    const float meanGainSq = gainSq / kLines;
    m_lateScale = 0.9f * std::sqrt(std::max(0.02f, 1.0f - meanGainSq));

    for (int i = 0; i < kDiffusers; ++i)
        m_apLen[i] = std::max(1, static_cast<int>(kApSamples[i] * sr / 48000.0));
    m_apCoef = 0.75f * param(Diffusion) / 100.0f;

    for (int i = 0; i < kTaps; ++i) {
        m_tapDelay[i] = m_preDelay + std::max(1, static_cast<int>(kTapMs[i] * roomScale * 0.001 * sr));
        m_tapGain[i] = kTapGain[i];
    }

    m_damp    = 0.85f * param(Damping) / 100.0f;
    m_erLevel = 0.6f * param(EarlyReflections) / 100.0f;
    m_width   = param(Width) / 100.0f;
    m_dry     = param(Dry) / 100.0f;
    m_wet     = param(Wet) / 100.0f;

    for (auto& b : m_lowCut)
        b.setCoefficients(Biquad::Type::HighPass, sr, param(LowCut), 0.707, 0.0);
    for (auto& b : m_highCut)
        b.setCoefficients(Biquad::Type::LowPass, sr, param(HighCut), 0.707, 0.0);
}

bool Reverb::isActive() const noexcept
{
    return m_wet > 0.0f || m_dry < 0.999f;
}

void Reverb::process(float* data, int frames) noexcept
{
    if (!m_prepared || frames <= 0) return;
    const int ch = m_channels;
    const float inScale = 1.0f / static_cast<float>(ch);

    for (int f = 0; f < frames; ++f) {
        float* frame = data + static_cast<size_t>(f) * ch;

        float mono = 0.0f;
        for (int c = 0; c < ch; ++c) mono += frame[c];
        mono *= inScale;

        // Pre-delay + early reflections share one input ring.
        m_input.push(mono);
        const float pre = m_input.read(m_preDelay + 1);
        float erL = 0.0f, erR = 0.0f;
        for (int t = 0; t < kTaps; ++t) {
            const float v = m_input.read(m_tapDelay[t] + 1) * m_tapGain[t];
            if (t & 1) erR += v; else erL += v;
        }

        // Input diffusion (lattice allpass: w = x + g·w[n-M], y = w[n-M] - g·w).
        float d = pre;
        for (int i = 0; i < kDiffusers; ++i) {
            const float delayed = m_ap[i].read(m_apLen[i]);
            const float w = d + m_apCoef * delayed;
            d = delayed - m_apCoef * w;
            m_ap[i].push(w);
        }

        // Feedback delay network.
        float out[kLines];
        float fb[kLines];
        for (int i = 0; i < kLines; ++i) {
            out[i] = m_line[i].read(m_lineLen[i]);
            m_lp[i] = out[i] + m_damp * (m_lp[i] - out[i]);   // one-pole low-pass
            fb[i] = m_lp[i] * m_lineGain[i];
        }
        hadamard8(fb);
        for (int i = 0; i < kLines; ++i) {
            const float inj = (i & 1) ? -d : d;
            float v = fb[i] + 0.5f * inj;
            if (!(std::fabs(v) < 1e6f)) v = 0.0f;   // NaN / runaway guard
            m_line[i].push(v);
        }

        float lateL = (out[0] + out[2] + out[4] + out[6]) * 0.5f * m_lateScale;
        float lateR = (out[1] + out[3] + out[5] + out[7]) * 0.5f * m_lateScale;

        float wl = lateL + erL * m_erLevel * 0.5f;
        float wr = lateR + erR * m_erLevel * 0.5f;
        // Width: blend toward mono as width falls.
        const float mid = 0.5f * (wl + wr);
        const float side = 0.5f * (wl - wr) * m_width;
        wl = m_highCut[0].processSample(m_lowCut[0].processSample(mid + side));
        wr = m_highCut[1].processSample(m_lowCut[1].processSample(mid - side));

        if (ch == 1) {
            frame[0] = m_dry * frame[0] + m_wet * 0.5f * (wl + wr);
        } else {
            frame[0] = m_dry * frame[0] + m_wet * wl;
            frame[1] = m_dry * frame[1] + m_wet * wr;
            // Extra channels (surround) get the mono wet signal.
            for (int c = 2; c < ch; ++c)
                frame[c] = m_dry * frame[c] + m_wet * 0.5f * (wl + wr);
        }
    }
}

std::unique_ptr<AudioProcessor> Reverb::clone() const
{
    return cloneAs<Reverb>();
}

} // namespace rt::audiofx
