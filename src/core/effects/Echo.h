/*
 * Echo — Premiere's Time > Echo: the clip combined with copies of itself
 * from other moments ("echoes").
 *
 * Echo has no shader. The compositor renders the clip once per entry of
 * planEcho() — each copy at its own time, with that copy's opacity and blend
 * mode — using the normal per-clip layer path, so effects, transforms (moving
 * clips leave trails), masks and every clip type work unchanged. Echo is a
 * TimeOnly effect: EffectStack::evaluate() never sends it to the GPU.
 *
 * Parameters (Premiere names): Echo Time (s, negative = earlier frames),
 * Number Of Echoes, Starting Intensity, Decay, Echo Operator. Copy k (k = 0 is
 * the current frame) shows time  t + k · EchoTime  at opacity
 * StartingIntensity · Decay^k.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace rt {

class EffectStack;

/// Echo Operator choices, in the effect's Choice order.
enum class EchoOperator : int
{
    Add = 0,
    Maximum,
    Minimum,
    Screen,
    CompositeInBack,
    CompositeInFront,
    Blend,
    Count
};

/// One rendered copy of the clip.
struct EchoCopy
{
    int     index{0};          ///< k: 0 = the current frame, 1.. = echoes
    int64_t tickOffset{0};     ///< added to the frame tick (timeline ticks)
    float   opacity{1.0f};     ///< multiplies the clip's own opacity
    int32_t blendMode{0};      ///< compositor BlendMode value (0 Normal, 2 Screen,
                               ///< 3 Add, 11 Darken, 12 Lighten)
};

/// Max echoes rendered (each is a full extra layer).
inline constexpr int kMaxEchoes = 16;

/// Copies to render for a clip at clip-local `localTick`, BOTTOM FIRST (the
/// compositor draws in this order). Empty when the stack has no enabled
/// Echo. Copies whose time falls outside [0, clipDuration) are left out,
/// as are copies with zero opacity. `clipBlendMode` is the clip's own blend
/// mode, kept for the bottom copy.
[[nodiscard]] std::vector<EchoCopy> planEcho(const EffectStack& stack, int64_t localTick,
                                             int64_t clipDuration, int32_t clipBlendMode);

} // namespace rt
