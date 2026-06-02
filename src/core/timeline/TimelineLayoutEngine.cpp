/*
 * TimelineLayoutEngine.cpp — Pure-math timeline layout.
 */

#include "timeline/TimelineLayoutEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rt {

// ─── Time ↔ Pixel conversion ────────────────────────────────────────────────

double TimelineLayoutEngine::timeToPixelX(int64_t tick) const
{
    double seconds = ticksToSeconds(tick);
    return seconds * m_pixelsPerSecond - m_scrollX;
}

int64_t TimelineLayoutEngine::pixelXToTime(double pixelX) const
{
    double seconds = (pixelX + m_scrollX) / m_pixelsPerSecond;
    return secondsToTicks(std::max(seconds, 0.0));
}

// ─── Zoom ────────────────────────────────────────────────────────────────────

void TimelineLayoutEngine::setPixelsPerSecond(double pps)
{
    m_pixelsPerSecond = clampPPS(pps);
}

void TimelineLayoutEngine::zoomAt(double pixelX, double factor)
{
    // Compute the time under the cursor BEFORE zoom (in seconds, no tick rounding).
    // Using direct double math here ensures the cursor stays exactly anchored even
    // at the timeline edges where int64-tick round-trips would shift by 1 sample.
    const double timeSeconds = (pixelX + m_scrollX) / m_pixelsPerSecond;

    // Apply zoom (clamped). If the clamp prevents change, scroll stays put.
    const double newPPS = clampPPS(m_pixelsPerSecond * factor);
    if (newPPS == m_pixelsPerSecond) return;
    m_pixelsPerSecond = newPPS;

    // Re-position scroll so the same time stays at the same pixel.
    //   pixelX = timeSeconds * newPPS - scrollX  →  scrollX = timeSeconds * newPPS - pixelX
    m_scrollX = timeSeconds * newPPS - pixelX;
    m_scrollX = std::max(m_scrollX, 0.0);
}

void TimelineLayoutEngine::zoomToFit(int64_t startTick, int64_t endTick, double viewportWidth)
{
    if (endTick <= startTick || viewportWidth <= 0) return;

    double durationSeconds = ticksToSeconds(endTick - startTick);
    m_pixelsPerSecond = clampPPS(viewportWidth / durationSeconds);
    m_scrollX = ticksToSeconds(startTick) * m_pixelsPerSecond;
    m_scrollX = std::max(m_scrollX, 0.0);
}

// ─── Scroll ──────────────────────────────────────────────────────────────────

void TimelineLayoutEngine::setScrollX(double px)
{
    m_scrollX = std::max(px, 0.0);
}

void TimelineLayoutEngine::setViewportWidth(double width)
{
    m_viewportWidth = std::max(width, 1.0);
}

void TimelineLayoutEngine::setTotalDuration(int64_t durationTicks)
{
    m_totalDuration = std::max(durationTicks, int64_t(0));
}

// ─── Visible range ───────────────────────────────────────────────────────────

int64_t TimelineLayoutEngine::visibleStartTime() const
{
    return pixelXToTime(0);
}

int64_t TimelineLayoutEngine::visibleEndTime() const
{
    return pixelXToTime(m_viewportWidth);
}

bool TimelineLayoutEngine::isTimeVisible(int64_t tick) const
{
    double px = timeToPixelX(tick);
    return px >= 0 && px <= m_viewportWidth;
}

bool TimelineLayoutEngine::isRangeVisible(int64_t inTick, int64_t outTick) const
{
    double pxIn  = timeToPixelX(inTick);
    double pxOut = timeToPixelX(outTick);
    return pxOut >= 0 && pxIn <= m_viewportWidth;
}

// ─── Playhead ────────────────────────────────────────────────────────────────

double TimelineLayoutEngine::playheadPixelX(int64_t playheadTick) const
{
    return timeToPixelX(playheadTick);
}

double TimelineLayoutEngine::scrollXToCenter(int64_t tick) const
{
    double seconds = ticksToSeconds(tick);
    double targetScrollX = seconds * m_pixelsPerSecond - m_viewportWidth * 0.5;
    return std::max(targetScrollX, 0.0);
}

double TimelineLayoutEngine::scrollXToShow(int64_t tick) const
{
    double px = timeToPixelX(tick);
    if (px >= 0 && px <= m_viewportWidth)
        return m_scrollX; // Already visible

    double seconds = ticksToSeconds(tick);
    double absoluteX = seconds * m_pixelsPerSecond;

    // Page-turn scroll: jump a full viewport width when the playhead
    // reaches the edge, like Premiere Pro's default auto-scroll.
    return std::max(absoluteX - m_viewportWidth * 0.05, 0.0);
}

