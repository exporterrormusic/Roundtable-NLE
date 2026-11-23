/*
 * AudioPresets — Essential Sound style one-click looks ("Make Distant",
 * "Telephone", "Large Hall", "Echo"…) built from the audiofx processors.
 *
 * A preset is a recipe: it APPENDS a configured EQ / Dynamics / Reverb /
 * Delay / Distortion to a clip's FxChain. The processors stay ordinary and
 * fully editable afterwards, like Premiere's Essential Sound sliders writing
 * into real effects.
 *
 * Drag-and-drop and menus carry one integer "audio fx code": values below
 * kPresetCodeBase are a ProcessorKind, values from kPresetCodeBase up are an
 * AudioPreset. applyAudioFxCode() handles both.
 */

#pragma once

#include "audiofx/FxChain.h"

namespace rt::audiofx {

enum class AudioPreset : int
{
    // Distance (Audition's Make Close Up / Medium Shot / Distant)
    MakeCloseUp = 0,
    MakeMediumShot,
    MakeDistant,
    // Spaces
    SmallRoom,
    LargeHall,
    Cathedral,
    NextRoom,
    Underwater,
    // Devices
    Telephone,
    OldRadio,
    WalkieTalkie,
    Megaphone,
    // Echo
    Echo,
    Slapback,
    // Voice
    PodcastVoice,
    Count
};

inline constexpr int kPresetCodeBase = 1000;

[[nodiscard]] const char* audioPresetName(AudioPreset p) noexcept;
/// Group heading for menus ("Distance", "Spaces", "Devices", "Echo", "Voice").
[[nodiscard]] const char* audioPresetCategory(AudioPreset p) noexcept;

/// Append the preset's processors to `chain` (prepared at the chain's rate).
void applyAudioPreset(FxChain& chain, AudioPreset p);

/// Apply a ProcessorKind or preset code (see header comment). Returns false
/// for an unknown code. A plain Dynamics gets the voice starting preset, as
/// the Add menu always did.
bool applyAudioFxCode(FxChain& chain, int code);

/// Display name for a code (processor display name or preset name).
[[nodiscard]] const char* audioFxCodeName(int code) noexcept;

} // namespace rt::audiofx
