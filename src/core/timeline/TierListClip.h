/*
 * TierListClip — a timeline "tier list" ranking board.
 *
 * A single background element that renders a ranking grid (S/A/B/… rows) and
 * evolves over the clip's duration via a list of timed events:
 *
 *   - POPUP   : an entry appears large & centred (the "discussion" spotlight)
 *               for the event's [start,end] window; the grid dims behind it.
 *   - DROP    : the entry flies from the spotlight into a tier row at an index
 *               over [start,end]; the vertical tier is final once dropped.
 *   - REORDER : re-rank an entry WITHIN a row (from → to index) over [start,end].
 *
 * Every visual decision is a pure deterministic function of the clip-local
 * tick (event replay), so preview and export produce identical frames.  The
 * actual pixel assembly happens in renderTierListClip() (ClipRenderers).
 *
 * This class is purely data + accessors.
 */

#pragma once

#include "timeline/Clip.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rt {

/// Per-list entry-thumbnail shape.  Uniform for every entry in a list; drives
/// row packing and the spotlight panel size.  Varies between projects.
enum class TierEntryAspect : uint8_t
{
    Banner   = 0,  ///< 16:9 landscape banner
    Square   = 1,  ///< 1:1
    Vertical = 2,  ///< 9:16 portrait
    Custom   = 3   ///< user w:h (customAspectW/H)
};

/// One tier row: a label ("S") and its colour (0xAARRGGBB).
struct TierDef
{
    std::string label;
    uint32_t    color{0xFFFFFFFFu};
};

/// A candidate entry in the pool.  One image — thumbnail and spotlight art are
/// the same picture at different scale.  `subbin` groups it in the pool browser.
struct TierEntry
{
    uint64_t    id{0};
    std::string imagePath;
    std::string title;
    std::string subbin;
};

/// Kinds of timed board events.  Which fields matter depends on `type`.
enum class TierEventType : uint8_t
{
    Popup   = 0,
    Drop    = 1,
    Reorder = 2
};

/// A timed event.  Times are CLIP-LOCAL ticks (relative to the clip start) so
/// events move with the clip.  Fields used per type:
///   Popup   : entryId, start, end
///   Drop    : entryId, tier, index (-1 = append), start, end
///   Reorder : tier, fromIndex, toIndex, start, end
struct TierEvent
{
    TierEventType type{TierEventType::Popup};
    uint64_t      entryId{0};
    int32_t       tier{0};        ///< index into tiers()
    int32_t       index{-1};      ///< Drop: insertion index in the row (-1 = append)
    int32_t       fromIndex{0};   ///< Reorder
    int32_t       toIndex{0};     ///< Reorder
    int64_t       start{0};       ///< clip-local ticks
    int64_t       end{0};
};

class TierListClip : public Clip
{
public:
    TierListClip();
    ~TierListClip() override = default;

    // ── Grid config ─────────────────────────────────────────────────────
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }
    void setTitle(const std::string& t) { m_title = t; }

    [[nodiscard]] const std::vector<TierDef>& tiers() const noexcept { return m_tiers; }
    std::vector<TierDef>& tiers() noexcept { return m_tiers; }

    [[nodiscard]] TierEntryAspect entryAspect() const noexcept { return m_entryAspect; }
    void setEntryAspect(TierEntryAspect a) noexcept { m_entryAspect = a; }

    [[nodiscard]] float customAspectW() const noexcept { return m_customAspectW; }
    [[nodiscard]] float customAspectH() const noexcept { return m_customAspectH; }
    void setCustomAspect(float w, float h) noexcept { m_customAspectW = w; m_customAspectH = h; }

    /// Entry width : height ratio for the active aspect.
    [[nodiscard]] float entryAspectRatio() const noexcept;

    // ── Safe margins (fraction of the canvas, 0..1) ─────────────────────
    /// Reserve the top band for the channel banner; nothing draws into it.
    [[nodiscard]] float topSafeMargin() const noexcept { return m_topSafeMargin; }
    void setTopSafeMargin(float f) noexcept { m_topSafeMargin = f; }
    /// Reserve the right strip for the (separate) commentators panel.
    [[nodiscard]] float rightSafeMargin() const noexcept { return m_rightSafeMargin; }
    void setRightSafeMargin(float f) noexcept { m_rightSafeMargin = f; }

    /// Background fill (0xAARRGGBB).  Default opaque black.
    [[nodiscard]] uint32_t backgroundColor() const noexcept { return m_bgColor; }
    void setBackgroundColor(uint32_t c) noexcept { m_bgColor = c; }

    // ── Entry pool ──────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<TierEntry>& entries() const noexcept { return m_entries; }
    std::vector<TierEntry>& entries() noexcept { return m_entries; }
    [[nodiscard]] const TierEntry* entryById(uint64_t id) const noexcept;

    /// Pool-bin subbin (folder) names, for the Project-Bin-style browser.
    [[nodiscard]] const std::vector<std::string>& subbins() const noexcept { return m_subbins; }
    std::vector<std::string>& subbins() noexcept { return m_subbins; }

    // ── Timed events ────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<TierEvent>& events() const noexcept { return m_events; }
    std::vector<TierEvent>& events() noexcept { return m_events; }

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
    std::string          m_title{"TIER LIST"};
    std::vector<TierDef> m_tiers;                 ///< default S–F, set in ctor
    TierEntryAspect      m_entryAspect{TierEntryAspect::Banner};
    float                m_customAspectW{16.0f};
    float                m_customAspectH{9.0f};
    float                m_topSafeMargin{0.0565f};///< reserve top band (channel banner) — measured
    float                m_rightSafeMargin{0.130f};///< reserve right strip (commentators) — measured
    uint32_t             m_bgColor{0xFF000000u};  ///< opaque black

    std::vector<TierEntry>   m_entries;
    std::vector<std::string> m_subbins;
    std::vector<TierEvent>   m_events;
};

} // namespace rt
