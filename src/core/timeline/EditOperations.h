/* EditOperations.h — pure-logic NLE editing operations (commands for undo/redo). */

#pragma once

#include "Constants.h"
#include "timeline/Clip.h"

#include <climits>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// Forward declarations
class Timeline;
class Track;
class Command;
class CommandStack;

// ─── Selection ───────────────────────────────────────────────────────────────

/// Identifies a clip on the timeline by track index and clip ID.
struct ClipRef
{
    size_t   trackIndex{0};
    uint64_t clipId{0};

    bool operator==(const ClipRef& o) const noexcept
    {
        return trackIndex == o.trackIndex && clipId == o.clipId;
    }
    bool operator!=(const ClipRef& o) const noexcept { return !(*this == o); }
};

/// Rectangle in time/track space for marquee selection.
struct TimelineRect
{
    int64_t startTick{0};
    int64_t endTick{0};
    size_t  topTrack{0};
    size_t  bottomTrack{0};
};

/// Manages the set of selected clips on the timeline.
class SelectionSet
{
public:
    SelectionSet() = default;

    /// Select a single clip (clears previous selection unless shift).
    void selectClip(const ClipRef& ref, bool addToSelection = false);

    /// Deselect a specific clip.
    void deselectClip(const ClipRef& ref);

    /// Toggle selection of a clip.
    void toggleClip(const ClipRef& ref);

    /// Select all clips within a rectangle (marquee).
    void selectRect(const Timeline& timeline,
                    const TimelineRect& rect);

    /// Select all clips on the timeline.
    void selectAll(const Timeline& timeline);

    /// Clear all selection.
    void clear();

    /// Is a specific clip selected?
    [[nodiscard]] bool isSelected(const ClipRef& ref) const;

    /// Is a clip selected by its ID (searches all tracks)?
    [[nodiscard]] bool isSelectedById(uint64_t clipId) const;

    /// Number of selected clips.
    [[nodiscard]] size_t count() const noexcept { return m_selected.size(); }

    /// Is the selection empty?
    [[nodiscard]] bool empty() const noexcept { return m_selected.empty(); }

    /// Access the selected clips.
    [[nodiscard]] const std::vector<ClipRef>& clips() const noexcept { return m_selected; }

    /// Get the single selected clip (or nullopt if 0 or >1).
    [[nodiscard]] std::optional<ClipRef> singleSelection() const;

private:
    std::vector<ClipRef> m_selected;
};

// ─── Snap Engine ─────────────────────────────────────────────────────────────

/// A potential snap target.
struct SnapTarget
{
    int64_t tick{0};

    enum class Type : uint8_t
    {
        ClipEdge,
        Playhead,
        Marker,
        GridLine,
        InPoint,
        OutPoint
    };
    Type type{Type::GridLine};
};

/// Result of a snap operation.
struct SnapResult
{
    int64_t snappedTick{0};     ///< The snapped position
    bool    didSnap{false};     ///< Whether snapping occurred
    int64_t delta{0};           ///< Offset applied by snapping (snapped - original)
    SnapTarget::Type snapType{SnapTarget::Type::GridLine};
};

/// Collects snap targets and performs snapping.
class SnapEngine
{
public:
    SnapEngine() = default;

    /// Enable/disable snapping globally.
    void setEnabled(bool enabled) noexcept { m_enabled = enabled; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }

    /// Set snap threshold in pixels (UI configurable).
    void setThresholdPixels(double pixels) noexcept { m_thresholdPx = pixels; }
    [[nodiscard]] double thresholdPixels() const noexcept { return m_thresholdPx; }

    /// Set the pixels-per-second for pixel→tick threshold conversion.
    void setPixelsPerSecond(double pps) noexcept { m_pps = pps; }

    /// Build the snap target list from the current timeline state.
    void buildTargets(const Timeline& timeline,
                      int64_t playhead,
                      double frameRate,
                      const std::vector<uint64_t>& excludeClipIds = {});

    /// Clear all targets.
    void clearTargets();

    /// Add a custom target (e.g. from ruler grid).
    void addTarget(const SnapTarget& target);

    /// Snap a tick to the nearest target within threshold.
    [[nodiscard]] SnapResult snap(int64_t tick) const;

