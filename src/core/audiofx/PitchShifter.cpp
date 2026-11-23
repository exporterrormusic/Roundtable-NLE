#include "audiofx/PitchShifter.h"

#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
#include <SoundTouch.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rt::audiofx {

namespace {
const ParamSpec kPitchSpecs[] = {
    {"Semitones", 0.0f, -24.0f, 24.0f, 1.0f, 0, " st"},
    {"Cents",     0.0f, -100.0f, 100.0f, 1.0f, 0, " ct"},
};
static_assert(sizeof(kPitchSpecs) / sizeof(kPitchSpecs[0]) == PitchShifter::Count);
} // namespace

PitchShifter::PitchShifter() : ParamProcessor("PitchShifter", kPitchSpecs, Count) {}
PitchShifter::~PitchShifter() = default;

bool PitchShifter::isActive() const noexcept
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    return std::fabs(param(Semitones) + param(Cents) / 100.0f) > 1e-4f;
#else
    return false;
#endif
}

int PitchShifter::latencyFrames() const noexcept
{
    return isActive() ? m_measuredLatency : 0;
}

void PitchShifter::prepare(double sampleRate, int channels)
{
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    m_channels = std::max(1, channels);
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    m_st = std::make_unique<soundtouch::SoundTouch>();
    m_st->setSampleRate(static_cast<unsigned>(m_sampleRate));
    m_st->setChannels(static_cast<unsigned>(m_channels));
    m_st->setTempo(1.0);
    m_st->setRate(1.0);
    m_st->setSetting(SETTING_USE_QUICKSEEK, 0);
    m_st->setSetting(SETTING_USE_AA_FILTER, 1);
    m_st->setSetting(SETTING_SEQUENCE_MS, 40);
    m_st->setSetting(SETTING_SEEKWINDOW_MS, 15);
    m_st->setSetting(SETTING_OVERLAP_MS, 8);
    m_prepared = true;
    onParamsChanged();   // sets the pitch and the latency, sizes the ring
#endif
}

void PitchShifter::onParamsChanged()
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    if (!m_st) return;
    m_st->setPitchSemiTones(static_cast<double>(param(Semitones)) + param(Cents) / 100.0);
    // Fixed latency: enough that the ring never runs dry once output starts
    // (initial buffering + one output batch of headroom).
    m_latency = m_st->getSetting(SETTING_INITIAL_LATENCY)
              + m_st->getSetting(SETTING_NOMINAL_OUTPUT_SEQUENCE);
    m_fifoFrames = static_cast<size_t>(m_latency) * 2 + kChunkFrames * 4;
    m_fifo.assign(m_fifoFrames * static_cast<size_t>(m_channels), 0.0f);
    m_receive.assign(static_cast<size_t>(kChunkFrames) * m_channels, 0.0f);
    // Let SoundTouch grow its internal buffers now, not on the audio thread.
    std::vector<float> prime(static_cast<size_t>(kChunkFrames) * m_channels, 0.0f);
    for (int i = 0; i < 4; ++i)
        m_st->putSamples(prime.data(), kChunkFrames);
    m_st->clear();
    reset();
    m_measuredLatency = isActive() ? measureDelay() : 0;
#endif
}

