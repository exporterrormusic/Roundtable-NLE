/*
 * SegmentRenderCache.cpp — see SegmentRenderCache.h for the contract.
 */

#include "cache/SegmentRenderCache.h"

namespace rt {

SegmentRenderCache::SegmentRenderCache(size_t budgetBytes)
    : m_budgetBytes(budgetBytes)
{
}

void SegmentRenderCache::dropEntry_locked(Map::iterator it)
{
    m_curBytes -= it->second.bytes;
    m_lru.erase(it->second.lruIt);
    m_entries.erase(it);
}

void SegmentRenderCache::evictToFit_locked()
{
    // Evict least-recently-used (LRU tail) until within budget.  Never evict
    // the very last entry below budget — if a single frame exceeds the whole
    // budget we still keep it (the alternative is never caching at all).
    while (m_curBytes > m_budgetBytes && m_lru.size() > 1) {
        const Key victim = m_lru.back();
        auto it = m_entries.find(victim);
        if (it == m_entries.end()) { m_lru.pop_back(); continue; }  // defensive
        dropEntry_locked(it);
    }
}

void SegmentRenderCache::put(int64_t tick, ResolutionTier tier,
                             uint64_t configHash,
                             std::shared_ptr<CachedFrame> frame)
{
    if (!frame) return;
    const Key key{tick, static_cast<uint8_t>(tier)};
    const size_t bytes = frame->memoryUsage();

    std::lock_guard lk(m_mtx);

    // Replace any existing entry for this (tick, tier).
    if (auto existing = m_entries.find(key); existing != m_entries.end())
        dropEntry_locked(existing);

    m_lru.push_front(key);
    Entry e;
    e.configHash = configHash;
    e.frame      = std::move(frame);
    e.bytes      = bytes;
    e.lruIt      = m_lru.begin();
    m_entries.emplace(key, std::move(e));
    m_curBytes += bytes;

    evictToFit_locked();
}

std::shared_ptr<CachedFrame> SegmentRenderCache::get(int64_t tick,
                                                     ResolutionTier tier,
                                                     uint64_t configHash)
{
    const Key key{tick, static_cast<uint8_t>(tier)};
    std::lock_guard lk(m_mtx);

    auto it = m_entries.find(key);
    if (it == m_entries.end()) return nullptr;

    if (it->second.configHash != configHash) {
        // Stale — the composite changed since this frame was rendered.  Drop
        // it so we don't keep probing a dead entry.
        dropEntry_locked(it);
        return nullptr;
    }

    // Promote to MRU.
    m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);
    return it->second.frame;
}

bool SegmentRenderCache::hasFresh(int64_t tick, ResolutionTier tier,
                                  uint64_t configHash) const
{
    const Key key{tick, static_cast<uint8_t>(tier)};
    std::lock_guard lk(m_mtx);
    auto it = m_entries.find(key);
    return it != m_entries.end() && it->second.configHash == configHash;
}

void SegmentRenderCache::clear()
{
    std::lock_guard lk(m_mtx);
    m_entries.clear();
    m_lru.clear();
    m_curBytes = 0;
}

void SegmentRenderCache::invalidateRange(int64_t fromTick, int64_t toTick)
{
    std::lock_guard lk(m_mtx);
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (it->first.tick >= fromTick && it->first.tick <= toTick) {
            const auto cur = it++;
            dropEntry_locked(cur);
        } else {
            ++it;
        }
    }
}

void SegmentRenderCache::setBudgetBytes(size_t budgetBytes)
{
    std::lock_guard lk(m_mtx);
    m_budgetBytes = budgetBytes;
    evictToFit_locked();
}

size_t SegmentRenderCache::sizeBytes() const
{
    std::lock_guard lk(m_mtx);
    return m_curBytes;
}

size_t SegmentRenderCache::count() const
{
    std::lock_guard lk(m_mtx);
    return m_entries.size();
}

} // namespace rt
