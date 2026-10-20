/*
 * ExportPlan.h -- AudioSync's export to the timeline, without the panel.
 *
 * planExport() decides what goes on the timeline (no timeline involved):
 * the MATCH tab's clip for each script line, in script order, laid back to
 * back from tick 0, one audio clip per kept range, and one visual group per
 * run of consecutive lines by the same character.
 *
 * applyExport() puts a plan on a timeline in one of two modes:
 *  - FULL (first export into a sequence; no clip carries a syncLine
 *    back-link): clears previous export-owned content and rebuilds.
 *  - RE-EXPORT (isReexport): clips tagged with a syncLine are diffed
 *    against the plan and updated in place (AudioSync wins on position and
 *    range), keeping clip identity, so the user's shot swaps, transforms,
 *    effects and transitions survive. Clips without a back-link (user
 *    media, captions, music) are never touched.
 * The shot layers of a visual group come from the caller
 * (PlaceGroupVisualsFn: AudioSync builds them from the default shot).
 *
 * lockedTrackInTheWay() is the preflight: a locked track the export would
 * have to change stops it before anything is touched.
 */

#pragma once

#include "audiosync/SyncClip.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt {

class Timeline;
struct Script;

namespace audiosync {

/// One audio clip the export places: a kept range of one line's clip.
struct PlannedAudio
{
    int         line{-1};        ///< script line (the clip's syncLine back-link)
    std::string character;       ///< speaker = audio track name ("VO" when empty)
    std::string sourceFile;
    std::string label;
    int64_t     timelineIn{0};
    int64_t     duration{0};
    int64_t     sourceIn{0};
    int64_t     sourceDuration{0};
};

/// One visual shot group: a run of consecutive lines by one character.
struct PlannedGroup
{
    std::string character;
    int         firstLine{-1};   ///< back-link of the group's visual clips
    int64_t     timelineStart{0};
    int64_t     totalDuration{0};
};

struct ExportPlan
{
    std::vector<PlannedAudio> audio;
    std::vector<PlannedGroup> groups;
};

/// The clips the export places, in order: for each script line, its best
/// confirmed clip (highest confidence: the one the MATCH tab shows). With
/// no script, every confirmed clip with a line, in `clips` order.
[[nodiscard]] std::vector<const SyncClip*> exportedClips(const Script* script,
                                                         const std::vector<SyncClip>& clips);

/// A clip's source ranges without its deleted regions (pieces under 50 ms
/// are dropped).
[[nodiscard]] std::vector<std::pair<double, double>> effectiveRanges(const SyncClip& clip);

/// The file name of `path` (exported clips are labelled with it).
[[nodiscard]] std::string fileNameOf(const std::string& path);

/// The plan for `clips` (from exportedClips). A line's character comes
/// from the script when it names one, else from the clip.
[[nodiscard]] ExportPlan planExport(const Script* script, const std::vector<const SyncClip*>& clips);

/// The sequence was exported to before: a clip carries a syncLine back-link.
[[nodiscard]] bool isReexport(const Timeline& timeline);

/// A locked track the export would have to change.
struct LockedTrack
{
    std::string track;
    std::string clipLabel;   ///< re-export: the back-linked clip on it
    int         line{-1};    ///< re-export: that clip's line
};
[[nodiscard]] std::optional<LockedTrack> lockedTrackInTheWay(const Timeline& timeline,
                                                             const ExportPlan& plan,
                                                             bool reexport);

/// Places one group's shot layers; returns the clips placed (0: none, e.g.
/// the character has no default shot).
using PlaceGroupVisualsFn = std::function<int(const PlannedGroup&)>;

/// Put `plan` on `timeline` (run lockedTrackInTheWay first). Without
/// `shots` a full export leaves the video tracks alone. Returns the number
/// of audio clips the timeline holds for the plan.
int applyExport(Timeline& timeline, const ExportPlan& plan, bool reexport, bool shots,
                const PlaceGroupVisualsFn& placeGroupVisuals);

} // namespace audiosync
} // namespace rt
