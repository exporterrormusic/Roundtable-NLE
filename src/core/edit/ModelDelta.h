/*
 * ModelDelta — what one edit changed in one sequence, and how to undo it.
 *
 * diffSequence(before, after) compares two states of the same sequence (an
 * exact copy taken when the edit began, and the live sequence when it ends).
 * It keeps the full track layout of both sides, which is small, plus copies of
 * only the clips that differ. applyDelta() turns a live sequence into either
 * side IN PLACE: tracks and clips are matched by id and updated through
 * Clip::assignStateFrom, so objects that survive keep their addresses and the
 * clip, track and keyframe-track pointers panels hold stay valid across
 * undo/redo.
 *
 * Compared: clip content (serialized, as saved to disk), track order,
 * membership and mix/lock flags, transitions, markers, settings, name.
 * Not compared (view state, never undoable): track height, collapsed,
 * targeting, playhead, in/out points.
 */

#pragma once

#include "project/Settings.h"
#include "timeline/Clip.h"     // owned (parked) clips
#include "timeline/Marker.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

class Timeline;

struct DeltaTrack
{
    uint64_t    id{0};
    TrackType   type{TrackType::Video};
    std::string name;
    bool        locked{false};
    bool        muted{false};
    bool        soloed{false};
    bool        syncLocked{true};
    bool        divider{false};
    bool        permanentDivider{false};
    bool        caption{false};
    uint32_t    color{0};
    float       volume{1.0f};
    float       pan{0.0f};
    // View state: only used when the track has to be re-created.
    float       height{80.0f};
    bool        collapsed{false};
    bool        targeted{true};

    std::vector<uint64_t>   clipIds;      ///< in track order
    std::vector<Transition> transitions;

    [[nodiscard]] bool sameContent(const DeltaTrack& o) const;
};

struct DeltaSequenceState
{
    std::string             name;
    Settings                settings;
    std::vector<Marker>     markers;
    std::vector<DeltaTrack> tracks;

    [[nodiscard]] bool sameContent(const DeltaSequenceState& o) const;
};

enum class DeltaSide : uint8_t { Before, After };

struct ModelDelta
{
    uint64_t           sequenceId{0};
    DeltaSequenceState before;
    DeltaSequenceState after;
    /// Changed or removed clips as they were before the edit.
    std::unordered_map<uint64_t, std::shared_ptr<const Clip>> clipsBefore;
    /// Changed or added clips as they are after the edit.
    std::unordered_map<uint64_t, std::shared_ptr<const Clip>> clipsAfter;

    /// Live objects currently OFF the timeline because of this step (removed
    /// by it, or brought back by its undo and removed again by its redo).
    /// applyDelta parks objects here instead of destroying them and takes
    /// them back when they return, so a clip/track keeps its address across
    /// undo/redo and pointers held elsewhere never dangle while the step is
    /// in history. Displaced duplicates go to the graveyards for the same
    /// reason.
    std::unordered_map<uint64_t, std::unique_ptr<Clip>>  parkedClips;
    std::unordered_map<uint64_t, std::unique_ptr<Track>> parkedTracks;
    std::vector<std::unique_ptr<Clip>>  clipGraveyard;
    std::vector<std::unique_ptr<Track>> trackGraveyard;

    void parkClip(std::unique_ptr<Clip> clip);
    void parkTrack(std::unique_ptr<Track> track);

    [[nodiscard]] bool empty() const;
    /// Distinct clips this delta touches.
    [[nodiscard]] size_t changedClipCount() const;
};

/// What changed from `before` to `after` (two states of one sequence).
[[nodiscard]] ModelDelta diffSequence(const Timeline& before, const Timeline& after);

/// Make `live` match one side of `delta`, reusing live and parked tracks and
/// clips (objects leaving the timeline are parked in `delta`, never freed).
/// Ends with one onTimelineStructureChanged() notification.
void applyDelta(Timeline& live, ModelDelta& delta, DeltaSide side);

} // namespace rt