int PitchShifter::measureDelay()
{
    // 440 Hz bursts every 0.25 s; pitch shifting keeps their timing, so the
    // envelope cross-correlation peak is the true delay.
    const size_t ch = static_cast<size_t>(m_channels);
    const int frames = static_cast<int>(m_sampleRate * 1.5) + m_latency;
    const int burst = static_cast<int>(m_sampleRate * 0.01);
    const int period = static_cast<int>(m_sampleRate * 0.25);
    std::vector<float> in(static_cast<size_t>(frames), 0.0f);
    for (int start = static_cast<int>(m_sampleRate * 0.1); start + burst < frames; start += period)
        for (int i = 0; i < burst; ++i)
            in[static_cast<size_t>(start + i)] = 0.5f * static_cast<float>(
                std::sin(2.0 * 3.14159265358979 * 440.0 * i / m_sampleRate));
    std::vector<float> buf(static_cast<size_t>(frames) * ch);
    for (int f = 0; f < frames; ++f)
        for (size_t c = 0; c < ch; ++c) buf[static_cast<size_t>(f) * ch + c] = in[static_cast<size_t>(f)];
    reset();
    process(buf.data(), frames);
    reset();

    auto envelope = [&](auto sample, int block) {
        std::vector<double> e(static_cast<size_t>(frames / block), 0.0);
        for (size_t b = 0; b < e.size(); ++b) {
            double acc = 0.0;
            for (int i = 0; i < block; ++i) {
                const double v = sample(static_cast<int>(b) * block + i);
                acc += v * v;
            }
            e[b] = std::sqrt(acc / block);
        }
        return e;
    };
    auto inAt = [&](int f) { return static_cast<double>(in[static_cast<size_t>(f)]); };
    auto outAt = [&](int f) { return static_cast<double>(buf[static_cast<size_t>(f) * ch]); };
    auto bestLag = [](const std::vector<double>& a, const std::vector<double>& b, int from, int to) {
        int best = from;
        double bestScore = -1.0;
        for (int lag = std::max(0, from); lag <= to; ++lag) {
            double acc = 0.0;
            for (size_t i = 0; i + static_cast<size_t>(lag) < b.size() && i < a.size(); ++i)
                acc += a[i] * b[i + static_cast<size_t>(lag)];
            if (acc > bestScore) { bestScore = acc; best = lag; }
        }
        return best;
    };
    // Coarse (16-frame blocks) over the plausible range, then fine (4-frame).
    const int maxLag = (m_latency * 2 + 2048) / 16;
    const int coarse = bestLag(envelope(inAt, 16), envelope(outAt, 16), 0, maxLag) * 16;
    const int fine = bestLag(envelope(inAt, 4), envelope(outAt, 4),
                             (coarse - 32) / 4, (coarse + 32) / 4) * 4;
    return std::max(0, fine);
}

void PitchShifter::reset() noexcept
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    if (m_st) m_st->clear();
#endif
    m_readFrame = 0;
    m_fillFrames = 0;
    m_silenceLeft = m_latency;
}

void PitchShifter::process(float* data, int frames) noexcept
{
#ifdef ROUNDTABLE_HAS_SOUNDTOUCH
    if (!m_prepared || !m_st || frames <= 0) return;
    const size_t ch = static_cast<size_t>(m_channels);

    for (int done = 0; done < frames;) {
        const int n = std::min(kChunkFrames, frames - done);
        float* block = data + static_cast<size_t>(done) * ch;

        // 1. Feed the input, collect everything SoundTouch has ready.
        m_st->putSamples(block, static_cast<unsigned>(n));
        for (;;) {
            const size_t space = m_fifoFrames - m_fillFrames;
            if (space == 0) break;
            const unsigned want = static_cast<unsigned>(std::min<size_t>(space, kChunkFrames));
            const unsigned got = m_st->receiveSamples(m_receive.data(), want);
            if (got == 0) break;
            for (unsigned f = 0; f < got; ++f) {
                const size_t w = (m_readFrame + m_fillFrames) % m_fifoFrames;
                std::memcpy(&m_fifo[w * ch], &m_receive[f * ch], ch * sizeof(float));
                ++m_fillFrames;
            }
        }

        // 2. Emit: the fixed leading silence, then the ring in order.
        for (int f = 0; f < n; ++f) {
            float* out = block + static_cast<size_t>(f) * ch;
            if (m_silenceLeft > 0) {
                --m_silenceLeft;
                std::fill(out, out + ch, 0.0f);
            } else if (m_fillFrames > 0) {
                std::memcpy(out, &m_fifo[m_readFrame * ch], ch * sizeof(float));
                m_readFrame = (m_readFrame + 1) % m_fifoFrames;
                --m_fillFrames;
            } else {
                std::fill(out, out + ch, 0.0f);   // underrun (should not happen)
            }
        }
        done += n;
    }
#else
    (void)data; (void)frames;
#endif
}

std::unique_ptr<AudioProcessor> PitchShifter::clone() const { return cloneAs<PitchShifter>(); }

} // namespace rt::audiofx
