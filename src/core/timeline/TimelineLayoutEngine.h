/*
 * TimelineLayoutEngine — Pure-math layout for the NLE timeline.
 *
 * Step 12: Converts between:
 *   - Time domain (ticks at 48kHz)
 *   - Pixel domain (horizontal position in widget)
 *
 * Handles:
 *   - Zoom (pixels per second) with clamping
 *   - Horizontal scroll position
 *   - Timecode ruler marks with adaptive subdivisions
 *   - Track vertical layout (accumulated heights + scroll)
 *   - Clip rectangles in pixel space
 *   - Playhead position
 *   - NLE scrollbar state (zoom/scroll handle positions)
 *   - Timecode formatting (HH:MM:SS:FF)
 *
 * This class has NO Qt dependency — all math uses plain types and std.
 */

#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "Constants.h"

namespace rt {

// ── Ruler mark ──────────────────────────────────────────────────────────────

struct RulerMark
{
    int64_t tick{0};       ///< Time position
    bool    isMajor{false}; ///< Major (labeled) vs minor (tick only)
    std::string label;      ///< Timecode label (e.g. "00:01:30:00") — empty for minor
};

// ── Track layout info ───────────────────────────────────────────────────────

struct TrackLayoutInfo
{
    size_t trackIndex{0};   ///< Index into Timeline::track()
    float  y{0.0f};         ///< Top edge in pixel space (after scroll)
    float  height{80.0f};   ///< Height in pixels
    bool   visible{true};   ///< Whether any part is in the visible viewport
};

// ── Clip rectangle ──────────────────────────────────────────────────────────

struct ClipLayoutRect
{
    double x{0};       ///< Left edge in pixels (relative to track content area)
    double width{0};   ///< Width in pixels
    float  y{0};       ///< Top edge in pixels (track's y position)
    float  height{0};  ///< Height in pixels (track height minus padding)

    static constexpr float kTrackPadding = 2.0f; ///< Top/bottom padding within track
};

// ── Scrollbar state ─────────────────────────────────────────────────────────

struct ScrollbarState
{
    double handleStart{0};  ///< Left edge of scrollbar handle [0, 1] normalized
    double handleEnd{1};    ///< Right edge of scrollbar handle [0, 1] normalized
    double totalDuration{0}; ///< Total timeline duration in seconds

    /// Handle width as fraction of scrollbar
    double handleWidth() const { return handleEnd - handleStart; }
};

// ═════════════════════════════════════════════════════════════════════════════

class TimelineLayoutEngine
{
public:
    TimelineLayoutEngine() = default;

    // ── Time ↔ pixel conversion ─────────────────────────────────────────

    /// Convert a time tick to a pixel X position (relative to scroll).
    [[nodiscard]] double timeToPixelX(int64_t tick) const;

    /// Convert a pixel X position to time ticks.
    [[nodiscard]] int64_t pixelXToTime(double pixelX) const;

    /// Convert ticks to seconds.
    static constexpr double ticksToSeconds(int64_t ticks) noexcept
    {
        return static_cast<double>(ticks) / kTicksPerSecond;
    }

    /// Convert seconds to ticks.
    static constexpr int64_t secondsToTicks(double seconds) noexcept
    {
        return static_cast<int64_t>(seconds * kTicksPerSecond);
    }

    // ── Zoom ────────────────────────────────────────────────────────────

    /// Set the zoom level in pixels per second.
    void setPixelsPerSecond(double pps);

    /// Get the current zoom level.
    [[nodiscard]] double pixelsPerSecond() const noexcept { return m_pixelsPerSecond; }

    /// Zoom centered on a pixel X position by a multiplicative factor.
    /// factor > 1 zooms in, factor < 1 zooms out.
    void zoomAt(double pixelX, double factor);

    /// Zoom to fit a time range in the viewport width.
    void zoomToFit(int64_t startTick, int64_t endTick, double viewportWidth);

    // ── Scroll ──────────────────────────────────────────────────────────

    /// Set the horizontal scroll position in pixels.
    void setScrollX(double px);

    /// Get the horizontal scroll position.
    [[nodiscard]] double scrollX() const noexcept { return m_scrollX; }

    /// Set the viewport width in pixels.
    void setViewportWidth(double width);

    /// Get the viewport width.
    [[nodiscard]] double viewportWidth() const noexcept { return m_viewportWidth; }

    /// Set the total timeline duration (for scrollbar computation).
    void setTotalDuration(int64_t durationTicks);

    /// Get total duration in ticks.
    [[nodiscard]] int64_t totalDuration() const noexcept { return m_totalDuration; }

    // ── Visible range ───────────────────────────────────────────────────

    /// The earliest visible time tick.
    [[nodiscard]] int64_t visibleStartTime() const;

    /// The latest visible time tick.
    [[nodiscard]] int64_t visibleEndTime() const;

    /// Check if a time tick is within the visible viewport.
    [[nodiscard]] bool isTimeVisible(int64_t tick) const;

    /// Check if a clip's time range overlaps the visible viewport.
    [[nodiscard]] bool isRangeVisible(int64_t inTick, int64_t outTick) const;

