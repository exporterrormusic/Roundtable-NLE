/*
 * MediaWatchController.h — Live media file-swap watching for TimelineWorkspace.
 *
 * Watches every media file the timeline references so overwriting one in
 * Windows Explorer (replacing a .png with a same-named new version) hot-reloads
 * it into the project, Premiere-style.  Extracted from TimelineWorkspace
 * (Integration/Deps .cpp files) as the first binder class of the god-class
 * decomposition (fable_cleanup.txt §3.1).
 *
 * Responsibilities:
 *  - arm/diff a QFileSystemWatcher against the timeline + MediaPool +
 *    ProjectBin media set (rescan)
 *  - debounced fileChanged handling with a (size, mtime) content-signature
 *    guard (Windows replays spurious events on window restore / attribute
 *    touches; reloading on those left the Program Monitor blank)
 *  - 2-second polling fallback for paths QFileSystemWatcher silently rejects
 *    (network/external drives like G:)
 *  - background-thread stat pass so a cold network drive can't freeze the UI
 *    for seconds at project-open
 *  - cross-thread coalescing of MediaPool onMediaOpened notifications into
 *    one debounced rescan (notifyMediaOpened)
 *
 * The reload reaction itself (cache eviction, recomposite, panel refresh)
 * stays in TimelineWorkspace::refreshChangedMedia — injected as
 * Config::onMediaChanged.
 */
#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

class QFileSystemWatcher;
class QTimer;

namespace rt {

class MediaPool;
class ProjectBin;
class Timeline;

class MediaWatchController : public QObject {
public:
    /// Collaborator accessors injected from the owning TimelineWorkspace.
    /// Accessor functions rather than raw pointers because the timeline /
    /// pool / bin are (re)injected over the workspace's lifetime.
    struct Config {
        std::function<Timeline*()>   timeline;
        std::function<MediaPool*()>  mediaPool;
        std::function<ProjectBin*()> projectBin;
        /// Reaction to a confirmed content change
        /// (TimelineWorkspace::refreshChangedMedia).
        std::function<void(const std::filesystem::path&)> onMediaChanged;
    };

    /// Must be constructed on the UI thread — notifyMediaOpened() and the
    /// background stat pass marshal back to this object's thread affinity.
    explicit MediaWatchController(Config cfg);
    ~MediaWatchController() override;

    /// (Re)arm the watcher for the current media set.  Cheap and idempotent —
    /// safe to call after any timeline mutation, project load, or media
    /// import.  Gathers candidates on the UI thread (no filesystem I/O),
    /// stats them on a background thread, then applies the watcher diff back
    /// on the UI thread.  Concurrent calls coalesce.
    void rescan();

    /// rescan() with the same-candidate-set early-out defeated.  Use after a
    /// file replacement: Qt drops a replaced path from the watcher and may
    /// report a stale files() list, so only a forced rescan guarantees the
    /// path is re-armed.
    void forceRescan();

    /// Schedule a forceRescan() after `delayMs`.  Used after a file
    /// replacement on network/external drives where the new file may not
    /// exist yet when the immediate rescan runs (Explorer mid-copy).
    void scheduleDelayedForceRescan(int delayMs = 3000);

    /// Thread-safe: MediaPool opened a file (prewarm, lookahead, timeline
    /// clip, bin preview...).  Coalesces a burst of opens into ONE rescan
    /// ~1.5 s later — an unthrottled rescan per open was a GUI-thread storm.
    void notifyMediaOpened();

private:
    /// UI-thread apply step for rescan(): given the off-thread existence +
    /// (size, mtime) results, diff the QFileSystemWatcher, arm the poll
    /// fallback for unwatchable paths, and seed content signatures.  Never
    /// touches the filesystem itself, so it stays cheap on the GUI thread.
    void applyScan(
        const std::set<std::string>& candidates,
        const std::set<std::string>& existing,
        const std::map<std::string, std::pair<std::uintmax_t, std::int64_t>>& sigs);

    Config m_cfg;

    /// fileChanged is debounced because editors/Explorer often write in
    /// several bursts.
    QFileSystemWatcher* m_watcher{nullptr};
    QTimer*             m_debounce{nullptr};
    std::set<std::string> m_pending;   ///< paths awaiting debounced reload

    /// Per-path (size, mtime) signature so a fileChanged whose content
    /// didn't actually change does NOT trigger a reload+composite-invalidate.
    std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> m_sig;

    /// Last media-path set applyScan() actually applied — lets a (debounced)
    /// rescan early-out as a cheap no-op when nothing changed.
    std::set<std::string> m_lastWant;

    // ── Polling fallback for unwatchable paths ─────────────────────────
    QTimer* m_pollTimer{nullptr};
    /// Paths QFileSystemWatcher could not watch — polled instead.
    std::set<std::string> m_pollSet;
    /// Last-known sig for polled paths (same format as m_sig).
    std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> m_pollSig;

    /// Coalescing for the background stat pass.
    bool m_scanInFlight{false};   ///< a background stat pass is running
    bool m_scanQueued{false};     ///< a rescan was requested mid-flight

    // ── MediaPool onMediaOpened coalescing ─────────────────────────────
    std::atomic<bool> m_openedMarshalQueued{false};  ///< one cross-thread marshal at a time
    QTimer*           m_openedRescanTimer{nullptr};  ///< 1.5 s single-shot debounce

    /// Destruction guard — checked by timer lambdas, the cross-thread
    /// marshal, and the background-thread continuation.  Same lifetime
    /// story as the pre-extraction code: the controller (like the
    /// workspace that owns it) lives until app shutdown, so the detached
    /// stat thread's invokeMethod target is valid in practice.
    std::atomic<bool> m_destroying{false};
};

} // namespace rt
