/*
 * Clip — base class for all timeline clip types.
 *
 * A Clip has a position on the timeline, a source in/out range, speed,
 * opacity, transform keyframes, and an effect stack.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "timeline/Keyframe.h"
#include "timeline/KeyframeTrack.h"
#include "timeline/Marker.h"
#include "timeline/OpacityMask.h"
#include "effects/EffectStack.h"

namespace rt {

/// Clip type discriminator
enum class ClipType : uint8_t
{
    Spine,       // Animated character (spine-cpp)
    Video,       // Video media file
    Audio,       // Audio media file
    Title,       // Text overlay (legacy, superseded by Graphic)
    Adjustment,  // Adjustment layer (effects only, no source)
    Image,       // Static image
    Graphic,     // Multi-layer graphic container (text, shapes)
    Sequence,    // Nested sequence (references another Timeline)
    Caption,     // Subtitle / closed-caption cue (lives on the caption track)
    PngPuppet,   // Veadotube-style PNG puppet character (4-image talk/blink loop)
    TierList     // Ranking board: grid + entry pool + timed POPUP/DROP/Reorder events
};

/// Base clip class. Derived classes add type-specific data.
class Clip
{
public:
    explicit Clip(ClipType type);
    virtual ~Clip();

    // Non-copyable, movable
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    Clip(Clip&&) noexcept = default;
    Clip& operator=(Clip&&) noexcept = default;

    // ── Identity ────────────────────────────────────────────────────────
    [[nodiscard]] ClipType          clipType() const noexcept { return m_type; }

    // ── Capability queries (cleanup audit §3.5) ─────────────────────
    // Centralized replacements for the boolean-ish ClipType:: comparisons
    // that used to be scattered across ~42 files.  Adding a new clip type
    // means reviewing THIS block (and overriding where the default is
    // wrong) instead of hunting call sites.  Exhaustive per-type dispatch
    // (serialization, rendering, UI labels) deliberately stays switch-based.

    /// Audio payload (the Audio clip type only — video clips' audio lives
    /// in their linked companion Audio clip).
    [[nodiscard]] virtual bool isAudio() const noexcept {
        return m_type == ClipType::Audio;
    }

    /// Renders visual content on a track.  Everything except Audio —
    /// including Caption, which paints a burn-in overlay.  (Sites that
    /// need "visual but not caption" should also test isCaption().)
    [[nodiscard]] virtual bool isVisual() const noexcept {
        return m_type != ClipType::Audio;
    }

    /// Subtitle/caption cue (lives on the pinned caption track).
    [[nodiscard]] bool isCaption() const noexcept {
        return m_type == ClipType::Caption;
    }

    /// Character clip: shows animation/talking/costume controls.
    /// Spine + PngPuppet always; VideoClip overrides to add
    /// isVideoCharacter() video characters.
    [[nodiscard]] virtual bool isCharacter() const noexcept {
        return m_type == ClipType::Spine || m_type == ClipType::PngPuppet;
    }

    /// Program-monitor transform overlay (move/scale/rotate the clip on
    /// the canvas).  Everything visual except Adjustment (no bounds of
    /// its own).
    [[nodiscard]] virtual bool supportsClipTransform() const noexcept {
        return m_type != ClipType::Audio && m_type != ClipType::Adjustment;
    }

    /// Crop L/R/T/B UI.  Spine + Video only (matches the existing
    /// EffectControls gate; Image deliberately excluded — its crop
    /// fields are not surfaced there today).
    [[nodiscard]] virtual bool supportsCrop() const noexcept {
        return m_type == ClipType::Spine || m_type == ClipType::Video;
    }

    /// Per-layer transforms (Essential Graphics layer list).
    [[nodiscard]] virtual bool supportsLayerTransform() const noexcept {
        return m_type == ClipType::Graphic;
    }

    /// In-place text editing in the Program Monitor (double-click).
    [[nodiscard]] virtual bool supportsTextEdit() const noexcept {
        return m_type == ClipType::Caption || m_type == ClipType::Title
            || m_type == ClipType::Graphic;
    }

    /// References an external media file (mediaPath() on the subclass).
    [[nodiscard]] virtual bool hasMediaPath() const noexcept {
        return m_type == ClipType::Video || m_type == ClipType::Audio
            || m_type == ClipType::Image;
    }

    // ── Crop accessors (percent cropped per edge) ───────────────────────
    // Defaults are no-crop / no-op; Spine, Video and Image carry the real
    // fields (their existing methods implicitly override these).  UI code
    // must still gate on supportsCrop() — Image stores crop but does not
    // surface it in the crop UI.
    [[nodiscard]] virtual float cropLeft()   const noexcept { return 0.0f; }
    [[nodiscard]] virtual float cropRight()  const noexcept { return 0.0f; }
    [[nodiscard]] virtual float cropTop()    const noexcept { return 0.0f; }
    [[nodiscard]] virtual float cropBottom() const noexcept { return 0.0f; }
    virtual void setCrop(float /*l*/, float /*r*/, float /*t*/, float /*b*/) {}

    [[nodiscard]] uint64_t          id()       const noexcept { return m_id; }
    /// Override the auto-assigned id. Used for synthetic clips (e.g. the
    /// audio clips a nested-sequence clip expands into) so their playback
    /// provider stays stable across reloads.
    void setId(uint64_t id) noexcept { m_id = id; }

    /// Advance the global id counter so the next auto-assigned id is strictly
    /// greater than `usedId`.  Call this after restoring a saved clip id (via
    /// setId) so freshly created clips never collide with restored ones.
    static void reserveId(uint64_t usedId) noexcept;
    [[nodiscard]] const std::string& label()   const noexcept { return m_label; }
    void setLabel(const std::string& label) { m_label = label; }

    /// Shot name for clip grouping (links visual layer clips to a ShotPreset).
    [[nodiscard]] const std::string& shotName() const noexcept { return m_shotName; }
    void setShotName(const std::string& name) { m_shotName = name; }

    /// Group ID: clips with the same non-zero groupId form a shot group.
    [[nodiscard]] uint64_t groupId() const noexcept { return m_groupId; }
    void setGroupId(uint64_t id) noexcept { m_groupId = id; }

    /// Link ID: clips with the same non-zero linkId move and select as
    /// one unit. Used for the video+audio companion produced when a
    /// media file with both streams is dropped onto the timeline; users
    /// can break the link for a single drag by holding Alt at click time.
    [[nodiscard]] uint64_t linkId() const noexcept { return m_linkId; }
    void setLinkId(uint64_t id) noexcept { m_linkId = id; }

    /// Layer ID within a shot group (e.g. "background_0", "char_0").
    [[nodiscard]] const std::string& layerId() const noexcept { return m_layerId; }
    void setLayerId(const std::string& id) { m_layerId = id; }

    /// AudioSync export back-link: the script line this clip was exported
    /// for (audio clips: their own line; visual shot-group clips: the
    /// group's FIRST line).  -1 = not created by the AudioSync export.
    /// Drives the non-destructive incremental re-export — clips carrying
    /// a line are updated in place, everything else is left untouched.
    [[nodiscard]] int32_t syncLine() const noexcept { return m_syncLine; }
    void setSyncLine(int32_t line) noexcept { m_syncLine = line; }

    [[nodiscard]] uint32_t color() const noexcept { return m_color; }
    void setColor(uint32_t rgba) noexcept { m_color = rgba; }

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool v) noexcept { m_enabled = v; }

    /// Offline flag — true if the source media file is missing/unavailable.
    [[nodiscard]] bool isOffline() const noexcept { return m_offline; }
    void setOffline(bool v) noexcept { m_offline = v; }

    /// Render status: 0=yellow (needs render), 1=green (rendered), 2=red (error).
    [[nodiscard]] uint8_t renderStatus() const noexcept { return m_renderStatus; }
    void setRenderStatus(uint8_t s) noexcept { m_renderStatus = s; }

    // ── Timeline position ───────────────────────────────────────────────
    /// Position on the timeline (in ticks from timeline start)
    [[nodiscard]] int64_t timelineIn()  const noexcept { return m_timelineIn; }
    [[nodiscard]] int64_t timelineOut() const noexcept { return m_timelineIn + m_duration; }
    [[nodiscard]] int64_t duration()    const noexcept { return m_duration; }

    void setTimelineIn(int64_t t) noexcept { m_timelineIn = t; }
    void setDuration(int64_t d)   noexcept { m_duration = d; }

    // ── Source range ────────────────────────────────────────────────────
    /// Source media in/out (in ticks, relative to source start)
    [[nodiscard]] int64_t sourceIn()  const noexcept { return m_sourceIn; }
    [[nodiscard]] int64_t sourceOut() const noexcept {
        return m_sourceIn + static_cast<int64_t>(std::llround(m_duration * m_speed));
    }
    void setSourceIn(int64_t t) noexcept { m_sourceIn = t; }

    // ── Speed ───────────────────────────────────────────────────────────
    [[nodiscard]] double speed() const noexcept { return m_speed; }
    void setSpeed(double s) noexcept { m_speed = s; }

    /// When true, pitch is preserved when speed != 1.0 (like Premiere Pro).
    [[nodiscard]] bool maintainPitch() const noexcept { return m_maintainPitch; }
    void setMaintainPitch(bool v) noexcept { m_maintainPitch = v; }

    /// Speed ramp (multiplier over clip-local time). Default 1.0 = uniform speed.
    KeyframeTrack<float>& speedRamp() noexcept { return m_speedRamp; }
    const KeyframeTrack<float>& speedRamp() const noexcept { return m_speedRamp; }

    /// Evaluate effective speed at a clip-local tick.
    [[nodiscard]] double effectiveSpeed(int64_t localTick) const noexcept {
        return m_speed * static_cast<double>(m_speedRamp.evaluate(localTick));
    }

    // ── Keyframeable properties ─────────────────────────────────────────
    KeyframeTrack<float>& opacity()   noexcept { return m_opacity; }
    KeyframeTrack<float>& positionX() noexcept { return m_posX; }
    KeyframeTrack<float>& positionY() noexcept { return m_posY; }
    KeyframeTrack<float>& scaleX()    noexcept { return m_scaleX; }
    KeyframeTrack<float>& scaleY()    noexcept { return m_scaleY; }
    KeyframeTrack<float>& rotation()  noexcept { return m_rotation; }
    /// Anchor point — clip-LOCAL pivot offset (REF-1920 px from the
    /// clip's geometric center) used by the compositor as the
    /// rotation/scale pivot. Defaults to (0,0), matching the legacy
    /// renderer that pivots around the layer center. Backward
    /// compatible: existing projects load anchor as 0 and render
    /// identically.
    KeyframeTrack<float>& anchorX()   noexcept { return m_anchorX; }
    KeyframeTrack<float>& anchorY()   noexcept { return m_anchorY; }

    // ── Effect stack ────────────────────────────────────────────────────
    EffectStack& effects() noexcept { return m_effects; }
    const EffectStack& effects() const noexcept { return m_effects; }

    // ── Opacity masks ───────────────────────────────────────────────────
    [[nodiscard]] const std::vector<OpacityMask>& masks() const noexcept { return m_masks; }
    std::vector<OpacityMask>& masks() noexcept { return m_masks; }
    size_t maskCount() const noexcept { return m_masks.size(); }
    void addMask(OpacityMask mask) { m_masks.push_back(std::move(mask)); }
    void removeMask(size_t index) {
        if (index < m_masks.size()) m_masks.erase(m_masks.begin() + static_cast<ptrdiff_t>(index));
    }

    // ── Blend mode ──────────────────────────────────────────────────────
    /// Blend mode for compositing (matches BlendMode enum values: 0=Normal, 1=Multiply, etc.)
    [[nodiscard]] int32_t blendMode() const noexcept { return m_blendMode; }
    void setBlendMode(int32_t mode) noexcept { m_blendMode = mode; }

    // ── Clip-level markers ───────────────────────────────────────────────
    [[nodiscard]] const std::vector<Marker>& markers() const noexcept { return m_markers; }
    std::vector<Marker>& markers() noexcept { return m_markers; }
    void addMarker(Marker m) { m_markers.push_back(std::move(m)); }
    void removeMarker(size_t index) {
        if (index < m_markers.size()) m_markers.erase(m_markers.begin() + static_cast<ptrdiff_t>(index));
    }

    /// Create a deep clone (for undo snapshots where delta is too complex)
    [[nodiscard]] virtual std::unique_ptr<Clip> clone() const = 0;

