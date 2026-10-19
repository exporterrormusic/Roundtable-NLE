/*
 * MediaWarmupController — background pre-opening of the timeline's media.
 *
 * After a timeline is set (project open, sequence switch, shot switch),
 * start() opens every video/image file the timeline uses on a few worker
 * threads, nearest the playhead first, and queues a short prefetch (or a
 * loop pre-decode for video characters) so the first composite is fast.
 * While a batch runs, isActive() is true: playback start and the project-
 * open input lock wait on it. When a batch finishes, legacy media masks
 * that needed source dimensions are migrated.
 *
 * A QObject so worker results can be queued to the UI thread; queued
 * calls die with it.
 */

#pragma once

#include <QObject>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rt {

class MediaPool;
class Timeline;

class MediaWarmupController : public QObject {
public:
    struct Config {
        std::function<Timeline*()>  timeline;
        std::function<MediaPool*()> mediaPool;
        /// False until the compositor exists (warming is pointless before).
        std::function<bool()>       canWarm;
        std::function<bool()>       destroying;
        std::function<void()>       invalidateCompositeCache;
        /// UI thread: `done` of `total` files opened.
        std::function<void(int, int)> progress;
        /// UI thread: the current batch finished; repaint with warmed media.
        std::function<void()>       warmed;
        /// UI thread: no batch is running any more.
        std::function<void()>       finished;
    };

    explicit MediaWarmupController(Config cfg) : m_cfg(std::move(cfg)) {}

    /// Start a warm-up of the current timeline, superseding earlier ones.
    void start();
    /// Supersede running batches without waiting for them.
    void cancel() noexcept;
    /// Cancel and join every worker (before the MediaPool goes away).
    void cancelAndJoin() noexcept;

    [[nodiscard]] bool isActive() const noexcept
    {
        return m_active.load(std::memory_order_acquire) != 0;
    }

private:
    struct Batch;

    void reapFinished();
    /// Upgrade legacy media masks once MediaPool knows the source
    /// dimensions/rotation. Offline sources stay in legacy space.
    int migrateDeferredLegacyMediaMasks();

    Config m_cfg;
    /// Batches still running (one count per batch, not per worker).
    std::atomic<int> m_active{0};
    std::atomic<uint64_t> m_generation{0};
    std::vector<std::shared_ptr<Batch>> m_batches;
};

} // namespace rt
