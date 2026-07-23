/*
 * ColorGrading — Unified color grading effect.
 *
 * All color grading in one effect node (single GPU shader pass):
 *   - Basic Correction: temperature, tint, exposure, contrast,
 *                        highlights, shadows, whites, blacks, saturation
 *   - Creative: faded film, sharpen, vibrance, creative saturation
 *   - Curves: master and per-channel response curves
 *   - Color Wheels: shadow/midtone/highlight lift-gamma-gain
 *   - HSL Secondary: hue/saturation/luminance qualifier and correction
 *   - Vignette: amount, midpoint, roundness, feather
 *
 * Sections can be individually bypassed via enable flags stored
 * alongside the keyframeable parameters.
 */

#pragma once

#include "effects/Effect.h"

#include <array>

namespace rt {

class ColorGrading : public Effect
{
public:
    ColorGrading();
    ~ColorGrading() override = default;

    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    // ── Parameter indices (into m_params) ───────────────────────────────
    // Must match the order that addParam() is called in the constructor,
    // and must match the order expected by the GPU compute shader
    // (shaders/lumetri_color.comp — legacy name, this class replaces
    // the old LumetriColor effect).
    enum Param : size_t {
        // Basic Correction (section 0)
        Temperature = 0,  // -100 to 100
        Tint,             // -100 to 100
        Exposure,         // -5 to 5 (stops)
        Contrast,         // -100 to 100
        Highlights,       // -100 to 100
        Shadows,          // -100 to 100
        Whites,           // -100 to 100
        Blacks,           // -100 to 100
        Saturation,       // 0 to 200 (100 = neutral)

        // Creative (section 1)
        FadedFilm,        // 0 to 100
        Sharpen,          // 0 to 100 (applied via unsharp mask)
        Vibrance,         // -100 to 100
        CreativeSat,      // 0 to 200 (100 = neutral)

        // Color Wheels (section 2) — lift/gamma/gain per tonal range
        ShadowR,          // -1 to 1  (shadow lift R offset)
        ShadowG,          // -1 to 1
        ShadowB,          // -1 to 1
        MidtoneR,         // -1 to 1  (midtone gamma R offset)
        MidtoneG,         // -1 to 1
        MidtoneB,         // -1 to 1
        HighlightR,       // -1 to 1  (highlight gain R offset)
        HighlightG,       // -1 to 1
        HighlightB,       // -1 to 1
        ShadowMaster,     // -1 to 1  (shadow luminance offset)
        MidtoneMaster,    // -1 to 1  (midtone luminance offset)
        HighlightMaster,  // -1 to 1  (highlight luminance offset)

        // Vignette (section 3)
        VignetteAmount,   // -5 to 5 (negative = darken, positive = lighten)
        VignetteMidpoint, // 0 to 100 (default 50)
        VignetteRoundness,// 0 to 100 (default 50)
        VignetteFeather,  // 0 to 100 (default 50)

        ParamCount
    };

    // ── Section enable/bypass ───────────────────────────────────────────
    bool basicEnabled()    const noexcept { return m_basicEnabled; }
    bool creativeEnabled() const noexcept { return m_creativeEnabled; }
    bool wheelsEnabled()   const noexcept { return m_wheelsEnabled; }
    bool curvesEnabled()   const noexcept { return m_curvesEnabled; }
    bool hslEnabled()      const noexcept { return m_hslEnabled; }
    bool vignetteEnabled() const noexcept { return m_vignetteEnabled; }

    void setBasicEnabled(bool v)    noexcept { m_basicEnabled = v; }
    void setCreativeEnabled(bool v) noexcept { m_creativeEnabled = v; }
    void setWheelsEnabled(bool v)   noexcept { m_wheelsEnabled = v; }
    void setCurvesEnabled(bool v)   noexcept { m_curvesEnabled = v; }
    void setHslEnabled(bool v)      noexcept { m_hslEnabled = v; }
    void setVignetteEnabled(bool v) noexcept { m_vignetteEnabled = v; }

    // ── Curves LUT (stored separately from push-constant params) ────────
    using CurveLUT = std::array<float, 256>;
    enum CurveChannel : uint8_t { CurveMaster = 0, CurveRed, CurveGreen, CurveBlue, CurveCount };

    void setCurveLUT(CurveChannel ch, const CurveLUT& lut);
    [[nodiscard]] const CurveLUT& curveLUT(CurveChannel ch) const noexcept { return m_curveLuts[ch]; }
    [[nodiscard]] bool curvesIdentity() const noexcept { return m_curvesIdentity; }
    void resetCurveLUTs();

    // ── HSL Secondary qualifier + correction ────────────────────────────
    struct HslParams {
        float hueCenter{0.0f};      // 0-360
        float hueWidth{60.0f};      // 0-180
        float satMin{0.0f};         // 0-100
        float satMax{100.0f};       // 0-100
        float lumMin{0.0f};         // 0-100
        float lumMax{100.0f};       // 0-100
        // Corrections applied to qualified pixels
        float hueShift{0.0f};       // -180 to 180
        float satAdjust{0.0f};      // -100 to 100
        float lumAdjust{0.0f};      // -100 to 100
    };

    void setHslParams(const HslParams& p) noexcept { m_hslParams = p; }
    [[nodiscard]] const HslParams& hslParams() const noexcept { return m_hslParams; }

    /// Complete editable state used by live UI gestures and undo/redo.  Keeping
    /// this in the model makes non-scalar controls (curves, wheels, section
    /// toggles, and HSL) just as reversible as ordinary parameter sliders.
    struct State {
        bool enabled{true};
        bool basicEnabled{true};
        bool creativeEnabled{true};
        bool wheelsEnabled{true};
        bool curvesEnabled{true};
        bool hslEnabled{true};
        bool vignetteEnabled{true};
        std::vector<KeyframeTrack<float>> paramTracks;
        std::array<CurveLUT, CurveCount> curveLuts;
        bool curvesIdentity{true};
        HslParams hslParams;
    };

    [[nodiscard]] State captureState() const;
    void restoreState(const State& state);

    /// Evaluate all params and pack section-enable flags into the float array
    /// so the GPU shader can skip disabled sections.
    [[nodiscard]] std::vector<float> evalAllParamsWithFlags(int64_t time) const;

    /// Repack params into 28 floats for GPU push constants.
    /// Layout: [0-8] Basic, [9] FadedFilm, [10] Vibrance, [11] CreativeSat,
    ///         [12-15] Vignette, [16-18] Shadow RGB, [19-21] Midtone RGB,
    ///         [22-24] Highlight RGB, [25-27] Master offsets.
    [[nodiscard]] std::vector<float> evalGpuParams(int64_t time) const;

    /// Additional passes run through the same GPU pipeline.  The vector size
    /// identifies the pass to the shader: 24 = curves, 9 = HSL Secondary.
    [[nodiscard]] std::vector<float> evalCurveGpuParams() const;
    [[nodiscard]] std::vector<float> evalHslGpuParams() const;

private:
    bool m_basicEnabled{true};
    bool m_creativeEnabled{true};
    bool m_wheelsEnabled{true};
    bool m_curvesEnabled{true};
    bool m_hslEnabled{true};
    bool m_vignetteEnabled{true};

    // Curves: 4 channels x 256 LUT entries (identity = linear ramp)
    std::array<CurveLUT, CurveCount> m_curveLuts;
    bool m_curvesIdentity{true};

    // HSL Secondary
    HslParams m_hslParams;
};

} // namespace rt
