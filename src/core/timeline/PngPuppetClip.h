/*
 * PngPuppetClip — a Veadotube-mini-style PNG puppet character clip.
 *
 * A puppet is built from 4 PNGs of the same character (one per mouth×eyes
 * state).  On the timeline it behaves like any other character clip: a single
 * infinitely-stretchable clip that loops forever.  Per frame it deterministically
 * selects one of the 4 PNGs:
 *
 *   - Idle  : mouth closed / eyes open, occasionally blinking (eyes closed) for
 *             a couple of frames.
 *   - Talk  : mouth swaps open/closed rapidly to simulate speech, while still
 *             blinking.
 *
 * It also applies a gentle breathing drift (sub-pixel sway + rise/fall + tiny
 * scale/rotation) so the character feels alive rather than static.
 *
 * IMPORTANT: every visual decision is a pure deterministic function of the
 * clip-local time, so preview and export produce byte-identical frames.  No
 * wall-clock time or RNG state is used.
 *
 * This class is purely data + deterministic selection logic.  The actual PNG
 * decode + frame assembly happens in renderPngPuppetClip() (ClipRenderers).
 */

#pragma once

#include "timeline/Clip.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace rt {

class PngPuppetClip : public Clip
{
public:
    /// Index into the 4-image face set.  The numeric values are serialized
    /// implicitly (as array order) so do not reorder.
    enum Face : int
    {
        MouthClosedEyesOpen   = 0,  ///< idle resting state
        MouthClosedEyesClosed = 1,  ///< blink while idle
        MouthOpenEyesOpen     = 2,  ///< talking
        MouthOpenEyesClosed   = 3,  ///< talking + blink
        FaceCount             = 4
    };

    struct BreathOffset
    {
        float dx{0.0f};      ///< horizontal sway, REF-1920 px
        float dy{0.0f};      ///< vertical rise/fall, REF-1080 px
        float scale{1.0f};   ///< subtle scale multiplier (~1.0)
        float rot{0.0f};     ///< subtle rotation, degrees
    };

    PngPuppetClip();
    PngPuppetClip(const std::string& characterName, const std::string& variant);
    ~PngPuppetClip() override = default;

    // ── Character identity ──────────────────────────────────────────────
    [[nodiscard]] const std::string& characterName() const noexcept { return m_characterName; }
    void setCharacterName(const std::string& name) { m_characterName = name; }

    [[nodiscard]] const std::string& variant() const noexcept { return m_variant; }
    void setVariant(const std::string& v) { m_variant = v; }

    // ── Face images (the 4 PNG paths for the active variant) ─────────────
    [[nodiscard]] const std::string& facePath(int face) const
    {
        return m_facePaths[clampFace(face)];
    }
    void setFacePath(int face, const std::string& path)
    {
        m_facePaths[clampFace(face)] = path;
    }
    [[nodiscard]] const std::array<std::string, FaceCount>& facePaths() const noexcept
    {
        return m_facePaths;
    }

    // ── Talking ─────────────────────────────────────────────────────────
    [[nodiscard]] bool isTalking() const noexcept { return m_talking; }
    void setTalking(bool v) noexcept { m_talking = v; }

    // ── Motion / timing parameters (seconds) ────────────────────────────
    [[nodiscard]] float talkSwapSeconds()    const noexcept { return m_talkSwapSeconds; }
    void  setTalkSwapSeconds(float s)        noexcept { m_talkSwapSeconds = s; }

    [[nodiscard]] float blinkIntervalSeconds() const noexcept { return m_blinkIntervalSeconds; }
    void  setBlinkIntervalSeconds(float s)     noexcept { m_blinkIntervalSeconds = s; }

    [[nodiscard]] float blinkDurationSeconds() const noexcept { return m_blinkDurationSeconds; }
    void  setBlinkDurationSeconds(float s)     noexcept { m_blinkDurationSeconds = s; }

    [[nodiscard]] float breathAmplitude()    const noexcept { return m_breathAmplitude; }
    void  setBreathAmplitude(float a)        noexcept { m_breathAmplitude = a; }

    [[nodiscard]] float breathSpeed()        const noexcept { return m_breathSpeed; }
    void  setBreathSpeed(float s)            noexcept { m_breathSpeed = s; }

    [[nodiscard]] float swayAmplitude()      const noexcept { return m_swayAmplitude; }
    void  setSwayAmplitude(float a)          noexcept { m_swayAmplitude = a; }