// ─── Framerate & Timecode ────────────────────────────────────────────────────

void TimelineLayoutEngine::setFrameRate(double fps)
{
    m_frameRate = std::max(fps, 1.0);
}

std::string TimelineLayoutEngine::formatTimecode(int64_t tick) const
{
    double totalSeconds = ticksToSeconds(std::max(tick, int64_t(0)));

    // Drop-frame timecode applies ONLY to the fractional NTSC rates
    // (29.97 = 30000/1001, 59.94 = 60000/1001), never to an exact 30/60.
    // A loose epsilon here previously matched exact 30.0 fps (|30 - 29.97| =
    // 0.03), so 30 fps projects rendered NTSC ";" timecode with shifted frame
    // numbers. Gate on the rate actually being non-integer.
    bool dropFrame = false;
    int nominalRate = static_cast<int>(std::round(m_frameRate));
    const bool fractionalRate =
        std::abs(m_frameRate - std::round(m_frameRate)) > 1e-3;
    if (fractionalRate && (nominalRate == 30 || nominalRate == 60)) {
        dropFrame = true;
    }

    if (dropFrame) {
        // SMPTE drop-frame algorithm
        int dropFrames = (nominalRate == 60) ? 4 : 2;  // frames to drop
        int framesPerMinute = nominalRate * 60 - dropFrames;
        int framesPer10Min  = framesPerMinute * 10 + dropFrames;

        int totalFrames = static_cast<int>(std::round(totalSeconds * m_frameRate));

        int m  = totalFrames % framesPer10Min;

        int adjustedFrames = totalFrames;
        if (m > dropFrames) {
            adjustedFrames += dropFrames * (1 + (m - dropFrames) / framesPerMinute);
        }

        int frames  = adjustedFrames % nominalRate;
        int seconds = (adjustedFrames / nominalRate) % 60;
        int minutes = (adjustedFrames / nominalRate / 60) % 60;
        int hours   = adjustedFrames / nominalRate / 3600;

        char buf[20];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d;%02d", hours, minutes, seconds, frames);
        return std::string(buf);
    }

    // Non-drop-frame
    int totalFrames = static_cast<int>(totalSeconds * m_frameRate);
    int frames  = totalFrames % static_cast<int>(m_frameRate);
    int seconds = (totalFrames / static_cast<int>(m_frameRate)) % 60;
    int minutes = (totalFrames / static_cast<int>(m_frameRate) / 60) % 60;
    int hours   = totalFrames / static_cast<int>(m_frameRate) / 3600;

    char buf[20];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hours, minutes, seconds, frames);
    return std::string(buf);
}

std::string TimelineLayoutEngine::formatCompactTime(int64_t tick) const
{
    double totalSeconds = ticksToSeconds(std::max(tick, int64_t(0)));
    int totalSecs = static_cast<int>(totalSeconds);
    int minutes = totalSecs / 60;
    int seconds = totalSecs % 60;

    char buf[16];
    if (minutes >= 60)
    {
        int hours = minutes / 60;
        minutes %= 60;
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, seconds);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, seconds);
    }
    return std::string(buf);
}

// ─── Ruler ───────────────────────────────────────────────────────────────────

TimelineLayoutEngine::RulerInterval TimelineLayoutEngine::chooseRulerInterval() const
{
    // Standard NLE ruler intervals: major interval in seconds, minor subdivisions
    struct Candidate
    {
        double majorSeconds;
        int    minorSubdivisions;
    };

    static constexpr Candidate candidates[] = {
        // Frame-level (at common framerates)
        {1.0 / 24.0,   1},   // 1 frame @24fps
        {1.0 / 12.0,   2},   // 2 frames @24fps
        {5.0 / 24.0,   5},   // 5 frames @24fps
        {10.0 / 24.0,  5},   // 10 frames @24fps
        // Sub-second
        {0.5,    5},   // 0.5s (minor every 0.1s)
        {1.0,    4},   // 1s (minor every 0.25s)
        {2.0,    4},   // 2s
        {5.0,    5},   // 5s
        {10.0,   5},   // 10s
        {15.0,   3},   // 15s
        {30.0,   6},   // 30s
        // Minutes
        {60.0,     4},  // 1min
        {120.0,    4},  // 2min
        {300.0,    5},  // 5min
        {600.0,    5},  // 10min
        {1800.0,   6},  // 30min
        {3600.0,   4},  // 1hr
    };

    // Pick the smallest interval where major marks are at least kMinMajorMarkSpacing apart
    for (const auto& c : candidates)
    {
        double majorPixels = c.majorSeconds * m_pixelsPerSecond;
        if (majorPixels >= kMinMajorMarkSpacing)
            return {c.majorSeconds, c.minorSubdivisions};
    }

    // Fallback: 1 hour
    return {3600.0, 4};
}