    /// Snap a pair of ticks (e.g. clip in + out) — returns the best snap.
    [[nodiscard]] SnapResult snapPair(int64_t tickA, int64_t tickB) const;

    /// The current targets (for UI visualization of snap lines).
    [[nodiscard]] const std::vector<SnapTarget>& targets() const noexcept { return m_targets; }

    /// Drop sticky-snap state. Call when a drag/operation ends so the next
    /// session starts fresh. Also invoked automatically by buildTargets().
    void resetHysteresis() const noexcept;

    /// Default snap threshold in pixels.
    static constexpr double kDefaultThresholdPx = 10.0;

    /// Release threshold = attract threshold * this. Sticky snap: once
    /// locked onto a seam, the user must drag past a wider zone to break
    /// free.  Kept modest (1.3) so adjacent-clip trimming doesn't feel
    /// "stuck" when making fine frame adjustments.
    static constexpr double kReleaseMultiplier = 1.3;

    /// Convert pixel threshold to a tick threshold. Public so callers
    /// driving multi-clip snap can probe edges via findNearestAttract.
    [[nodiscard]] int64_t thresholdTicks() const;

    struct AttractHit {
        int64_t          tick{0};
        int64_t          dist{std::numeric_limits<int64_t>::max()};
        SnapTarget::Type type{SnapTarget::Type::GridLine};
        bool             found{false};
    };
    /// Non-hysteretic nearest-target probe. Use this when iterating across
    /// many candidate ticks (e.g. every edge of every dragged clip in a
    /// multi-selection drag) where the stuck-target state of snap()/
    /// snapPair() would interfere across probes.
    [[nodiscard]] AttractHit findNearestAttract(int64_t tick, int64_t attractTicks) const;

private:
    bool   m_enabled{true};
    double m_thresholdPx{kDefaultThresholdPx};
    double m_pps{100.0};
    std::vector<SnapTarget> m_targets;

    /// Hysteresis: tick of the target we're currently locked onto (or
    /// INT64_MIN when not stuck). Mutable so snap() / snapPair() can remain
    /// logically const while caching per-session lock state.
    mutable int64_t          m_stuckTick{INT64_MIN};
    mutable SnapTarget::Type m_stuckType{SnapTarget::Type::GridLine};
};

// ─── Clipboard ───────────────────────────────────────────────────────────────

/// Holds cloned clips for copy/paste operations.
struct ClipboardContents
{
    struct Entry
    {
        size_t                trackIndex;  ///< Original track index
        int64_t               relativeTime;  ///< Offset from earliest clip
        std::unique_ptr<Clip> clip;         ///< Cloned clip data
    };
    std::vector<Entry> entries;

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    void clear() { entries.clear(); }
};

// ─── Edit Tool Types ─────────────────────────────────────────────────────────

/// The active editing tool.
enum class EditTool : uint8_t
{
    Selection,  // A — default pointer/selection
    Razor,      // B — split at cursor
    Rolling,    // R — rolling edit
    Ripple,     // N — ripple edit
    Slip,       // Y — slip source content
    Slide,      // U — slide clip
    Text,       // T — create / edit graphic text
    Zoom,       // Z — click to zoom in, Alt+click to zoom out
    Eyedropper  // (internal) — pick color from program monitor
};

/// Edge of a clip (for trim operations).
enum class ClipEdge : uint8_t
{
    Head,   // Left edge (timelineIn)
    Tail    // Right edge (timelineOut)
};

// ─── EditOperations (stateless helper — creates commands) ────────────────────

class EditOperations
{
public:
    EditOperations() = delete; // All static

    // ── Razor / Split ───────────────────────────────────────────────────

    /// Split a clip at the given time. Returns a compound command that
    /// trims the original and inserts the new right-half clip.
    [[nodiscard]] static std::unique_ptr<Command> splitClip(
        Timeline& timeline, size_t trackIndex, uint64_t clipId, int64_t splitTime);

    /// Split all clips at the playhead position across all unlocked tracks.
    [[nodiscard]] static std::unique_ptr<Command> splitAllAtPlayhead(
        Timeline& timeline, int64_t playhead);

    // ── Trim ────────────────────────────────────────────────────────────

