/*
 * AdjustmentClip — an adjustment layer clip.
 *
 * Has no source media of its own. Instead, it applies its effect stack
 * to all clips on tracks below it in the compositing order.
 * Think of it like an After Effects adjustment layer.
 */

#pragma once

#include "timeline/Clip.h"
#include <string>

namespace rt {

class AdjustmentClip : public Clip
{
public:
    AdjustmentClip();
    ~AdjustmentClip() override = default;

    // ── Blend mode ──────────────────────────────────────────────────────
    // The blend mode determines how this adjustment layer's output
    // composites with the layers below it.
    // (Actual BlendMode enum is used by the compositor — here we store as int)
    [[nodiscard]] uint8_t blendMode() const noexcept { return m_blendMode; }
    void setBlendMode(uint8_t mode) noexcept { m_blendMode = mode; }

    // ── Scope ───────────────────────────────────────────────────────────
    /// If true, only affects the track directly below, not all tracks below.
    [[nodiscard]] bool affectsSingleTrack() const noexcept { return m_singleTrack; }
    void setAffectsSingleTrack(bool v) noexcept { m_singleTrack = v; }

    // ── Clone ───────────────────────────────────────────────────────────
protected:
    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;
    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    uint8_t m_blendMode{0};     // 0 = Normal
    bool    m_singleTrack{false};
};

} // namespace rt
