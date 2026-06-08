/*
 * GlitchEffects.cpp — constructors (param registration) + clone() for the
 * procedural glitch building blocks. Grouped into one translation unit like
 * SimpleEffects.cpp to keep build overhead down.
 */

#include "effects/GlitchEffects.h"

#include <initializer_list>
#include <utility>

namespace rt {

// Boilerplate clone(): copy enabled flag + every param track.
#define RT_GLITCH_CLONE(Klass)                                   \
    std::unique_ptr<Effect> Klass::clone() const                 \
    {                                                            \
        auto copy = std::make_unique<Klass>();                   \
        copy->m_enabled = m_enabled;                             \
        for (size_t i = 0; i < m_params.size(); ++i)             \
            copy->m_params[i].track = m_params[i].track;         \
        return copy;                                             \
    }

// ── Scanlines ───────────────────────────────────────────────────────────────
Scanlines::Scanlines()
    : Effect(EffectType::Scanlines)
{
    addParam("Intensity",  0.6f,   0.0f,   1.0f);
    addParam("Line Count", 400.0f, 1.0f,   2000.0f);
    addParam("Roll",       0.2f,  -2.0f,   2.0f);
    addParam("Interlace",  0.0f,   0.0f,   1.0f);
    addParam("Tint R",     0.0f,   0.0f,   1.0f);
    addParam("Tint G",     0.0f,   0.0f,   1.0f);
    addParam("Tint B",     0.0f,   0.0f,   1.0f);
    addParam("Flicker",    0.0f,   0.0f,   1.0f);
}
RT_GLITCH_CLONE(Scanlines)

// ── Block Glitch ────────────────────────────────────────────────────────────
BlockGlitch::BlockGlitch()
    : Effect(EffectType::BlockGlitch)
{
    addParam("Intensity",  0.5f,  0.0f,   1.0f);
    addParam("Block Size", 32.0f, 2.0f,   256.0f);
    addParam("Speed",      4.0f,  0.0f,   10.0f);
    addParam("Smear",      0.3f,  0.0f,   1.0f);
    addParam("Seed",       17.0f, 0.0f,   100.0f);
}
RT_GLITCH_CLONE(BlockGlitch)

// ── Chromatic Split ─────────────────────────────────────────────────────────
ChromaticSplit::ChromaticSplit()
    : Effect(EffectType::ChromaticSplit)
{
    addParam("Intensity", 0.5f,  0.0f,   1.0f);
    addParam("Amount",    6.0f,  0.0f,   50.0f);
    addParam("Angle",     0.0f,  0.0f,   360.0f);
    addParam("Jitter",    0.0f,  0.0f,   1.0f);
    addParam("Speed",     6.0f,  0.0f,   20.0f);
}
RT_GLITCH_CLONE(ChromaticSplit)

// ── Turbulent Displace ──────────────────────────────────────────────────────
TurbulentDisplace::TurbulentDisplace()
    : Effect(EffectType::TurbulentDisplace)
{
    addParam("Intensity",  0.5f, 0.0f,  1.0f);
    addParam("Scale",      4.0f, 0.5f,  20.0f);
    addParam("Speed",      1.0f, 0.0f,  5.0f);
    addParam("Complexity", 2.0f, 1.0f,  5.0f);
}
RT_GLITCH_CLONE(TurbulentDisplace)

// ── Posterize / Banding ─────────────────────────────────────────────────────
Posterize::Posterize()
    : Effect(EffectType::Posterize)
{
    addParam("Intensity",   1.0f, 0.0f,  1.0f);
    addParam("Levels",      8.0f, 2.0f,  64.0f);
    addParam("Block Quant", 0.0f, 0.0f,  1.0f);
}
RT_GLITCH_CLONE(Posterize)

// ── Grain / Noise ───────────────────────────────────────────────────────────
Grain::Grain()
    : Effect(EffectType::Grain)
{
    addParam("Intensity", 0.4f, 0.0f, 1.0f);
    addParam("Size",      1.0f, 1.0f, 8.0f);
    addParam("Color",     0.0f, 0.0f, 1.0f);
}
RT_GLITCH_CLONE(Grain)

// ── Signal Tear ─────────────────────────────────────────────────────────────
SignalTear::SignalTear()
    : Effect(EffectType::SignalTear)
{
    addParam("Intensity",   0.6f,  0.0f,  1.0f);
    addParam("Tear Amount", 0.5f,  0.0f,  1.0f);
    addParam("Frequency",   10.0f, 1.0f,  50.0f);
    addParam("Mirror",      0.2f,  0.0f,  1.0f);
    addParam("Seed",        7.0f,  0.0f,  100.0f);
}
RT_GLITCH_CLONE(SignalTear)

#undef RT_GLITCH_CLONE

// ── Presets ─────────────────────────────────────────────────────────────────

const char* glitchPresetName(GlitchPreset p) noexcept
{
    switch (p) {
    case GlitchPreset::Datamosh:    return "Datamosh";
    case GlitchPreset::Compression: return "Compression";
    case GlitchPreset::VHS:         return "VHS";
    case GlitchPreset::Chromatic:   return "Chromatic";
    case GlitchPreset::Hologram:    return "Hologram";
    case GlitchPreset::SignalTear:  return "Signal Tear";
    default:                        return "Glitch Preset";
    }
}

namespace {
// Make a building block and override the listed param defaults (index → value).
std::unique_ptr<Effect> mk(EffectType type,
                           std::initializer_list<std::pair<size_t, float>> vals)
{
    auto e = createEffect(type);
    for (const auto& [idx, value] : vals)
        if (idx < e->paramCount())
            e->param(idx).track.setDefaultValue(value);
    return e;
}
} // namespace

std::vector<std::unique_ptr<Effect>> buildGlitchPreset(GlitchPreset p)
{
    using S  = Scanlines::Param;
    using B  = BlockGlitch::Param;
    using C  = ChromaticSplit::Param;
    using T  = TurbulentDisplace::Param;
    using PO = Posterize::Param;
    using G  = Grain::Param;
    using ST = SignalTear::Param;

    std::vector<std::unique_ptr<Effect>> fx;
    switch (p) {
    case GlitchPreset::Datamosh:
        fx.push_back(mk(EffectType::BlockGlitch,
            {{B::Intensity, 0.6f}, {B::BlockSize, 24.0f}, {B::Speed, 6.0f}, {B::Smear, 0.5f}}));
        fx.push_back(mk(EffectType::ChromaticSplit,
            {{C::Intensity, 0.5f}, {C::Amount, 8.0f}, {C::Jitter, 0.6f}, {C::Speed, 10.0f}}));
        fx.push_back(mk(EffectType::Grain, {{G::Intensity, 0.25f}}));
        break;
    case GlitchPreset::Compression:
        fx.push_back(mk(EffectType::Posterize,
            {{PO::Intensity, 0.9f}, {PO::Levels, 6.0f}, {PO::BlockQuant, 0.7f}}));
        fx.push_back(mk(EffectType::BlockGlitch,
            {{B::Intensity, 0.25f}, {B::BlockSize, 16.0f}, {B::Speed, 2.0f}, {B::Smear, 0.1f}}));
        fx.push_back(mk(EffectType::ChromaticSplit, {{C::Intensity, 0.3f}, {C::Amount, 3.0f}}));
        break;
    case GlitchPreset::VHS:
        fx.push_back(mk(EffectType::Scanlines,
            {{S::Intensity, 0.5f}, {S::LineCount, 480.0f}, {S::Roll, 0.4f},
             {S::Interlace, 0.3f}, {S::Flicker, 0.15f}}));
        fx.push_back(mk(EffectType::ChromaticSplit, {{C::Intensity, 0.4f}, {C::Amount, 4.0f}}));
        fx.push_back(mk(EffectType::Grain, {{G::Intensity, 0.3f}}));
        fx.push_back(mk(EffectType::TurbulentDisplace,
            {{T::Intensity, 0.15f}, {T::Scale, 3.0f}, {T::Speed, 0.6f}}));
        break;
    case GlitchPreset::Chromatic:
        fx.push_back(mk(EffectType::ChromaticSplit,
            {{C::Intensity, 0.7f}, {C::Amount, 10.0f}, {C::Jitter, 0.2f}, {C::Speed, 6.0f}}));
        break;
    case GlitchPreset::Hologram:
        fx.push_back(mk(EffectType::Scanlines,
            {{S::Intensity, 0.5f}, {S::LineCount, 360.0f}, {S::Roll, 0.6f}, {S::Interlace, 0.5f},
             {S::TintR, 0.0f}, {S::TintG, 0.6f}, {S::TintB, 0.8f}, {S::Flicker, 0.2f}}));
        fx.push_back(mk(EffectType::TurbulentDisplace,
            {{T::Intensity, 0.2f}, {T::Scale, 5.0f}, {T::Speed, 1.0f}}));
        fx.push_back(mk(EffectType::ChromaticSplit, {{C::Intensity, 0.3f}, {C::Amount, 4.0f}}));
        fx.push_back(mk(EffectType::Grain, {{G::Intensity, 0.15f}}));
        break;
    case GlitchPreset::SignalTear:
        fx.push_back(mk(EffectType::SignalTear,
            {{ST::Intensity, 0.6f}, {ST::TearAmount, 0.5f}, {ST::Frequency, 12.0f}, {ST::Mirror, 0.25f}}));
        fx.push_back(mk(EffectType::Grain, {{G::Intensity, 0.2f}}));
        fx.push_back(mk(EffectType::ChromaticSplit,
            {{C::Intensity, 0.3f}, {C::Amount, 5.0f}, {C::Jitter, 0.4f}, {C::Speed, 8.0f}}));
        break;
    default:
        break;
    }
    return fx;
}

} // namespace rt
