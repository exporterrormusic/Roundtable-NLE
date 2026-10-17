/*
 * SnapshotPublisher — read-only copies of the edited project for render threads.
 *
 * The UI thread is the only thread that may touch the live Project/Timeline.
 * Everything that renders off the UI thread (FrameProducer, prewarm) reads an
 * immutable RenderSnapshot published here instead, so an edit can never change
 * a timeline while a frame is being built from it.
 *
 *   UI thread:   markDirty() after an edit  ->  publish() once per event-loop turn
 *   any thread:  latest()  -> shared_ptr<const RenderSnapshot>, held for one frame
 *
 * Sequences are copied with Timeline::cloneForSnapshot() (clip ids preserved).
 * A sequence that wasn't marked dirty is shared with the previous snapshot.
 * Copies are destroyed on the UI thread: when the last render thread drops one
 * it is parked in a retire list that publish()/collectRetired() frees.
 *
 * Missed markDirty() calls are caught by verifyActiveSequence(), which the UI
 * runs while idle: it hashes the live active sequence against the snapshot,
 * logs the first differing clip and republishes.
 */

#pragma once

#include "playback/EngineContracts.h"
#include "timeline/TimelineObserver.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

class Project;
class Timeline;

class SnapshotPublisher final : private TimelineObserver
{
public:
    enum class DirtyScope : uint8_t
    {
        None,
        ActiveSequence,   ///< an edit to the sequence open in the timeline
        AllSequences      ///< undo/redo, project load, sequence add/remove
    };

    struct PublishStats
    {
        uint64_t editVersion{0};
        size_t   sequencesCloned{0};
        size_t   sequencesShared{0};
        size_t   clipsCloned{0};
        double   cloneMs{0.0};
    };

    SnapshotPublisher();
    ~SnapshotPublisher();

    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    // ── UI thread ───────────────────────────────────────────────────────
    /// Point at the live model. `project` may be null (a bare timeline) and
    /// must own `activeSequence` when set. The publisher observes the active
    /// timeline: track add/remove/move marks it dirty and its destruction
    /// unbinds it. Marks everything dirty; the next publish() copies every
    /// sequence.
    void bind(const Project* project, Timeline* activeSequence);

    void markDirty(DirtyScope scope) noexcept;
    [[nodiscard]] bool isDirty() const noexcept { return m_dirty != DirtyScope::None; }

    /// Copy the dirty sequences and swap in a new snapshot. No-op (returns
    /// false) when nothing is dirty. Also frees retired copies.
    bool publish();

    /// Free copies that render threads have finished with.
    void collectRetired();

    /// Compare live sequences with their published copies: the active one on
    /// every call plus one other in rotation. Returns true when they match.
    /// On a mismatch, fills `detail` (which sequence, first differing clip),
    /// marks the sequences dirty and returns false; the caller logs and
    /// publishes. Costs up to two serialization passes, so run it while idle.
    bool verifySequences(std::string* detail = nullptr);

    [[nodiscard]] PublishStats lastPublishStats() const noexcept { return m_lastStats; }

    // ── Any thread ──────────────────────────────────────────────────────
    /// The most recent snapshot, or null before the first publish / when
    /// unbound. Callers hold it for the duration of one render.
    [[nodiscard]] std::shared_ptr<const RenderSnapshot> latest() const;

private:
    struct RetireList
    {
        std::mutex mutex;
        std::vector<std::unique_ptr<const Timeline>> items;
    };

    // TimelineObserver (active sequence only)
    void onTrackAdded(size_t) override { markDirty(DirtyScope::ActiveSequence); }
    void onTrackRemoved(size_t) override { markDirty(DirtyScope::ActiveSequence); }
    void onTrackMoved(size_t, size_t) override { markDirty(DirtyScope::ActiveSequence); }
    void onTrackPropertyChanged(size_t) override { markDirty(DirtyScope::ActiveSequence); }
    void onTransitionChanged(size_t, size_t) override { markDirty(DirtyScope::ActiveSequence); }
    void onTimelineStructureChanged() override { markDirty(DirtyScope::ActiveSequence); }
    void onTimelineDestroyed(Timeline* tl) override;

    [[nodiscard]] std::shared_ptr<const Timeline> makeCopy(const Timeline& live);
    [[nodiscard]] std::vector<const Timeline*> liveSequences(size_t* activeIndex) const;
    bool verifyOne(size_t index, const Timeline& live, const Timeline& copy,
                   std::string* detail);

    const Project*  m_project{nullptr};
    Timeline*       m_active{nullptr};
    DirtyScope      m_dirty{DirtyScope::AllSequences};
    uint64_t        m_nextEditVersion{1};

    // Live sequence pointers the current snapshot was copied from, by index.
    std::vector<const Timeline*> m_publishedFrom;
    // Hash of each published copy, computed on first verify (copies never
    // change, so one pass per copy). Parallel to the snapshot's sequences.
    std::vector<uint64_t> m_copyHash;
    std::vector<bool>     m_copyHashValid;
    size_t                m_verifyCursor{0};

    mutable std::mutex m_latestMutex;
    std::shared_ptr<const RenderSnapshot> m_latest;

    std::shared_ptr<RetireList> m_retired{std::make_shared<RetireList>()};
    PublishStats m_lastStats{};
};

} // namespace rt
