/*
 * ClipLoudness — integrated loudness (LUFS) of what an audio clip plays,
 * for Essential Sound "Auto-Match".
 *
 * Reads the clip's source range (sourceIn, duration x speed), runs it through
 * the clip's processing chain (Essential Sound + audio effects) and measures
 * it with the BS.1770 meter. Clip volume, fades and pan are NOT applied:
 * Auto-Match sets the clip volume from this number.
 */

#pragma once

#include <optional>

namespace rt {

class AudioClip;

/// LUFS of the clip's processed source; nullopt when the media can't be
/// read or is silent.
[[nodiscard]] std::optional<double> measureClipLoudness(const AudioClip& clip);

/// Linear volume that brings a clip measuring `measuredLufs` to `targetLufs`
/// (clamped to Premiere's clip gain range, -96..+24 dB).
[[nodiscard]] float autoMatchVolume(double measuredLufs, double targetLufs);

} // namespace rt
