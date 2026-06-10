/*
 * Track.cpp — Track data model implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/Transition.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

// ── Construction ────────────────────────────────────────────────────────────

Track::Track(TrackType type, const std::string& name)
    : m_type(type)
    , m_name(name)
{
}

Track::~Track() = default;

// ── Clip management ─────────────────────────────────────────────────────────

Clip* Track::addClip(std::unique_ptr<Clip> clip)
{
    if (!clip) return nullptr;
    if (m_isDivider) return nullptr;  // Dividers cannot hold clips

    // Insert maintaining sorted order by timelineIn
    int64_t newIn = clip->timelineIn();
    auto it = std::lower_bound(m_clips.begin(), m_clips.end(), newIn,
        [](const std::unique_ptr<Clip>& c, int64_t t) { return c->timelineIn() < t; }
    );

    auto* ptr = clip.get();
    m_clips.insert(it, std::move(clip));
    return ptr;
}

std::unique_ptr<Clip> Track::removeClip(size_t index)
{
    if (index >= m_clips.size()) return nullptr;
    auto clip = std::move(m_clips[index]);
    const uint64_t removedId = clip->id();
    m_clips.erase(m_clips.begin() + static_cast<ptrdiff_t>(index));

    // Drop any transitions that referenced the removed clip — otherwise
    // the dissolve keeps rendering at the (now stale) edit-point tick,
    // showing the old fade through empty space after a cut+delete.
    for (size_t i = m_transitions.size(); i-- > 0; ) {
        const auto& t = m_transitions[i];
        if (t.leftClipId == removedId || t.rightClipId == removedId) {
            m_transitions.erase(m_transitions.begin() + static_cast<ptrdiff_t>(i));
        }
    }
    return clip;
}

size_t Track::findClipIndexById(uint64_t clipId) const noexcept
{
    for (size_t i = 0; i < m_clips.size(); ++i)
    {
        if (m_clips[i]->id() == clipId) return i;
    }
    return m_clips.size(); // not found
}

std::unique_ptr<Clip> Track::removeClipById(uint64_t clipId)
{
    size_t idx = findClipIndexById(clipId);
    if (idx == m_clips.size()) return nullptr;
    return removeClip(idx);
}

void Track::moveClip(size_t index, int64_t newTimelinePosition)
{
    if (index >= m_clips.size()) return;

    auto clip = std::move(m_clips[index]);
    m_clips.erase(m_clips.begin() + static_cast<ptrdiff_t>(index));

    clip->setTimelineIn(newTimelinePosition);

    // Keep transitions attached to this clip: update editPointTick so the
    // transition visually moves with the clip it belongs to.
    const uint64_t movedId = clip->id();
    const int64_t  newIn   = clip->timelineIn();
    const int64_t  newOut  = clip->timelineOut();
    for (auto& t : m_transitions) {
        if (t.leftClipId == movedId)
            t.editPointTick = newOut;   // tail of left clip
        else if (t.rightClipId == movedId)
            t.editPointTick = newIn;    // head of right clip
    }

    // Re-insert in sorted position
    auto it = std::lower_bound(m_clips.begin(), m_clips.end(), newTimelinePosition,
        [](const std::unique_ptr<Clip>& c, int64_t t) { return c->timelineIn() < t; }
    );
    m_clips.insert(it, std::move(clip));
}

size_t Track::clipCount() const noexcept
{
    return m_clips.size();
}

Clip* Track::clip(size_t index) noexcept
{
    return index < m_clips.size() ? m_clips[index].get() : nullptr;
}

const Clip* Track::clip(size_t index) const noexcept
{
    return index < m_clips.size() ? m_clips[index].get() : nullptr;
}

std::vector<Clip*> Track::clipsAtTime(int64_t timeTick) const
{
    std::vector<Clip*> result;
    for (const auto& c : m_clips)
    {
        if (c->timelineIn() <= timeTick && timeTick < c->timelineOut())
            result.push_back(c.get());
    }
    return result;
}

// ── Transition management ───────────────────────────────────────────────────

size_t Track::addTransition(Transition t)
{
    // Add-or-replace: one transition per edit point.  See Track.h.
    const size_t existing = findTransition(t.leftClipId, t.rightClipId);
    if (existing != kNoTransition) {
        m_transitions[existing] = t;
        return existing;
    }
    m_transitions.push_back(t);
    return m_transitions.size() - 1;
}

size_t Track::findTransition(uint64_t leftClipId,
                             uint64_t rightClipId) const noexcept
{
    if (leftClipId == 0 && rightClipId == 0) return kNoTransition;
    for (size_t i = 0; i < m_transitions.size(); ++i) {
        if (m_transitions[i].leftClipId == leftClipId &&
            m_transitions[i].rightClipId == rightClipId)
            return i;
    }
    return kNoTransition;
}

Transition Track::removeTransition(size_t index)
{
    if (index >= m_transitions.size()) return {};
    Transition t = m_transitions[index];
    m_transitions.erase(m_transitions.begin() + static_cast<ptrdiff_t>(index));
    return t;
}

void Track::setTransition(size_t index, const Transition& t)
{
    if (index < m_transitions.size())
        m_transitions[index] = t;
}

const Transition* Track::transition(size_t index) const noexcept
{
    return index < m_transitions.size() ? &m_transitions[index] : nullptr;
}

size_t Track::transitionCount() const noexcept
{
    return m_transitions.size();
}

const std::vector<Transition>& Track::transitions() const noexcept
{
    return m_transitions;
}

std::vector<Transition>& Track::transitions() noexcept
{
    return m_transitions;
}

// ── Duration ────────────────────────────────────────────────────────────────

int64_t Track::duration() const noexcept
{
    if (m_clips.empty()) return 0;

    int64_t maxEnd = 0;
    for (const auto& c : m_clips)
    {
        int64_t end = c->timelineOut();
        if (end > maxEnd) maxEnd = end;
    }
    return maxEnd;
}

} // namespace rt

