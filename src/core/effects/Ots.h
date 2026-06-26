/*
 * Ots — "Over the Shoulder" news-broadcast graphic effect.
 *
 * One C++ class drives `EffectType::OtsLeft`, `EffectType::OtsRight` and
 * `EffectType::OtsIntro`; the side is encoded in the effect type (and as the
 * first shader param — Left/Intro = 0, Right = 1).
 *
 * On construction, defaults are loaded from
 *   assets/presets/effects/OTS_LEFT.json  (or OTS_RIGHT.json / OTS_INTRO.json)
 * if present.  saveAsDefault() writes the current parameter values back to
 * that file so subsequent applications of the effect pick up the same
 * positioning, stroke, shadow, etc.  OtsIntro shares the same parameters and
 * shader; it is just an independent preset slot configured by the user.
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class Ots : public Effect
{
public:
    /// `type` must be EffectType::OtsLeft, OtsRight, or OtsIntro.
    explicit Ots(EffectType type);
    ~Ots() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Side = 0,           // 0 = left, 1 = right (driven by effect type, not user-edited)
        PosX,               // 0..1 of frame width  (offset from corresponding side)
        PosY,               // 0..1 of frame height (offset from top)
        Scale,              // 0..1 of frame width  (box width)
        StrokeWidth,        // px @ 1080p reference
        StrokeR,            // 0..1
        StrokeG,            // 0..1
        StrokeB,            // 0..1
        ShadowOffsetX,      // px @ 1080p
        ShadowOffsetY,      // px @ 1080p
        ShadowBlur,         // px @ 1080p
        ShadowOpacity,      // 0..1
        CornerRadius,       // px @ 1080p (0 = sharp/mitered)
        AspectMode,         // 0 = crop-fit to 16:9, 1 = preserve source aspect
        CropFocusX,         // 0..1
        CropFocusY,         // 0..1
        ParamCount
    };

    /// Persist current parameter values as the new defaults for this side.
    /// Returns true on success.
    bool saveAsDefault() const;

private:
    /// Load defaults from disk and overwrite each parameter's track with
    /// a single static-value track at the loaded value.  Silently no-op if
    /// the defaults file does not exist.
    void loadDefaultsFromDisk();
};

} // namespace rt
