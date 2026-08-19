#pragma once

#include "timeline/Timeline.h"

namespace rt {

class Clip;

/// Resolve a clip identity against the current timeline model. This remains
/// valid after undo/redo replaces track and clip objects.
[[nodiscard]] inline Clip* resolveClipById(Timeline* timeline, uint64_t clipId,
                                           Track** owningTrack = nullptr) noexcept
{
    if (owningTrack) *owningTrack = nullptr;
    if (!timeline || clipId == 0) return nullptr;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* track = timeline->track(ti);
        if (!track) continue;
        const size_t ci = track->findClipIndexById(clipId);
        if (ci >= track->clipCount()) continue;
        if (owningTrack) *owningTrack = track;
        return track->clip(ci);
    }
    return nullptr;
}

[[nodiscard]] inline Clip* resolveMutableClipById(
    Timeline* timeline, uint64_t clipId, Track** owningTrack = nullptr) noexcept
{
    Track* track = nullptr;
    Clip* clip = resolveClipById(timeline, clipId, &track);
    if (owningTrack) *owningTrack = track;
    return clip && track && !track->isLocked() ? clip : nullptr;
}

/// Fail-closed preflight for user-triggered edits to a bound clip.
/// Also rejects stale or mismatched clip/track bindings.
[[nodiscard]] inline bool canMutateClip(const Clip* clip, const Track* track) noexcept
{
    if (!clip || !track || track->isLocked()) return false;
    const size_t index = track->findClipIndexById(clip->id());
    return index < track->clipCount() && track->clip(index) == clip;
}

} // namespace rt
