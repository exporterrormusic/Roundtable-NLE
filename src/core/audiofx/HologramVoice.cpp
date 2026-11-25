#include "audiofx/HologramVoice.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace rt::audiofx {

namespace {

const ParamSpec kHologramVoiceSpecs[] = {
    {"Mix",         50.0f,   0.0f, 100.0f, 1.0f, 0, " %"},
    {"Bands",       28.0f,   8.0f,  40.0f, 1.0f, 0, ""},
    {"Smoothing",  120.0f,  20.0f, 400.0f, 5.0f, 0, " Hz"},
    {"Brightness",   0.0f, -12.0f,  12.0f, 0.5f, 1, " dB"},
    {"Output",       0.0f, -24.0f,  12.0f, 0.5f, 1, " dB"},
};
static_assert(sizeof(kHologramVoiceSpecs) / sizeof(kHologramVoiceSpecs[0]) == HologramVoice::Count);

constexpr double kLowestBandHz  = 90.0;
constexpr double kHighestBandHz = 15000.0;
// The reference renders matched the layer to the voice once per file. A 3 s
// follower measured the same (shorter ones pump with each phrase).
constexpr double kLevelMatchSeconds = 3.0;
constexpr float  kMaxLayerGain = 8.0f;   // never lift a near-silent layer more than +18 dB
constexpr uint32_t kNoiseSeed = 0x9E3779B9u;
constexpr double kPi = 3.14159265358979323846;

} // namespace

std::array<HologramVoice::Section, 4>
HologramVoice::designBandPass(double lo, double hi, double sampleRate)
{
    using C = std::complex<double>;
    constexpr int kOrder = 4;
    const double fs = sampleRate;
    hi = std::min(hi, fs * 0.49);
    lo = std::clamp(lo, 1.0, hi * 0.999);

    // Pre-warped analog band edges, centre and width.
    const double wl = 2.0 * fs * std::tan(kPi * lo / fs);
    const double wh = 2.0 * fs * std::tan(kPi * hi / fs);
    const double bw = wh - wl;
    const double w0 = std::sqrt(wl * wh);

    // Butterworth low-pass prototype poles → band-pass → bilinear (z-plane).
    // Keep the upper-half-plane pole of each conjugate pair: one section each.
    std::array<Section, 4> out{};
    int n = 0;
    for (int m = -kOrder + 1; m <= kOrder - 1; m += 2) {
        const C p = -std::exp(C(0.0, kPi * m / (2.0 * kOrder)));
        const C half = p * bw / 2.0;
        const C root = std::sqrt(half * half - w0 * w0);
        for (const C s : {half + root, half - root}) {
            const C z = (2.0 * fs + s) / (2.0 * fs - s);
            if (z.imag() <= 0.0 || n >= 4) continue;
            Section& sec = out[static_cast<size_t>(n++)];
            sec.a1 = -2.0 * z.real();
            sec.a2 = std::norm(z);
            // Zeros: one at z = 1 (DC) and one at z = -1 (Nyquist) per section.
            sec.b0 = 1.0; sec.b1 = 0.0; sec.b2 = -1.0;
        }
    }

    // Unity gain at the band centre, spread evenly over the sections.
    const double theta = 2.0 * std::atan(w0 / (2.0 * fs));
    const C zi = std::exp(C(0.0, -theta));
    C h(1.0, 0.0);
    for (const auto& s : out)
        h *= (s.b0 + s.b1 * zi + s.b2 * zi * zi) / (1.0 + s.a1 * zi + s.a2 * zi * zi);
    const double g = std::pow(1.0 / std::max(std::abs(h), 1e-30), 1.0 / 4.0);
    for (auto& s : out) { s.b0 *= g; s.b1 *= g; s.b2 *= g; }
    return out;
}

HologramVoice::HologramVoice()
    : ParamProcessor("HologramVoice", kHologramVoiceSpecs, Count)
{
    onParamsChanged();
}

void HologramVoice::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels   = std::max(1, channels);
    // Every band is allocated up front: Bands only changes how many are used,
    // so a parameter change never resizes what process() reads.
    m_carrier.assign(kMaxBands, BandFilter{});
    m_voice.assign(static_cast<size_t>(m_channels) * kMaxBands, BandFilter{});
    m_shelf.assign(static_cast<size_t>(m_channels), Biquad{});
    m_powIn.assign(static_cast<size_t>(m_channels), 0.0);
    m_powLayer.assign(static_cast<size_t>(m_channels), 0.0);
    m_powCoef = std::exp(-1.0 / (kLevelMatchSeconds * m_sampleRate));
    m_prepared = true;
    onParamsChanged();
    reset();
}

void HologramVoice::reset() noexcept
{
    for (auto& b : m_carrier) b.reset();
    for (auto& b : m_voice) b.reset();
    for (auto& s : m_shelf) s.reset();
    std::fill(m_powIn.begin(), m_powIn.end(), 0.0);
    std::fill(m_powLayer.begin(), m_powLayer.end(), 0.0);
    m_noise = kNoiseSeed;   // same noise every render: preview == export
    m_hasSpare = false;
}

