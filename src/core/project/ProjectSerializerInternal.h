/*
 * ProjectSerializerInternal.h — limits and load repairs shared by the
 * record loader (ProjectSerializer.cpp, v48+) and the positional loader
 * (ProjectSerializerLegacy.cpp, v1–47). Internal to the serializer.
 */
#pragma once

#include <cstdint>

namespace rt {
class Project;
class Track;
}

namespace rt::project_detail {

// Sanity limits: a count or size beyond these is treated as corruption.
inline constexpr uint32_t kMaxProjectSections = 256;
inline constexpr uint32_t kMaxSequences = 10000;
inline constexpr uint32_t kMaxTracksPerSequence = 65536;
inline constexpr uint32_t kMaxClipsPerTrack = 1000000;
inline constexpr uint32_t kMaxTransitionsPerTrack = 1000000;
inline constexpr uint32_t kMaxMarkersPerSequence = 1000000;
inline constexpr uint32_t kMaxAssets = 1000000;
inline constexpr uint32_t kMaxCharacters = 100000;
inline constexpr uint32_t kMaxBinItems = 1000000;
inline constexpr uint32_t kMaxBinFolders = 100000;
inline constexpr uint32_t kMaxFolderChildren = 1000000;
inline constexpr uint32_t kMaxSectionBytes = 256u * 1024u * 1024u;

/// Re-link a track's transitions to the clips meeting at their edit point.
void relinkTransitionsByGeometry(Track& track);
/// Drop transitions that repeat another's (left, right) clip pair.
void dedupeTransitions(Track& track);
/// Move masks still in legacy sequence-frame coordinates into clip-local
/// ones once the clip's source size is known. Returns how many moved.
int migrateLegacyMasks(Project& project);
/// One-time snap of pre-v46 clips onto each sequence's frame grid.
int alignLegacyClipsToFrameGrid(Project& project);

} // namespace rt::project_detail
