/*
 * FrameScheduler — Unit-of-work scheduler for the frame decode pipeline.
 *
 * Mediates between frame consumers (CompositeService/FrameProducer) and
 * the decode workers (MediaPoolPrefetch).  Replaces the push-based prefetch
 * model ("decode N frames ahead of the playhead unconditionally") with a
 * pull-based model where the consumer requests specific frames and the
 * scheduler bounds lookahead, prioritises urgent frames, and cancels stale
 * work on seek.
 *
 * Key properties:
 *   - Bounded lookahead: never schedules decode >N frames from playhead
 *   - Urgent prioritisation: frames needed RIGHT NOW go to front of queue
 *   - Cancel on seek: flushAll() discards all pending work instantly
 *   - Per-handle sequencing: frames for the same handle are decoded in order
 *   - Backpressure: when maxWorkers are busy, new requests are queued
 *
 * Thread safety: all public methods are thread-safe (internal mutex).
 */

#pragma once

#include "cache/FrameCache.h"  // CachedFrame, ResolutionTier

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

/// Opaque handle for media files (matches MediaPool::MediaHandle).
using MediaHandle = uint64_t;

// ── Forward declarations ────────────────────────────────────────────────────

struct CachedFrame;

// ═════════════════════════════════════════════════════════════════════════════

class FrameScheduler
{
public:
    /// Describes a single frame request from the consumer.
    struct Request
    {
        MediaHandle    handle{0};
        int64_t        frameNumber{0};
        ResolutionTier tier{ResolutionTier::Full};
        bool           urgent{false};       ///< Front of the queue
        uint64_t       sequenceId{0};       ///< Monotonic ordering / dedup
        int64_t        playheadTick{0};     ///< Current playhead for lookahead calc
    };

    /// A work item dequeued by a decode worker.
    struct WorkItem
    {
        MediaHandle    handle;
        int64_t        frameNumber;
        ResolutionTier tier;
        bool           urgent{false};
        uint64_t       sequenceId;
    };

    /// Statistics snapshot.
    struct Stats
    {
        int    pendingCount{0};        ///< Items in the work queue
        int    activeDecodes{0};       ///< Items currently being decoded
        int    cancelledThisTick{0};   ///< Items cancelled since last stats()
        size_t cacheHitBytes{0};       ///< Bytes served from cache
        size_t decodeBytes{0};         ///< Bytes decoded this session
    };

    FrameScheduler();
    ~FrameScheduler() = default;

    FrameScheduler(const FrameScheduler&) = delete;
    FrameScheduler& operator=(const FrameScheduler&) = delete;

    // ── Consumer API (called from composite thread) ─────────────────────

    /// Submit a frame request.  Returns the cached frame if already present,
    /// or nullptr if the frame needs to be decoded.  When returning nullptr,
    /// the request is enqueued for a decode worker (subject to lookahead
    /// and worker limits).
    ///
    /// The caller MUST provide the current playheadTick so the scheduler can
    /// enforce lookahead bounds.  The optional cachedFrame parameter lets
    /// callers pass a frame they already looked up (avoids double-lookup).
    [[nodiscard]] std::shared_ptr<CachedFrame> request(
        const Request& req,
        std::shared_ptr<CachedFrame> cachedFrame = nullptr);

    /// Called by the consumer after a request() returned nullptr to signal
    /// that this frame is the MOST important one right now.  If it's already
    /// queued, it gets promoted to the front.
    void prioritize(MediaHandle handle, int64_t frameNumber, ResolutionTier tier);

    /// Notify the scheduler of the current playhead position so it can
    /// bound lookahead and trim stale work.
    void setPlayhead(int64_t tick);

    /// Cancel all pending and in-flight work for a specific media handle.
    /// Called when a media handle is released.
    void cancel(MediaHandle handle);

    /// Cancel ALL pending and in-flight work.  Called on seek.
    void cancelAll();

