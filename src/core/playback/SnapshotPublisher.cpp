/*
 * SnapshotPublisher.cpp — see SnapshotPublisher.h.
 */

#include "playback/SnapshotPublisher.h"

#include "project/BinaryIO.h"
#include "project/ClipSerialization.h"
#include "project/Project.h"
#include "project/RenderStateHash.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <chrono>
#include <string>

namespace rt {

namespace {

[[nodiscard]] size_t clipCountOf(const Timeline& tl)
{
    size_t n = 0;
    for (size_t ti = 0; ti < tl.trackCount(); ++ti)
        if (const Track* t = tl.track(ti)) n += t->clipCount();
    return n;
}

[[nodiscard]] std::vector<uint8_t> serializedClip(const Clip& clip)
{
    BinaryWriter w;
    writeClip(w, clip);
    return w.data();
}

// Name the first thing that differs between a live sequence and its copy, so
// a missed markDirty() points straight at the edit that skipped it.
[[nodiscard]] std::string describeFirstDifference(const Timeline& live,
                                                  const Timeline& copy)
{
    if (live.trackCount() != copy.trackCount())
        return "track count " + std::to_string(copy.trackCount()) + " -> "
             + std::to_string(live.trackCount());
    for (size_t ti = 0; ti < live.trackCount(); ++ti) {
        const Track* a = live.track(ti);
        const Track* b = copy.track(ti);
        if (!a || !b) continue;
        const std::string where = "track " + std::to_string(ti) + " '" + a->name() + "'";
        if (a->clipCount() != b->clipCount())
            return where + " clip count " + std::to_string(b->clipCount()) + " -> "
                 + std::to_string(a->clipCount());
        for (size_t ci = 0; ci < a->clipCount(); ++ci) {
            const Clip* ca = a->clip(ci);
            const Clip* cb = b->clip(ci);
            if (!ca || !cb) continue;
            if (serializedClip(*ca) != serializedClip(*cb))
                return where + " clip id=" + std::to_string(ca->id()) + " type="
                     + std::to_string(static_cast<int>(ca->clipType()))
                     + " label='" + ca->label() + "'";
        }
        if (a->transitionCount() != b->transitionCount())
            return where + " transition count";
        if (a->isMuted() != b->isMuted() || a->isSoloed() != b->isSoloed()
                || a->volume() != b->volume() || a->pan() != b->pan())
            return where + " track mute/solo/mix";
    }
    return "settings or transition parameters";
}

} // namespace

SnapshotPublisher::SnapshotPublisher() = default;

SnapshotPublisher::~SnapshotPublisher()
{
    if (m_active) m_active->removeObserver(this);
    {
        std::lock_guard lk(m_latestMutex);
        m_latest.reset();
    }
    collectRetired();
}

void SnapshotPublisher::bind(const Project* project, Timeline* activeSequence)
{
    if (m_active != activeSequence) {
        if (m_active) m_active->removeObserver(this);
        if (activeSequence) activeSequence->addObserver(this);
    }
    m_project = project;
    m_active = activeSequence;
    m_dirty = DirtyScope::AllSequences;
}

void SnapshotPublisher::onTimelineDestroyed(Timeline* tl)
{
    // Only compare: `tl` is mid-destruction. The published snapshot owns its
    // own copies, so render threads are unaffected.
    if (tl == m_active) {
        m_active = nullptr;
        m_dirty = DirtyScope::AllSequences;
    }
}

void SnapshotPublisher::markDirty(DirtyScope scope) noexcept
{
    if (static_cast<uint8_t>(scope) > static_cast<uint8_t>(m_dirty))
        m_dirty = scope;
}

std::shared_ptr<const RenderSnapshot> SnapshotPublisher::latest() const
{
    std::lock_guard lk(m_latestMutex);
    return m_latest;
}

std::shared_ptr<const Timeline> SnapshotPublisher::makeCopy(const Timeline& live)
{
    std::unique_ptr<Timeline> copy = live.cloneForSnapshot();
    // The deleter parks the copy instead of destroying it: the last owner is
    // often a render thread, and model destructors belong on the UI thread.
    return std::shared_ptr<const Timeline>(
        copy.release(),
        [retired = m_retired](const Timeline* t) noexcept {
            try {
                std::lock_guard lk(retired->mutex);
                retired->items.emplace_back(t);
            } catch (...) {
                delete t;
            }
        });
}

std::vector<const Timeline*> SnapshotPublisher::liveSequences(size_t* activeIndex) const
{
    std::vector<const Timeline*> live;
    if (m_project) {
        live.reserve(m_project->sequenceCount() + 1);
        for (size_t i = 0; i < m_project->sequenceCount(); ++i)
            live.push_back(m_project->sequence(i));
    }
    size_t active = live.size();
    for (size_t i = 0; i < live.size(); ++i)
        if (live[i] && live[i] == m_active) { active = i; break; }
    // A timeline outside the project (tests, a bare sequence) goes last so the
    // project's nested-sequence indices stay valid.
    if (m_active && active == live.size())
        live.push_back(m_active);
    if (activeIndex) *activeIndex = m_active ? active : live.size();
    return live;
}

bool SnapshotPublisher::publish()
{
    collectRetired();
    if (m_dirty == DirtyScope::None) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const std::shared_ptr<const RenderSnapshot> prev = latest();

    size_t activeIndex = 0;
    const std::vector<const Timeline*> live = liveSequences(&activeIndex);
    const bool reusable = prev && m_dirty == DirtyScope::ActiveSequence
                       && m_publishedFrom == live
                       && prev->sequences.size() == live.size();

    auto snap = std::make_shared<RenderSnapshot>();
    snap->editVersion = m_nextEditVersion++;
    snap->sequences.reserve(live.size());
    std::vector<uint64_t> hashes(live.size(), 0);
    std::vector<bool> hashValid(live.size(), false);

    PublishStats stats;
    stats.editVersion = snap->editVersion;
    for (size_t i = 0; i < live.size(); ++i) {
        if (!live[i]) {
            snap->sequences.push_back(nullptr);
            continue;
        }
        if (reusable && i != activeIndex && prev->sequences[i]) {
            snap->sequences.push_back(prev->sequences[i]);
            if (i < m_copyHashValid.size() && m_copyHashValid[i]) {
                hashes[i] = m_copyHash[i];
                hashValid[i] = true;
            }
            ++stats.sequencesShared;
            continue;
        }
        snap->sequences.push_back(makeCopy(*live[i]));
        ++stats.sequencesCloned;
        stats.clipsCloned += clipCountOf(*live[i]);
    }
    snap->sequenceIndex = activeIndex;
    if (activeIndex < snap->sequences.size())
        snap->timeline = snap->sequences[activeIndex];

    m_publishedFrom = live;
    m_copyHash = std::move(hashes);
    m_copyHashValid = std::move(hashValid);
    m_dirty = DirtyScope::None;

    {
        std::lock_guard lk(m_latestMutex);
        m_latest = std::move(snap);
    }

    stats.cloneMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    m_lastStats = stats;
    return true;
}

void SnapshotPublisher::collectRetired()
{
    std::vector<std::unique_ptr<const Timeline>> doomed;
    {
        std::lock_guard lk(m_retired->mutex);
        doomed.swap(m_retired->items);
    }
    // Destroyed here, on the caller's (UI) thread, outside the lock.
}

bool SnapshotPublisher::verifyOne(size_t index, const Timeline& live,
                                  const Timeline& copy, std::string* detail)
{
    if (index >= m_copyHash.size()) return true;
    if (!m_copyHashValid[index]) {
        m_copyHash[index] = hashTimelineState(copy);
        m_copyHashValid[index] = true;
    }
    if (hashTimelineState(live) == m_copyHash[index]) return true;
    if (detail)
        *detail = "sequence " + std::to_string(index) + " '" + live.name() + "': "
                + describeFirstDifference(live, copy);
    return false;
}

bool SnapshotPublisher::verifySequences(std::string* detail)
{
    // Pending edits are about to be published anyway; comparing now would
    // only report what markDirty() already knows.
    if (isDirty()) return true;
    const std::shared_ptr<const RenderSnapshot> snap = latest();
    if (!snap) return true;

    size_t activeIndex = 0;
    const std::vector<const Timeline*> live = liveSequences(&activeIndex);
    if (live != m_publishedFrom || live.size() != snap->sequences.size()) {
        if (detail) *detail = "sequence list changed";
        markDirty(DirtyScope::AllSequences);
        return false;
    }

    if (activeIndex < live.size() && live[activeIndex] && snap->sequences[activeIndex]
            && !verifyOne(activeIndex, *live[activeIndex],
                          *snap->sequences[activeIndex], detail)) {
        markDirty(DirtyScope::ActiveSequence);
        return false;
    }

    // One other sequence per call keeps the idle cost bounded.
    if (live.size() > 1) {
        m_verifyCursor = (m_verifyCursor + 1) % live.size();
        if (m_verifyCursor == activeIndex)
            m_verifyCursor = (m_verifyCursor + 1) % live.size();
        const size_t i = m_verifyCursor;
        if (i != activeIndex && live[i] && snap->sequences[i]
                && !verifyOne(i, *live[i], *snap->sequences[i], detail)) {
            markDirty(DirtyScope::AllSequences);
            return false;
        }
    }
    return true;
}

} // namespace rt
