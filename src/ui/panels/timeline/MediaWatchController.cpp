/*
 * MediaWatchController.cpp — live media file-swap watching.
 * See the header for the responsibility breakdown; the code was lifted
 * verbatim from TimelineWorkspaceIntegration.cpp / TimelineWorkspaceDeps.cpp
 * during the god-class decomposition (cleanup audit §3.1).
 */

#include "panels/timeline/MediaWatchController.h"

#include "PathUtils.h"

#include "panels/project/ProjectBin.h"

#include "playback/MediaPool.h"
#include "timeline/AudioClip.h"
#include "timeline/Clip.h"
#include "timeline/ImageClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace rt {

namespace {
// (size, mtime-ticks) content signature for live-reload change detection.
// Returns {0,0} if the file can't be stat'd (treated as "changed" so a
// genuine first appearance still reloads).
std::pair<std::uintmax_t, std::int64_t> mediaFileSig(const std::string& p)
{
    std::error_code ec;
    std::uintmax_t sz = std::filesystem::file_size(p, ec);
    if (ec) sz = 0;
    std::error_code ec2;
    auto lwt = std::filesystem::last_write_time(p, ec2);
    std::int64_t mt = ec2 ? 0
        : static_cast<std::int64_t>(lwt.time_since_epoch().count());
    return {sz, mt};
}
} // namespace

MediaWatchController::MediaWatchController(Config cfg)
    : m_cfg(std::move(cfg))
{
}

MediaWatchController::~MediaWatchController()
{
    m_destroying.store(true, std::memory_order_release);
}

void MediaWatchController::forceRescan()
{
    m_lastWant.clear();
    rescan();
}

void MediaWatchController::scheduleDelayedForceRescan(int delayMs)
{
    QTimer::singleShot(delayMs, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        forceRescan();
    });
}

void MediaWatchController::notifyMediaOpened()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!m_openedMarshalQueued.compare_exchange_strong(expected, true))
        return;  // a marshal is already queued — coalesce
    // Callers fire on arbitrary decode/prewarm threads.  Marshal to the GUI
    // thread and (re)start a single-shot debounce timer instead of
    // rescanning immediately: MediaPool opens files constantly during
    // playback, and an unthrottled rescan per open is a GUI-thread storm.
    // A real timeline edit still rescans synchronously via the edit hooks;
    // a swap of an already-watched file fires fileChanged without a rescan,
    // so a ~1.5 s coalescing delay here is harmless.
    QMetaObject::invokeMethod(this, [this]() {
        m_openedMarshalQueued.store(false, std::memory_order_release);
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_openedRescanTimer) {
            m_openedRescanTimer = new QTimer(this);
            m_openedRescanTimer->setSingleShot(true);
            m_openedRescanTimer->setInterval(1500);
            connect(m_openedRescanTimer, &QTimer::timeout, this, [this]() {
                if (m_destroying.load(std::memory_order_acquire)) return;
                rescan();
            });
        }
        if (!m_openedRescanTimer->isActive())
            m_openedRescanTimer->start();
    }, Qt::QueuedConnection);
}

