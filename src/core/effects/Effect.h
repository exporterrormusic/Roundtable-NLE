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

#include <algorithm>
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

/// Display name, and the effect families EffectStack::evaluate() feeds a
/// clock to (procedural: clip-local seconds at kGlitchTimeSlot; beat:
/// also the pulse at kBeatPulseSlot). All read the effect table
/// (EffectRegistry.h).
[[nodiscard]] const char* effectTypeName(EffectType t) noexcept;
[[nodiscard]] bool isAudioEffect(EffectType t) noexcept;
[[nodiscard]] bool isProceduralTimeEffect(EffectType t) noexcept;
[[nodiscard]] bool isBeatReactEffect(EffectType t) noexcept;

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
    /// An effect of `type` with the parameters its table entry lists
    /// (EffectRegistry.h), at their defaults. Most effects are exactly this;
    /// subclasses exist only for state beyond the parameters (LUT cube,
    /// grading curves, beat onsets, Ultra Key presets, OTS saved defaults).
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
    /// New effect (new id) with this one's enabled flag, parameters and
    /// subclass extras, but no masks — use cloneWithMasks() for a full copy
    /// (EffectStack::clone, effect copy/paste).
    [[nodiscard]] std::unique_ptr<Effect> clone() const;

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

    /// cloneWithMasks() that keeps this effect's id — for render snapshots
    /// and undo copies, which must match the live effect they mirror.
    [[nodiscard]] std::unique_ptr<Effect> cloneKeepingId() const
    {
        auto copy = cloneWithMasks();
        if (copy) copy->m_id = m_id;
        return copy;
    }

    /// Overwrite this effect's state with `src` (same type) in place: enabled,
    /// masks, every parameter's keyframes, and subclass extras. The object and
    /// its parameter tracks keep their addresses, so panels holding pointers
    /// into them stay valid across undo/redo.
    void assignStateFrom(const Effect& src)
    {
        m_enabled = src.m_enabled;
        m_masks = src.m_masks;
        const size_t n = std::min(m_params.size(), src.m_params.size());
        for (size_t i = 0; i < n; ++i)
            m_params[i].track = src.m_params[i].track;
        copyExtraStateFrom(src);
    }

protected:
    /// Subclass state beyond params/enabled/masks (LUT data, grading toggles,
    /// beat onsets). clone() and assignStateFrom() both call it, so the list
    /// lives once.
    virtual void copyExtraStateFrom(const Effect& /*src*/) {}

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
