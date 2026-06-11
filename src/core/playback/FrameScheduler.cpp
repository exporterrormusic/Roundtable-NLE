/*
 * FrameScheduler.cpp — See FrameScheduler.h for architecture.
 */

#include "playback/FrameScheduler.h"
#include "cache/FrameCache.h"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Construction
// ═════════════════════════════════════════════════════════════════════════════

FrameScheduler::FrameScheduler() = default;

// ═════════════════════════════════════════════════════════════════════════════
//  Consumer API
// ═════════════════════════════════════════════════════════════════════════════

std::shared_ptr<CachedFrame> FrameScheduler::request(
    const Request& req,
    std::shared_ptr<CachedFrame> cachedFrame)
{
    // If the caller already looked up the frame and it's valid, just return it.
    // This avoids a redundant cache lookup inside the scheduler.
    if (cachedFrame && cachedFrame->width > 0) {
        m_cacheHitBytes.fetch_add(
            static_cast<size_t>(cachedFrame->width) * cachedFrame->height * 4,
            std::memory_order_relaxed);
        return cachedFrame;
    }

    // Quick check: is the frame already in the active set (being decoded)?
    // If so, don't re-enqueue — just return nullptr and the worker will
    // deliver it via onFrameDecoded.
    {
        std::lock_guard lock(m_mtx);
        ActiveKey key{req.handle, req.frameNumber};
        if (m_active.find(key) != m_active.end()) {
            return nullptr;
        }
    }

    // Update playhead if provided.
    if (req.playheadTick > 0) {
        m_playheadFrame.store(req.playheadTick, std::memory_order_relaxed);
    }

    // Encode (handle, frameNumber) as a single dedup key.
    uint64_t dedupKey = (static_cast<uint64_t>(req.handle) << 32) |
                         (static_cast<uint64_t>(req.frameNumber) & 0xFFFFFFFF);

    std::lock_guard lock(m_mtx);

    // Dedup: if this exact (handle, frame) is already pending, skip.
    if (m_pendingKeys.find(dedupKey) != m_pendingKeys.end()) {
        // But promote to urgent if needed.
        if (req.urgent) {
            for (auto& pw : m_pending) {
                if (pw.item.handle == req.handle &&
                    pw.item.frameNumber == req.frameNumber) {
                    pw.item.urgent = true;
                    break;
                }
            }
        }
        return nullptr;
    }

    // Check lookahead bounds.
    int64_t playhead = m_playheadFrame.load(std::memory_order_relaxed);
    if (!withinLookahead(req.frameNumber, playhead) && !req.urgent) {
        // Outside lookahead window: don't schedule, return nullptr.
        // The caller will handle it via last-good-frame fallback.
        return nullptr;
    }

    // Enqueue.
    uint64_t seqId = req.sequenceId > 0
        ? req.sequenceId
        : m_nextSequence.fetch_add(1, std::memory_order_relaxed);

    PendingWork pw;
    pw.item.handle      = req.handle;
    pw.item.frameNumber = req.frameNumber;
    pw.item.tier        = req.tier;
    pw.item.urgent      = req.urgent;
    pw.item.sequenceId  = seqId;
    pw.enqueuedAt       = std::chrono::steady_clock::now();

    if (req.urgent) {
        m_pending.push_front(std::move(pw));
    } else {
        m_pending.push_back(std::move(pw));
    }
    m_pendingKeys.insert(dedupKey);

    // Notify workers.
    m_workerCv.notify_one();

    return nullptr;
}

void FrameScheduler::prioritize(MediaHandle handle, int64_t frameNumber,
                                 ResolutionTier /*tier*/)
{
    std::lock_guard lock(m_mtx);

    // Search for the item and move it to the front.
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it->item.handle == handle && it->item.frameNumber == frameNumber) {
            it->item.urgent = true;
            auto item = std::move(*it);
            m_pending.erase(it);
            m_pending.push_front(std::move(item));
            m_workerCv.notify_one();
            return;
        }
    }
}

void FrameScheduler::setPlayhead(int64_t tick)
{
    m_playheadFrame.store(tick, std::memory_order_relaxed);
}

