/*
 * CompositeHold.cpp -- see CompositeHold.h.
 */

#include "playback/CompositeHold.h"

#include "cache/FrameCache.h"               // CachedFrame
#include "playback/FrameFallbackPolicy.h"   // isNearbyPlaybackCompositeTick

#include <algorithm>
#include <cstdlib>

namespace rt {

bool sameVisualClipSet(const std::vector<uint64_t>& current,
                       const std::vector<uint64_t>& previous)
{
    auto sortedPrevious = previous;
    std::sort(sortedPrevious.begin(), sortedPrevious.end());
    sortedPrevious.erase(
        std::unique(sortedPrevious.begin(), sortedPrevious.end()),
        sortedPrevious.end());
    return current == sortedPrevious;
}

std::shared_ptr<CachedFrame> CompositeHold::lastGood() const
{
    std::lock_guard lock(m_mutex);
    return m_lastGood;
}

std::shared_ptr<CachedFrame> CompositeHold::lastGoodWithoutGpu()
{
    std::lock_guard lock(m_mutex);
    if (m_lastGood) {
        m_lastGood->gpuReady     = false;
        m_lastGood->gpuImageView = 0;
        m_lastGood->gpuSampler   = 0;
        m_lastGood->gpuSemaphore = 0;
    }
    return m_lastGood;
}

bool CompositeHold::isHeldFromAnotherTick(const std::shared_ptr<CachedFrame>& frame,
                                          int64_t tick) const
{
    std::lock_guard lock(m_mutex);
    return frame == m_lastGood && m_lastGoodTick >= 0 && m_lastGoodTick != tick;
}

void CompositeHold::adoptLastGood(std::shared_ptr<CachedFrame> frame, int64_t tick)
{
    std::lock_guard lock(m_mutex);
    m_lastGood = std::move(frame);
    m_lastGoodTick = tick;
}

void CompositeHold::clearAll()
{
    std::lock_guard lock(m_mutex);
    m_lastGood.reset();
    m_lastGoodTick = -1;
    m_lastGoodClipIds.clear();
    m_static.reset();
    m_staticKey = 0;
    // Re-arm the settle clock so the next partial composite gets the same
    // first-view grace as cold startup instead of a stale timestamp.
    m_lastCompleteAt = Clock::time_point{};
}

void CompositeHold::clearLastGoodAndSettle()
{
    std::lock_guard lock(m_mutex);
    m_lastGood.reset();
    m_lastGoodTick = -1;
    m_lastCompleteAt = Clock::time_point{};
}

void CompositeHold::invalidateRange(int64_t fromTick, int64_t toTick)
{
    std::lock_guard lock(m_mutex);
    m_static.reset();
    m_staticKey = 0;
    // Keep a held frame outside the range to bridge the next composite.
    if (m_lastGood && m_lastGoodTick >= fromTick && m_lastGoodTick <= toTick) {
        m_lastGood.reset();
        m_lastGoodTick = -1;
        m_lastGoodClipIds.clear();
        // The held frame covered the edit, so re-arm the settle clock too:
        // the next partial composite gets the first-view hold.
        m_lastCompleteAt = Clock::time_point{};
    }
}

void CompositeHold::clearStatic()
{
    std::lock_guard lock(m_mutex);
    m_static.reset();
    m_staticKey = 0;
}

CompositeHold::Settle CompositeHold::settle(
    Clock::time_point now, int64_t tick,
    const std::function<std::vector<uint64_t>()>& currentClipIds)
{
    Settle result;
    std::lock_guard lock(m_mutex);
    // First view: arm the settle clock on the first incomplete composite.
    // Without this the very first composite at startup (or after an
    // invalidation) finds the clock at epoch 0, skips the settle entirely,
    // and layers pop in one by one while the cold decoders open.
    if (m_lastCompleteAt.time_since_epoch().count() == 0) {
        m_lastCompleteAt = now;
        result.firstView = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastCompleteAt).count();
    if (elapsed >= kSettleWindowMs)
        return result;
    // Shot/seek guard: the complete active visual clip set must match.
    // "Any overlap" is not enough: a long-lived bar/logo above the edit
    // would make every shot look continuous.
    bool sameShot = true;
    if (m_lastGood && !m_lastGoodClipIds.empty()) {
        sameShot = sameVisualClipSet(currentClipIds(), m_lastGoodClipIds) &&
                   m_lastGoodTick >= 0 &&
                   isNearbyPlaybackCompositeTick(tick, m_lastGoodTick);
    }
    if (sameShot) {
        result.frame = m_lastGood;   // may be null on first view
        result.withinWindow = true;
    }
    return result;
}

CompositeHold::Stale CompositeHold::staleFallback(
    int64_t tick, const std::vector<uint64_t>& currentClipIds)
{
    Stale result;
    std::lock_guard lock(m_mutex);
    if (!m_lastGood || m_lastGoodTick < 0)
        return result;
    result.lastTick = m_lastGoodTick;
    const bool sameShot =
        sameVisualClipSet(currentClipIds, m_lastGoodClipIds) &&
        isNearbyPlaybackCompositeTick(tick, m_lastGoodTick);
    if (!sameShot) {
        // Drop it so later ticks here don't keep returning the old shot.
        m_lastGood.reset();
        m_lastGoodTick = -1;
        m_lastGoodClipIds.clear();
        result.kind = Stale::Kind::DifferentShot;
        return result;
    }
    result.delta = std::abs(tick - m_lastGoodTick);
    if (result.delta <= kMaxStaleTicks) {
        result.kind = Stale::Kind::Reuse;
        result.frame = m_lastGood;
    } else {
        result.kind = Stale::Kind::TooOld;
    }
    return result;
}

std::shared_ptr<CachedFrame> CompositeHold::staticHit(
    uint64_t key, int64_t tick, std::vector<uint64_t> clipIds)
{
    std::lock_guard lock(m_mutex);
    if (!m_static || m_staticKey != key)
        return nullptr;
    if (!m_static->hasNv12()) {
        m_lastGood = m_static;
        m_lastGoodTick = tick;
        m_lastGoodClipIds = std::move(clipIds);
    }
    return m_static;
}

bool CompositeHold::rememberFresh(std::shared_ptr<CachedFrame> frame, int64_t tick,
                                  std::vector<uint64_t> clipIds,
                                  std::optional<uint64_t> staticKey, bool complete)
{
    std::lock_guard lock(m_mutex);
    const bool wasLastGood = (m_lastGood == frame);
    m_lastGood = frame;
    m_lastGoodTick = tick;
    if (staticKey) {
        m_static = std::move(frame);
        m_staticKey = *staticKey;
    } else {
        m_static.reset();
        m_staticKey = 0;
    }
    m_lastGoodClipIds = std::move(clipIds);
    if (complete)
        m_lastCompleteAt = Clock::now();
    return wasLastGood;
}

void CompositeHold::rememberStaticOnly(std::shared_ptr<CachedFrame> frame,
                                       std::optional<uint64_t> staticKey)
{
    std::lock_guard lock(m_mutex);
    if (staticKey) {
        m_static = std::move(frame);
        m_staticKey = *staticKey;
    } else {
        m_static.reset();
        m_staticKey = 0;
    }
}

} // namespace rt