    // ── Playhead ────────────────────────────────────────────────────────

    /// Get the playhead X position in pixels.
    [[nodiscard]] double playheadPixelX(int64_t playheadTick) const;

    /// Compute scroll X needed to center a time tick in the viewport.
    [[nodiscard]] double scrollXToCenter(int64_t tick) const;

    /// Compute scroll X needed to keep a tick just visible (auto-scroll).
    /// Returns current scrollX if already visible, else adjusts minimally.
    [[nodiscard]] double scrollXToShow(int64_t tick) const;

    // ── Ruler ───────────────────────────────────────────────────────────

    /// Set the framerate for timecode display (default: 24 fps).
    void setFrameRate(double fps);

    /// Get the framerate.
    [[nodiscard]] double frameRate() const noexcept { return m_frameRate; }

    /// Compute ruler marks for the currently visible range.
    [[nodiscard]] std::vector<RulerMark> computeRulerMarks() const;

    /// Format a tick as SMPTE timecode: HH:MM:SS:FF
    [[nodiscard]] std::string formatTimecode(int64_t tick) const;

    /// Format a tick as compact label (adaptive: "1:30", "0:05", etc.)
    [[nodiscard]] std::string formatCompactTime(int64_t tick) const;

    // ── Track layout ────────────────────────────────────────────────────

    /// Compute vertical layout for tracks.
    /// @param trackHeights  Heights of each track in pixels.
    /// @param scrollY       Vertical scroll offset.
    /// @param viewHeight    Height of the visible viewport.
    /// @return Layout info for each track (y position, visibility).
    [[nodiscard]] std::vector<TrackLayoutInfo> computeTrackLayout(
        const std::vector<float>& trackHeights,
        float scrollY,
        float viewHeight) const;

    /// Compute total height of all tracks.
    [[nodiscard]] static float totalTrackHeight(const std::vector<float>& trackHeights);

    // ── Clip layout ─────────────────────────────────────────────────────

    /// Compute the pixel rectangle for a clip.
    /// @param timelineIn   Clip's timeline in-point (ticks).
    /// @param duration     Clip's duration (ticks).
    /// @param trackY       Track's Y position in pixels.
    /// @param trackHeight  Track's height in pixels.
    [[nodiscard]] ClipLayoutRect clipRect(int64_t timelineIn,
                                          int64_t duration,
                                          float trackY,
                                          float trackHeight) const;

    // ── Scrollbar ───────────────────────────────────────────────────────

    /// Compute NLE scrollbar handle state.
    [[nodiscard]] ScrollbarState computeScrollbar() const;

    /// Apply a scrollbar handle drag (normalized position).
    void applyScrollbarDrag(double newHandleStart);

    /// Apply a scrollbar handle resize (zoom) from left or right handle.
    void applyScrollbarResize(double newHandleStart, double newHandleEnd);

    // ── Snapping ────────────────────────────────────────────────────────

    /// Snap a tick (or a signed offset) to the nearest frame boundary.
    [[nodiscard]] int64_t snapToFrame(int64_t tick) const;

    /// Start of the frame on screen at `tick` (the playhead parks here).
    [[nodiscard]] int64_t frameStartAt(int64_t tick) const;

    /// Snap a pixel X to the nearest ruler mark.
    [[nodiscard]] int64_t snapToGrid(int64_t tick) const;

    // ── Constants ───────────────────────────────────────────────────────

    static constexpr double kMinPixelsPerSecond = 0.05;    ///< Most zoomed out (~hours visible)
    static constexpr double kMaxPixelsPerSecond = 20000.0; ///< Most zoomed in
    static constexpr double kDefaultPixelsPerSecond = 100.0;
    static constexpr double kMinMajorMarkSpacing = 80.0;   ///< Min pixels between major marks
    static constexpr double kMinMinorMarkSpacing = 8.0;    ///< Min pixels between minor marks
    static constexpr double kDefaultTrackHeight = 80.0f;

private:
    /// Returns the virtual scrollable extent in seconds — extends well past
    /// the last clip so the user can pan/zoom into empty space (Premiere style).
    [[nodiscard]] double paddedScrollableSeconds() const;

    double  m_pixelsPerSecond{kDefaultPixelsPerSecond};
    double  m_scrollX{0};
    double  m_viewportWidth{800};
    int64_t m_totalDuration{0};
    double  m_frameRate{60.0};

    // ── Ruler mark cache ────────────────────────────────────────────────
    mutable std::vector<RulerMark> m_cachedRulerMarks;
    mutable double  m_cachedRulerScrollX{-1};
    mutable double  m_cachedRulerPPS{-1};
    mutable double  m_cachedRulerViewportW{-1};

    /// Clamp pixels-per-second to valid range.
    static double clampPPS(double pps);

    /// Choose the best ruler interval based on current zoom.
    struct RulerInterval
    {
        double majorSeconds;    ///< Seconds between major marks
        int    minorSubdivisions; ///< Number of minor marks between majors
    };
    [[nodiscard]] RulerInterval chooseRulerInterval() const;
};

} // namespace rt
