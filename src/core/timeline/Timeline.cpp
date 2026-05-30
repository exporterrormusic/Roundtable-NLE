/*
 * Timeline.cpp — Timeline data model implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Timeline.h"
#include "timeline/Clip.h"

#include <algorithm>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace rt {

// ── Construction ────────────────────────────────────────────────────────────

Timeline::Timeline()  = default;

Timeline::~Timeline()
{
    // Tell observers we're going away so any cached raw Timeline* is nulled
    // before this object's storage is reclaimed. Without this, e.g. the
    // AudioMixer meter timer or ExportPanel could dereference a freed timeline
    // during project/sequence close (the bug the /EHa band-aids masked).
    // Copy first: a callback may call removeObserver() and mutate m_observers.
    const auto observers = m_observers;
    for (auto* obs : observers)
        if (obs) obs->onTimelineDestroyed(this);
}

// ── Track management ────────────────────────────────────────────────────────

Track* Timeline::addVideoTrack(const std::string& name)
{
    std::string trackName = name;
    if (trackName.empty()) {
        // Count existing video tracks to get the next number. Dividers are
        // TrackType::Video but aren't real tracks — counting them would
        // produce the wrong "V<N>" name (e.g. "V4" with one divider and two
        // real video tracks).
        int videoCount = 0;
        for (const auto& t : m_tracks)
            if (t->type() == TrackType::Video && !t->isDivider()) ++videoCount;
        trackName = "V" + std::to_string(videoCount + 1);
    }
    // Insert before the first audio track OR the V/A divider, whichever
    // comes first. The divider sits between the video and audio sections,
    // so a new video track must land above it; otherwise it would slot in
    // between the divider and the audio tracks, putting the divider in the
    // middle of the video stack instead of at the V/A boundary.
    size_t insertIdx = m_tracks.size();
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i]->type() == TrackType::Audio || m_tracks[i]->isDivider()) {
            insertIdx = i;
            break;
        }
    }
    auto track = std::make_unique<Track>(TrackType::Video, trackName);
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(insertIdx),
                    std::move(track));
    notifyTrackAdded(insertIdx);
    spdlog::debug("Added video track '{}' at index {}", trackName, insertIdx);
    return ptr;
}

Track* Timeline::addAudioTrack(const std::string& name)
{
    spdlog::info("[Timeline] ENTER addAudioTrack: name='{}'", name);
    std::string trackName = name;
    if (trackName.empty()) {
        // Count existing audio tracks to get the next number
        int audioCount = 0;
        for (const auto& t : m_tracks)
            if (t->type() == TrackType::Audio) ++audioCount;
        trackName = "A" + std::to_string(audioCount + 1);
    }
    m_tracks.push_back(std::make_unique<Track>(TrackType::Audio, trackName));
    size_t idx = m_tracks.size() - 1;
    spdlog::info("[Timeline] addAudioTrack: created Track ptr={} name='{}' idx={}", (void*)m_tracks.back().get(), trackName, idx);
    notifyTrackAdded(idx);
    spdlog::debug("Added audio track '{}' at index {}", trackName, idx);
    return m_tracks.back().get();
}

Track* Timeline::addDividerTrack(size_t insertIndex, bool permanent)
{
    auto track = std::make_unique<Track>(TrackType::Video, "");
    track->setDivider(true);
    track->setPermanentDivider(permanent);
    track->setHeight(10.0f);
    track->setTargeted(false);
    track->setSyncLocked(false);
    if (insertIndex > m_tracks.size()) insertIndex = m_tracks.size();
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(insertIndex),
                    std::move(track));
    notifyTrackAdded(insertIndex);
    spdlog::debug("Added divider track at index {} permanent={}", insertIndex, permanent);
    return ptr;
}

void Timeline::removeTrack(size_t index)
{
    if (index >= m_tracks.size()) return;
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(index));
    notifyTrackRemoved(index);
}

Track* Timeline::insertTrack(size_t index, std::unique_ptr<Track> track)
{
    if (!track) return nullptr;
    if (index > m_tracks.size()) index = m_tracks.size();
    auto* ptr = track.get();
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(index), std::move(track));
    notifyTrackAdded(index);
    return ptr;
}

std::unique_ptr<Track> Timeline::takeTrack(size_t index)
{
    if (index >= m_tracks.size()) return nullptr;
    auto track = std::move(m_tracks[index]);
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(index));
    notifyTrackRemoved(index);
    return track;
}

void Timeline::moveTrack(size_t from, size_t to)
{
    if (from >= m_tracks.size() || to >= m_tracks.size() || from == to) return;

    auto track = std::move(m_tracks[from]);
    m_tracks.erase(m_tracks.begin() + static_cast<ptrdiff_t>(from));
    m_tracks.insert(m_tracks.begin() + static_cast<ptrdiff_t>(to), std::move(track));

    for (auto* obs : m_observers)
        obs->onTrackMoved(from, to);
}

void Timeline::sortTracksByType()
{
    // Legacy migration: really old project files could end up with video
    // and audio tracks interleaved. Stable-partition fixes that by pushing
    // all video tracks above all audio tracks.
    //
    // BUT: dividers are TrackType::Video even when the user placed one
    // in the audio section (e.g. between A1 and A2). Partitioning by type
    // would yank audio-section dividers up to the video region — that
    // was the "dividers reset to the middle on load" bug. The serializer
    // now preserves track order anyway, so we only apply this migration
    // when there are zero dividers (genuinely legacy files). With any
    // divider present, trust the saved layout.
    const bool anyDividers = std::any_of(
        m_tracks.begin(), m_tracks.end(),
        [](const std::unique_ptr<Track>& t) { return t && t->isDivider(); });
    if (anyDividers) return;

    std::stable_partition(m_tracks.begin(), m_tracks.end(),
        [](const std::unique_ptr<Track>& t) {
            return t->type() == TrackType::Video;
        });
}

size_t Timeline::trackCount() const noexcept
{
    return m_tracks.size();
}

Track* Timeline::track(size_t index) noexcept
{
    return index < m_tracks.size() ? m_tracks[index].get() : nullptr;
}

const Track* Timeline::track(size_t index) const noexcept
{
    return index < m_tracks.size() ? m_tracks[index].get() : nullptr;
}

// ── Marker management ───────────────────────────────────────────────────────

void Timeline::addMarker(TimeTick time, const std::string& label, uint32_t color)
{
    Marker m;
    m.time  = time;
    m.label = label;
    m.color = color;

    // Insert sorted by time
    auto it = std::lower_bound(m_markers.begin(), m_markers.end(), m);
    m_markers.insert(it, m);

    for (auto* obs : m_observers)
        obs->onMarkerChanged();
}

void Timeline::removeMarker(size_t index)
{
    if (index >= m_markers.size()) return;
    m_markers.erase(m_markers.begin() + static_cast<ptrdiff_t>(index));

    for (auto* obs : m_observers)
        obs->onMarkerChanged();
}

const std::vector<Marker>& Timeline::markers() const noexcept
{
    return m_markers;
}

// ── Playback state ──────────────────────────────────────────────────────────

TimeTick Timeline::playheadPosition() const noexcept
{
    return m_playhead;
}

void Timeline::setPlayheadPosition(TimeTick pos) noexcept
{
    if (m_playhead != pos)
    {
        m_playhead = pos;
        for (auto* obs : m_observers)
            obs->onPlayheadChanged(m_playhead);
    }
}

TimeTick Timeline::inPoint() const noexcept  { return m_inPoint; }
TimeTick Timeline::outPoint() const noexcept { return m_outPoint; }

void Timeline::setInPoint(TimeTick t) noexcept
{
    m_inPoint = t;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

void Timeline::setOutPoint(TimeTick t) noexcept
{
    m_outPoint = t;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

void Timeline::clearInOutPoints() noexcept
{
    m_inPoint  = -1;
    m_outPoint = -1;
    for (auto* obs : m_observers)
        obs->onInOutChanged();
}

TimeTick Timeline::duration() const noexcept
{
    TimeTick maxDur = 0;
    for (const auto& t : m_tracks)
    {
        TimeTick d = t->duration();
        if (d > maxDur) maxDur = d;
    }
    return maxDur;
}

// ── Observer ────────────────────────────────────────────────────────────────

void Timeline::addObserver(TimelineObserver* obs)
{
    if (obs && std::find(m_observers.begin(), m_observers.end(), obs) == m_observers.end())
        m_observers.push_back(obs);
}

void Timeline::removeObserver(TimelineObserver* obs)
{
    m_observers.erase(
        std::remove(m_observers.begin(), m_observers.end(), obs),
        m_observers.end()
    );
}

// ── Serialization ───────────────────────────────────────────────────────────

const std::string& Timeline::name() const noexcept { return m_name; }
void Timeline::setName(const std::string& name) { m_name = name; }

// ── Notifications ───────────────────────────────────────────────────────────

void Timeline::notifyTrackAdded(size_t index)
{
    for (auto* obs : m_observers)
        obs->onTrackAdded(index);
}

void Timeline::notifyTrackRemoved(size_t index)
{
    for (auto* obs : m_observers)
        obs->onTrackRemoved(index);
}

void Timeline::notifyStructureChanged()
{
    for (auto* obs : m_observers)
        obs->onTimelineStructureChanged();
}

// ── Deep clone ──────────────────────────────────────────────────────────────

std::unique_ptr<Timeline> Timeline::clone() const
{
    auto dup = std::make_unique<Timeline>();
    dup->setName(m_name);

    // Copy all tracks and their clips
    for (size_t ti = 0; ti < m_tracks.size(); ++ti) {
        const Track* srcTrack = m_tracks[ti].get();
        Track* dstTrack = nullptr;
        if (srcTrack->type() == TrackType::Video)
            dstTrack = dup->addVideoTrack(srcTrack->name());
        else
            dstTrack = dup->addAudioTrack(srcTrack->name());

        dstTrack->setLocked(srcTrack->isLocked());
        dstTrack->setMuted(srcTrack->isMuted());
        dstTrack->setSoloed(srcTrack->isSoloed());
        dstTrack->setTargeted(srcTrack->isTargeted());
        dstTrack->setCollapsed(srcTrack->isCollapsed());
        dstTrack->setSyncLocked(srcTrack->isSyncLocked());
        // Divider tracks are stored as TrackType::Video with isDivider=true.
        // Without copying this flag the divider clones as a regular V-track,
        // producing the "extra video layer" seen in duplicated sequences.
        dstTrack->setDivider(srcTrack->isDivider());
        dstTrack->setHeight(srcTrack->height());
        dstTrack->setColor(srcTrack->color());
        dstTrack->setVolume(srcTrack->volume());
        dstTrack->setPan(srcTrack->pan());

        // Clone clips and build old→new ID map for transition remapping
        std::unordered_map<uint64_t, uint64_t> idMap;
        for (size_t ci = 0; ci < srcTrack->clipCount(); ++ci) {
            const Clip* srcClip = srcTrack->clip(ci);
            uint64_t oldId = srcClip->id();
            auto cloned = srcClip->clone();
            uint64_t newId = cloned->id();
            idMap[oldId] = newId;
            dstTrack->addClip(std::move(cloned));
        }

        // Copy transitions, remapping clip IDs to the new cloned clips
        for (size_t tri = 0; tri < srcTrack->transitionCount(); ++tri) {
            Transition t = *srcTrack->transition(tri);
            // Remap leftClipId (0 = no clip, e.g. fade-in from nothing)
            if (t.leftClipId != 0) {
                auto it = idMap.find(t.leftClipId);
                if (it != idMap.end())
                    t.leftClipId = it->second;
            }
            // Remap rightClipId (0 = no clip, e.g. fade-out to nothing)
            if (t.rightClipId != 0) {
                auto it = idMap.find(t.rightClipId);
                if (it != idMap.end())
                    t.rightClipId = it->second;
            }
            dstTrack->addTransition(t);
        }
    }

    // Copy markers
    for (const auto& marker : m_markers) {
        dup->addMarker(marker.time, marker.label, marker.color);
    }

    // Copy playback state
    dup->setPlayheadPosition(m_playhead);
    dup->setInPoint(m_inPoint);
    dup->setOutPoint(m_outPoint);

    return dup;
}

} // namespace rt

