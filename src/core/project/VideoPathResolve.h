/*
 * VideoPathResolve.h — where a saved video path should be opened from now.
 */
#pragma once

#include <string>

namespace rt {

/// A saved video path as it should be opened now: a missing .mov/.mp4/.webm
/// falls back to the other extension when that file exists, and old Wells
/// costume paths map to their HEVC proxies. Applied on every load (both
/// formats).
std::string resolveVideoPath(const std::string& path);

} // namespace rt