void HologramVoice::onParamsChanged()
{
    const float w = std::clamp(param(Mix) / 100.0f, 0.0f, 1.0f);
    // The layer is level-matched to the voice and the two are uncorrelated,
    // so dividing by the root of the summed squares keeps loudness constant.
    const float norm = 1.0f / std::sqrt((1.0f - w) * (1.0f - w) + w * w);
    m_dryGain = (1.0f - w) * norm;
    m_wetGain = w * norm;
    m_out = dbToLinear(param(Output));
    m_bandCount = std::clamp(static_cast<int>(param(Bands) + 0.5f), 8, kMaxBands);

    if (!m_prepared || m_carrier.size() < static_cast<size_t>(kMaxBands)) return;

    // Only coefficients change; filter state is kept so retuning during
    // playback never clicks.
    auto retune = [](BandFilter& f, const std::array<Section, 4>& design) {
        for (size_t i = 0; i < design.size(); ++i) {
            f.bp[i].b0 = design[i].b0; f.bp[i].b1 = design[i].b1; f.bp[i].b2 = design[i].b2;
            f.bp[i].a1 = design[i].a1; f.bp[i].a2 = design[i].a2;
        }
    };
    Biquad env;
    env.setCoefficients(Biquad::Type::LowPass, m_sampleRate,
                        static_cast<double>(param(Smoothing)), 0.7071, 0.0);

    const double top = std::min(kHighestBandHz, m_sampleRate * 0.45);
    const double ratio = std::pow(top / kLowestBandHz, 1.0 / m_bandCount);
    for (int b = 0; b < m_bandCount; ++b) {
        const double lo = kLowestBandHz * std::pow(ratio, b);
        const auto design = designBandPass(lo, lo * ratio, m_sampleRate);
        auto setEnv = [&env](Biquad& e) {
            e.b0 = env.b0; e.b1 = env.b1; e.b2 = env.b2; e.a1 = env.a1; e.a2 = env.a2;
        };
        retune(m_carrier[static_cast<size_t>(b)], design);
        setEnv(m_carrier[static_cast<size_t>(b)].env);
        for (int c = 0; c < m_channels; ++c) {
            auto& v = m_voice[static_cast<size_t>(c) * kMaxBands + b];
            retune(v, design);
            setEnv(v.env);
        }
    }
    for (auto& s : m_shelf)
        s.setCoefficients(Biquad::Type::HighShelf, m_sampleRate, 4000.0, 0.7071,
                          param(Brightness));
}

bool HologramVoice::isActive() const noexcept
{
    return m_wetGain > 0.0f || std::fabs(m_out - 1.0f) > 1e-4f;
}

float HologramVoice::nextGaussian() noexcept
{
    // Box–Muller on xorshift32: unit-variance white noise, like the reference.
    if (m_hasSpare) { m_hasSpare = false; return m_spareGaussian; }
    auto uniform = [this]() {
        m_noise ^= m_noise << 13; m_noise ^= m_noise >> 17; m_noise ^= m_noise << 5;
        return (static_cast<double>(m_noise) + 1.0) / 4294967297.0;   // (0, 1)
    };
    const double r = std::sqrt(-2.0 * std::log(uniform()));
    const double t = 2.0 * kPi * uniform();
    m_spareGaussian = static_cast<float>(r * std::sin(t));
    m_hasSpare = true;
    return static_cast<float>(r * std::cos(t));
}

void HologramVoice::process(float* data, int frames) noexcept
{
    if (!m_prepared || frames <= 0) return;
    const int ch = m_channels;
    const int nb = m_bandCount;
    float carrier[kMaxBands];

    for (int f = 0; f < frames; ++f) {
        // White noise split into the carrier bands, each flattened to a unit
        // envelope.
        const double n = nextGaussian();
        for (int b = 0; b < nb; ++b) {
            auto& cf = m_carrier[static_cast<size_t>(b)];
            const float cb = static_cast<float>(cf.band(n));
            const float ce = std::max(cf.env.processSample(std::fabs(cb)), 1e-6f);
            carrier[b] = cb / ce;
        }

        float* frame = data + static_cast<size_t>(f) * ch;
        for (int c = 0; c < ch; ++c) {
            const float x = frame[c];
            BandFilter* vb = &m_voice[static_cast<size_t>(c) * kMaxBands];
            float layer = 0.0f;
            for (int b = 0; b < nb; ++b) {
                const float xb = static_cast<float>(vb[b].band(x));
                const float env = std::max(vb[b].env.processSample(std::fabs(xb)), 0.0f);
                layer += carrier[b] * env;
            }
            layer = m_shelf[static_cast<size_t>(c)].processSample(layer);

            // Slow level match: keep the layer as loud as the voice.
            double& pi = m_powIn[static_cast<size_t>(c)];
            double& pl = m_powLayer[static_cast<size_t>(c)];
            pi = m_powCoef * pi + (1.0 - m_powCoef) * static_cast<double>(x) * x;
            pl = m_powCoef * pl + (1.0 - m_powCoef) * static_cast<double>(layer) * layer;
            const float g = std::min(static_cast<float>(std::sqrt((pi + 1e-12) / (pl + 1e-12))),
                                     kMaxLayerGain);

            frame[c] = m_out * (m_dryGain * x + m_wetGain * g * layer);
        }
    }
}

std::unique_ptr<AudioProcessor> HologramVoice::clone() const
{
    return cloneAs<HologramVoice>();
}

} // namespace rt::audiofx
