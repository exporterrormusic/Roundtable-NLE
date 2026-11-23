/*
 * Distortion — waveshaper / bitcrusher for character sounds (telephone,
 * radio, megaphone, lo-fi).
 *
 * Modes: Soft Clip (tanh), Hard Clip, Tube (asymmetric, adds even
 * harmonics), Bit Crush (bit depth + sample-and-hold downsampling).
 * A tone low-pass and a DC blocker follow the shaper; Mix blends with the
 * clean signal and Output sets the level.
 */

#pragma once

#include "audiofx/Biquad.h"
#include "audiofx/ParamProcessor.h"

#include <vector>

namespace rt::audiofx {

class Distortion final : public ParamProcessor
{
public:
    enum Mode : int { SoftClip = 0, HardClip, Tube, BitCrush, ModeCount };

    enum Param : int
    {
        Type = 0,     ///< Mode
        Drive,        ///< dB of gain into the shaper
        Tone,         ///< Hz, low-pass after the shaper
        Bits,         ///< Bit Crush depth
        Downsample,   ///< Bit Crush sample-and-hold factor
        Mix,          ///< 0–100 %
        Output,       ///< dB
        Count
    };

    Distortion();

    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    std::vector<Biquad> m_tone;
    std::vector<Biquad> m_dcBlock;
    std::vector<float>  m_held;
    int   m_holdCounter{0};
    int   m_mode{SoftClip};
    float m_drive{1.0f};
    float m_levels{128.0f};
    int   m_downsample{1};
    float m_mix{1.0f};
    float m_out{1.0f};
    bool  m_prepared{false};
};

} // namespace rt::audiofx
