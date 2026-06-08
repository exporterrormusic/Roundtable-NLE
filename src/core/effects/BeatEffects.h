/*
 * BeatEffects — beat-reactive effects for music videos.
 *
 * Each effect pulses on a beat clock: a quick attack then an eased decay on
 * every beat. The clock is either a manual tempo grid (BPM + offset + rate) or
 * a baked list of detected onset times (bass/kick aware) from a chosen audio
 * source. EffectStack computes the 0–1 pulse envelope on the CPU and forwards
 * it to the GPU shader at kBeatPulseSlot, so the shaders stay trivial.
 *
 * Like the glitch family, the reactions stay *inside* the frame (edge-clamped
 * sampling, source alpha preserved) so borders/crop never change, and they
 * adapt to any clip length because the clock is clip-local.
 */

#pragma once

#include "effects/Effect.h"

#include <vector>

namespace rt {

// ── Beat-reactive base ──────────────────────────────────────────────────────
// Holds the shared tempo/onset config (as keyframeable params so the existing
// generic param UI edits them) plus the baked onset times for Auto mode.
class BeatReactEffect : public Effect
{
public:
    explicit BeatReactEffect(EffectType type);

    /// Shared params. Subclasses add Amount at index 0 first, then call
    /// addBeatParams() so these land at the indices below.
    enum BeatParam : size_t {
        Amount = 0,  // effect-specific strength (units vary per effect)
        BPM,         // 1–300   manual tempo
        Offset,      // -2–2 s  phase offset of beat 1
        Rate,        // 0.25–4  pulses per beat (1/4-note .. quad)
        Attack,      // 0–0.5 s rise time
        Decay,       // 0.01–2 s fall time
        Mode,        // 0 = manual BPM, 1 = detected onsets
        BeatParamCount
    };

    /// Compute the 0–1 pulse envelope at a clip-local time (ticks).
    [[nodiscard]] float computePulse(int64_t clipLocalTick) const;

    // ── Auto-mode onset data (clip-local seconds, ascending) ────────────
    void setBeatTimes(std::vector<float> times);
    [[nodiscard]] const std::vector<float>& beatTimes() const noexcept { return m_beatTimes; }

    /// Identifier of the audio source the onsets were detected from (a media
    /// id or clip id, 0 = none). Persisted so "re-detect" knows the source.
    void     setAudioSourceId(uint64_t id) noexcept { m_audioSourceId = id; }
    [[nodiscard]] uint64_t audioSourceId() const noexcept { return m_audioSourceId; }

protected:
    /// Register the shared beat params (call after adding "Amount").
    void addBeatParams();

    /// Copy base state (params, beat times, source) into a freshly cloned obj.
    void copyBeatStateTo(BeatReactEffect& dst) const;

    std::vector<float> m_beatTimes;       ///< detected onset times (seconds)
    uint64_t           m_audioSourceId{0};
};

// ── Reactions ───────────────────────────────────────────────────────────────
class BeatZoom : public BeatReactEffect
{
public:
    BeatZoom();
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

class BeatFlash : public BeatReactEffect
{
public:
    BeatFlash();
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

class BeatShake : public BeatReactEffect
{
public:
    BeatShake();
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

class BeatChroma : public BeatReactEffect
{
public:
    BeatChroma();
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

// Combined "drop": zoom + flash + chroma in one pass, one Amount knob.
class BeatDrop : public BeatReactEffect
{
public:
    BeatDrop();
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;
};

} // namespace rt
