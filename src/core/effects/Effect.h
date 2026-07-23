/*
 * Effect — base class for all GPU-accelerated effects.
 *
 * Each effect has:
 *  - A unique type ID and human-readable name
 *  - A set of keyframeable parameters (KeyframeTrack<float>)
 *  - Enable/disable toggle
 *  - Serialization support via parameter map
 *
 * Effects are processed by EffectProcessor (GPU compute shaders).
 * The CPU-side Effect objects define parameters; the GPU side executes them.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/OpacityMask.h"

namespace rt {

// ── Effect type identifiers ─────────────────────────────────────────────────

enum class EffectType : uint8_t
{
    ColorCorrect,
    Blur,
    Sharpen,
    Glow,
    ChromaKey,
    Transform2D,
    Vignette,
    LUT,
    Letterbox,
    ColorGrading,
    LumetriColor,
    OtsLeft,
    OtsRight,
    // Audio effects
    FillLeftWithRight,
    FillRightWithLeft,
    // New video effects appended here so existing serialized projects
    // (effect type stored as a raw uint8) keep their numbering.
    FlipHorizontal,
    FlipVertical,
    // Glitch building blocks — procedural and time-driven. EffectStack injects
    // clip-local seconds at params[kGlitchTimeSlot]; see isProceduralTimeEffect.
    Scanlines,
    BlockGlitch,
    ChromaticSplit,
    TurbulentDisplace,
    Posterize,
    Grain,
    SignalTear,
    // Beat-reactive effects (music videos) — pulse on a tempo/onset clock.
    // EffectStack injects the pulse envelope at params[kBeatPulseSlot].
    BeatZoom,
    BeatFlash,
    BeatShake,
    BeatChroma,
    BeatDrop,
    // Appended at the END to preserve serialized uint8 numbering. Third OTS
    // ("over the shoulder") variant — an intro-graphic preset with its own
    // OTS_INTRO.json defaults; identical shader/params to OtsLeft/OtsRight.
    OtsIntro,
    // Premiere-style black/white luminance color mapping. Appended to keep
    // every existing serialized uint8 effect identifier stable.
    Tint,
    Count
};

/// Shader param slot that EffectStack fills with clip-local seconds for
/// procedural effects (the glitch/beat family). Fixed so it is independent
/// of an effect's user-param count. Shaders read pc.params[15].
inline constexpr size_t kGlitchTimeSlot = 15;

/// Shader param slot that EffectStack fills with the 0–1 beat-pulse envelope
/// for beat-reactive effects. Shaders read pc.params[14].
inline constexpr size_t kBeatPulseSlot = 14;

/// Human-readable name for an effect type
inline const char* effectTypeName(EffectType t) noexcept
{
    switch (t) {
    case EffectType::ColorCorrect: return "Color Correct";
    case EffectType::Blur:         return "Gaussian Blur";
    case EffectType::Sharpen:      return "Sharpen";
    case EffectType::Glow:         return "Glow";
    case EffectType::ChromaKey:    return "Ultra Key";
    case EffectType::Transform2D:  return "Transform 2D";
    case EffectType::Vignette:     return "Vignette";
    case EffectType::LUT:          return "LUT";
    case EffectType::Letterbox:    return "Letterbox";
    case EffectType::ColorGrading: return "Color Grading";
    case EffectType::LumetriColor: return "Color Grading (legacy)";
    case EffectType::OtsLeft:      return "OTS LEFT";
    case EffectType::OtsRight:     return "OTS RIGHT";
    case EffectType::OtsIntro:     return "OTS INTRO";
    case EffectType::Tint:         return "Tint";
    case EffectType::FlipHorizontal: return "Flip Horizontal";
    case EffectType::FlipVertical:   return "Flip Vertical";
    case EffectType::Scanlines:        return "Scanlines";
    case EffectType::BlockGlitch:      return "Block Glitch";
    case EffectType::ChromaticSplit:   return "Chromatic Split";
    case EffectType::TurbulentDisplace: return "Turbulent Displace";
    case EffectType::Posterize:        return "Posterize / Banding";
    case EffectType::Grain:            return "Grain / Noise";
    case EffectType::SignalTear:       return "Signal Tear";
    case EffectType::BeatZoom:         return "Beat Zoom Punch";
    case EffectType::BeatFlash:        return "Beat Flash";
    case EffectType::BeatShake:        return "Beat Shake";
    case EffectType::BeatChroma:       return "Beat Chromatic Kick";
    case EffectType::BeatDrop:         return "Beat Drop";
    case EffectType::FillLeftWithRight: return "Fill Left with Right";
    case EffectType::FillRightWithLeft: return "Fill Right with Left";
    default:                       return "Unknown";
    }
}

/// Returns true for audio-only effects (no GPU shader needed)
inline bool isAudioEffect(EffectType t) noexcept
{
    return t == EffectType::FillLeftWithRight
        || t == EffectType::FillRightWithLeft;
}

/// Returns true for procedural effects that animate off a continuous clock.
/// EffectStack::evaluate() injects clip-local seconds at kGlitchTimeSlot for
/// these so the GPU shader can drive itself without authored keyframes.
inline bool isProceduralTimeEffect(EffectType t) noexcept
{
    switch (t) {
    case EffectType::Scanlines:
    case EffectType::BlockGlitch:
    case EffectType::ChromaticSplit:
    case EffectType::TurbulentDisplace:
    case EffectType::Posterize:
    case EffectType::Grain:
    case EffectType::SignalTear:
        return true;
    default:
        return false;
    }
}

/// Returns true for the beat-reactive effect family (pulse-driven). These also
/// receive the clip-local clock at kGlitchTimeSlot; EffectStack additionally
/// computes their 0–1 pulse envelope and injects it at kBeatPulseSlot.
inline bool isBeatReactEffect(EffectType t) noexcept
{
    switch (t) {
    case EffectType::BeatZoom:
    case EffectType::BeatFlash:
    case EffectType::BeatShake:
    case EffectType::BeatChroma:
    case EffectType::BeatDrop:
        return true;
    default:
        return false;
    }
}

// ── Parameter descriptor ────────────────────────────────────────────────────

struct EffectParam
{
    std::string          name;
    KeyframeTrack<float> track;
    float                minVal{0.0f};
    float                maxVal{1.0f};
};

// ── Effect base class ───────────────────────────────────────────────────────

class Effect
{
public:
    explicit Effect(EffectType type);
    virtual ~Effect();

    // Non-copyable, movable
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;
    Effect(Effect&&) noexcept = default;
    Effect& operator=(Effect&&) noexcept = default;

    // ── Identity ────────────────────────────────────────────────────────
    [[nodiscard]] EffectType          effectType() const noexcept { return m_type; }
    [[nodiscard]] const char*         name()       const noexcept { return effectTypeName(m_type); }
    [[nodiscard]] uint64_t            id()         const noexcept { return m_id; }

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool v) noexcept { m_enabled = v; }

    // ── Parameters ──────────────────────────────────────────────────────
    [[nodiscard]] size_t                         paramCount() const noexcept { return m_params.size(); }
    [[nodiscard]] const EffectParam&             param(size_t i) const { return m_params[i]; }
    [[nodiscard]] EffectParam&                   param(size_t i) { return m_params[i]; }
    [[nodiscard]] const std::vector<EffectParam>& params() const noexcept { return m_params; }

    /// Find parameter by name. Returns nullptr if not found.
    [[nodiscard]] EffectParam*       findParam(const std::string& name);
    [[nodiscard]] const EffectParam* findParam(const std::string& name) const;

    /// Evaluate parameter at time (convenience)
    [[nodiscard]] float evalParam(size_t i, int64_t time) const
    {
        return m_params[i].track.evaluate(time);
    }

    /// Evaluate all parameters at a given time into a flat float array.
    /// Returns a vector of values in parameter order — used by GPU dispatch.
    [[nodiscard]] std::vector<float> evalAllParams(int64_t time) const;

    // ── Effect masks (Premiere Pro: masks limit where the effect applies) ─
    [[nodiscard]] const std::vector<OpacityMask>& masks() const noexcept { return m_masks; }
    [[nodiscard]] std::vector<OpacityMask>&       masks() noexcept { return m_masks; }
    [[nodiscard]] size_t maskCount() const noexcept { return m_masks.size(); }
    void addMask(OpacityMask mask) { m_masks.push_back(std::move(mask)); }
    void removeMask(size_t index)
    {
        if (index < m_masks.size())
            m_masks.erase(m_masks.begin() + static_cast<ptrdiff_t>(index));
    }

    // ── Clone ───────────────────────────────────────────────────────────
    /// NOTE: derived clone() implementations copy type/params only. Callers
    /// that need a full copy (EffectStack::clone, effect copy/paste) must
    /// also copy masks — use cloneWithMasks() unless masks are unwanted.
    [[nodiscard]] virtual std::unique_ptr<Effect> clone() const = 0;

    /// Full copy: clone() + enabled flag + effect masks.
    [[nodiscard]] std::unique_ptr<Effect> cloneWithMasks() const
    {
        auto copy = clone();
        if (copy) {
            copy->setEnabled(m_enabled);
            copy->m_masks = m_masks;
        }
        return copy;
    }

protected:
    /// Derived classes call this in their constructor to register parameters
    void addParam(const std::string& name, float defaultValue,
                  float minVal = 0.0f, float maxVal = 1.0f);

    EffectType m_type;
    uint64_t   m_id;
    bool       m_enabled{true};

    std::vector<EffectParam> m_params;
    std::vector<OpacityMask> m_masks;   ///< Effect masks (limit effect region)

    static uint64_t s_nextId;
};

// ── Factory ─────────────────────────────────────────────────────────────────

/// Create an effect instance by type.
[[nodiscard]] std::unique_ptr<Effect> createEffect(EffectType type);

} // namespace rt
