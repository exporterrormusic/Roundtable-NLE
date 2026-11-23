/*
 * EssentialSound — Premiere's Essential Sound panel model.
 *
 * A clip is tagged Dialogue / Music / SFX / Ambience and gets a few
 * task-oriented controls (0-10 amount sliders, as in Premiere). The settings
 * live on the AudioClip and are COMPILED into an FxChain that runs before the
 * clip's own audio effects (AudioClip::buildProcessingChain()), so the panel
 * never edits the user's own effects and can be switched off as a whole.
 *
 * Which controls a type shows (the struct holds them all):
 *   Dialogue — Repair (Reduce Rumble, DeHum, DeEss), Clarity (Dynamics, EQ
 *              preset, Enhance Speech), Creative (Reverb preset)
 *   Music    — (loudness + ducking only; ducking writes volume keyframes)
 *   SFX      — Creative (Reverb preset)
 *   Ambience — Creative (Reverb preset), Stereo Width
 * Loudness Auto-Match and Ducking are one-shot actions that write clip volume
 * (see Loudness.h and computeDuckingRanges()).
 */

#pragma once

#include "audiofx/FxChain.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace rt::audiofx {

enum class SoundType : int { None = 0, Dialogue, Music, SFX, Ambience, Count };

/// Dialogue Clarity EQ presets (Premiere-style names).
enum class ClarityEqPreset : int
{
    Subtle = 0, PodcastVoice, WarmVoice, BackgroundVoice, Telephone, OldRadio,
    Intercom, OutsideTheRoom, Count
};

/// Creative reverb presets.
enum class SpaceReverbPreset : int
{
    SmallRoom = 0, LargeRoom, Auditorium, Church, Outside, Distant, Count
};

struct EssentialSoundSettings
{
    SoundType type{SoundType::None};

    // Repair (Dialogue)
    bool  reduceRumble{false}; float rumbleAmount{5.0f};
    bool  deHum{false};        float deHumAmount{5.0f}; bool hum60Hz{true};
    bool  deEss{false};        float deEssAmount{5.0f};

    // Clarity (Dialogue)
    bool  dynamics{false};     float dynamicsAmount{5.0f};
    bool  clarityEq{false};    int   eqPreset{0};       float eqAmount{5.0f};
    bool  enhanceSpeech{false};

    // Creative (Dialogue, SFX, Ambience)
    bool  reverb{false};       int   reverbPreset{0};   float reverbAmount{5.0f};

    // Ambience
    bool  stereoWidth{false};  float widthAmount{5.0f};

    /// Auto-Match target, LUFS (Dialogue default -23 like Premiere).
    float loudnessTarget{-23.0f};

    bool operator==(const EssentialSoundSettings&) const = default;
};

[[nodiscard]] const char* soundTypeName(SoundType t) noexcept;
[[nodiscard]] const char* clarityEqPresetName(ClarityEqPreset p) noexcept;
[[nodiscard]] const char* spaceReverbPresetName(SpaceReverbPreset p) noexcept;

/// The processors the settings ask for (empty chain for None / nothing on).
[[nodiscard]] FxChain compileEssentialSound(const EssentialSoundSettings& s);

/// Default Auto-Match target for a type (Dialogue -23, Music -25, SFX -21,
/// Ambience -30 LUFS, close to Premiere's).
[[nodiscard]] float defaultLoudnessTarget(SoundType t) noexcept;

/// Ducking: the music time ranges to turn down so dialogue reads, from the
/// dialogue clips' time ranges (ticks). Ranges closer than `mergeGap` are
/// joined; each is padded by `fade` on both sides for the ramps.
[[nodiscard]] std::vector<std::pair<int64_t, int64_t>>
computeDuckingRanges(std::vector<std::pair<int64_t, int64_t>> dialogue,
                     int64_t mergeGap, int64_t fade);

} // namespace rt::audiofx
