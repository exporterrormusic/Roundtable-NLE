/*
 * ColorCorrect — color correction effect.
 *
 * Parameters: brightness, contrast, saturation, hue, temperature, tint,
 *             gamma, gain, lift (shadows), midtones, highlights.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class ColorCorrect final : public Effect
{
public:
    ColorCorrect() : Effect(EffectType::ColorCorrect) {}

    // Convenience accessors (indices into m_params)
    enum Param : size_t {
        Brightness = 0,
        Contrast,
        Saturation,
        Hue,
        Temperature,
        Tint,
        Gamma,
        Gain,
        Lift,
        ParamCount
    };
};

} // namespace rt
