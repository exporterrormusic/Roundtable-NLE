/*
 * EffectStack — ordered list of effects applied to a clip.
 *
 * Each clip has one EffectStack. Effects are processed in order (top-to-bottom).
 * Provides add/remove/move/enable operations and evaluation at a given time.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "effects/Effect.h"

namespace rt {

class EffectStack
{
public:
    EffectStack();
    ~EffectStack();

    EffectStack(const EffectStack&) = delete;
    EffectStack& operator=(const EffectStack&) = delete;
    EffectStack(EffectStack&&) noexcept = default;
    EffectStack& operator=(EffectStack&&) noexcept = default;

    // ── Effect management ───────────────────────────────────────────────
    /// Add an effect at the end (or at a specific index)
    void addEffect(std::unique_ptr<Effect> effect);
    void insertEffect(size_t index, std::unique_ptr<Effect> effect);

    /// Remove and return effect at index
    [[nodiscard]] std::unique_ptr<Effect> removeEffect(size_t index);

    /// Move effect from one index to another
    void moveEffect(size_t fromIndex, size_t toIndex);

    // ── Accessors ───────────────────────────────────────────────────────
    [[nodiscard]] size_t effectCount() const noexcept { return m_effects.size(); }
    [[nodiscard]] bool   isEmpty()     const noexcept { return m_effects.empty(); }

    [[nodiscard]] const Effect& effect(size_t i) const { return *m_effects[i]; }
    [[nodiscard]] Effect&       effect(size_t i) { return *m_effects[i]; }

    [[nodiscard]] const Effect* effectById(uint64_t id) const;
    [[nodiscard]] Effect*       effectById(uint64_t id);

    /// Find index of effect by ID. Returns effectCount() if not found.
    [[nodiscard]] size_t indexOf(uint64_t effectId) const;

    // ── Evaluation ──────────────────────────────────────────────────────
    /// Returns true if any enabled effect exists
    [[nodiscard]] bool hasActiveEffects() const;

    /// Evaluate all enabled effects' parameters at a given time.
    /// Returns a vector of { effectType, paramValues[] } pairs for GPU dispatch.
    /// Each snapshot also carries the source effect's id and its evaluated
    /// effect masks (empty = effect applies to the whole clip).
    struct EffectSnapshot
    {
        EffectType         type;
        std::vector<float> params;
        uint64_t           effectId{0};
        std::vector<MaskRenderState> masks;
    };
    [[nodiscard]] std::vector<EffectSnapshot> evaluate(int64_t time) const;

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<EffectStack> clone() const;

private:
    std::vector<std::unique_ptr<Effect>> m_effects;
};

} // namespace rt