    /// Trim a clip's head (left edge) or tail (right edge).
    /// Returns the trim command with clamped values.
    [[nodiscard]] static std::unique_ptr<Command> trimClip(
        Timeline& timeline, size_t trackIndex, uint64_t clipId,
        ClipEdge edge, int64_t newEdgeTime);

    // ── Rolling Edit ────────────────────────────────────────────────────

    /// Rolling edit: move an edit point between two adjacent clips.
    /// The total duration remains the same.
    [[nodiscard]] static std::unique_ptr<Command> rollingEdit(
        Timeline& timeline, size_t trackIndex,
        uint64_t leftClipId, uint64_t rightClipId, int64_t newEditPoint);

    // ── Ripple Edit ─────────────────────────────────────────────────────

    /// Ripple trim: trim a clip and shift all subsequent clips to fill/open the gap.
    [[nodiscard]] static std::unique_ptr<Command> rippleTrim(
        Timeline& timeline, size_t trackIndex, uint64_t clipId,
        ClipEdge edge, int64_t newEdgeTime);

    /// Ripple delete: remove selected clips and close the gaps.
    [[nodiscard]] static std::unique_ptr<Command> rippleDelete(
        Timeline& timeline, const SelectionSet& selection);

    /// Close a gap: shift all clips at or after gapEnd left by (gapEnd - gapStart).
    [[nodiscard]] static std::unique_ptr<Command> closeGap(
        Timeline& timeline, size_t trackIndex,
        int64_t gapStart, int64_t gapEnd);

    /// Open a gap: shift all clips whose start is at or after insertTick
    /// right by insertDuration. Used by Ctrl+drop to ripple-push existing
    /// clips out of the way so the dropped clip inserts rather than
    /// overwriting. Sync-locked tracks shift too, matching closeGap().
    /// Returns nullptr if nothing needed to move.
    [[nodiscard]] static std::unique_ptr<Command> openGap(
        Timeline& timeline, size_t trackIndex,
        int64_t insertTick, int64_t insertDuration);

    // ── Slip ────────────────────────────────────────────────────────────

    /// Slip: shift the source in-point without changing timeline position.
    [[nodiscard]] static std::unique_ptr<Command> slipClip(
        Timeline& timeline, size_t trackIndex, uint64_t clipId,
        int64_t sourceInDelta);

    // ── Slide ───────────────────────────────────────────────────────────

    /// Slide: move a clip on the timeline, adjusting neighbors' durations.
    [[nodiscard]] static std::unique_ptr<Command> slideClip(
        Timeline& timeline, size_t trackIndex, uint64_t clipId,
        int64_t slideDelta);

    // ── Move ────────────────────────────────────────────────────────────

    /// Move a clip to a new position (same track).
    [[nodiscard]] static std::unique_ptr<Command> moveClip(
        Timeline& timeline, size_t trackIndex, uint64_t clipId,
        int64_t newTimelineIn);

    /// Move a clip to a different track.
    [[nodiscard]] static std::unique_ptr<Command> moveClipToTrack(
        Timeline& timeline, size_t fromTrack, size_t toTrack,
        uint64_t clipId, int64_t newTimelineIn);

    // ── Delete ──────────────────────────────────────────────────────────

    /// Delete all selected clips (no gap close — use rippleDelete for that).
    [[nodiscard]] static std::unique_ptr<Command> deleteSelection(
        Timeline& timeline, const SelectionSet& selection);
    // ── Lift / Extract (I/O range) ──────────────────────────────────

    /// Lift: remove all clip content between in and out points (leave gap).
    /// Splits clips at boundaries if necessary, then removes the middle.
    [[nodiscard]] static std::unique_ptr<Command> liftInOut(
        Timeline& timeline, int64_t inPoint, int64_t outPoint);

    /// Extract: remove all clip content between in and out points and close gaps.
    /// Same as Lift but ripple-shifts subsequent clips.
    [[nodiscard]] static std::unique_ptr<Command> extractInOut(
        Timeline& timeline, int64_t inPoint, int64_t outPoint);

    // ── Auto Gap Close ──────────────────────────────────────────────────

    /// Close all gaps between clips on every unlocked track.
    [[nodiscard]] static std::unique_ptr<Command> closeAllGaps(Timeline& timeline);

