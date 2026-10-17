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

    // Blend mode is the shared Clip::blendMode() — this class used to keep a
    // second copy that nothing rendered and that hid the shared one when the
    // clip was reached as an AdjustmentClip.

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
    bool    m_singleTrack{false};
};

} // namespace rt
