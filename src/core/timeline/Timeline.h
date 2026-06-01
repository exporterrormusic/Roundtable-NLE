/*
 * ROUNDTABLE NLE v2.0
 * Timeline data model — the heart of the NLE.
 *
 * A Timeline contains an ordered list of Tracks. Each Track contains an ordered
 * list of Clips. Clips can have Transitions between them, KeyframeTracks for
 * animated properties, and an EffectStack.
 *
 * This is a pure data model with no UI dependency. UI observes changes via
 * TimelineObserver callbacks.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Constants.h"
#include "timeline/Track.h"
#include "timeline/Marker.h"
#include "timeline/TimelineObserver.h"

namespace rt {

/// A Sequence (timeline) containing tracks, markers, and playback state.
class Timeline
{
public:
    Timeline();
    ~Timeline();

    // ── Track management ────────────────────────────────────────────────
    Track* addVideoTrack(const std::string& name = "");
    Track* addAudioTrack(const std::string& name = "");
    /// Add a visual-separator divider track (no clips, short, no controls).
    /// The divider is inserted at a specific index (defaults to end).
    /// `permanent=true` marks it as the auto-managed V/A boundary divider —
    /// reserved for ensureSectionDivider; user-added dividers stay non-permanent.
    Track* addDividerTrack(size_t insertIndex = static_cast<size_t>(-1),
                           bool permanent = false);
    /// Add (or return the existing) dedicated caption/subtitle track, pinned
    /// at index 0 so it composites on top of all video. Only one caption
    /// track ever exists; calling again returns the current one.
    Track* addCaptionTrack();
    /// The caption track if one exists, else nullptr.
    [[nodiscard]] Track* captionTrack() noexcept;
    void   removeTrack(size_t index);
    void   moveTrack(size_t from, size_t to);
    [[nodiscard]] size_t       trackCount() const noexcept;
    [[nodiscard]] Track*       track(size_t index) noexcept;
    [[nodiscard]] const Track* track(size_t index) const noexcept;

    /// Insert a track at a specific index (for undo support)
    Track* insertTrack(size_t index, std::unique_ptr<Track> track);

    /// Remove a track and return ownership (for undo support)
    std::unique_ptr<Track> takeTrack(size_t index);

    /// Reorder tracks so all video tracks come before audio tracks.
    void sortTracksByType();

    // ── Marker management ───────────────────────────────────────────────
    void    addMarker(TimeTick time, const std::string& label, uint32_t color = 0xFF4444FF);
    void    removeMarker(size_t index);
    [[nodiscard]] const std::vector<Marker>& markers() const noexcept;

    // ── Playback state ──────────────────────────────────────────────────
    [[nodiscard]] TimeTick playheadPosition() const noexcept;
    void                   setPlayheadPosition(TimeTick pos) noexcept;

    [[nodiscard]] TimeTick inPoint() const noexcept;
    [[nodiscard]] TimeTick outPoint() const noexcept;
    void                   setInPoint(TimeTick t) noexcept;
    void                   setOutPoint(TimeTick t) noexcept;
    void                   clearInOutPoints() noexcept;

    /// The total duration (end of last clip across all tracks).
    [[nodiscard]] TimeTick duration() const noexcept;

    // ── Observer ────────────────────────────────────────────────────────
    void addObserver(TimelineObserver* obs);
    void removeObserver(TimelineObserver* obs);

    // ── Serialization helpers ───────────────────────────────────────────
    [[nodiscard]] const std::string& name() const noexcept;
    void setName(const std::string& name);

    /// Deep-clone the entire timeline (tracks, clips, markers, playback state).
    /// Observers are NOT copied — the clone starts with no observers.
    [[nodiscard]] std::unique_ptr<Timeline> clone() const;

private:
    std::string                        m_name{"Sequence 1"};
    std::vector<std::unique_ptr<Track>> m_tracks;
    std::vector<Marker>                m_markers;
    TimeTick                           m_playhead{0};
    TimeTick                           m_inPoint{-1};   // -1 = not set
    TimeTick                           m_outPoint{-1};
    std::vector<TimelineObserver*>     m_observers;

    void notifyTrackAdded(size_t index);
    void notifyTrackRemoved(size_t index);
    void notifyStructureChanged();
};

} // namespace rt
