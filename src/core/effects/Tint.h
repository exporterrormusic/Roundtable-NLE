/*
 * Tint — Premiere-style luminance color mapping.
 *
 * Black and white are mapped to user-selected colors. Intermediate
 * luminance values are interpolated between them, then Amount to Tint blends
 * the mapped result with the original image.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Tint : public Effect
{
public:
    Tint();
    ~Tint() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        MapBlackR = 0,
        MapBlackG,
        MapBlackB,
        MapWhiteR,
        MapWhiteG,
        MapWhiteB,
        AmountToTint, // 0-100 percent
        ParamCount
    };
};

} // namespace rt
