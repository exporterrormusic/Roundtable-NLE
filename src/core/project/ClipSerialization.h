/*
 * ClipSerialization.h — Internal helpers shared between ProjectSerializer.cpp
 *                       and ClipSerialization.cpp.
 *
 * NOT a public header — only #included by the serializer implementation files.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "project/BinaryIO.h"   // BinaryWriter / BinaryReader primitives

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"

namespace rt {

// ── Clip-level serialization (implemented in ClipSerialization.cpp) ────────

void writeKeyframeTrack(BinaryWriter& w, const KeyframeTrack<float>& track);
void readKeyframeTrack(BinaryReader& r, KeyframeTrack<float>& track, uint32_t version = 1);

void writeClip(BinaryWriter& w, const Clip& clip);
std::unique_ptr<Clip> readClip(BinaryReader& r, uint32_t version = 1);

} // namespace rt
