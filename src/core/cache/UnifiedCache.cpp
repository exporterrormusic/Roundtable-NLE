/*
 * UnifiedCache.cpp — see header for design.
 */

#include "UnifiedCache.h"
#include "FrameCache.h"
#include "GpuTextureCache.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace rt {

UnifiedCache::UnifiedCache() = default;
UnifiedCache::~UnifiedCache() = default;

// ─── Generation ────────────────────────────────────────────────────────────

uint64_t UnifiedCache::onFrameStart() noexcept
{
    return ++m_generation;
}

void UnifiedCache::markAccess(const Key& key) noexcept
{
    m_lastAccess[key] = m_generation;
}

uint64_t UnifiedCache::lastAccess(const Key& key) const noexcept
{
    auto it = m_lastAccess.find(key);
    return it == m_lastAccess.end() ? 0ull : it->second;
}

// ─── Playhead window ───────────────────────────────────────────────────────

void UnifiedCache::setPlayheadWindow(uint64_t mediaId,
                                     int64_t playheadFrame,
                                     int aheadCount,
                                     int behindCount,
                                     ResolutionTier tier)
{
    Window& w = m_windows[mediaId];
    w.playheadFrame = playheadFrame;
    w.aheadCount    = std::max(0, aheadCount);
    w.behindCount   = std::max(0, behindCount);
    w.tier          = tier;
    w.lastUpdate    = std::chrono::steady_clock::now();

    // Forward to the underlying FrameCache so its eviction pass-1 honors
    // the wider window.  GpuTextureCache doesn't have a per-media playhead
    // concept yet — its pinning is via per-key pin counts (see
    // GpuUploadManager::recordPin).  When the full UnifiedCache rewrite
    // lands (Section G of RENDER_GRAPH_PLAN.txt), both caches share one
    // LRU and this forwarding goes away.
    if (m_frameCache) {
        m_frameCache->setPlayheadWindow(mediaId, playheadFrame,
                                        aheadCount, behindCount);
    }
}

bool UnifiedCache::isInWindow(const Key& key) const noexcept
{
    auto it = m_windows.find(key.mediaId);
    if (it == m_windows.end()) return false;
    const Window& w = it->second;
    // Tier is informational only — a Half-tier frame within the window is
    // still pinned even if the window declared Full (lower-tier frames are
    // cheaper to re-decode if needed, but we still prefer to keep them).
    const int64_t lo = w.playheadFrame - w.behindCount;
    const int64_t hi = w.playheadFrame + w.aheadCount;
    return key.frameNumber >= lo && key.frameNumber <= hi;
}

// ─── Eviction coordination ─────────────────────────────────────────────────

void UnifiedCache::runEvictionPass()
{
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (now - m_lastEvictionPass < kEvictionInterval)
        return;
    m_lastEvictionPass = now;

    // Drop stale windows (mediaId no longer active for >30s).
    constexpr auto kWindowTtl = seconds(30);
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if (now - it->second.lastUpdate > kWindowTtl) {
            it = m_windows.erase(it);
        } else {
            ++it;
        }
    }

    // Prune the lastAccess map if it has grown beyond the soft cap.
    // Keep only the kLastAccessSoftCap most-recent generations.
    if (m_lastAccess.size() > kLastAccessSoftCap) {
        // Compute a generation threshold: keep top half, drop bottom half.
        // Quick partial sort over generations.
        std::vector<uint64_t> gens;
        gens.reserve(m_lastAccess.size());
        for (auto& [k, g] : m_lastAccess) gens.push_back(g);
        const size_t mid = gens.size() / 2;
        std::nth_element(gens.begin(), gens.begin() + mid, gens.end());
        const uint64_t cutoff = gens[mid];
        for (auto it = m_lastAccess.begin(); it != m_lastAccess.end();) {
            if (it->second < cutoff) {
                it = m_lastAccess.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ─── Budget rebalance (B5) ─────────────────────────────────────────────────

void UnifiedCache::rebalanceBudgets()
{
    if (m_generation - m_lastRebalanceGen < kRebalanceEveryNFrames)
        return;
    m_lastRebalanceGen = m_generation;

    // Pure stat snapshot for now — actual budget shifting is left to the
    // existing CacheCoordinator (it owns CPU + VRAM hard budgets).  This
    // method logs the imbalance so an operator can confirm the heuristic
    // is sensible before we wire it to setCapacity/setBudget.
    if (!m_frameCache || !m_gpuTexCache) return;

    const auto cpuStats = m_frameCache->stats();
    const double cpuHit = cpuStats.hitRate();
    const size_t gpuHits = m_gpuTexCache->hits();
    const size_t gpuMisses = m_gpuTexCache->misses();
    const double gpuHit = (gpuHits + gpuMisses > 0)
        ? static_cast<double>(gpuHits) / static_cast<double>(gpuHits + gpuMisses)
        : 0.0;

    static int s_log = 0;
    if (++s_log % 4 == 0) {  // every ~120 frames at 30 frames-per-rebalance
        spdlog::info("[UNIFIED] gen={} cpuHit={:.1f}% gpuHit={:.1f}% "
                     "lastAccessEntries={} windows={}",
                     m_generation, cpuHit * 100.0, gpuHit * 100.0,
                     m_lastAccess.size(), m_windows.size());
    }
}

UnifiedCache::Stats UnifiedCache::stats() const noexcept
{
    Stats s;
    s.generation = m_generation;
    s.activeKeys = m_lastAccess.size();

    size_t pinned = 0;
    for (const auto& [mediaId, w] : m_windows) {
        pinned += static_cast<size_t>(w.aheadCount + w.behindCount + 1);
    }
    s.pinnedWindowCount = pinned;

    if (m_frameCache) {
        const auto fs = m_frameCache->stats();
        s.cpuHitRate = fs.hitRate();
    }
    if (m_gpuTexCache) {
        const size_t h = m_gpuTexCache->hits();
        const size_t m = m_gpuTexCache->misses();
        s.gpuHitRate = (h + m > 0)
            ? static_cast<double>(h) / static_cast<double>(h + m) : 0.0;
    }
    return s;
}

} // namespace rt
