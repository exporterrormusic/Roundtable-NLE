/*
 * BeatDetector.cpp — energy-flux onset detection (see header).
 */

#include "media/BeatDetector.h"

#include <algorithm>
#include <cmath>

namespace rt {

std::vector<float> detectBeats(const float* mono, size_t n,
                               uint32_t sampleRate, const BeatDetectorParams& p)
{
    std::vector<float> onsets;
    if (!mono || n == 0 || sampleRate == 0) return onsets;

    // ── 1. Optional bass low-pass (one-pole) ────────────────────────────
    std::vector<float> filt;
    const float* src = mono;
    if (p.bassOnly) {
        filt.resize(n);
        const float dt = 1.0f / static_cast<float>(sampleRate);
        const float rc = 1.0f / (2.0f * 3.14159265f * std::max(p.bassCutoffHz, 1.0f));
        const float a  = dt / (rc + dt);
        float y = 0.0f;
        for (size_t i = 0; i < n; ++i) { y += a * (mono[i] - y); filt[i] = y; }
        src = filt.data();
    }

    // ── 2. Short-time energy envelope ───────────────────────────────────
    const size_t hop = std::max<size_t>(1, sampleRate / 100);  // ~10 ms hop
    const size_t win = hop * 2;
    const size_t frames = n / hop;
    if (frames < 3) return onsets;

    std::vector<float> energy(frames, 0.0f);
    for (size_t k = 0; k < frames; ++k) {
        const size_t start = k * hop;
        const size_t end   = std::min(start + win, n);
        float e = 0.0f;
        for (size_t i = start; i < end; ++i) e += src[i] * src[i];
        energy[k] = e / static_cast<float>(win);
    }

    // ── 3. Positive energy flux (onset strength) ────────────────────────
    std::vector<float> flux(frames, 0.0f);
    for (size_t k = 1; k < frames; ++k)
        flux[k] = std::max(0.0f, energy[k] - energy[k - 1]);

    // ── 4. Adaptive-threshold peak picking ──────────────────────────────
    const size_t avgWin = std::max<size_t>(3, (sampleRate / hop) / 4);  // ~250 ms
    const float  framesPerSec = static_cast<float>(sampleRate) / static_cast<float>(hop);
    const size_t minGap = static_cast<size_t>(p.minIntervalSec * framesPerSec);
    size_t lastOnset = 0;
    bool   haveOnset = false;

    for (size_t k = 1; k + 1 < frames; ++k) {
        // Local mean of flux around k.
        const size_t lo = (k > avgWin) ? k - avgWin : 0;
        const size_t hi = std::min(frames, k + avgWin + 1);
        float mean = 0.0f;
        for (size_t j = lo; j < hi; ++j) mean += flux[j];
        mean /= static_cast<float>(hi - lo);

        const float thr = mean * p.sensitivity + 1e-6f;
        const bool isPeak = flux[k] > thr && flux[k] >= flux[k - 1] && flux[k] > flux[k + 1];
        if (!isPeak) continue;
        if (haveOnset && (k - lastOnset) < minGap) continue;

        onsets.push_back(static_cast<float>(k * hop) / static_cast<float>(sampleRate));
        lastOnset = k;
        haveOnset = true;
    }

    return onsets;
}

std::vector<float> detectBeatsInterleaved(const float* interleaved, int64_t frames,
                                          uint16_t channels, uint32_t sampleRate,
                                          const BeatDetectorParams& p)
{
    if (!interleaved || frames <= 0 || channels == 0) return {};
    if (channels == 1)
        return detectBeats(interleaved, static_cast<size_t>(frames), sampleRate, p);

    std::vector<float> mono(static_cast<size_t>(frames));
    for (int64_t f = 0; f < frames; ++f) {
        float s = 0.0f;
        for (uint16_t c = 0; c < channels; ++c)
            s += interleaved[f * channels + c];
        mono[static_cast<size_t>(f)] = s / static_cast<float>(channels);
    }
    return detectBeats(mono.data(), mono.size(), sampleRate, p);
}

} // namespace rt
