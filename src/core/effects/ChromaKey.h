/*
 * ChromaKey — Ultra Key chroma keyer (green/blue screen removal).
 *
 * Premiere Pro–style Ultra Key with full matte generation, matte cleanup,
 * spill suppression, and foreground color correction.
 *
 * GPU pipeline: 3-pass compute (matte gen → matte cleanup → finalize).
 */

#pragma once

#include "effects/Effect.h"

namespace rt {

class ChromaKey : public Effect
{
public:
    ChromaKey();
    ~ChromaKey() override = default;


    // ── Parameters (ordered to match GPU push constant layout) ──────────
    enum Param : size_t {
        // Key color (RGB, sampled from frame via eyedropper)
        KeyColorR = 0,    // 0–1  (default 0.0 = green screen)
        KeyColorG,        // 0–1  (default 1.0)
        KeyColorB,        // 0–1  (default 0.0)

        // Output / preset
        OutputMode,       // 0=Composite, 1=Alpha Matte, 2=Color Channel,
                          // 3=Original, 4=Removed Color, 5=Spill Map
        Setting,          // 0=Default, 1=Relaxed, 2=Aggressive, 3=Custom, 4=ComfyUI Green

        // Matte Generation
        Transparency,     // 0–100  (default 0; foreground stays opaque)
        Highlight,        // 0–100  (default 10) — restores bright areas
        Shadow,           // 0–100  (default 50) — restores dark areas
        Tolerance,        // 0–100  (default 50) — chroma radius
        Pedestal,         // 0–100  (default 10) — noise floor removal

        // Matte Cleanup
        Choke,            // 0–100  (default 0)  — morphological erosion
        Soften,           // 0–100  (default 0)  — matte blur
        Contrast,         // 0–100  (default 0)  — alpha contrast
        MidPoint,         // 0–100  (default 50) — contrast pivot

        // Spill Suppression
        Desaturate,       // 0–50   (default 25)
        SpillRange,       // 0–100  (default 50)
        Spill,            // 0–100  (default 50)
        Luma,             // 0–100  (default 50) — brightness restoration

        // Foreground Color Correction
        Saturation,       // 0–200  (default 100)
        Hue,              // -180–180 (default 0)
        Luminance,        // 0–200  (default 100)

        ParamCount        // = 21
    };

    /// Apply a preset (Default / Relaxed / Aggressive) by batch-setting params.
    void applyPreset(int preset);
    /// Clean, flat green outputs such as ComfyUI/ToonOut renders.
    void applyFlatGreenPreset();
};

} // namespace rt
