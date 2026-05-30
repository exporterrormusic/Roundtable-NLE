/*
 * TimelineObserver — interface for UI to observe timeline model changes.
 *
 * This is the core of the Model-View separation. The UI registers as an
 * observer and receives notifications when the data model changes.
 */

#pragma once

#include <cstdint>

namespace rt {

class Timeline;

class TimelineObserver
{
public:
    virtual ~TimelineObserver() = default;

    /// A track was added at the given index
    virtual void onTrackAdded(size_t /*trackIndex*/) {}

    /// A track was removed from the given index
    virtual void onTrackRemoved(size_t /*trackIndex*/) {}

    /// Track order changed (move, reorder)
    virtual void onTrackMoved(size_t /*from*/, size_t /*to*/) {}

    /// A track property changed (lock, mute, solo, name, height)
    virtual void onTrackPropertyChanged(size_t /*trackIndex*/) {}

    /// A clip was added to a track
    virtual void onClipAdded(size_t /*trackIndex*/, size_t /*clipIndex*/) {}

    /// A clip was removed from a track
    virtual void onClipRemoved(size_t /*trackIndex*/, size_t /*clipIndex*/) {}

    /// A clip was modified (position, duration, properties)
    virtual void onClipChanged(size_t /*trackIndex*/, size_t /*clipIndex*/) {}

    /// A transition was added/removed/modified
    virtual void onTransitionChanged(size_t /*trackIndex*/, size_t /*transitionIndex*/) {}

    /// A marker was added/removed/modified
    virtual void onMarkerChanged() {}

    /// Playhead position changed
    virtual void onPlayheadChanged(int64_t /*newPosition*/) {}

    /// In/Out points changed
    virtual void onInOutChanged() {}

    /// Major structural change — full UI rebuild recommended
    virtual void onTimelineStructureChanged() {}

    /// The timeline is being destroyed. Observers MUST drop any cached raw
    /// Timeline* (and stop anything that would dereference it) before
    /// returning -- after this call the object's storage is reclaimed.
    /// Fired from ~Timeline(); do NOT dereference `tl` here, only compare it.
    virtual void onTimelineDestroyed(Timeline* /*tl*/) {}
};

} // namespace rt
