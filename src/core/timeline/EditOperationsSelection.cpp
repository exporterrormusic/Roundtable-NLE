/*
 * EditOperationsSelection.cpp -- SelectionSet and SnapEngine.
 *
 * Split from EditOperations.cpp for maintainability.
 */

#include "timeline/EditOperations.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Marker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  SelectionSet
// ═════════════════════════════════════════════════════════════════════════════

void SelectionSet::selectClip(const ClipRef& ref, bool addToSelection)
{
    if (!addToSelection)
        m_selected.clear();

    // Don't add duplicates
    for (const auto& s : m_selected)
        if (s == ref) return;

    m_selected.push_back(ref);
}

void SelectionSet::deselectClip(const ClipRef& ref)
{
    m_selected.erase(
        std::remove(m_selected.begin(), m_selected.end(), ref),
        m_selected.end());
}

void SelectionSet::toggleClip(const ClipRef& ref)
{
    if (isSelected(ref))
        deselectClip(ref);
    else
        selectClip(ref, true);
}

void SelectionSet::selectRect(const Timeline& timeline, const TimelineRect& rect)
{
    m_selected.clear();

    size_t firstTrack = std::min(rect.topTrack, rect.bottomTrack);
    size_t lastTrack  = std::max(rect.topTrack, rect.bottomTrack);
    int64_t startT    = std::min(rect.startTick, rect.endTick);
    int64_t endT      = std::max(rect.startTick, rect.endTick);

    for (size_t ti = firstTrack; ti <= lastTrack && ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            // Clip overlaps the rectangle?
            if (clip->timelineOut() > startT && clip->timelineIn() < endT)
            {
                m_selected.push_back({ti, clip->id()});
            }
        }
    }
}

void SelectionSet::selectAll(const Timeline& timeline)
{
    m_selected.clear();
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            m_selected.push_back({ti, track->clip(ci)->id()});
        }
    }
}

void SelectionSet::clear()
{
    m_selected.clear();
}

bool SelectionSet::isSelected(const ClipRef& ref) const
{
    for (const auto& s : m_selected)
        if (s == ref) return true;
    return false;
}

bool SelectionSet::isSelectedById(uint64_t clipId) const
{
    for (const auto& s : m_selected)
        if (s.clipId == clipId) return true;
    return false;
}

std::optional<ClipRef> SelectionSet::singleSelection() const
{
    if (m_selected.size() == 1)
        return m_selected[0];
    return std::nullopt;
}

//  SnapEngine
// ═════════════════════════════════════════════════════════════════════════════

int64_t SnapEngine::thresholdTicks() const
{
    if (m_pps <= 0.0) return 0;
    // Convert pixel threshold to tick threshold
    double secondsThreshold = m_thresholdPx / m_pps;
    return static_cast<int64_t>(secondsThreshold * 48000.0);
}

void SnapEngine::buildTargets(const Timeline& timeline,
                               int64_t playhead,
                               double /*frameRate*/,
                               const std::vector<uint64_t>& excludeClipIds)
{
    m_targets.clear();
    // A fresh target set implies a new drag/edit session — start unlocked.
    resetHysteresis();

    // Convert excludeClipIds to a set for O(log n) lookup
    std::set<uint64_t> excludeSet(excludeClipIds.begin(), excludeClipIds.end());

    // Playhead
    m_targets.push_back({playhead, SnapTarget::Type::Playhead});

    // In/Out points
    if (timeline.inPoint() >= 0)
        m_targets.push_back({timeline.inPoint(), SnapTarget::Type::InPoint});
    if (timeline.outPoint() >= 0)
        m_targets.push_back({timeline.outPoint(), SnapTarget::Type::OutPoint});

    // Markers
    for (const auto& marker : timeline.markers())
        m_targets.push_back({marker.time, SnapTarget::Type::Marker});

    // Clip edges (all tracks)
    for (size_t ti = 0; ti < timeline.trackCount(); ++ti)
    {
        const Track* track = timeline.track(ti);
        for (size_t ci = 0; ci < track->clipCount(); ++ci)
        {
            const Clip* clip = track->clip(ci);
            if (excludeSet.count(clip->id())) continue;

            m_targets.push_back({clip->timelineIn(), SnapTarget::Type::ClipEdge});
            m_targets.push_back({clip->timelineOut(), SnapTarget::Type::ClipEdge});
        }
    }

    // Sort for binary search
    std::sort(m_targets.begin(), m_targets.end(),
              [](const SnapTarget& a, const SnapTarget& b) { return a.tick < b.tick; });
}

void SnapEngine::clearTargets()
{
    m_targets.clear();
}

void SnapEngine::addTarget(const SnapTarget& target)
{
    m_targets.push_back(target);
}

void SnapEngine::resetHysteresis() const noexcept
{
    m_stuckTick = INT64_MIN;
    m_stuckType = SnapTarget::Type::GridLine;
}

SnapEngine::AttractHit
SnapEngine::findNearestAttract(int64_t tick, int64_t attractTicks) const
{
    AttractHit hit;
    if (attractTicks <= 0) return hit;
    for (const auto& target : m_targets)
    {
        int64_t dist = std::abs(tick - target.tick);
        if (dist < hit.dist)
        {
            hit.dist = dist;
            hit.tick = target.tick;
            hit.type = target.type;
        }
    }
    hit.found = (hit.dist <= attractTicks);
    return hit;
}

