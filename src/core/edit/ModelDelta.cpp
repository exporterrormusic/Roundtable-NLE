/*
 * ModelDelta.cpp — see ModelDelta.h.
 */

#include "edit/ModelDelta.h"

#include "project/BinaryIO.h"
#include "project/ClipSerialization.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"

#include <spdlog/spdlog.h>

#include <unordered_set>

namespace rt {

// Reaches the containers behind Timeline/Track (friend of both) so a delta can
// rebuild the layout without firing per-track observer notifications.
struct ModelDeltaAccess
{
    static std::vector<std::unique_ptr<Track>>& tracks(Timeline& t) { return t.m_tracks; }
    static std::vector<std::unique_ptr<Clip>>& clips(Track& t) { return t.m_clips; }
    static void setTrackId(Track& t, uint64_t id) { t.m_id = id; }
    static void setMarkers(Timeline& t, std::vector<Marker> m) { t.m_markers = std::move(m); }
    static void notifyStructure(Timeline& t) { t.notifyStructureChanged(); }

    static void applyHeader(Track& t, const DeltaTrack& h, bool created)
    {
        t.m_name = h.name;
        t.m_locked = h.locked;
        t.m_muted = h.muted;
        t.m_soloed = h.soloed;
        t.m_syncLocked = h.syncLocked;
        t.m_isDivider = h.divider;
        t.m_isPermanentDivider = h.permanentDivider;
        t.m_isCaption = h.caption;
        t.m_color = h.color;
        t.m_volume = h.volume;
        t.m_pan = h.pan;
        if (created) {
            t.m_height = h.height;
            t.m_collapsed = h.collapsed;
            t.m_targeted = h.targeted;
        }
        t.m_transitions = h.transitions;
    }
};

namespace {

[[nodiscard]] bool sameTransition(const Transition& a, const Transition& b)
{
    return a.type == b.type && a.duration == b.duration && a.offset == b.offset
        && a.leftClipId == b.leftClipId && a.rightClipId == b.rightClipId
        && a.editPointTick == b.editPointTick && a.param1 == b.param1
        && a.param2 == b.param2;
}

[[nodiscard]] std::vector<uint8_t> serialized(const Clip& clip)
{
    BinaryWriter w;
    writeClip(w, clip);
    return w.data();
}

[[nodiscard]] DeltaSequenceState captureState(const Timeline& tl)
{
    DeltaSequenceState s;
    s.name = tl.name();
    s.settings = tl.settings();
    s.markers = tl.markers();
    s.tracks.reserve(tl.trackCount());
    for (size_t ti = 0; ti < tl.trackCount(); ++ti) {
        const Track* t = tl.track(ti);
        if (!t) continue;
        DeltaTrack h;
        h.id = t->id();
        h.type = t->type();
        h.name = t->name();
        h.locked = t->isLocked();
        h.muted = t->isMuted();
        h.soloed = t->isSoloed();
        h.syncLocked = t->isSyncLocked();
        h.divider = t->isDivider();
        h.permanentDivider = t->isPermanentDivider();
        h.caption = t->isCaptionTrack();
        h.color = t->color();
        h.volume = t->volume();
        h.pan = t->pan();
        h.height = t->height();
        h.collapsed = t->isCollapsed();
        h.targeted = t->isTargeted();
        h.clipIds.reserve(t->clipCount());
        for (size_t ci = 0; ci < t->clipCount(); ++ci)
            if (const Clip* c = t->clip(ci)) h.clipIds.push_back(c->id());
        h.transitions = t->transitions();
        s.tracks.push_back(std::move(h));
    }
    return s;
}

[[nodiscard]] std::unordered_map<uint64_t, const Clip*> clipsById(const Timeline& tl)
{
    std::unordered_map<uint64_t, const Clip*> out;
    for (size_t ti = 0; ti < tl.trackCount(); ++ti) {
        const Track* t = tl.track(ti);
        if (!t) continue;
        for (size_t ci = 0; ci < t->clipCount(); ++ci)
            if (const Clip* c = t->clip(ci)) out.emplace(c->id(), c);
    }
    return out;
}

} // namespace

bool DeltaTrack::sameContent(const DeltaTrack& o) const
{
    if (id != o.id || type != o.type || name != o.name || locked != o.locked
            || muted != o.muted || soloed != o.soloed || syncLocked != o.syncLocked
            || divider != o.divider || permanentDivider != o.permanentDivider
            || caption != o.caption || color != o.color || volume != o.volume
            || pan != o.pan || clipIds != o.clipIds
            || transitions.size() != o.transitions.size())
        return false;
    for (size_t i = 0; i < transitions.size(); ++i)
        if (!sameTransition(transitions[i], o.transitions[i])) return false;
    return true;
}

bool DeltaSequenceState::sameContent(const DeltaSequenceState& o) const
{
    if (name != o.name || !(settings == o.settings)
            || markers.size() != o.markers.size() || tracks.size() != o.tracks.size())
        return false;
    for (size_t i = 0; i < markers.size(); ++i)
        if (markers[i].time != o.markers[i].time || markers[i].label != o.markers[i].label
                || markers[i].color != o.markers[i].color)
            return false;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (!tracks[i].sameContent(o.tracks[i])) return false;
    return true;
}

bool ModelDelta::empty() const
{
    return clipsBefore.empty() && clipsAfter.empty() && before.sameContent(after);
}

size_t ModelDelta::changedClipCount() const
{
    std::unordered_set<uint64_t> ids;
    for (const auto& [id, c] : clipsBefore) ids.insert(id);
    for (const auto& [id, c] : clipsAfter) ids.insert(id);
    return ids.size();
}

ModelDelta diffSequence(const Timeline& before, const Timeline& after)
{
    ModelDelta d;
    d.sequenceId = after.id();
    d.before = captureState(before);
    d.after = captureState(after);

    const auto beforeClips = clipsById(before);
    const auto afterClips = clipsById(after);
    for (const auto& [id, a] : afterClips) {
        auto it = beforeClips.find(id);
        if (it == beforeClips.end()) {
            d.clipsAfter.emplace(id, a->cloneForSnapshot());
            continue;
        }
        const Clip* b = it->second;
        if (b->clipType() != a->clipType() || serialized(*b) != serialized(*a)) {
            d.clipsBefore.emplace(id, b->cloneForSnapshot());
            d.clipsAfter.emplace(id, a->cloneForSnapshot());
        }
    }
    for (const auto& [id, b] : beforeClips)
        if (!afterClips.count(id))
            d.clipsBefore.emplace(id, b->cloneForSnapshot());
    return d;
}

void ModelDelta::parkClip(std::unique_ptr<Clip> clip)
{
    if (!clip) return;
    auto& slot = parkedClips[clip->id()];
    if (slot) clipGraveyard.push_back(std::move(slot));   // keep, never free
    slot = std::move(clip);
}

void ModelDelta::parkTrack(std::unique_ptr<Track> track)
{
    if (!track) return;
    auto& slot = parkedTracks[track->id()];
    if (slot) trackGraveyard.push_back(std::move(slot));
    slot = std::move(track);
}

void applyDelta(Timeline& live, ModelDelta& delta, DeltaSide side)
{
    const DeltaSequenceState& state = side == DeltaSide::After ? delta.after : delta.before;
    const auto& copies = side == DeltaSide::After ? delta.clipsAfter : delta.clipsBefore;

    // Take every track and clip out of the live sequence, keyed by id.
    std::unordered_map<uint64_t, std::unique_ptr<Track>> trackPool;
    std::unordered_map<uint64_t, std::unique_ptr<Clip>> clipPool;
    auto poolTrackClips = [&clipPool](Track& t) {
        for (auto& c : ModelDeltaAccess::clips(t))
            if (c) clipPool.emplace(c->id(), std::move(c));
        ModelDeltaAccess::clips(t).clear();
    };
    for (auto& t : ModelDeltaAccess::tracks(live)) {
        if (!t) continue;
        poolTrackClips(*t);
        const uint64_t id = t->id();
        trackPool.emplace(id, std::move(t));
    }
    ModelDeltaAccess::tracks(live).clear();

    // A clip this side needs that isn't live: the parked object if this step
    // removed it earlier (same address as before), else a fresh copy.
    auto takeParkedClip = [&delta](uint64_t id) -> std::unique_ptr<Clip> {
        auto it = delta.parkedClips.find(id);
        if (it == delta.parkedClips.end()) return nullptr;
        auto clip = std::move(it->second);
        delta.parkedClips.erase(it);
        return clip;
    };

    // Rebuild the layout of the requested side.
    for (const DeltaTrack& h : state.tracks) {
        std::unique_ptr<Track> track;
        bool created = false;
        if (auto it = trackPool.find(h.id); it != trackPool.end()) {
            track = std::move(it->second);
            trackPool.erase(it);
        } else if (auto parked = delta.parkedTracks.find(h.id);
                   parked != delta.parkedTracks.end()) {
            track = std::move(parked->second);
            delta.parkedTracks.erase(parked);
            poolTrackClips(*track);   // a removed track may still hold clips
        } else {
            track = std::make_unique<Track>(h.type, h.name);
            ModelDeltaAccess::setTrackId(*track, h.id);
            created = true;
        }
        ModelDeltaAccess::applyHeader(*track, h, created);

        auto& clips = ModelDeltaAccess::clips(*track);
        clips.reserve(h.clipIds.size());
        for (const uint64_t id : h.clipIds) {
            std::unique_ptr<Clip> object;
            if (auto pooled = clipPool.find(id); pooled != clipPool.end()) {
                object = std::move(pooled->second);
                clipPool.erase(pooled);
            } else {
                object = takeParkedClip(id);
            }

            auto copy = copies.find(id);
            if (copy != copies.end()) {
                if (object && object->assignStateFrom(*copy->second)) {
                    clips.push_back(std::move(object));
                } else {
                    delta.parkClip(std::move(object));   // type changed: keep it alive
                    clips.push_back(copy->second->cloneForSnapshot());
                }
            } else if (object) {
                clips.push_back(std::move(object));
            } else {
                spdlog::error("[EDIT] undo: clip {} is neither in the sequence nor "
                              "in the undo record; skipped", id);
            }
        }
        ModelDeltaAccess::tracks(live).push_back(std::move(track));
    }

    live.setName(state.name);
    live.setSettings(state.settings);
    ModelDeltaAccess::setMarkers(live, state.markers);
    // Tracks and clips not part of this side leave the timeline. They are
    // parked, not destroyed: a panel or an older undo step may still point
    // at them, and the opposite side brings these exact objects back.
    for (auto& [id, clip] : clipPool) delta.parkClip(std::move(clip));
    for (auto& [id, track] : trackPool) delta.parkTrack(std::move(track));
    ModelDeltaAccess::notifyStructure(live);
}

} // namespace rt