std::vector<RulerMark> TimelineLayoutEngine::computeRulerMarks() const
{
    // Return cached marks if scroll/zoom/viewport haven't changed.
    if (m_cachedRulerScrollX == m_scrollX &&
        m_cachedRulerPPS == m_pixelsPerSecond &&
        m_cachedRulerViewportW == m_viewportWidth &&
        !m_cachedRulerMarks.empty())
    {
        return m_cachedRulerMarks;
    }

    std::vector<RulerMark> marks;

    auto interval = chooseRulerInterval();
    double majorSec = interval.majorSeconds;
    int    subdivs  = interval.minorSubdivisions;
    double minorSec = majorSec / subdivs;

    // Compute visible range in seconds
    double startSec = m_scrollX / m_pixelsPerSecond;
    double endSec   = (m_scrollX + m_viewportWidth) / m_pixelsPerSecond;

    // Align start to a major mark
    double firstMajor = std::floor(startSec / majorSec) * majorSec;

    // Generate marks with some padding
    for (double t = firstMajor; t <= endSec + majorSec; t += minorSec)
    {
        if (t < 0) continue;

        int64_t tick = secondsToTicks(t);
        double px = timeToPixelX(tick);

        // Skip marks well outside viewport
        if (px < -50 || px > m_viewportWidth + 50) continue;

        // Is this a major mark? Check if t is a multiple of majorSec
        double remainder = std::fmod(t, majorSec);
        bool isMajor = (std::abs(remainder) < minorSec * 0.01) ||
                        (std::abs(remainder - majorSec) < minorSec * 0.01);

        RulerMark mark;
        mark.tick = tick;
        mark.isMajor = isMajor;
        if (isMajor)
        {
            // Choose label format based on zoom level
            if (m_pixelsPerSecond > 500.0)
                mark.label = formatTimecode(tick);  // Frame-level detail
            else
                mark.label = formatCompactTime(tick);
        }
        marks.push_back(std::move(mark));
    }

    // Cache the result.
    m_cachedRulerScrollX = m_scrollX;
    m_cachedRulerPPS = m_pixelsPerSecond;
    m_cachedRulerViewportW = m_viewportWidth;
    m_cachedRulerMarks = marks;

    return marks;
}

// ─── Track layout ────────────────────────────────────────────────────────────

std::vector<TrackLayoutInfo> TimelineLayoutEngine::computeTrackLayout(
    const std::vector<float>& trackHeights,
    float scrollY,
    float viewHeight) const
{
    std::vector<TrackLayoutInfo> layout;
    layout.reserve(trackHeights.size());

    float accumulatedY = 0.0f;
    for (size_t i = 0; i < trackHeights.size(); ++i)
    {
        TrackLayoutInfo info;
        info.trackIndex = i;
        info.height = trackHeights[i];
        info.y = accumulatedY - scrollY;

        // Check if any part of this track is visible
        float trackBottom = info.y + info.height;
        info.visible = (trackBottom > 0.0f) && (info.y < viewHeight);

        layout.push_back(info);
        accumulatedY += trackHeights[i];
    }

    return layout;
}

float TimelineLayoutEngine::totalTrackHeight(const std::vector<float>& trackHeights)
{
    float total = 0.0f;
    for (float h : trackHeights) total += h;
    return total;
}

// ─── Clip layout ─────────────────────────────────────────────────────────────

ClipLayoutRect TimelineLayoutEngine::clipRect(int64_t timelineIn,
                                               int64_t duration,
                                               float trackY,
                                               float trackHeight) const
{
    ClipLayoutRect rect;
    rect.x = timeToPixelX(timelineIn);
    rect.width = (ticksToSeconds(duration)) * m_pixelsPerSecond;
    rect.y = trackY + ClipLayoutRect::kTrackPadding;
    rect.height = trackHeight - 2.0f * ClipLayoutRect::kTrackPadding;
    return rect;
}

// ─── Scrollbar ───────────────────────────────────────────────────────────────

