/*
 * PitchShifter — change pitch without changing timing (Premiere's Pitch
 * Shifter: semitones + cents). SoundTouch (WSOLA) does the shifting, as for
 * the pitch-preserving speed change.
 *
 * SoundTouch hands out audio in bursts after an initial buffering delay, so
 * this processor runs at a FIXED latency: it outputs a fixed lead of
 * silence, then the shifted stream. SoundTouch's own alignment shifts with
 * the pitch amount, so latencyFrames() is MEASURED whenever the settings
 * change (a burst train through this instance), giving
 * output[n] ~= shifted input[n - latencyFrames()]. Offline hosts (playback bake,
 * export) feed latencyFrames() extra samples and drop that many from the
 * front, so the clip stays in sync (FxChain::latencyFrames()). A track bus
 * runs it uncompensated (the track is that much late).
 *
 * All buffers are allocated in prepare(); process() does not allocate.
 */

#pragma once

#include "audiofx/ParamProcessor.h"

#include <memory>
#include <vector>

namespace soundtouch { class SoundTouch; }

namespace rt::audiofx {

class PitchShifter final : public ParamProcessor
{
public:
    enum Param : int { Semitones = 0, Cents, Count };

    PitchShifter();
    ~PitchShifter() override;

    void prepare(double sampleRate, int channels) override;
    void reset() noexcept override;
    void process(float* data, int frames) noexcept override;
    [[nodiscard]] bool isActive() const noexcept override;
    [[nodiscard]] int latencyFrames() const noexcept override;
    [[nodiscard]] std::unique_ptr<AudioProcessor> clone() const override;

protected:
    void onParamsChanged() override;

private:
    static constexpr int kChunkFrames = 4096;

    /// Run a burst train through this instance and return where it really
    /// comes out (frames). SoundTouch's own offset changes with the pitch,
    /// so the reported latency is measured, not assumed. Resets afterwards.
    int measureDelay();

    std::unique_ptr<soundtouch::SoundTouch> m_st;
    std::vector<float> m_fifo;      ///< ring of shifted frames waiting to go out
    std::vector<float> m_receive;   ///< scratch for receiveSamples
    size_t m_fifoFrames{0};         ///< capacity in frames
    size_t m_readFrame{0};
    size_t m_fillFrames{0};         ///< frames currently in the ring
    int    m_latency{0};            ///< leading silence emitted (frames)
    int    m_measuredLatency{0};    ///< true input->output delay (frames)
    int    m_silenceLeft{0};        ///< leading silence still to emit
    bool   m_prepared{false};
};

} // namespace rt::audiofx
