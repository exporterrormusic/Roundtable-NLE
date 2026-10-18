/*
 * LegacyProjectWriterV47.h — the frozen v47 project writer, for tests (see
 * the .cpp).
 */
#pragma once

#include <cstdint>
#include <vector>

namespace rt {
class BinaryWriter;
class Clip;
class Project;
template <typename T> class KeyframeTrack;
}

namespace rt::legacy_v47 {

inline constexpr uint32_t kVersion = 47;

void writeKeyframeTrack(BinaryWriter& w, const KeyframeTrack<float>& track);
void writeClip(BinaryWriter& w, const Clip& clip);
/// The whole project as a v47 file.
std::vector<uint8_t> serialize(const Project& project);

} // namespace rt::legacy_v47
