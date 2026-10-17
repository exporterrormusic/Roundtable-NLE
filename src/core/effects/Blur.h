/*
 * Blur — Gaussian blur effect.
 *
 * Gaussian blur with a Premiere-style Repeat Edge Pixels option. Sigma is
 * derived inside the shader as radius / 3.0 (the standard "3-sigma"
 * rule that keeps the kernel weight curve well-shaped for any radius).
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Blur final : public Effect
{
public:
    Blur() : Effect(EffectType::Blur) {}

    enum Param : size_t {
        Radius = 0,         // 0–100 pixels
        RepeatEdgePixels,   // boolean: 1 repeats edge pixels, 0 fades outside
        ParamCount
    };
};

} // namespace rt
