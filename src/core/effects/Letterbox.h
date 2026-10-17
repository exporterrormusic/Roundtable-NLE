/*
 * Letterbox — cinematic letterbox/pillarbox bars overlay.
 *
 * Parameters: aspect ratio, bar opacity, bar color (RGB), feathering.
 * Uses compute shader for GPU-accelerated processing.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Letterbox final : public Effect
{
public:
    Letterbox() : Effect(EffectType::Letterbox) {}

    enum Param : size_t {
        AspectWidth = 0,  // 1–4 (numerator, e.g. 2.39 for 2.39:1)
        AspectHeight,     // 1–4 (denominator, typically 1)
        BarOpacity,       // 0–1 (bar opacity, 1 = fully black)
        BarR,             // 0–1 (bar color red)
        BarG,             // 0–1 (bar color green)
        BarB,             // 0–1 (bar color blue)
        Feather,          // 0–0.1 (softness at bar edge, normalized)
        ParamCount
    };
};

} // namespace rt