void MediaWatchController::rescan()
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    Timeline* timeline = m_cfg.timeline ? m_cfg.timeline() : nullptr;
    if (!timeline) return;

    // Lazily create the watcher + its debounce timer on first use.
    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        m_debounce = new QTimer(this);
        m_debounce->setSingleShot(true);
        m_debounce->setInterval(250);  // coalesce write bursts

        connect(m_watcher, &QFileSystemWatcher::fileChanged, this,
                [this](const QString& path) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            spdlog::warn("[LIVE-RELOAD] QFileSystemWatcher::fileChanged fired "
                         "for '{}'", path.toStdString());
            m_pending.insert(path.toStdString());
            m_debounce->start();
        });

        connect(m_debounce, &QTimer::timeout, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            auto pending = std::move(m_pending);
            m_pending.clear();
            for (const auto& p : pending) {
                std::error_code ec;
                // A still-missing file is mid-rewrite — requeue and wait.
                if (!std::filesystem::exists(p, ec)) {
                    spdlog::warn("[LIVE-RELOAD] debounce: '{}' missing "
                                 "(mid-rewrite) — requeue", p);
                    m_pending.insert(p);
                    continue;
                }
                // Content-change guard: QFileSystemWatcher fires on
                // attribute touches and Windows replays buffered events on
                // window restore. Only reload if (size, mtime) actually
                // changed — otherwise we'd needlessly invalidate the
                // composite cache and leave the Program Monitor blank.
                auto sig = mediaFileSig(p);
                auto prev = m_sig.find(p);
                if (prev != m_sig.end() && prev->second == sig) {
                    spdlog::warn("[LIVE-RELOAD] debounce: '{}' unchanged "
                                 "(size+mtime same) — skip (spurious event). "
                                 "Handle already released by fileChanged handler.",
                                 p);
                    // Even though the content sig matches, fileChanged
                    // already dropped this path from the watcher.  Re-arm
                    // so the NEXT overwrite is still detected.  Forced,
                    // because the early-out in rescan() would block us.
                    // Also schedule a delayed re-scan for network drives
                    // where the new file may not appear immediately.
                    forceRescan();
                    scheduleDelayedForceRescan();
                    continue;
                }
                m_sig[p] = sig;
                spdlog::warn("[LIVE-RELOAD] debounce: '{}' content changed — "
                             "refreshing", p);
                if (m_cfg.onMediaChanged)
                    m_cfg.onMediaChanged(std::filesystem::path(p));
            }
            if (!m_pending.empty())
                m_debounce->start();
        });
    }

    // ── Gather candidate media paths (UI thread, NO filesystem I/O) ──────
    // We only read the (thread-unsafe) Timeline / MediaPool / ProjectBin here,
    // which is cheap. The existence checks and (size, mtime) stats — which on a
    // cold network drive (G:) blocked the UI for 3–10 s at project-open — are
    // deferred to a background thread below.
    std::set<std::string> candidates;
    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        auto* track = timeline->track(ti);
        if (!track) continue;
        for (size_t ci = 0; ci < track->clipCount(); ++ci) {
            auto* clip = track->clip(ci);
            if (!clip) continue;
            std::string mp;
            if      (auto* v = dynamic_cast<VideoClip*>(clip)) mp = v->mediaPath();
            else if (auto* i = dynamic_cast<ImageClip*>(clip)) mp = i->mediaPath();
            else if (auto* a = dynamic_cast<AudioClip*>(clip)) mp = a->mediaPath();
            if (!mp.empty()) candidates.insert(std::move(mp));
        }
    }
    // The MediaPool is ground truth for files the app actually has open (catches
    // nested sequences and shot-boundary prewarm opens the clip walk misses).
    if (MediaPool* pool = m_cfg.mediaPool ? m_cfg.mediaPool() : nullptr)
        for (const auto& p : pool->openMediaPaths())
            if (!p.empty()) candidates.insert(pathToUtf8(p));
    // Project-Bin assets live-reload even before they're on the timeline.
    if (ProjectBin* bin = m_cfg.projectBin ? m_cfg.projectBin() : nullptr)
        for (const auto& p : bin->allFiles())
            if (!p.empty()) candidates.insert(pathToUtf8(p));

    // Cheap early-out (no I/O): the exact same candidate set is already applied
    // and the watcher still holds paths. A genuine content swap fires
    // fileChanged on an already-watched path and is handled separately; those
    // handlers call forceRescan() to defeat this early-out.
    if (candidates == m_lastWant
        && m_watcher && !m_watcher->files().isEmpty())
        return;

    // Coalesce: never run two stat passes concurrently. If one is in flight,
    // record the request and let its completion handler re-run us.
    if (m_scanInFlight) {
        m_scanQueued = true;
        return;
    }
    m_lastWant = candidates;
    m_scanInFlight = true;

    // Snapshot the currently-watched paths so the worker can stat them too
    // (to refresh/seed signatures) without touching Qt off the UI thread.
    std::vector<std::string> watchedSnapshot;
    for (const QString& w : m_watcher->files())
        watchedSnapshot.push_back(w.toStdString());

    // ── Background: existence + (size, mtime) for every path ─────────────
    std::thread([this, candidates, watchedSnapshot]() {
        std::set<std::string> existing;
        std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> sigs;
        std::set<std::string> all = candidates;
        all.insert(watchedSnapshot.begin(), watchedSnapshot.end());
        for (const auto& p : all) {
            std::error_code ec;
            if (std::filesystem::exists(p, ec) && !ec) {
                existing.insert(p);
                sigs.emplace(p, mediaFileSig(p));
            }
        }
        // Hop back to the UI thread to mutate the QFileSystemWatcher.
        QMetaObject::invokeMethod(this,
            [this, candidates, existing = std::move(existing),
             sigs = std::move(sigs)]() mutable {
                m_scanInFlight = false;
                if (m_destroying.load(std::memory_order_acquire)) return;
                applyScan(candidates, existing, sigs);
                if (m_scanQueued) {
                    m_scanQueued = false;
                    forceRescan();
                }
            });
    }).detach();
}

