/*
 * GlitchEffects.cpp — glitch presets: curated stacks of the procedural
 * building blocks (whose parameters are defined in EffectRegistry.cpp).
 */

#include "effects/GlitchEffects.h"

#include <initializer_list>
#include <utility>

namespace rt {

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