    [[nodiscard]] uint32_t seed()            const noexcept { return m_seed; }
    void  setSeed(uint32_t s)                noexcept { m_seed = s; }

    // ── Deterministic animation evaluation ──────────────────────────────
    /// Whether the mouth is open at clip-local time `t` (seconds).
    [[nodiscard]] bool mouthOpenAt(double t) const noexcept
    {
        if (!m_talking || m_talkSwapSeconds <= 0.0f) return false;
        const long long idx =
            static_cast<long long>(std::floor(t / static_cast<double>(m_talkSwapSeconds)));
        const uint32_t h = hashU32(static_cast<uint32_t>(idx) ^ (m_seed * 0x9E3779B9u + 1u));
        // ~55% open while talking gives a lively, slightly irregular flap.
        return (h % 100u) < 55u;
    }

    /// Whether the eyes are closed (blinking) at clip-local time `t` (seconds).
    [[nodiscard]] bool eyesClosedAt(double t) const noexcept
    {
        if (m_blinkIntervalSeconds <= 0.0f || m_blinkDurationSeconds <= 0.0f) return false;
        const double interval = static_cast<double>(m_blinkIntervalSeconds);
        const double dur      = static_cast<double>(m_blinkDurationSeconds);
        const long long slot  = static_cast<long long>(std::floor(t / interval));
        const double frac     = t - static_cast<double>(slot) * interval;
        const uint32_t h      = hashU32(static_cast<uint32_t>(slot) ^ (m_seed * 0x85EBCA6Bu + 7u));
        const double jit      = static_cast<double>(h & 0xFFFFFFu) / static_cast<double>(0x1000000);
        double span = interval - dur;
        if (span < 0.0) span = 0.0;
        const double start = jit * span;
        return frac >= start && frac < start + dur;
    }

    /// Select which of the 4 face images to show at clip-local time `t` (seconds).
    [[nodiscard]] int selectFace(double t) const noexcept
    {
        if (t < 0.0) t = 0.0;
        const bool mouthOpen  = mouthOpenAt(t);
        const bool eyesClosed = eyesClosedAt(t);
        if (mouthOpen)  return eyesClosed ? MouthOpenEyesClosed   : MouthOpenEyesOpen;
        return            eyesClosed ? MouthClosedEyesClosed : MouthClosedEyesOpen;
    }

    /// Gentle "breathing" transform offset at clip-local time `t` (seconds).
    [[nodiscard]] BreathOffset breathing(double t) const noexcept
    {
        if (t < 0.0) t = 0.0;
        constexpr double kTwoPi = 6.283185307179586;
        const double phase = t * static_cast<double>(m_breathSpeed) * kTwoPi;
        BreathOffset o;
        o.dy    = static_cast<float>(static_cast<double>(m_breathAmplitude) * std::sin(phase));
        o.dx    = static_cast<float>(static_cast<double>(m_swayAmplitude)   * std::sin(phase * 0.5));
        o.scale = static_cast<float>(1.0 + static_cast<double>(m_breathAmplitude) * 0.0015 *
                                           (0.5 - 0.5 * std::cos(phase)));
        o.rot   = static_cast<float>(0.4 * std::sin(phase * 0.5));
        return o;
    }

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
    static int clampFace(int f) noexcept
    {
        return (f < 0) ? 0 : (f >= FaceCount ? FaceCount - 1 : f);
    }

    /// Fast integer avalanche hash (Murmur-ish finaliser) — deterministic,
    /// no global state, gives natural-looking blink/talk jitter.
    static uint32_t hashU32(uint32_t x) noexcept
    {
        x ^= x >> 16; x *= 0x7FEB352Du;
        x ^= x >> 15; x *= 0x846CA68Bu;
        x ^= x >> 16;
        return x;
    }

    std::string m_characterName;
    std::string m_variant{"default"};
    std::array<std::string, FaceCount> m_facePaths{};

    bool     m_talking{false};

    // Veadotube-like defaults.
    float    m_talkSwapSeconds{0.08f};       ///< mouth open/closed cadence
    float    m_blinkIntervalSeconds{4.0f};   ///< average seconds between blinks
    float    m_blinkDurationSeconds{0.12f};  ///< how long a blink lasts
    float    m_breathAmplitude{8.0f};        ///< vertical breathing travel (REF px)
    float    m_breathSpeed{0.22f};           ///< breaths per second
    float    m_swayAmplitude{4.0f};          ///< horizontal sway (REF px)
    uint32_t m_seed{1u};                     ///< per-clip jitter seed
};

} // namespace rt
