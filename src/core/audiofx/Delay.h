/*
 * Delay — echo with filtered, optionally saturated feedback (Premiere
 * "Analog Delay" style).
 *
 * Each channel has a ring buffer up to 2 s long. The feedback path runs
 * through a low cut, a high cut ("tone") and an optional soft saturator, so
 * every repeat comes back darker and warmer, like tape. Ping-Pong sends the
 * mono input to the left line and cross-feeds the lines so repeats bounce
 * left / right (stereo only).
 */

#pragma once

#include "audiofx/Biquad.h"
#include "audiofx/ParamProcessor.h"

#include <vector>

namespace rt::audiofx {

class Delay final : public ParamProcessor
{
public:
    enum Param : int
    {
        Time = 0,     ///< ms
        Feedback,     ///< 0–95 %
        PingPong,     ///< toggle
        LowCut,       ///< Hz, in the feedback path
        HighCut,      ///< Hz, in the feedback path
        Saturation,   ///< 0–100 %
        Dry,          ///< 0–100 %
        Wet,          ///< 0–100 %
        Count
    };

    Delay();

    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    static constexpr float kMaxTimeMs = 2000.0f;

    std::vector<std::vector<float>> m_buf;   ///< one ring per channel
    std::vector<Biquad> m_lowCut;
    std::vector<Biquad> m_highCut;
    int   m_size{1};
    int   m_w{0};
    int   m_delay{1};
    float m_feedback{0.35f};
    float m_satDrive{1.0f};
    float m_dry{1.0f};
    float m_wet{0.35f};
    bool  m_pingPong{false};
    bool  m_prepared{false};
};

} // namespace rt::audiofx
