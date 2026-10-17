/*
 * Flip — mirror the image horizontally or vertically.
 *
 * One C++ class drives both `EffectType::FlipHorizontal` and
 * `EffectType::FlipVertical`; the axis is encoded in the effect type and
 * passed to the GPU shader as the first param (0 = horizontal, 1 = vertical).
 * No user-editable parameters — like Premiere Pro's Flip effects.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Flip final : public Effect
{
public:
    /// `type` must be EffectType::FlipHorizontal or EffectType::FlipVertical.
    explicit Flip(EffectType type) : Effect(type) {}

    enum Param : size_t {
        Axis = 0,   // 0 = horizontal, 1 = vertical (driven by effect type)
        ParamCount
    };
};

} // namespace rt
