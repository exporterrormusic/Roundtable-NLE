/*
 * BeatDetector.cpp — energy-flux onset detection (see header).
 */

#include "audio/BeatDetector.h"

#include <algorithm>
#include <cmath>

namespace rt {

std::vector<float> detectBeats(const float* mono, size_t n,
                               uint32_t sampleRate, const BeatDetectorParams& p)
{
    std::vector<float> onsets;
    if (!mono || n == 0 || sampleRate == 0) return onsets;

    // ── 1. Pre-emphasis: boost transients (high-pass).  This makes onset
    //    detection MUCH more reliable for all genres — the energy flux
    //    after pre-emphasis is dominated by attacks, not sustained tones.
    std::vector<float> preEmp(n);
    {
        float prev = 0.0f;
        const float alpha = 0.95f;  // classic pre-emphasis coefficient
        for (size_t i = 0; i < n; ++i) {
            preEmp[i] = mono[i] - alpha * prev;
            prev = mono[i];
        }
    }

    // ── 2. Optional bass enhancement: apply a gentle low-pass AFTER
    //    pre-emphasis so we capture kick/bass transients rather than
    //    sustained low-end rumble.
    std::vector<float> bassFilt;
    const float* src = preEmp.data();
    if (p.bassOnly) {
        bassFilt.resize(n);
        const float dt = 1.0f / static_cast<float>(sampleRate);
        const float rc = 1.0f / (2.0f * 3.14159265f * std::max(p.bassCutoffHz, 1.0f));
        const float a  = dt / (rc + dt);
        float y = 0.0f;
        for (size_t i = 0; i < n; ++i) { y += a * (preEmp[i] - y); bassFilt[i] = y; }
        src = bassFilt.data();
    }

    // ── 3. Short-time energy envelope with 50 % overlap for better
    //    time resolution.  ~23 ms window (1024 samples @ 44.1k) is long
    //    enough to capture a kick-drum transient while short enough to
    //    separate consecutive 16th notes at 140 BPM.
    const size_t hop = std::max<size_t>(1, sampleRate / 86);   // ~11.6 ms hop
    const size_t win = std::max<size_t>(hop * 2, size_t{512}); // ~23 ms window
    const size_t frames = (n >= win) ? ((n - win) / hop + 1) : size_t{0};
    if (frames < 3) return onsets;

    std::vector<float> energy(frames, 0.0f);
    for (size_t k = 0; k < frames; ++k) {
        const size_t start = k * hop;
        const size_t end   = std::min(start + win, n);
        float e = 0.0f;
        for (size_t i = start; i < end; ++i) e += src[i] * src[i];
        energy[k] = e / static_cast<float>(end - start);
    }

    // ── 4. Weighted energy flux — the difference between current energy
    //    and a trailing exponential moving average.  This is more robust
    //    than raw frame-to-frame difference because it adapts to the
    //    local energy level.
    std::vector<float> flux(frames, 0.0f);
    {
        const float emaAlpha = 0.3f;  // fast decay — reacts within ~3 hops
        float ema = energy[0];
        for (size_t k = 1; k < frames; ++k) {
            ema = emaAlpha * energy[k] + (1.0f - emaAlpha) * ema;
            float diff = energy[k] - ema;
            flux[k] = (diff > 0.0f) ? diff : 0.0f;
        }
    }

    // ── 5. Adaptive-threshold peak picking with look-ahead.  Uses a
    //    shorter averaging window (100 ms) so the threshold adapts
    //    quickly to dynamics changes, and a longer look-behind to
    //    suppress false positives after a strong onset.
    const size_t  avgWin = std::max<size_t>(2, (sampleRate / hop) / 10); // ~100 ms
    const float   framesPerSec = static_cast<float>(sampleRate) / static_cast<float>(hop);
    const size_t  minGap = static_cast<size_t>(p.minIntervalSec * framesPerSec);
    size_t  lastOnset = 0;
    bool    haveOnset = false;
    float   peakFlux  = 0.0f;
    size_t  peakIdx   = 0;

    for (size_t k = 1; k < frames; ++k) {
        // Track the strongest flux in the current inter-onset window.
        if (flux[k] > peakFlux) {
            peakFlux = flux[k];
            peakIdx  = k;
        }

        // Compute local mean flux (exclude the current peak-search window
        // to avoid self-thresholding).
        const size_t lo = (k > avgWin) ? k - avgWin : 0;
        const size_t hi = std::min(frames, k + avgWin + 1);
        float mean = 0.0f;
        for (size_t j = lo; j < hi; ++j) mean += flux[j];
        mean /= static_cast<float>(hi - lo);

        const float thr = mean * p.sensitivity + 1e-8f;

        // Is the strongest flux in this window above threshold AND
        // are we past the minimum inter-onset gap?
        if (peakFlux > thr &&
            (!haveOnset || (peakIdx - lastOnset) >= minGap))
        {
            onsets.push_back(static_cast<float>(peakIdx * hop) / static_cast<float>(sampleRate));
            lastOnset = peakIdx;
            haveOnset = true;
            peakFlux  = 0.0f;
        }

        // Reset peak search when we've passed the min-gap boundary or
        // when the flux drops back to near zero.
        if (haveOnset && (k - lastOnset) >= minGap) {
            peakFlux = 0.0f;
        }
        if (flux[k] < thr * 0.1f) {
            peakFlux = std::max(peakFlux, flux[k]); // keep tracking
        }
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