void FrameScheduler::cancel(MediaHandle handle)
{
    std::lock_guard lock(m_mtx);

    // Remove pending items for this handle.
    auto count = removeIf([handle](const PendingWork& pw) {
        return pw.item.handle == handle;
    });
    m_cancelledCount.fetch_add(count, std::memory_order_relaxed);

    // Remove from active set.
    for (auto it = m_active.begin(); it != m_active.end(); ) {
        if (it->handle == handle) {
            m_cancelledCount.fetch_add(1, std::memory_order_relaxed);
            it = m_active.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameScheduler::cancelAll()
{
    std::lock_guard lock(m_mtx);

    int count = static_cast<int>(m_pending.size()) + static_cast<int>(m_active.size());
    m_cancelledCount.fetch_add(count, std::memory_order_relaxed);
    m_pending.clear();
    m_pendingKeys.clear();
    m_active.clear();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Decode worker API
// ═════════════════════════════════════════════════════════════════════════════

std::optional<FrameScheduler::WorkItem> FrameScheduler::dequeue(
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(m_mtx);

    if (m_pending.empty()) {
        m_workerCv.wait_for(lock, timeout, [this] {
            return !m_pending.empty();
        });
    }

    if (m_pending.empty()) {
        return std::nullopt;
    }

    // Pop the front (highest priority).
    PendingWork pw = std::move(m_pending.front());
    m_pending.pop_front();

    // Remove from dedup set.
    uint64_t dedupKey = (static_cast<uint64_t>(pw.item.handle) << 32) |
                         (static_cast<uint64_t>(pw.item.frameNumber) & 0xFFFFFFFF);
    m_pendingKeys.erase(dedupKey);

    // Add to active set.
    ActiveKey key{pw.item.handle, pw.item.frameNumber};
    m_active.insert(key);

    return pw.item;
}

void FrameScheduler::onFrameDecoded(MediaHandle handle, int64_t frameNumber,
                                     ResolutionTier /*tier*/,
                                     std::shared_ptr<CachedFrame> frame)
{
    if (frame && frame->width > 0) {
        m_decodeBytes.fetch_add(
            static_cast<size_t>(frame->width) * frame->height * 4,
            std::memory_order_relaxed);
    }

    std::lock_guard lock(m_mtx);
    ActiveKey key{handle, frameNumber};
    m_active.erase(key);
}

void FrameScheduler::onFrameFailed(MediaHandle handle, int64_t frameNumber,
                                    ResolutionTier /*tier*/)
{
    std::lock_guard lock(m_mtx);
    ActiveKey key{handle, frameNumber};
    m_active.erase(key);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Stats
// ═════════════════════════════════════════════════════════════════════════════

FrameScheduler::Stats FrameScheduler::stats() const
{
    std::lock_guard lock(m_mtx);
    Stats s;
    s.pendingCount      = static_cast<int>(m_pending.size());
    s.activeDecodes     = static_cast<int>(m_active.size());
    s.cancelledThisTick = m_cancelledCount.exchange(0, std::memory_order_relaxed);
    s.cacheHitBytes     = m_cacheHitBytes.load(std::memory_order_relaxed);
    s.decodeBytes       = m_decodeBytes.load(std::memory_order_relaxed);
    return s;
}

void FrameScheduler::resetStats()
{
    m_cancelledCount.store(0, std::memory_order_relaxed);
    m_cacheHitBytes.store(0, std::memory_order_relaxed);
    m_decodeBytes.store(0, std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Private helpers
// ═════════════════════════════════════════════════════════════════════════════

bool FrameScheduler::withinLookahead(int64_t frameNumber, int64_t playheadFrame) const
{
    if (playheadFrame <= 0) return true;  // No playhead set = allow all
    int64_t diff = frameNumber - playheadFrame;
    return diff >= 0 && diff <= m_maxLookahead;
}

template<typename Pred>
int FrameScheduler::removeIf(Pred&& pred)
{
    auto it = std::remove_if(m_pending.begin(), m_pending.end(),
                              std::forward<Pred>(pred));
    int count = static_cast<int>(std::distance(it, m_pending.end()));

    // Clean up dedup keys.
    for (auto rit = it; rit != m_pending.end(); ++rit) {
        uint64_t dedupKey = (static_cast<uint64_t>(rit->item.handle) << 32) |
                             (static_cast<uint64_t>(rit->item.frameNumber) & 0xFFFFFFFF);
        m_pendingKeys.erase(dedupKey);
    }

    m_pending.erase(it, m_pending.end());
    return count;
}

} // namespace rt
