/*
 * Track — a single video or audio track in the timeline.
 *
 * Contains an ordered, non-overlapping sequence of clips with optional
 * transitions between adjacent clips. Supports lock, mute, solo, and naming.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "timeline/Transition.h"

namespace rt {

// Forward declarations
class Clip;

/// Track type
enum class TrackType : uint8_t
{
    Video,
    Audio
};

class Track
{
public:
    explicit Track(TrackType type, const std::string& name = "");
    ~Track();

    // ── Properties ──────────────────────────────────────────────────────
    [[nodiscard]] TrackType          type() const noexcept { return m_type; }
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    [[nodiscard]] bool isLocked() const noexcept { return m_locked; }
    [[nodiscard]] bool isMuted()  const noexcept { return m_muted; }
    [[nodiscard]] bool isSoloed() const noexcept { return m_soloed; }
    void setLocked(bool v) noexcept { m_locked = v; }
    void setMuted(bool v)  noexcept { m_muted = v; }
    void setSoloed(bool v) noexcept { m_soloed = v; }

    /// Track targeting — targeted tracks receive paste/insert edits (Premiere Pro V1/A1 patch).
    [[nodiscard]] bool isTargeted() const noexcept { return m_targeted; }
    void setTargeted(bool v) noexcept { m_targeted = v; }

    /// Track collapse — collapsed tracks show a minimal thin header.
    [[nodiscard]] bool isCollapsed() const noexcept { return m_collapsed; }
    void setCollapsed(bool v) noexcept { m_collapsed = v; }

    /// Sync Lock — sync-locked tracks shift when ripple edits occur on other tracks.
    [[nodiscard]] bool isSyncLocked() const noexcept { return m_syncLocked; }
    void setSyncLocked(bool v) noexcept { m_syncLocked = v; }

    /// Divider — visual separator track; cannot hold clips, has no header controls.
    [[nodiscard]] bool isDivider() const noexcept { return m_isDivider; }
    void setDivider(bool v) noexcept { m_isDivider = v; }

    /// Permanent divider — the auto-created V/A boundary separator. Cannot be
    /// moved or deleted by the user; ensureSectionDivider keeps it pinned at
    /// the boundary. NOT serialised (re-derived on every project load).
    [[nodiscard]] bool isPermanentDivider() const noexcept { return m_isPermanentDivider; }
    void setPermanentDivider(bool v) noexcept { m_isPermanentDivider = v; }

    /// Caption track — the dedicated subtitle/closed-caption track pinned
    /// above all other tracks (Premiere-style). Stored as TrackType::Video
    /// so the existing compositor/mixer paths treat it uniformly, but flagged
    /// so the UI renders it distinctly and keeps it pinned & non-reorderable.
    /// Holds only CaptionClip clips; renders as a burned-in overlay on top.
    [[nodiscard]] bool isCaptionTrack() const noexcept { return m_isCaption; }
    void setCaptionTrack(bool v) noexcept { m_isCaption = v; }

    [[nodiscard]] float height() const noexcept { return m_height; }
    void setHeight(float h) noexcept { m_height = h; }

    /// User-assignable track color (0 = no custom color, uses default type color).
    [[nodiscard]] uint32_t color() const noexcept { return m_color; }
    void setColor(uint32_t rgba) noexcept { m_color = rgba; }

    // ── Audio mixer ─────────────────────────────────────────────────────
    /// Per-track volume (linear gain, 0.0 = silent, 1.0 = unity, >1.0 = boost).
    [[nodiscard]] float volume() const noexcept { return m_volume; }
    void setVolume(float v) noexcept { m_volume = v; }

    /// Per-track pan (-1.0 = full left, 0.0 = center, 1.0 = full right).
    [[nodiscard]] float pan() const noexcept { return m_pan; }
    void setPan(float p) noexcept { m_pan = p; }

    // ── Clip management ─────────────────────────────────────────────────
    Clip*  addClip(std::unique_ptr<Clip> clip);
    std::unique_ptr<Clip> removeClip(size_t index);
    void   moveClip(size_t index, int64_t newTimelinePosition);
    [[nodiscard]] size_t       clipCount() const noexcept;
    [[nodiscard]] Clip*        clip(size_t index) noexcept;
    [[nodiscard]] const Clip*  clip(size_t index) const noexcept;

    /// Find clip index by unique ID. Returns clipCount() if not found.
    [[nodiscard]] size_t findClipIndexById(uint64_t clipId) const noexcept;

    /// Remove clip by ID, returning ownership. Returns nullptr if not found.
    std::unique_ptr<Clip> removeClipById(uint64_t clipId);

    /// Find all clips active at a given time
    [[nodiscard]] std::vector<Clip*> clipsAtTime(int64_t timeTick) const;

    // ── Transition management ───────────────────────────────────────────
    size_t addTransition(Transition t);
    Transition removeTransition(size_t index);
    void setTransition(size_t index, const Transition& t);
    [[nodiscard]] const Transition* transition(size_t index) const noexcept;
    [[nodiscard]] size_t transitionCount() const noexcept;
    [[nodiscard]] const std::vector<Transition>& transitions() const noexcept;
    [[nodiscard]] std::vector<Transition>& transitions() noexcept;

    /// Duration of this track (end of last clip)
    [[nodiscard]] int64_t duration() const noexcept;

private:
    TrackType                              m_type;
    std::string                            m_name;
    bool                                   m_locked{false};
    bool                                   m_muted{false};
    bool                                   m_soloed{false};
    bool                                   m_targeted{true};  // All tracks targeted by default
    bool                                   m_collapsed{false};
    bool                                   m_syncLocked{true}; // Sync lock on by default
    bool                                   m_isDivider{false}; // Visual separator track
    bool                                   m_isPermanentDivider{false}; // Auto-managed V/A boundary divider (not persisted)
    bool                                   m_isCaption{false}; // Dedicated caption/subtitle track (pinned on top)
    float                                  m_height{80.0f};  // pixels
    uint32_t                               m_color{0};       // custom RGBA (0=none)
    float                                  m_volume{1.0f};   // linear gain
    float                                  m_pan{0.0f};      // -1..+1
    std::vector<std::unique_ptr<Clip>>  m_clips;
    std::vector<Transition>             m_transitions;
};

} // namespace rt
