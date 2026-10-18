/*
 * ClipSerializationLegacy.h — reading clips from positional project files
 * (format v1–47); see ClipSerializationLegacy.cpp. Internal to the
 * serializer.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "project/BinaryIO.h"   // BinaryWriter / BinaryReader primitives

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

// ── Legacy clip reading (ClipSerializationLegacy.cpp) ──────────────────────

void readKeyframeTrack(BinaryReader& r, KeyframeTrack<float>& track, uint32_t version = 1);

std::unique_ptr<Clip> readClip(BinaryReader& r, uint32_t version = 1);

} // namespace rt
