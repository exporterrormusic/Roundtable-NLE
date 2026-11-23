/*
 * RepairFx — small dialogue-repair / imaging processors used by Essential
 * Sound (and available on their own in the Add menu):
 *
 *   DeEsser     — dynamic high shelf: a high-passed sidechain above
 *                 Frequency drives a shelf cut (up to Max Reduction) while
 *                 sibilance exceeds Threshold; at rest the shelf is flat.
 *   DeHum       — notch filters at the mains frequency (50 / 60 Hz) and its
 *                 harmonics.
 *   StereoWidth — mid/side width (0 % = mono, 100 % = unchanged, 200 % = wide).
 */

#pragma once

#include "audiofx/Biquad.h"
#include "audiofx/ParamProcessor.h"

#include <array>
#include <vector>

namespace rt::audiofx {

class DeEsser final : public ParamProcessor
{
public:
    enum Param : int { Frequency = 0, Threshold, MaxReduction, Count };

    DeEsser();
    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    std::vector<Biquad> m_band;   ///< per-channel high pass (sidechain only)
    std::vector<Biquad> m_shelf;  ///< per-channel high shelf (the cut)
    float m_env{0.0f};            ///< stereo-linked band envelope (linear)
    float m_appliedDb{0.0f};      ///< shelf gain currently in the filters
    int   m_sinceUpdate{0};       ///< frames since the shelf was redesigned
    float m_threshold{0.03f};
    float m_floor{0.4f};          ///< minimum band gain (from MaxReduction)
    float m_attack{0.0f}, m_release{0.0f};
    bool  m_prepared{false};
};

class DeHum final : public ParamProcessor
{
public:
    enum Param : int { MainsFrequency = 0, Harmonics, Depth, Count };
    static constexpr int kMaxHarmonics = 8;

    DeHum();
    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    std::vector<std::array<Biquad, kMaxHarmonics>> m_notch;   ///< per channel
    int   m_count{4};
    float m_mix{1.0f};
    bool  m_prepared{false};
};

class StereoWidth final : public ParamProcessor
{
public:
    enum Param : int { Width = 0, Count };

    StereoWidth();
    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override {}
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override {}
};

} // namespace rt::audiofx
