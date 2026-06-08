/*
 * GlitchEffects — procedural, time-driven "glitch" building blocks.
 *
 * Each effect is a single GPU compute pass that scrambles content *inside*
 * the frame only: shaders sample with clamped UV and preserve the source
 * alpha, so the clip rectangle, crop, and borders never change.
 *
 * All animate off a continuous clock — EffectStack::evaluate() injects
 * clip-local seconds at kGlitchTimeSlot — so they adapt to any clip length
 * with no authored keyframes (every param is still keyframeable).
 *
 * These are the draggable building blocks; curated one-click combos are
 * assembled from them as preset macros in the Effects panel.
 */

#pragma once

#include "effects/Effect.h"

#include <vector>

namespace rt {

// ── Glitch presets ──────────────────────────────────────────────────────────
// One-click curated combos. Each preset is just a stack of the building blocks
// below with tuned defaults, so the user can drop one and then tweak or remove
// the individual pieces.
enum class GlitchPreset : int {
    Datamosh = 0,   // hard blocky tears + RGB jitter + grain
    Compression,    // posterize/banding + block quant + mild split
    VHS,            // rolling scanlines + colour bleed + grain + wobble
    Chromatic,      // clean RGB split
    Hologram,       // tinted scanlines + warp + split + flicker grain
    SignalTear,     // horizontal sync tears + mirror flashes + grain
    Count
};

/// Human-readable preset name (for the effects browser).
[[nodiscard]] const char* glitchPresetName(GlitchPreset p) noexcept;

/// Build the curated, pre-configured building-block effects for a preset.
/// Returns them in stack order (index 0 applied first).
[[nodiscard]] std::vector<std::unique_ptr<Effect>> buildGlitchPreset(GlitchPreset p);

// ── Scanlines ───────────────────────────────────────────────────────────────
class Scanlines : public Effect
{
public:
    Scanlines();
    ~Scanlines() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   line darkening strength
        LineCount,      // 1–2000 scanlines down the frame
        Roll,           // -2–2  vertical roll speed
        Interlace,      // 0–1   per-row interlace flicker
        TintR,          // 0–1   additive tint (e.g. hologram cyan)
        TintG,          // 0–1
        TintB,          // 0–1
        Flicker,        // 0–1   global brightness flicker
        ParamCount
    };
};

// ── Block Glitch (datamosh) ─────────────────────────────────────────────────
class BlockGlitch : public Effect
{
public:
    BlockGlitch();
    ~BlockGlitch() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   fraction of blocks that glitch
        BlockSize,      // 2–256 px block grid
        Speed,          // 0–10  how often blocks re-roll (Hz)
        Smear,          // 0–1   horizontal datamosh smear
        Seed,           // 0–100 random seed
        ParamCount
    };
};

// ── Chromatic Split ─────────────────────────────────────────────────────────
class ChromaticSplit : public Effect
{
public:
    ChromaticSplit();
    ~ChromaticSplit() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   overall strength
        Amount,         // 0–50  channel offset in px
        Angle,          // 0–360 split direction
        Jitter,         // 0–1   pulsed random jitter
        Speed,          // 0–20  jitter rate (Hz)
        ParamCount
    };
};

// ── Turbulent Displace ──────────────────────────────────────────────────────
class TurbulentDisplace : public Effect
{
public:
    TurbulentDisplace();
    ~TurbulentDisplace() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   displacement strength
        Scale,          // 0.5–20 noise frequency
        Speed,          // 0–5   evolution speed
        Complexity,     // 1–5   fbm octaves
        ParamCount
    };
};

// ── Posterize / Banding ─────────────────────────────────────────────────────
class Posterize : public Effect
{
public:
    Posterize();
    ~Posterize() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   blend toward quantized
        Levels,         // 2–64  colour levels per channel
        BlockQuant,     // 0–1   8×8 block averaging (compression look)
        ParamCount
    };
};

// ── Grain / Noise ───────────────────────────────────────────────────────────
class Grain : public Effect
{
public:
    Grain();
    ~Grain() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   noise amount
        Size,           // 1–8   grain pixel size
        Color,          // 0–1   0 = monochrome, 1 = RGB
        ParamCount
    };
};

// ── Signal Tear ─────────────────────────────────────────────────────────────
class SignalTear : public Effect
{
public:
    SignalTear();
    ~SignalTear() override = default;
    [[nodiscard]] std::unique_ptr<Effect> clone() const override;

    enum Param : size_t {
        Intensity = 0,  // 0–1   fraction of bands that tear
        TearAmount,     // 0–1   max horizontal displacement
        Frequency,      // 1–50  number of horizontal bands
        Mirror,         // 0–1   chance a torn band is mirrored
        Seed,           // 0–100 random seed
        ParamCount
    };
};

} // namespace rt