    // ── Decode worker API ───────────────────────────────────────────────

    /// Dequeue the next work item for a worker thread.
    /// Blocks for up to `timeout` if the queue is empty.
    /// Returns std::nullopt if no work is available (timeout or shutdown).
    [[nodiscard]] std::optional<WorkItem> dequeue(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

    /// Called by a decode worker when a frame has been decoded.
    /// Stores the frame in the cache and marks the work item as complete.
    void onFrameDecoded(MediaHandle handle, int64_t frameNumber,
                        ResolutionTier tier,
                        std::shared_ptr<CachedFrame> frame);

    /// Called by a decode worker when a frame decode FAILED.
    /// Removes the work item without caching anything.
    void onFrameFailed(MediaHandle handle, int64_t frameNumber,
                       ResolutionTier tier);

    /// Notify workers that new work is available (wake from dequeue wait).
    void notifyWorkers() { m_workerCv.notify_all(); }

    // ── Configuration ───────────────────────────────────────────────────

    void setMaxLookahead(int maxFrames) { m_maxLookahead = std::max(1, maxFrames); }
    [[nodiscard]] int maxLookahead() const { return m_maxLookahead; }

    void setMaxWorkers(int count) { m_maxWorkers = std::max(1, count); }
    [[nodiscard]] int maxWorkers() const { return m_maxWorkers; }

    // ── Lookahead query ──────────────────────────────────────────────

    /// Check whether a frame at `frameNumber` is within the bounded lookahead
    /// window from the given playhead position.  Used by MediaPool's
    /// schedulePrefetch() to skip frames outside the window.
    [[nodiscard]] bool withinLookahead(int64_t frameNumber, int64_t playheadFrame) const;

    // ── Stats ───────────────────────────────────────────────────────────

    [[nodiscard]] Stats stats() const;
    void resetStats();

private:
    /// Internal work queue entry.
    struct PendingWork
    {
        WorkItem     item;
        std::chrono::steady_clock::time_point enqueuedAt;
    };

    /// Remove all pending work matching a predicate.
    template<typename Pred>
    int removeIf(Pred&& pred);

    /// Current playhead frame (for lookahead bounding).
    std::atomic<int64_t> m_playheadFrame{0};

    /// Max frames to decode ahead of playhead.
    int m_maxLookahead{8};

    /// Max concurrent decode workers.
    int m_maxWorkers{2};

    // ── Work queue ──────────────────────────────────────────────────────
    mutable std::mutex              m_mtx;
    std::deque<PendingWork>         m_pending;       ///< Ordered work queue
    std::unordered_set<uint64_t>    m_pendingKeys;   ///< (handle<<32|frame) for dedup

    // ── Active decodes ──────────────────────────────────────────────────
    // Set of (handle, frameNumber) tuples currently being decoded.
    // Used to avoid scheduling duplicate work.
    struct ActiveKey {
        MediaHandle handle;
        int64_t     frameNumber;
        bool operator==(const ActiveKey& o) const {
            return handle == o.handle && frameNumber == o.frameNumber;
        }
    };
    struct ActiveKeyHash {
        size_t operator()(const ActiveKey& k) const {
            return std::hash<uint64_t>{}(k.handle) ^
                   (std::hash<int64_t>{}(k.frameNumber) << 1);
        }
    };
    std::unordered_set<ActiveKey, ActiveKeyHash> m_active;

    // ── Monotonic sequence counter ──────────────────────────────────────
    std::atomic<uint64_t> m_nextSequence{1};

    // ── Worker notification ─────────────────────────────────────────────
    std::mutex              m_workerMtx;
    std::condition_variable m_workerCv;

    // ── Stats counters (mutable for const stats() access) ───────────────
    mutable std::atomic<int>     m_cancelledCount{0};
    mutable std::atomic<size_t>  m_cacheHitBytes{0};
    mutable std::atomic<size_t>  m_decodeBytes{0};
};

} // namespace rt
