/*
 * Vignette — edge-darkening / color-tinting vignette effect.
 *
 * Parameters: intensity, roundness, softness, center offset X/Y.
 * Uses compute shader for GPU-accelerated processing.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Vignette final : public Effect
{
public:
    Vignette() : Effect(EffectType::Vignette) {}

    enum Param : size_t {
        Intensity = 0,  // 0–2 (0 = no effect, 1 = standard, 2 = heavy)
        Roundness,      // 0–1 (0 = rectangular, 1 = circular)
        Softness,       // 0.01–1 (edge feathering)
        CenterX,        // -1 to 1 (offset from center)
        CenterY,        // -1 to 1 (offset from center)
        ParamCount
    };
};

} // namespace rt