void MediaWatchController::applyScan(
    const std::set<std::string>& candidates,
    const std::set<std::string>& existing,
    const std::map<std::string, std::pair<std::uintmax_t, std::int64_t>>& sigs)
{
    if (m_destroying.load(std::memory_order_acquire)) return;
    if (!m_watcher) return;
    const auto _t0 = std::chrono::steady_clock::now();

    // Validated want-set = candidates that actually exist on disk.
    QStringList want;
    for (const auto& c : candidates)
        if (existing.count(c)) want << QString::fromStdString(c);
    const QSet<QString> wantSet(want.begin(), want.end());

    // Diff against the currently-watched set: drop stale, add new.
    QStringList toRemove;
    for (const QString& w : m_watcher->files())
        if (!wantSet.contains(w)) toRemove << w;
    if (!toRemove.isEmpty())
        m_watcher->removePaths(toRemove);

    int addFailed = 0;
    for (const QString& w : want)
        if (!m_watcher->addPath(w)) ++addFailed;

    const QStringList nowWatched = m_watcher->files();
    const QSet<QString> nowWatchedSet(nowWatched.begin(), nowWatched.end());

    // Wanted paths the watcher couldn't take (network/external) → poll fallback.
    std::set<std::string> unwatchable;
    for (const QString& w : want)
        if (!nowWatchedSet.contains(w)) unwatchable.insert(w.toStdString());

    if (!unwatchable.empty() && !m_pollTimer) {
        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(2000);  // poll every 2s
        connect(m_pollTimer, &QTimer::timeout, this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            for (const auto& p : m_pollSet) {
                std::error_code ec;
                if (!std::filesystem::exists(p, ec)) continue;
                auto sig = mediaFileSig(p);
                auto prev = m_pollSig.find(p);
                if (prev != m_pollSig.end() && prev->second == sig)
                    continue;  // unchanged
                m_pollSig[p] = sig;
                spdlog::debug("[LIVE-RELOAD] poll detected change in '{}' — "
                              "refreshing", p);
                if (m_cfg.onMediaChanged)
                    m_cfg.onMediaChanged(std::filesystem::path(p));
            }
        });
    }
    m_pollSet = std::move(unwatchable);
    // Seed poll sigs from the worker's stats (no UI-thread I/O).
    for (const auto& p : m_pollSet) {
        auto it = sigs.find(p);
        if (it != sigs.end()) m_pollSig.emplace(p, it->second);
    }
    for (auto it = m_pollSig.begin(); it != m_pollSig.end(); )
        it = (m_pollSet.count(it->first) == 0)
                 ? m_pollSig.erase(it) : std::next(it);
    if (m_pollTimer) {
        if (m_pollSet.empty())
            m_pollTimer->stop();
        else if (!m_pollTimer->isActive())
            m_pollTimer->start();
    }

    // Seed the content signature for every watched path (only if absent — never
    // clobber a sig the debounce handler already advanced), from worker stats.
    std::set<std::string> watchedSet;
    for (const QString& w : nowWatched) {
        std::string s = w.toStdString();
        watchedSet.insert(s);
        auto it = sigs.find(s);
        if (it != sigs.end()) m_sig.emplace(s, it->second);
    }
    for (auto it = m_sig.begin(); it != m_sig.end(); )
        it = (watchedSet.count(it->first) == 0)
                 ? m_sig.erase(it) : std::next(it);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - _t0).count();
    // One concise summary line. Stay quiet at warn level unless the apply was
    // unexpectedly slow (watcher mutation is normally a few ms).
    if (ms > 100.0)
        spdlog::warn("[LIVE-RELOAD] media watch apply SLOW: {} wanted, {} watched, "
                     "{} polled, {} unwatched ({:.0f}ms UI)",
                     want.size(), nowWatched.size(), m_pollSet.size(),
                     addFailed, ms);
    else
        spdlog::debug("[LIVE-RELOAD] media watch updated: {} wanted, {} watched, "
                      "{} polled, {} unwatched ({:.0f}ms UI)",
                      want.size(), nowWatched.size(), m_pollSet.size(),
                      addFailed, ms);
}

// ═════════════════════════════════════════════════════════════════════════════

} // namespace rt
