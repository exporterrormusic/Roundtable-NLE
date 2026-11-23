/*
 * Reverb — algorithmic room reverb (Premiere "Studio Reverb" style controls).
 *
 * Signal flow (mono sum in, stereo out):
 *
 *   in ─► pre-delay ─┬─► early reflections (8 taps, alternating L/R) ──┐
 *                    └─► 4 series allpass diffusers ─► 8-line FDN ────┤
 *                                                     (Hadamard mix,  │
 *                                                      damped loops)  ▼
 *                       wet = low/high cut ( ER + late, width ) ─► dry·in + wet
 *
 * Decay is an RT60 in seconds: each FDN line's loop gain is set so the tail
 * falls 60 dB in that time. Damping is a one-pole low-pass inside each loop,
 * so highs die faster than lows like a real room.
 *
 * Tails are capped at 4 s: timeline playback bakes effects into windows that
 * begin 4 s before the playhead, and a longer tail would not have settled.
 * As in Premiere, a clip effect's tail ends at the clip's end.
 */

#pragma once

#include "audiofx/Biquad.h"
#include "audiofx/ParamProcessor.h"

#include <array>
#include <vector>

namespace rt::audiofx {

class Reverb final : public ParamProcessor
{
public:
    enum Param : int
    {
        RoomSize = 0,   ///< 0–100 %: scales delay lengths and reflection spacing
        Decay,          ///< RT60 seconds
        PreDelay,       ///< ms
        Damping,        ///< 0–100 %: high-frequency loss inside the tail
        Diffusion,      ///< 0–100 %: how smeared the onset is
        EarlyReflections, ///< 0–100 %: level of the discrete first echoes
        Width,          ///< 0–100 %: stereo width of the wet signal
        LowCut,         ///< Hz, high-pass on the wet signal
        HighCut,        ///< Hz, low-pass on the wet signal
        Dry,            ///< 0–100 %
        Wet,            ///< 0–100 %
        Count
    };

    Reverb();

    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    static constexpr int kLines = 8;
    static constexpr int kDiffusers = 4;
    static constexpr int kTaps = 8;

    struct Ring
    {
        std::vector<float> buf;
        int                w{0};
        void  resize(int n) { buf.assign(static_cast<size_t>(n > 1 ? n : 1), 0.0f); w = 0; }
        void  clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); w = 0; }
        [[nodiscard]] float read(int delay) const noexcept
        {
            const int n = static_cast<int>(buf.size());
            int r = w - delay;
            while (r < 0) r += n;
            return buf[static_cast<size_t>(r)];
        }
        void push(float v) noexcept
        {
            buf[static_cast<size_t>(w)] = v;
            if (++w >= static_cast<int>(buf.size())) w = 0;
        }
    };

    Ring m_input;                          ///< pre-delay + early-reflection taps
    std::array<Ring, kDiffusers> m_ap;
    std::array<int, kDiffusers>  m_apLen{};
    std::array<Ring, kLines>     m_line;
    std::array<int, kLines>      m_lineLen{};
    std::array<float, kLines>    m_lineGain{};
    std::array<float, kLines>    m_lp{};
    std::array<int, kTaps>       m_tapDelay{};
    std::array<float, kTaps>     m_tapGain{};
    std::array<Biquad, 2>        m_lowCut;
    std::array<Biquad, 2>        m_highCut;

    int   m_preDelay{0};
    float m_apCoef{0.5f};
    float m_damp{0.3f};
    float m_lateScale{0.3f};
    float m_erLevel{0.3f};
    float m_width{1.0f};
    float m_dry{1.0f};
    float m_wet{0.3f};
    bool  m_prepared{false};
};

} // namespace rt::audiofx
