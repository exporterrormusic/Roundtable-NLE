/*
 * EffectRegistry.cpp — the effect table. See EffectRegistry.h.
 *
 * Order here is the Effects panel order. Within one effect, parameter order
 * is the saved-project order: append only.
 */

#include "effects/EffectRegistry.h"

#include <array>

namespace rt {

namespace {

using K = ParamKind;
namespace F = EffectFlag;

constexpr const char* kVideo  = "Video Effects";
constexpr const char* kAudio  = "Audio Effects";
constexpr const char* kGlitch = "Glitch";
constexpr const char* kBeat   = "Beat / Music";
// Premiere-style sub-folders of Video Effects ("Parent/Child" = nested folder).
constexpr const char* kBlurSharpen = "Video Effects/Blur & Sharpen";
constexpr const char* kColorFx     = "Video Effects/Color Correction";
constexpr const char* kDistort     = "Video Effects/Distort";
constexpr const char* kKeying      = "Video Effects/Keying";
constexpr const char* kPerspective = "Video Effects/Perspective";
constexpr const char* kStylize     = "Video Effects/Stylize";
constexpr const char* kTransform   = "Video Effects/Transform";
constexpr const char* kTime        = "Video Effects/Time";

ParamDef color(const char* name, float def, ParamKind kind,
               const char* group = nullptr, const char* widget = nullptr)
{
    ParamDef p{name, def, 0.0f, 1.0f, kind};
    p.group = group;
    p.widget = widget;
    return p;
}

ParamDef choice(const char* name, float def, std::vector<const char*> labels)
{
    ParamDef p{name, def, 0.0f, static_cast<float>(labels.size() - 1), K::Choice};
    p.choices = std::move(labels);
    return p;
}

ParamDef hidden(const char* name, float def, float min, float max)
{
    return ParamDef{name, def, min, max, K::Hidden};
}

/// A number row with explicit display precision / step / suffix.
ParamDef num(const char* name, float def, float min, float max,
             int decimals = 0, double step = 1.0, const char* suffix = nullptr)
{
    ParamDef p{name, def, min, max, K::Number};
    p.decimals = decimals;
    p.step = step;
    p.suffix = suffix;
    return p;
}

ParamDef toggle(const char* name, float def)
{
    return ParamDef{name, def, 0.0f, 1.0f, K::Toggle};
}

std::vector<ParamDef> otsParams(float side)
{
    return {
        hidden("Side", side, 0.0f, 1.0f),
        {"PosX",          0.05f, 0.0f, 1.0f},
        {"PosY",          0.55f, 0.0f, 1.0f},
        {"Scale",         0.35f, 0.0f, 1.0f},
        {"StrokeWidth",   3.0f,  0.0f, 32.0f},
        color("StrokeR", 1.0f, K::ColorR, "Stroke Color"),
        color("StrokeG", 1.0f, K::ColorG),
        color("StrokeB", 1.0f, K::ColorB),
        {"ShadowOffsetX", 4.0f, -64.0f, 64.0f},
        {"ShadowOffsetY", 4.0f, -64.0f, 64.0f},
        {"ShadowBlur",   12.0f,  0.0f, 128.0f},
        {"ShadowOpacity", 0.6f,  0.0f, 1.0f},
        {"CornerRadius",  0.0f,  0.0f, 64.0f},
        choice("AspectMode", 0.0f, {"Crop to 16:9", "Keep Source Aspect"}),
        {"CropFocusX",    0.5f,  0.0f, 1.0f},
        {"CropFocusY",    0.5f,  0.0f, 1.0f},
    };
}

std::vector<ParamDef> beatParams(float amount, float amountMax)
{
    return {
        {"Amount", amount, 0.0f, amountMax},
        {"BPM",    120.0f, 1.0f,  300.0f},
        {"Offset", 0.0f,  -2.0f,  2.0f},
        {"Rate",   1.0f,   0.25f, 4.0f},
        {"Attack", 0.01f,  0.0f,  0.5f},
        {"Decay",  0.25f,  0.01f, 2.0f},
        choice("Mode", 0.0f, {"Manual BPM", "Detected Beats"}),
    };
}

std::vector<ParamDef> colorGradingParams()
{
    return {
        // Basic Correction
        {"Temperature",   0.0f, -100.0f, 100.0f},
        {"Tint",          0.0f, -100.0f, 100.0f},
        {"Exposure",      0.0f,  -5.0f,   5.0f},
        {"Contrast",      0.0f, -100.0f, 100.0f},
        {"Highlights",    0.0f, -100.0f, 100.0f},
        {"Shadows",       0.0f, -100.0f, 100.0f},
        {"Whites",        0.0f, -100.0f, 100.0f},
        {"Blacks",        0.0f, -100.0f, 100.0f},
        {"Saturation",  100.0f,   0.0f, 200.0f},
        // Creative
        {"Faded Film",    0.0f,   0.0f, 100.0f},
        {"Sharpen",       0.0f,   0.0f, 100.0f},
        {"Vibrance",      0.0f, -100.0f, 100.0f},
        {"Creative Sat", 100.0f,  0.0f, 200.0f},
        // Color wheels (offsets)
        {"Shadow R",      0.0f,  -1.0f,   1.0f},
        {"Shadow G",      0.0f,  -1.0f,   1.0f},
        {"Shadow B",      0.0f,  -1.0f,   1.0f},
        {"Midtone R",     0.0f,  -1.0f,   1.0f},
        {"Midtone G",     0.0f,  -1.0f,   1.0f},
        {"Midtone B",     0.0f,  -1.0f,   1.0f},
        {"Highlight R",   0.0f,  -1.0f,   1.0f},
        {"Highlight G",   0.0f,  -1.0f,   1.0f},
        {"Highlight B",   0.0f,  -1.0f,   1.0f},
        {"Shadow Master",    0.0f, -1.0f, 1.0f},
        {"Midtone Master",   0.0f, -1.0f, 1.0f},
        {"Highlight Master", 0.0f, -1.0f, 1.0f},
        // Vignette
        {"Vignette Amount",    0.0f,  -5.0f,   5.0f},
        {"Vignette Midpoint", 50.0f,   0.0f, 100.0f},
        {"Vignette Roundness", 0.0f, -100.0f, 100.0f},
        {"Vignette Feather",  50.0f,   0.0f, 100.0f},
    };
}

std::vector<EffectDef> buildTable()
{
    std::vector<EffectDef> t;

    // ── Audio ──────────────────────────────────────────────────────────
    t.push_back({EffectType::FillLeftWithRight, "fill_left_with_right",
                 "Fill Left with Right", kAudio, F::Audio, nullptr, {}});
    t.push_back({EffectType::FillRightWithLeft, "fill_right_with_left",
                 "Fill Right with Left", kAudio, F::Audio, nullptr, {}});

    // ── Video ──────────────────────────────────────────────────────────
    t.push_back({EffectType::ColorCorrect, "color_correct", "Color Correct", kColorFx, 0,
                 "color_correct.comp.spv", {
        {"Brightness",  0.0f, -1.0f, 1.0f},
        {"Contrast",    1.0f,  0.0f, 3.0f},
        {"Saturation",  1.0f,  0.0f, 3.0f},
        {"Hue",         0.0f, -180.0f, 180.0f},
        {"Temperature", 0.0f, -1.0f, 1.0f},
        {"Tint",        0.0f, -1.0f, 1.0f},
        {"Gamma",       1.0f,  0.1f, 5.0f},
        {"Gain",        1.0f,  0.0f, 5.0f},
        {"Lift",        0.0f, -1.0f, 1.0f},
    }});
    {
        ParamDef repeat{"Repeat Edge Pixels", 1.0f, 0.0f, 1.0f, K::Toggle};
        repeat.widget = "blurRepeatEdgePixelsCheck";
        t.push_back({EffectType::Blur, "gaussian_blur", "Gaussian Blur", kBlurSharpen, 0,
                     "blur.comp.spv", {
            {"Radius", 15.0f, 0.0f, 100.0f},
            // Added later: older projects load only Radius and keep this
            // default (the historical clamp-to-edge look).
            repeat,
        }});
    }
    t.push_back({EffectType::Sharpen, "sharpen", "Sharpen", kBlurSharpen, 0,
                 "sharpen.comp.spv", {
        {"Amount",    1.0f, 0.0f, 5.0f},
        {"Radius",    1.0f, 0.1f, 10.0f},
        {"Threshold", 0.0f, 0.0f, 1.0f},
    }});
    t.push_back({EffectType::Glow, "glow", "Glow", kStylize, 0, "glow.comp.spv", {
        {"Threshold", 0.8f, 0.0f, 1.0f},
        {"Intensity", 1.0f, 0.0f, 5.0f},
        {"Radius",   10.0f, 1.0f, 50.0f},
    }});
    // Ultra Key: grouped UI of its own; the order matches the shader's
    // push constants.
    t.push_back({EffectType::ChromaKey, "ultra_key", "Ultra Key", kKeying, 0,
                 "chroma_key.comp.spv", {
        color("Key Color R", 0.0f, K::ColorR, "Key Color"),
        color("Key Color G", 1.0f, K::ColorG),
        color("Key Color B", 0.0f, K::ColorB),
        choice("Output", 0.0f, {"Composite", "Alpha Matte", "Color Channel",
                                "Original", "Removed Color", "Spill Map"}),
        choice("Setting", 0.0f, {"Default", "Relaxed", "Aggressive", "Custom",
                                 "ComfyUI Green"}),
        {"Transparency",   0.0f, 0.0f, 100.0f},
        {"Highlight",     10.0f, 0.0f, 100.0f},
        {"Shadow",        50.0f, 0.0f, 100.0f},
        {"Tolerance",     50.0f, 0.0f, 100.0f},
        {"Pedestal",      10.0f, 0.0f, 100.0f},
        {"Choke",          0.0f, 0.0f, 100.0f},
        {"Soften",         0.0f, 0.0f, 100.0f},
        {"Contrast",       0.0f, 0.0f, 100.0f},
        {"Mid Point",     50.0f, 0.0f, 100.0f},
        {"Desaturate",    25.0f, 0.0f, 50.0f},
        {"Range",         50.0f, 0.0f, 100.0f},
        {"Spill",         50.0f, 0.0f, 100.0f},
        {"Luma",          50.0f, 0.0f, 100.0f},
        {"Saturation",   100.0f, 0.0f, 200.0f},
        {"Hue",            0.0f, -180.0f, 180.0f},
        {"Luminance",    100.0f, 0.0f, 200.0f},
    }});
    t.push_back({EffectType::Transform2D, "transform_2d", "Transform 2D", kDistort, 0,
                 "transform2d.comp.spv", {
        {"Offset X",  0.0f, -2000.0f, 2000.0f},
        {"Offset Y",  0.0f, -2000.0f, 2000.0f},
        {"Scale",     1.0f,     0.01f,   10.0f},
        {"Rotation",  0.0f,  -360.0f,  360.0f},
        {"Anchor X",  0.5f,     0.0f,    1.0f},
        {"Anchor Y",  0.5f,     0.0f,    1.0f},
    }});
    t.push_back({EffectType::Vignette, "vignette", "Vignette", kStylize, 0,
                 "vignette.comp.spv", {
        {"Intensity",  1.0f, 0.0f, 2.0f},
        {"Roundness",  0.5f, 0.0f, 1.0f},
        {"Softness",   0.5f, 0.01f, 1.0f},
        {"Center X",   0.0f, -1.0f, 1.0f},
        {"Center Y",   0.0f, -1.0f, 1.0f},
    }});
    // LUT binds its 3D cube at binding 2 (own pipeline layout).
    t.push_back({EffectType::LUT, "lut", "LUT", kColorFx, 0, "lut.comp.spv", {
        {"Intensity", 1.0f, 0.0f, 1.0f},
    }});
    t.push_back({EffectType::Letterbox, "letterbox", "Letterbox", kTransform, 0,
                 "letterbox.comp.spv", {
        {"Aspect Width", 2.39f, 1.0f, 4.0f},
        {"Aspect Height", 1.0f, 1.0f, 4.0f},
        {"Bar Opacity",  1.0f,  0.0f, 1.0f},
        color("Bar R", 0.0f, K::ColorR, "Bar Color"),
        color("Bar G", 0.0f, K::ColorG),
        color("Bar B", 0.0f, K::ColorB),
        {"Feather",      0.0f,  0.0f, 0.1f},
    }});
    t.push_back({EffectType::ColorGrading, "color_grading", "Color Grading", kColorFx, 0,
                 "lumetri_color.comp.spv", colorGradingParams()});
    {
        ParamDef amount{"Amount to Tint", 100.0f, 0.0f, 100.0f};
        amount.decimals = 1;
        amount.step = 1.0;
        amount.suffix = " %";
        amount.widget = "tintAmountSpin";
        t.push_back({EffectType::Tint, "tint", "Tint", kColorFx, 0, "tint.comp.spv", {
            color("Map Black To R", 0.0f, K::ColorR, "Map Black To", "tintMapBlackButton"),
            color("Map Black To G", 0.0f, K::ColorG),
            color("Map Black To B", 0.0f, K::ColorB),
            color("Map White To R", 1.0f, K::ColorR, "Map White To", "tintMapWhiteButton"),
            color("Map White To G", 1.0f, K::ColorG),
            color("Map White To B", 1.0f, K::ColorB),
            amount,
        }});
    }
    // OTS defaults are then overridden from assets/presets/effects/*.json.
    t.push_back({EffectType::OtsLeft,  "ots_left",  "OTS LEFT",  kVideo, 0,
                 "ots.comp.spv", otsParams(0.0f)});
    t.push_back({EffectType::OtsRight, "ots_right", "OTS RIGHT", kVideo, 0,
                 "ots.comp.spv", otsParams(1.0f)});
    t.push_back({EffectType::OtsIntro, "ots_intro", "OTS INTRO", kVideo, 0,
                 "ots.comp.spv", otsParams(0.0f)});
    // Hologram: one-colour, see-through, glowing outline, rolling scanlines,
    // flicker and the odd glitch. Driven by clip-local time.
    t.push_back({EffectType::Hologram, "hologram", "Hologram", kVideo,
                 F::ProceduralTime, "hologram.comp.spv", {
        color("Color R", 0.25f, K::ColorR, "Color"),
        color("Color G", 0.85f, K::ColorG),
        color("Color B", 1.0f, K::ColorB),
        num("Opacity", 70.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Edge Glow", 60.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Edge Width", 6.0f, 1.0f, 40.0f, 1, 0.5, " px"),
        num("Scanlines", 40.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Line Spacing", 4.0f, 2.0f, 40.0f, 1, 0.5, " px"),
        num("Scroll Speed", 20.0f, -200.0f, 200.0f, 0, 1.0, " px/s"),
        num("Flicker", 15.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Glitch", 30.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Original Color", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Random Seed", 0.0f, 0.0f, 100.0f, 0, 1.0),
    }});
    // The axis is fixed by the type; it only rides along to the shader.
    t.push_back({EffectType::FlipHorizontal, "flip_horizontal", "Flip Horizontal",
                 kTransform, 0, "flip.comp.spv", {hidden("Axis", 0.0f, 0.0f, 1.0f)}});
    t.push_back({EffectType::FlipVertical, "flip_vertical", "Flip Vertical",
                 kTransform, 0, "flip.comp.spv", {hidden("Axis", 1.0f, 0.0f, 1.0f)}});

    // ── Glitch (procedural: driven by clip-local time) ─────────────────
    t.push_back({EffectType::Scanlines, "scanlines", "Scanlines", kGlitch,
                 F::ProceduralTime, "scanlines.comp.spv", {
        {"Intensity",  0.6f,   0.0f,   1.0f},
        {"Line Count", 400.0f, 1.0f,   2000.0f},
        {"Roll",       0.2f,  -2.0f,   2.0f},
        {"Interlace",  0.0f,   0.0f,   1.0f},
        color("Tint R", 0.0f, K::ColorR, "Tint"),
        color("Tint G", 0.0f, K::ColorG),
        color("Tint B", 0.0f, K::ColorB),
        {"Flicker",    0.0f,   0.0f,   1.0f},
    }});
    t.push_back({EffectType::BlockGlitch, "block_glitch", "Block Glitch", kGlitch,
                 F::ProceduralTime, "block_glitch.comp.spv", {
        {"Intensity",  0.5f,  0.0f,   1.0f},
        {"Block Size", 32.0f, 2.0f,   256.0f},
        {"Speed",      4.0f,  0.0f,   10.0f},
        {"Smear",      0.3f,  0.0f,   1.0f},
        {"Seed",       17.0f, 0.0f,   100.0f},
    }});
    t.push_back({EffectType::ChromaticSplit, "chromatic_split", "Chromatic Split",
                 kGlitch, F::ProceduralTime, "chromatic_split.comp.spv", {
        {"Intensity", 0.5f,  0.0f,   1.0f},
        {"Amount",    6.0f,  0.0f,   50.0f},
        {"Angle",     0.0f,  0.0f,   360.0f},
        {"Jitter",    0.0f,  0.0f,   1.0f},
        {"Speed",     6.0f,  0.0f,   20.0f},
    }});
    t.push_back({EffectType::TurbulentDisplace, "turbulent_displace",
                 "Turbulent Displace", kGlitch, F::ProceduralTime,
                 "turbulent_displace.comp.spv", {
        {"Intensity",  0.5f, 0.0f, 1.0f},
        {"Scale",      4.0f, 0.5f, 20.0f},
        {"Speed",      1.0f, 0.0f, 5.0f},
        {"Complexity", 2.0f, 1.0f, 5.0f},
    }});
    t.push_back({EffectType::Posterize, "posterize", "Posterize / Banding", kGlitch,
                 F::ProceduralTime, "posterize.comp.spv", {
        {"Intensity",   1.0f, 0.0f, 1.0f},
        {"Levels",      8.0f, 2.0f, 64.0f},
        {"Block Quant", 0.0f, 0.0f, 1.0f},
    }});
    t.push_back({EffectType::Grain, "grain", "Grain / Noise", kGlitch,
                 F::ProceduralTime, "grain.comp.spv", {
        {"Intensity", 0.4f, 0.0f, 1.0f},
        {"Size",      1.0f, 1.0f, 8.0f},
        {"Color",     0.0f, 0.0f, 1.0f},
    }});
    t.push_back({EffectType::SignalTear, "signal_tear", "Signal Tear", kGlitch,
                 F::ProceduralTime, "signal_tear.comp.spv", {
        {"Intensity",   0.6f, 0.0f, 1.0f},
        {"Tear Amount", 0.5f, 0.0f, 1.0f},
        {"Frequency",  10.0f, 1.0f, 50.0f},
        {"Mirror",      0.2f, 0.0f, 1.0f},
        {"Seed",        7.0f, 0.0f, 100.0f},
    }});

    // ── Beat / Music (pulse envelope from tempo or detected onsets) ────
    t.push_back({EffectType::BeatZoom, "beat_zoom", "Beat Zoom Punch", kBeat,
                 F::BeatReact, "beat_zoom.comp.spv", beatParams(0.06f, 0.5f)});
    t.push_back({EffectType::BeatFlash, "beat_flash", "Beat Flash", kBeat,
                 F::BeatReact, "beat_flash.comp.spv", beatParams(0.6f, 2.0f)});
    t.push_back({EffectType::BeatShake, "beat_shake", "Beat Shake", kBeat,
                 F::BeatReact, "beat_shake.comp.spv", beatParams(20.0f, 150.0f)});
    t.push_back({EffectType::BeatChroma, "beat_chroma", "Beat Chromatic Kick", kBeat,
                 F::BeatReact, "beat_chroma.comp.spv", beatParams(12.0f, 60.0f)});
    t.push_back({EffectType::BeatDrop, "beat_drop", "Beat Drop", kBeat,
                 F::BeatReact, "beat_drop.comp.spv", beatParams(1.0f, 2.0f)});

    // ── Premiere-parity batch (2026-11) ────────────────────────────────
    // Stylize
    t.push_back({EffectType::LensChromaticAberration, "lens_chromatic_aberration",
                 "Chromatic Aberration", kStylize, 0, "lens_chromatic.comp.spv", {
        num("Amount", 8.0f, 0.0f, 100.0f, 1, 0.5, " px"),
        num("Center X", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Center Y", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Falloff", 2.0f, 1.0f, 4.0f, 2, 0.05),
        num("Smoothness", 0.5f, 0.0f, 1.0f, 2, 0.01),
    }});
    t.push_back({EffectType::StrobeLight, "strobe_light", "Strobe Light", kStylize,
                 F::ProceduralTime, "strobe.comp.spv", {
        color("Strobe Color R", 1.0f, K::ColorR, "Strobe Color"),
        color("Strobe Color G", 1.0f, K::ColorG),
        color("Strobe Color B", 1.0f, K::ColorB),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Strobe Duration", 0.1f, 0.01f, 5.0f, 2, 0.01, " s"),
        num("Strobe Period", 1.0f, 0.02f, 10.0f, 2, 0.01, " s"),
        num("Random Strobe Probability", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        choice("Strobe", 0.0f, {"Operates On Color", "Makes Layer Transparent", "Invert"}),
        num("Random Seed", 0.0f, 0.0f, 100.0f, 0, 1.0),
    }});
    t.push_back({EffectType::Mosaic, "mosaic", "Mosaic", kStylize, 0, "mosaic.comp.spv", {
        num("Horizontal Blocks", 10.0f, 1.0f, 400.0f, 0, 1.0),
        num("Vertical Blocks", 10.0f, 1.0f, 400.0f, 0, 1.0),
        toggle("Sharp Colors", 0.0f),
    }});
    t.push_back({EffectType::FindEdges, "find_edges", "Find Edges", kStylize, 0,
                 "find_edges.comp.spv", {
        toggle("Invert", 0.0f),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});
    t.push_back({EffectType::Emboss, "emboss", "Emboss", kStylize, 0, "emboss.comp.spv", {
        num("Direction", 45.0f, -360.0f, 360.0f, 1, 1.0, "°"),
        num("Relief", 2.0f, 0.0f, 10.0f, 1, 0.1, " px"),
        num("Contrast", 200.0f, 0.0f, 500.0f, 0, 1.0, " %"),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        toggle("Color Emboss", 0.0f),
    }});
    t.push_back({EffectType::Threshold, "threshold", "Threshold", kStylize, 0,
                 "threshold.comp.spv", {
        hidden("Mode", 0.0f, 0.0f, 0.0f),
        num("Level", 128.0f, 0.0f, 255.0f, 0, 1.0),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});
    t.push_back({EffectType::Solarize, "solarize", "Solarize", kStylize, 0,
                 "threshold.comp.spv", {
        hidden("Mode", 1.0f, 1.0f, 1.0f),
        num("Threshold", 128.0f, 0.0f, 255.0f, 0, 1.0),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});
    t.push_back({EffectType::Replicate, "replicate", "Replicate", kStylize, 0, "warp.comp.spv", {
        hidden("Mode", 6.0f, 6.0f, 6.0f),
        num("Count", 2.0f, 1.0f, 16.0f, 0, 1.0),
    }});

    // Color Correction
    t.push_back({EffectType::Invert, "invert", "Invert", kColorFx, 0, "color_simple.comp.spv", {
        hidden("Mode", 0.0f, 0.0f, 0.0f),
        choice("Channel", 0.0f, {"RGB", "Red", "Green", "Blue", "Alpha", "Luminance"}),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});
    t.push_back({EffectType::BlackAndWhite, "black_and_white", "Black & White", kColorFx, 0,
                 "color_simple.comp.spv", {
        hidden("Mode", 1.0f, 1.0f, 1.0f),
        hidden("Unused", 0.0f, 0.0f, 1.0f),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});
    t.push_back({EffectType::LeaveColor, "leave_color", "Leave Color", kColorFx, 0,
                 "color_simple.comp.spv", {
        hidden("Mode", 2.0f, 2.0f, 2.0f),
        num("Amount to Decolor", 100.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        hidden("Unused", 0.0f, 0.0f, 1.0f),
        color("Color To Leave R", 1.0f, K::ColorR, "Color To Leave"),
        color("Color To Leave G", 0.0f, K::ColorG),
        color("Color To Leave B", 0.0f, K::ColorB),
        num("Tolerance", 15.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Edge Softness", 10.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        choice("Match Colors", 1.0f, {"Using RGB", "Using Hue"}),
    }});

    // Distort
    t.push_back({EffectType::Twirl, "twirl", "Twirl", kDistort, 0, "warp.comp.spv", {
        hidden("Mode", 0.0f, 0.0f, 0.0f),
        num("Angle", 90.0f, -1440.0f, 1440.0f, 1, 1.0, "°"),
        num("Twirl Radius", 50.0f, 0.0f, 100.0f, 1, 0.5, " %"),
        num("Center X", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Center Y", 0.0f, -1.0f, 1.0f, 2, 0.01),
    }});
    t.push_back({EffectType::Spherize, "spherize", "Spherize", kDistort, 0, "warp.comp.spv", {
        hidden("Mode", 1.0f, 1.0f, 1.0f),
        num("Amount", 50.0f, -100.0f, 100.0f, 0, 1.0),
        num("Radius", 50.0f, 0.0f, 100.0f, 1, 0.5, " %"),
        num("Center X", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Center Y", 0.0f, -1.0f, 1.0f, 2, 0.01),
    }});
    t.push_back({EffectType::WaveWarp, "wave_warp", "Wave Warp", kDistort,
                 F::ProceduralTime, "warp.comp.spv", {
        hidden("Mode", 2.0f, 2.0f, 2.0f),
        choice("Wave Type", 0.0f, {"Sine", "Square", "Triangle", "Sawtooth"}),
        num("Wave Height", 10.0f, 0.0f, 500.0f, 0, 1.0, " px"),
        num("Wave Width", 40.0f, 1.0f, 2000.0f, 0, 1.0, " px"),
        num("Direction", 90.0f, -360.0f, 360.0f, 1, 1.0, "°"),
        num("Wave Speed", 1.0f, -10.0f, 10.0f, 2, 0.05, " /s"),
        num("Phase", 0.0f, -360.0f, 360.0f, 1, 1.0, "°"),
    }});
    t.push_back({EffectType::LensDistortion, "lens_distortion", "Lens Distortion", kDistort, 0,
                 "warp.comp.spv", {
        hidden("Mode", 3.0f, 3.0f, 3.0f),
        num("Curvature", 0.0f, -100.0f, 100.0f, 0, 1.0),
        num("Vertical Decentering", 0.0f, -100.0f, 100.0f, 0, 1.0),
        num("Horizontal Decentering", 0.0f, -100.0f, 100.0f, 0, 1.0),
        toggle("Fill Alpha", 1.0f),
    }});
    t.push_back({EffectType::Mirror, "mirror", "Mirror", kDistort, 0, "warp.comp.spv", {
        hidden("Mode", 4.0f, 4.0f, 4.0f),
        num("Reflection Center X", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Reflection Center Y", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Reflection Angle", 0.0f, -360.0f, 360.0f, 1, 1.0, "°"),
    }});
    t.push_back({EffectType::Offset, "offset", "Offset", kDistort, 0, "warp.comp.spv", {
        hidden("Mode", 5.0f, 5.0f, 5.0f),
        num("Shift X", 0.0f, -100.0f, 100.0f, 1, 0.5, " %"),
        num("Shift Y", 0.0f, -100.0f, 100.0f, 1, 0.5, " %"),
        num("Blend With Original", 0.0f, 0.0f, 100.0f, 0, 1.0, " %"),
    }});

    // Blur & Sharpen
    t.push_back({EffectType::DirectionalBlur, "directional_blur", "Directional Blur",
                 kBlurSharpen, 0, "motion_blur.comp.spv", {
        hidden("Mode", 0.0f, 0.0f, 0.0f),
        num("Direction", 0.0f, -360.0f, 360.0f, 1, 1.0, "°"),
        num("Blur Length", 10.0f, 0.0f, 500.0f, 1, 0.5, " px"),
    }});
    t.push_back({EffectType::RadialBlur, "radial_blur", "Radial Blur", kBlurSharpen, 0,
                 "motion_blur.comp.spv", {
        hidden("Mode", 1.0f, 1.0f, 1.0f),
        num("Amount", 10.0f, 0.0f, 100.0f, 0, 1.0),
        choice("Blur Method", 1.0f, {"Spin", "Zoom"}),
        num("Center X", 0.0f, -1.0f, 1.0f, 2, 0.01),
        num("Center Y", 0.0f, -1.0f, 1.0f, 2, 0.01),
    }});

    // Time
    t.push_back({EffectType::PosterizeTime, "posterize_time", "Posterize Time", kTime,
                 F::TimeOnly, nullptr, {
        num("Frame Rate", 12.0f, 1.0f, 60.0f, 2, 0.5, " fps"),
    }});
    t.push_back({EffectType::Echo, "echo", "Echo", kTime, F::TimeOnly, nullptr, {
        num("Echo Time", -0.033f, -5.0f, 5.0f, 3, 0.001, " s"),
        num("Number Of Echoes", 1.0f, 0.0f, 16.0f, 0, 1.0),
        num("Starting Intensity", 1.0f, 0.0f, 1.0f, 2, 0.01),
        num("Decay", 1.0f, 0.0f, 1.0f, 2, 0.01),
        choice("Echo Operator", 0.0f, {"Add", "Maximum", "Minimum", "Screen",
                                       "Composite In Back", "Composite In Front", "Blend"}),
    }});

    // Perspective
    t.push_back({EffectType::DropShadow, "drop_shadow", "Drop Shadow", kPerspective, 0,
                 "drop_shadow.comp.spv", {
        color("Shadow Color R", 0.0f, K::ColorR, "Shadow Color"),
        color("Shadow Color G", 0.0f, K::ColorG),
        color("Shadow Color B", 0.0f, K::ColorB),
        num("Opacity", 50.0f, 0.0f, 100.0f, 0, 1.0, " %"),
        num("Direction", 135.0f, -360.0f, 360.0f, 1, 1.0, "°"),
        num("Distance", 10.0f, 0.0f, 500.0f, 1, 0.5, " px"),
        num("Softness", 10.0f, 0.0f, 200.0f, 1, 0.5, " px"),
        toggle("Shadow Only", 0.0f),
    }});

    // ── Legacy ─────────────────────────────────────────────────────────
    // Old projects' Lumetri Color loads as Color Grading.
    t.push_back({EffectType::LumetriColor, "lumetri_color", "Color Grading (legacy)",
                 nullptr, F::Hidden, "lumetri_color.comp.spv", colorGradingParams()});
    return t;
}

const EffectDef& unknownDef()
{
    static const EffectDef def{EffectType::Count, "unknown", "Unknown", nullptr,
                               F::Hidden, nullptr, {}};
    return def;
}

} // namespace

const std::vector<EffectDef>& allEffectDefs()
{
    static const std::vector<EffectDef> table = buildTable();
    return table;
}

const EffectDef& effectDef(EffectType type)
{
    static const auto index = [] {
        std::array<const EffectDef*, static_cast<size_t>(EffectType::Count)> byType{};
        for (const auto& def : allEffectDefs())
            byType[static_cast<size_t>(def.type)] = &def;
        return byType;
    }();
    const auto i = static_cast<size_t>(type);
    if (i < index.size() && index[i])
        return *index[i];
    return unknownDef();
}

const char* effectTypeName(EffectType t) noexcept
{
    return effectDef(t).name;
}

bool isAudioEffect(EffectType t) noexcept
{
    return effectDef(t).has(EffectFlag::Audio);
}

bool isProceduralTimeEffect(EffectType t) noexcept
{
    return effectDef(t).has(EffectFlag::ProceduralTime);
}

bool isBeatReactEffect(EffectType t) noexcept
{
    return effectDef(t).has(EffectFlag::BeatReact);
}

} // namespace rt
