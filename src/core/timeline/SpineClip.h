/*
 * SpineClip — a timeline clip representing an animated Spine character.
 *
 * Contains all data needed to evaluate and render a Spine skeleton at any
 * point in time: character identity, outfit/skin, animation name, stance,
 * talking mode, and additional transform overrides.
 *
 * The actual skeleton evaluation happens in SpineEngine (core/spine/).
 * The GPU rendering happens in SpineRenderer (gpu/).
 * This class is purely data.
 */

#pragma once

#include "timeline/Clip.h"
#include <string>

namespace rt {

/// Stance determines which skeleton file to load (aim/cover/default)
enum class CharacterStance : uint8_t
{
    Default,
    Aim,
    Cover
};

class SpineClip : public Clip
{
public:
    SpineClip();
    /// Convenience constructor: sets character name and outfit.
    SpineClip(const std::string& characterName, const std::string& outfit);
    ~SpineClip() override = default;

    // ── Character identity ──────────────────────────────────────────────
    [[nodiscard]] const std::string& characterName() const noexcept { return m_characterName; }
    void setCharacterName(const std::string& name) { m_characterName = name; }

    [[nodiscard]] const std::string& outfit() const noexcept { return m_outfit; }
    void setOutfit(const std::string& outfit) { m_outfit = outfit; }

    [[nodiscard]] CharacterStance stance() const noexcept { return m_stance; }
    void setStance(CharacterStance s) noexcept { m_stance = s; }

    // ── Animation ───────────────────────────────────────────────────────
    [[nodiscard]] const std::string& animationName() const noexcept { return m_animation; }
    void setAnimationName(const std::string& anim) { m_animation = anim; }

    [[nodiscard]] bool isLooping() const noexcept { return m_looping; }
    void setLooping(bool v) noexcept { m_looping = v; }

    [[nodiscard]] bool isTalking() const noexcept { return m_talking; }
    void setTalking(bool v) noexcept { m_talking = v; }

    /// Animation playback speed multiplier (independent of clip speed)
    [[nodiscard]] float animationSpeed() const noexcept { return m_animSpeed; }
    void setAnimationSpeed(float s) noexcept { m_animSpeed = s; }

    /// When true, looping animations use global timeline time instead of
    /// clip-local time, so the animation loop position carries smoothly
    /// across cuts between same-character clips (animation continuity).
    /// "Action" and "special" animations should typically use clip-local time.
    [[nodiscard]] bool useGlobalTime() const noexcept { return m_useGlobalTime; }
    void setUseGlobalTime(bool v) noexcept { m_useGlobalTime = v; }

    // ── Crop ────────────────────────────────────────────────────────────
    [[nodiscard]] float cropLeft()   const noexcept { return m_cropL; }
    [[nodiscard]] float cropRight()  const noexcept { return m_cropR; }
    [[nodiscard]] float cropTop()    const noexcept { return m_cropT; }
    [[nodiscard]] float cropBottom() const noexcept { return m_cropB; }
    void setCrop(float l, float r, float t, float b) noexcept
    {
        m_cropL = l; m_cropR = r; m_cropT = t; m_cropB = b;
    }

    // ── Clone ───────────────────────────────────────────────────────────
protected:
    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;
    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    std::string     m_characterName;
    std::string     m_outfit{"default"};
    CharacterStance m_stance{CharacterStance::Default};
    std::string     m_animation{"idle"};
    bool            m_looping{true};
    bool            m_talking{false};
    float           m_animSpeed{1.0f};
    bool            m_useGlobalTime{true};  // seamless animation across cuts

    // Crop (0.0 = no crop, 0.5 = crop 50% from that side)
    float m_cropL{0.0f}, m_cropR{0.0f}, m_cropT{0.0f}, m_cropB{0.0f};
};

} // namespace rt
