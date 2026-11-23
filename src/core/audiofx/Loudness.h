/*
 * Loudness — ITU-R BS.1770-4 integrated loudness (LUFS), as used by
 * Premiere's Essential Sound "Auto-Match" and broadcast specs (EBU R128
 * targets -23 LUFS, streaming / podcasts commonly -16 LUFS).
 *
 * K-weighting (high shelf + high pass, coefficients designed for any sample
 * rate), 400 ms blocks with 75 % overlap, an absolute gate at -70 LUFS and a
 * relative gate 10 LU below the absolute-gated mean. Channels 0-2 weigh 1.0,
 * further (surround) channels 1.41.
 *
 * Feed audio in any block sizes; the result does not depend on them.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace rt::audiofx {

class LoudnessMeter
{
public:
    LoudnessMeter(double sampleRate, int channels);

    /// Add `frames` of interleaved float audio.
    void add(const float* interleaved, int64_t frames);

    /// Integrated loudness in LUFS; nullopt when everything was below the
    /// absolute gate (silence) or less than one 400 ms block was fed.
    [[nodiscard]] std::optional<double> integrated() const;

private:
    struct Stage { double b0, b1, b2, a1, a2; };
    struct State { double z1{0}, z2{0}; };

    double m_sampleRate;
    int    m_channels;
    Stage  m_shelf{};
    Stage  m_highPass{};
    std::vector<State> m_shelfState;
    std::vector<State> m_hpState;

    int64_t m_stepFrames{1};          ///< 100 ms
    int64_t m_inStep{0};              ///< frames accumulated in the current step
    std::vector<double> m_stepEnergy; ///< per channel, current step
    std::vector<double> m_steps;      ///< weighted energy sum per finished step
    std::vector<double> m_blocks;     ///< weighted mean square per 400 ms block
};

/// Convenience: integrated loudness of a whole buffer.
[[nodiscard]] std::optional<double> integratedLoudness(const float* interleaved, int64_t frames,
                                                       int channels, double sampleRate);

} // namespace rt::audiofx
