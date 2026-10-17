/*
 * Sharpen — Unsharp-mask sharpening effect.
 *
 * Parameters: amount, radius, threshold.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Sharpen final : public Effect
{
public:
    Sharpen() : Effect(EffectType::Sharpen) {}

    enum Param : size_t {
        Amount = 0,    // 0–5.0
        Radius,        // 0.1–10.0
        Threshold,     // 0–1.0
        ParamCount
    };
};

} // namespace rt
