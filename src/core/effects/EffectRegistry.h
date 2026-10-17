/*
 * EffectRegistry — every effect, defined once.
 *
 * One EffectDef per EffectType holds the effect's name, browser category,
 * flags, compute shader and parameter list (name, default, range and how
 * Effect Controls shows it). The factory, clone, names, the Effects panel,
 * the GPU pipeline lookup and the generic Effect Controls rows all read it.
 *
 * Adding an effect: append to EffectType (Effect.h), add one entry to the
 * table in EffectRegistry.cpp, write its shader. Parameters are saved by
 * INDEX, so an existing effect's list may only ever be appended to.
 */

#pragma once

#include "effects/Effect.h"

#include <cstdint>
#include <vector>

namespace rt {

/// How Effect Controls shows a parameter. The value is always one float.
enum class ParamKind : uint8_t
{
    Number,   ///< scrubby number
    Toggle,   ///< checkbox: 0 or 1
    Choice,   ///< combo box: the value is an index into `choices`
    ColorR,   ///< first of three consecutive params shown as one colour
    ColorG,
    ColorB,
    Hidden    ///< passed to the shader, never shown (e.g. Flip's axis)
};

struct ParamDef
{
    const char* name;
    float def;
    float min;
    float max;
    ParamKind kind{ParamKind::Number};
    /// ColorR: the colour row's label (e.g. "Map Black To").
    const char* group{nullptr};
    /// Choice: one label per value 0..n-1.
    std::vector<const char*> choices{};
    int decimals{2};
    double step{0.01};
    const char* suffix{nullptr};
    /// Object name of the control (automation / UI tests); optional.
    const char* widget{nullptr};
};

namespace EffectFlag {
inline constexpr uint32_t Audio          = 1u << 0;  ///< audio effect, no shader
inline constexpr uint32_t ProceduralTime = 1u << 1;  ///< gets clip-local seconds
inline constexpr uint32_t BeatReact      = 1u << 2;  ///< gets the beat pulse too
inline constexpr uint32_t Hidden         = 1u << 3;  ///< not offered in the browser
}

struct EffectDef
{
    EffectType type;
    const char* key;            ///< stable identifier, e.g. "gaussian_blur"
    const char* name;           ///< display name
    const char* category;       ///< Effects panel folder; null = not listed
    uint32_t flags{0};
    const char* shader{nullptr};///< compute shader (.spv); null = none
    std::vector<ParamDef> params;

    [[nodiscard]] bool has(uint32_t flag) const noexcept { return (flags & flag) != 0; }
};

/// The definition of `type` (every type below EffectType::Count has one).
[[nodiscard]] const EffectDef& effectDef(EffectType type);

/// All definitions, in Effects panel order.
[[nodiscard]] const std::vector<EffectDef>& allEffectDefs();

} // namespace rt