    // ── Clipboard ───────────────────────────────────────────────────────

    /// Copy selected clips to clipboard.
    static void copySelection(const Timeline& timeline,
                               const SelectionSet& selection,
                               ClipboardContents& clipboard);

    /// Cut selected clips (copy + delete).
    [[nodiscard]] static std::unique_ptr<Command> cutSelection(
        Timeline& timeline, const SelectionSet& selection,
        ClipboardContents& clipboard);

    /// Paste clipboard contents at the playhead position.
    [[nodiscard]] static std::unique_ptr<Command> paste(
        Timeline& timeline, const ClipboardContents& clipboard,
        int64_t playhead);

    /// Insert-paste: paste clipboard at playhead, pushing subsequent clips right
    /// on targeted tracks (Premiere Pro "," key).
    [[nodiscard]] static std::unique_ptr<Command> pasteInsert(
        Timeline& timeline, const ClipboardContents& clipboard,
        int64_t playhead);

    /// Duplicate selected clips (paste in-place offset by a small amount).
    [[nodiscard]] static std::unique_ptr<Command> duplicateSelection(
        Timeline& timeline, const SelectionSet& selection);

    // ── Overwrite / Overlap Resolution ──────────────────────────────────

    /// Resolve overlaps on a track after a clip has been moved.
    /// Trims or removes other clips that overlap with the specified clip.
    /// Returns a CompoundCommand, or nullptr if no overlaps exist.
    [[nodiscard]] static std::unique_ptr<Command> resolveOverlaps(
        Timeline& timeline, size_t trackIndex, uint64_t movedClipId);

    // ── In/Out Points ───────────────────────────────────────────────────

    /// Set in-point at playhead.
    static void setInPoint(Timeline& timeline, int64_t playhead);

    /// Set out-point at playhead.
    static void setOutPoint(Timeline& timeline, int64_t playhead);

    /// Clear in/out points.
    static void clearInOutPoints(Timeline& timeline);

    // ── Navigation ──────────────────────────────────────────────────────

    /// Find the next edit point (clip edge) from the current time.
    [[nodiscard]] static int64_t nextEditPoint(const Timeline& timeline, int64_t fromTime);

    /// Find the previous edit point (clip edge) from the current time.
    [[nodiscard]] static int64_t prevEditPoint(const Timeline& timeline, int64_t fromTime);

    // ── Match Frame ───────────────────────────────────────────────────

    /// Match frame result: identifies the source clip and its source time.
    struct MatchFrameResult
    {
        size_t      trackIndex{0};
        uint64_t    clipId{0};
        int64_t     sourceTime{0};  ///< Time within the source media
        bool        valid{false};
    };

    /// Find the topmost video clip at the playhead and return its source time.
    [[nodiscard]] static MatchFrameResult matchFrame(
        const Timeline& timeline, int64_t playhead);

    /// Extended match frame result that includes the source media path.
    struct MatchFrameResultEx : MatchFrameResult
    {
        std::string mediaPath;  ///< Source media file path for loading in SourceMonitor
    };

    /// Match frame with source media path (for loading in SourceMonitor).
    [[nodiscard]] static MatchFrameResultEx matchFrameEx(
        const Timeline& timeline, int64_t playhead);

    // ── Helpers ─────────────────────────────────────────────────────────

    /// Find the clip at a given time on a given track. Returns nullptr if none.
    [[nodiscard]] static const Clip* clipAtTime(const Track& track, int64_t tick);

    /// Find adjacent clip pairs around an edit point.
    struct EditPoint
    {
        const Clip* leftClip{nullptr};
        const Clip* rightClip{nullptr};
        int64_t     editTime{0};
    };
    [[nodiscard]] static EditPoint findEditPoint(const Track& track, int64_t nearTime);

private:
    /// Internal split that accepts an optional group-remap table.
    /// When splitting multiple clips from the same group, the remap table
    /// ensures all right-half clones receive the same new group ID.
    [[nodiscard]] static std::unique_ptr<Command> splitClipInternal(
        Timeline& timeline, size_t trackIndex, uint64_t clipId, int64_t splitTime,
        std::unordered_map<uint64_t, uint64_t>* groupRemap);
};

} // namespace rt