ScrollbarState TimelineLayoutEngine::computeScrollbar() const
{
    ScrollbarState state;
    double totalSeconds = ticksToSeconds(m_totalDuration);
    state.totalDuration = totalSeconds;

    if (totalSeconds <= 0)
    {
        // Empty timeline: still allow zoom/pan into empty space.
        double paddedSeconds = paddedScrollableSeconds();
        double totalPixels = paddedSeconds * m_pixelsPerSecond;
        if (totalPixels <= 0) {
            state.handleStart = 0;
            state.handleEnd = 1;
            return state;
        }
        state.handleStart = m_scrollX / totalPixels;
        state.handleEnd   = (m_scrollX + m_viewportWidth) / totalPixels;
        state.handleStart = std::clamp(state.handleStart, 0.0, 1.0);
        state.handleEnd   = std::clamp(state.handleEnd, 0.0, 1.0);
        return state;
    }

    // Premiere-style: the scrollable virtual range extends well beyond
    // the last clip so the user can always pan/zoom into empty space.
    double paddedSeconds = paddedScrollableSeconds();
    double totalPixels = paddedSeconds * m_pixelsPerSecond;

    if (totalPixels <= 0)
    {
        state.handleStart = 0;
        state.handleEnd = 1;
        return state;
    }

    state.handleStart = m_scrollX / totalPixels;
    state.handleEnd   = (m_scrollX + m_viewportWidth) / totalPixels;

    // Clamp
    state.handleStart = std::clamp(state.handleStart, 0.0, 1.0);
    state.handleEnd   = std::clamp(state.handleEnd, 0.0, 1.0);

    // Ensure minimum handle width
    constexpr double kMinHandleWidth = 0.02;
    if (state.handleEnd - state.handleStart < kMinHandleWidth)
        state.handleEnd = std::min(state.handleStart + kMinHandleWidth, 1.0);

    return state;
}

void TimelineLayoutEngine::applyScrollbarDrag(double newHandleStart)
{
    double paddedSeconds = paddedScrollableSeconds();
    double totalPixels = paddedSeconds * m_pixelsPerSecond;

    m_scrollX = std::max(newHandleStart * totalPixels, 0.0);
}

void TimelineLayoutEngine::applyScrollbarResize(double newHandleStart, double newHandleEnd)
{
    double paddedSeconds = paddedScrollableSeconds();

    if (newHandleEnd <= newHandleStart || paddedSeconds <= 0) return;

    // The new visible seconds range
    double visStartSec = newHandleStart * paddedSeconds;
    double visEndSec   = newHandleEnd * paddedSeconds;
    double visDuration = visEndSec - visStartSec;

    if (visDuration <= 0) return;

    // Compute new PPS
    m_pixelsPerSecond = clampPPS(m_viewportWidth / visDuration);
    m_scrollX = std::max(visStartSec * m_pixelsPerSecond, 0.0);
}

// ─── Snapping ────────────────────────────────────────────────────────────────

int64_t TimelineLayoutEngine::snapToFrame(int64_t tick) const
{
    double ticksPerFrame = kTicksPerSecond / m_frameRate;
    int64_t frame = static_cast<int64_t>(std::round(tick / ticksPerFrame));
    return static_cast<int64_t>(frame * ticksPerFrame);
}

int64_t TimelineLayoutEngine::snapToGrid(int64_t tick) const
{
    auto interval = chooseRulerInterval();
    double majorTicks = interval.majorSeconds * kTicksPerSecond;
    double minorTicks = majorTicks / interval.minorSubdivisions;

    int64_t snapped = static_cast<int64_t>(std::round(tick / minorTicks) * minorTicks);
    return std::max(snapped, int64_t(0));
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

double TimelineLayoutEngine::clampPPS(double pps)
{
    return std::clamp(pps, kMinPixelsPerSecond, kMaxPixelsPerSecond);
}

double TimelineLayoutEngine::paddedScrollableSeconds() const
{
    // Premiere-style: the user can always pan and zoom into empty space
    // beyond the last clip. The scrollable virtual range is the largest of:
    //   - 1.5× the content duration
    //   - content duration + 60 seconds
    //   - 2× the current viewport's seconds (so zooming further out always
    //     reveals more empty space)
    //   - a 60-second floor for empty timelines
    const double totalSeconds = ticksToSeconds(m_totalDuration);
    const double viewportSeconds =
        (m_pixelsPerSecond > 0.0) ? (m_viewportWidth / m_pixelsPerSecond) : 0.0;
    return std::max({
        totalSeconds * 1.5,
        totalSeconds + 60.0,
        viewportSeconds * 2.0,
        60.0
    });
}

} // namespace rt