protected:
    ClipType m_type;
    uint64_t m_id;
    std::string m_label;
    std::string m_shotName;   ///< Shot preset name (for grouping)
    std::string m_layerId;    ///< Layer within shot group
    uint64_t m_groupId{0};    ///< Non-zero = part of a shot group
    uint64_t m_linkId{0};     ///< Non-zero = linked A/V pair (move/select together)
    int32_t  m_syncLine{-1};  ///< AudioSync export back-link (script line), -1 = none
    uint32_t m_color{0xFF888888};
    bool     m_enabled{true};
    bool     m_offline{false};
    uint8_t  m_renderStatus{0}; ///< 0=needs render, 1=rendered, 2=error

    int64_t m_timelineIn{0};
    int64_t m_duration{0};
    int64_t m_sourceIn{0};
    double  m_speed{1.0};
    bool    m_maintainPitch{true};  ///< Preserve pitch when speed != 1.0
    KeyframeTrack<float> m_speedRamp{1.0f};  ///< Speed multiplier ramp

    // Keyframeable transform properties (default: single keyframe at t=0)
    KeyframeTrack<float> m_opacity{1.0f};
    KeyframeTrack<float> m_posX{0.0f};
    KeyframeTrack<float> m_posY{0.0f};
    KeyframeTrack<float> m_scaleX{1.0f};
    KeyframeTrack<float> m_scaleY{1.0f};
    KeyframeTrack<float> m_rotation{0.0f};
    KeyframeTrack<float> m_anchorX{0.0f};
    KeyframeTrack<float> m_anchorY{0.0f};

    int32_t m_blendMode{0}; ///< Compositor blend mode (0=Normal)
    EffectStack m_effects;
    std::vector<OpacityMask> m_masks;  ///< Opacity masks (applied before compositing)
    std::vector<Marker> m_markers;    ///< Clip-level markers (move with clip)
};

} // namespace rt