SnapResult SnapEngine::snap(int64_t tick) const
{
    if (!m_enabled || m_targets.empty())
        return {tick, false, 0, SnapTarget::Type::GridLine};

    int64_t attract = thresholdTicks();
    if (attract <= 0)
        return {tick, false, 0, SnapTarget::Type::GridLine};

    // ── Hysteresis: stay locked on the previously-stuck target until the
    //    user drags outside the wider release zone. This is the Premiere
    //    "detent" feel — snapping doesn't flicker as the cursor wiggles.
    if (m_stuckTick != INT64_MIN)
    {
        const int64_t release = static_cast<int64_t>(attract * kReleaseMultiplier);
        if (std::abs(tick - m_stuckTick) <= release)
            return {m_stuckTick, true, m_stuckTick - tick, m_stuckType};
        resetHysteresis();
    }

    // ── Fresh attract: nearest target within the (narrower) attract zone.
    AttractHit hit = findNearestAttract(tick, attract);
    if (hit.found)
    {
        m_stuckTick = hit.tick;
        m_stuckType = hit.type;
        return {hit.tick, true, hit.tick - tick, hit.type};
    }
    return {tick, false, 0, SnapTarget::Type::GridLine};
}

SnapResult SnapEngine::snapPair(int64_t tickA, int64_t tickB) const
{
    if (!m_enabled || m_targets.empty())
        return {tickA, false, 0, SnapTarget::Type::GridLine};

    int64_t attract = thresholdTicks();
    if (attract <= 0)
        return {tickA, false, 0, SnapTarget::Type::GridLine};

    // ── Hysteresis (pair): release if BOTH edges leave the release zone of
    //    the stuck target. Whichever edge is closer wins, with the delta
    //    expressed relative to tickA so callers (which translate by delta)
    //    move the whole clip correctly.
    if (m_stuckTick != INT64_MIN)
    {
        const int64_t release = static_cast<int64_t>(attract * kReleaseMultiplier);
        const int64_t dA = std::abs(tickA - m_stuckTick);
        const int64_t dB = std::abs(tickB - m_stuckTick);
        if (dA <= release || dB <= release)
        {
            if (dA <= dB)
                return {m_stuckTick, true, m_stuckTick - tickA, m_stuckType};
            const int64_t delta = m_stuckTick - tickB;
            return {tickA + delta, true, delta, m_stuckType};
        }
        resetHysteresis();
    }

    // ── Fresh attract: probe both edges, return the smaller delta.
    AttractHit hitA = findNearestAttract(tickA, attract);
    AttractHit hitB = findNearestAttract(tickB, attract);
    if (!hitA.found && !hitB.found)
        return {tickA, false, 0, SnapTarget::Type::GridLine};

    const bool preferA = hitA.found && (!hitB.found || hitA.dist <= hitB.dist);
    if (preferA)
    {
        m_stuckTick = hitA.tick;
        m_stuckType = hitA.type;
        return {hitA.tick, true, hitA.tick - tickA, hitA.type};
    }
    m_stuckTick = hitB.tick;
    m_stuckType = hitB.type;
    const int64_t delta = hitB.tick - tickB;
    return {tickA + delta, true, delta, hitB.type};
}

SnapResult SnapEngine::snapEdges(const std::vector<int64_t>& edges) const
{
    if (!m_enabled || m_targets.empty() || edges.empty())
        return {0, false, 0, SnapTarget::Type::GridLine};

    const int64_t attract = thresholdTicks();
    if (attract <= 0)
        return {0, false, 0, SnapTarget::Type::GridLine};

    // ── Hysteresis: hold the stuck target while the edge nearest to it is
    //    still inside the release zone. The nearest edge is re-chosen each
    //    call, so the lock survives whichever dragged edge made it.
    if (m_stuckTick != INT64_MIN)
    {
        const int64_t release = static_cast<int64_t>(attract * kReleaseMultiplier);
        int64_t bestAbs = std::numeric_limits<int64_t>::max();
        int64_t bestDelta = 0;
        for (int64_t e : edges) {
            const int64_t d = m_stuckTick - e;
            if (std::abs(d) < bestAbs) { bestAbs = std::abs(d); bestDelta = d; }
        }
        if (bestAbs <= release)
            return {m_stuckTick, true, bestDelta, m_stuckType};
        resetHysteresis();
    }

    // ── Fresh attract: nearest target to any edge within the attract zone.
    AttractHit best;
    int64_t bestDelta = 0;
    for (int64_t e : edges) {
        AttractHit hit = findNearestAttract(e, attract);
        if (hit.found && hit.dist < best.dist) {
            best = hit;
            bestDelta = hit.tick - e;
        }
    }
    if (!best.found)
        return {0, false, 0, SnapTarget::Type::GridLine};

    m_stuckTick = best.tick;
    m_stuckType = best.type;
    return {best.tick, true, bestDelta, best.type};
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
