/*
 * FxChain — an ordered series of AudioProcessors applied to one audio stream.
 *
 * Attach a chain to a clip or a track; the host calls prepare() once (and on
 * any format change) then process() per block. Processors run top-to-bottom,
 * matching the visual order in an effects panel.
 *
 * Ownership and structural edits (add/remove/move) happen on the control
 * thread. process() only walks the existing list, so a host that mutates the
 * chain while audio is running must swap chains atomically (the audio engine
 * already swaps its source list under a lock for exactly this reason).
 */

#pragma once

#include "audiofx/AudioProcessor.h"

#include <memory>
#include <string>
#include <vector>

namespace rt::audiofx {

/// Built-in processor kinds the factory can create (for serialization / UI).
enum class ProcessorKind
{
    ParametricEQ,
    Dynamics,
};

[[nodiscard]] const char* processorKindName(ProcessorKind k) noexcept;
[[nodiscard]] std::unique_ptr<AudioProcessor> createProcessor(ProcessorKind k);

class FxChain
{
public:
    FxChain() = default;

    // Movable, non-copyable (owns unique processors).
    FxChain(FxChain&&) noexcept = default;
    FxChain& operator=(FxChain&&) noexcept = default;
    FxChain(const FxChain&) = delete;
    FxChain& operator=(const FxChain&) = delete;

    // ── Structure (control thread) ──────────────────────────────────────
    AudioProcessor* add(std::unique_ptr<AudioProcessor> p);
    AudioProcessor* add(ProcessorKind kind);
    std::unique_ptr<AudioProcessor> remove(size_t index);
    void move(size_t from, size_t to);
    void clear() noexcept { m_processors.clear(); }

    /// Deep copy (settings only; the copy is un-prepared until prepare()).
    [[nodiscard]] FxChain clone() const;

    [[nodiscard]] size_t size() const noexcept { return m_processors.size(); }
    [[nodiscard]] bool   empty() const noexcept { return m_processors.empty(); }
    [[nodiscard]] AudioProcessor&       at(size_t i)       { return *m_processors[i]; }
    [[nodiscard]] const AudioProcessor& at(size_t i) const { return *m_processors[i]; }

    // ── Audio lifecycle ─────────────────────────────────────────────────
    void prepare(double sampleRate, int channels);
    void reset() noexcept;
    void process(float* data, int frames) noexcept;

    /// True if any enabled processor would alter the signal.
    [[nodiscard]] bool isActive() const noexcept;

private:
    std::vector<std::unique_ptr<AudioProcessor>> m_processors;
    double m_sampleRate{48000.0};
    int    m_channels{2};
};

} // namespace rt::audiofx
