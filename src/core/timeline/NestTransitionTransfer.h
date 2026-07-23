/*
 * NestTransitionTransfer.h - transition preservation for nesting clips.
 *
 * Transitions live on Track rather than Clip, so cloning only the selected
 * clips silently loses their fades/dissolves when the originals are removed.
 * These small model helpers snapshot, remap, transfer, and restore them.
 */

#pragma once

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

struct NestTransitionSnapshot
{
    size_t trackIndex{0};
    Transition transition;
};

[[nodiscard]] inline std::vector<NestTransitionSnapshot>
captureTransitionsForNest(
    const Timeline& timeline,
    const std::unordered_set<uint64_t>& selectedClipIds)
{
    std::vector<NestTransitionSnapshot> result;
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti) {
        const Track* track = timeline.track(ti);
        if (!track) continue;
        for (const auto& transition : track->transitions()) {
            const bool touchesSelection =
                (transition.leftClipId != 0 &&
                 selectedClipIds.count(transition.leftClipId) != 0) ||
                (transition.rightClipId != 0 &&
                 selectedClipIds.count(transition.rightClipId) != 0);
            if (touchesSelection)
                result.push_back({ti, transition});
        }
    }
    return result;
}

[[nodiscard]] inline bool transitionFitsInsideNest(
    const Transition& transition,
    const std::unordered_set<uint64_t>& selectedClipIds) noexcept
{
    const bool leftInside = transition.leftClipId == 0 ||
        selectedClipIds.count(transition.leftClipId) != 0;
    const bool rightInside = transition.rightClipId == 0 ||
        selectedClipIds.count(transition.rightClipId) != 0;
    return leftInside && rightInside;
}

inline size_t addTransitionsInsideNest(
    Timeline& nestedTimeline,
    const std::vector<NestTransitionSnapshot>& snapshots,
    const std::unordered_set<uint64_t>& selectedClipIds,
    const std::unordered_map<size_t, size_t>& sourceToNestedTrack,
    const std::unordered_map<uint64_t, uint64_t>& sourceToNestedClip,
    int64_t nestTimelineOrigin)
{
    size_t added = 0;
    for (const auto& snapshot : snapshots) {
        const auto& original = snapshot.transition;
        if (!transitionFitsInsideNest(original, selectedClipIds)) continue;

        auto trackIt = sourceToNestedTrack.find(snapshot.trackIndex);
        if (trackIt == sourceToNestedTrack.end()) continue;
        Track* track = nestedTimeline.track(trackIt->second);
        if (!track) continue;

        Transition remapped = original;
        if (original.leftClipId != 0) {
            auto it = sourceToNestedClip.find(original.leftClipId);
            if (it == sourceToNestedClip.end()) continue;
            remapped.leftClipId = it->second;
        }
        if (original.rightClipId != 0) {
            auto it = sourceToNestedClip.find(original.rightClipId);
            if (it == sourceToNestedClip.end()) continue;
            remapped.rightClipId = it->second;
        }
        remapped.editPointTick -= nestTimelineOrigin;
        track->addTransition(remapped);
        ++added;
    }
    return added;
}

// A transition joining a selected clip to an unselected neighbour cannot live
// wholly inside the nest. Preserve it on the parent track and replace the
// selected endpoint with the new SequenceClip, matching Premiere's behavior.
inline size_t addBoundaryTransitionsToNestClip(
    Timeline& parentTimeline,
    const std::vector<NestTransitionSnapshot>& snapshots,
    const std::unordered_set<uint64_t>& selectedClipIds,
    size_t nestTrackIndex,
    uint64_t nestClipId)
{
    if (nestClipId == 0 || nestTrackIndex >= parentTimeline.trackCount())
        return 0;
    Track* track = parentTimeline.track(nestTrackIndex);
    if (!track) return 0;

    size_t added = 0;
    for (const auto& snapshot : snapshots) {
        if (snapshot.trackIndex != nestTrackIndex ||
            transitionFitsInsideNest(snapshot.transition, selectedClipIds)) {
            continue;
        }

        Transition remapped = snapshot.transition;
        const bool leftSelected = remapped.leftClipId != 0 &&
            selectedClipIds.count(remapped.leftClipId) != 0;
        const bool rightSelected = remapped.rightClipId != 0 &&
            selectedClipIds.count(remapped.rightClipId) != 0;
        // A captured boundary transition should have exactly one selected
        // endpoint. Skip malformed/multi-boundary data defensively.
        if (leftSelected == rightSelected) continue;
        if (leftSelected) remapped.leftClipId = nestClipId;
        if (rightSelected) remapped.rightClipId = nestClipId;
        track->addTransition(remapped);
        ++added;
    }
    return added;
}

inline size_t restoreTransitionsAfterNestUndo(
    Timeline& parentTimeline,
    const std::vector<NestTransitionSnapshot>& snapshots,
    const std::unordered_map<uint64_t, uint64_t>& restoredClipIds)
{
    size_t added = 0;
    for (const auto& snapshot : snapshots) {
        if (snapshot.trackIndex >= parentTimeline.trackCount()) continue;
        Track* track = parentTimeline.track(snapshot.trackIndex);
        if (!track) continue;

        Transition restored = snapshot.transition;
        if (auto it = restoredClipIds.find(restored.leftClipId);
            it != restoredClipIds.end()) {
            restored.leftClipId = it->second;
        }
        if (auto it = restoredClipIds.find(restored.rightClipId);
            it != restoredClipIds.end()) {
            restored.rightClipId = it->second;
        }
        track->addTransition(restored);
        ++added;
    }
    return added;
}

} // namespace rt
