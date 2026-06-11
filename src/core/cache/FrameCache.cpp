/*
 * FrameCache.cpp — LRU frame cache implementation
 */

#include "FrameCache.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace rt {

// ─── Construction ───────────────────────────────────────────────────────────

FrameCache::FrameCache(size_t capacityBytes)
    : m_capacity(capacityBytes)
{
    spdlog::info("FrameCache created — capacity {:.1f} MB",
                 m_capacity / (1024.0 * 1024.0));
}

FrameCache::~FrameCache()
{
    clear();
}

// ─── Insert ─────────────────────────────────────────────────────────────────

void FrameCache::put(std::shared_ptr<CachedFrame> frame)
{
    if (!frame || (frame->pixels.empty() && !frame->gpuReady && !frame->lazyReadback)) return;

    CacheKey key{frame->mediaId, frame->frameNumber, frame->tier};
    size_t frameBytes = frame->memoryUsage();

    // If this single frame exceeds total capacity, don't cache it
    if (frameBytes > m_capacity) {
        spdlog::warn("FrameCache: frame too large ({:.1f} MB > {:.1f} MB capacity)",
                     frameBytes / (1024.0 * 1024.0), m_capacity / (1024.0 * 1024.0));
        return;
    }

    std::lock_guard lock(m_mutex);

    // If key already exists, keep it (first decode wins).  The same
    // (mediaId, frameNumber, tier) tuple should produce identical pixels,
    // but in practice NVDEC and the software decoder render H.264 frames
    // with subtly different RGB (BT.709 limited vs sws_scale's default
    // colorspace).  When both pipelines race to fill the cache and the
    // second overwrote the first, downstream GPU uploads alternated
    // between the two outputs and the user saw flicker on dark areas.
    // Promote the existing entry to MRU so it isn't evicted.
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);
        return;
    }

    // Evict until we have space
    evictUntilFits(frameBytes);

    // Insert at front of LRU
    m_lru.push_front(key);
    CacheEntry entry{std::move(frame), m_lru.begin(), frameBytes};
    m_map.emplace(key, std::move(entry));
    m_used += frameBytes;
}

// ─── Lookup ─────────────────────────────────────────────────────────────────

std::shared_ptr<CachedFrame> FrameCache::get(
    uint64_t mediaId, int64_t frameNumber, ResolutionTier tier)
{
    std::lock_guard lock(m_mutex);

    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it == m_map.end()) {
        ++m_misses;
        return nullptr;
    }

    ++m_hits;

    // Promote to front of LRU
    m_lru.erase(it->second.lruIt);
    m_lru.push_front(key);
    it->second.lruIt = m_lru.begin();

    return it->second.frame;
}

std::shared_ptr<CachedFrame> FrameCache::getNoPromote(
    uint64_t mediaId, int64_t frameNumber, ResolutionTier tier) const
{
    std::lock_guard lock(m_mutex);

    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it == m_map.end()) {
        ++m_misses;
        return nullptr;
    }

    ++m_hits;
    // Do NOT promote in LRU — this is a fallback/nearby search.
    // Promoting old frames ahead of prefetch causes eviction thrashing.
    return it->second.frame;
}

void FrameCache::setPlayhead(uint64_t mediaId, int64_t frameNumber)
{
    std::lock_guard lock(m_mutex);
    m_playheads[mediaId] = frameNumber;
}

void FrameCache::setPlayheadWindow(uint64_t mediaId,
                                   int64_t playheadFrame,
                                   int aheadCount,
                                   int behindCount)
{
    std::lock_guard lock(m_mutex);
    m_playheads[mediaId] = playheadFrame;
    WindowExtent& w = m_windows[mediaId];
    w.behind = std::max(0, behindCount);
    w.ahead  = std::max(0, aheadCount);
}

bool FrameCache::contains(uint64_t mediaId, int64_t frameNumber,
                           ResolutionTier tier) const
{
    std::lock_guard lock(m_mutex);
    CacheKey key{mediaId, frameNumber, tier};
    return m_map.find(key) != m_map.end();
}

std::shared_ptr<CachedFrame> FrameCache::getNearestBefore(
    uint64_t mediaId, int64_t preferFrame, ResolutionTier tier) const
{
    std::lock_guard lock(m_mutex);

    // Scan the map for the best candidate.  This is O(N) in cache size
    // but only runs on the rare miss path (SKIPPING-clip fallback), so
    // the extra cost is paid only when we'd otherwise drop a layer.
    std::shared_ptr<CachedFrame> bestBefore;
    int64_t bestBeforeDist = INT64_MAX;
    std::shared_ptr<CachedFrame> bestAny;
    int64_t bestAnyDist = INT64_MAX;

    for (const auto& [key, entry] : m_map) {
        if (key.mediaId != mediaId || key.tier != tier) continue;
        if (!entry.frame) continue;
        if (entry.frame->pixels.empty() && !entry.frame->gpuReady) continue;
        const int64_t dist = std::abs(key.frameNumber - preferFrame);
        if (key.frameNumber <= preferFrame && dist < bestBeforeDist) {
            bestBeforeDist = dist;
            bestBefore = entry.frame;
        }
        if (dist < bestAnyDist) {
            bestAnyDist = dist;
            bestAny = entry.frame;
        }
    }
    return bestBefore ? bestBefore : bestAny;
}

// ─── Bulk operations ────────────────────────────────────────────────────────

void FrameCache::evictMedia(uint64_t mediaId)
{
    std::lock_guard lock(m_mutex);

    auto it = m_lru.begin();
    while (it != m_lru.end()) {
        if (it->mediaId == mediaId) {
            auto mapIt = m_map.find(*it);
            if (mapIt != m_map.end()) {
                m_used -= mapIt->second.sizeBytes;
                m_map.erase(mapIt);
                ++m_evictions;
            }
            it = m_lru.erase(it);
        } else {
            ++it;
        }
    }

    // Remove stale playhead so eviction doesn't protect phantom media.
    m_playheads.erase(mediaId);
}


// ─── GPU-co-owned eviction (cross-cache VRAM pressure) ─────────────────────

size_t FrameCache::evictGpuCoOwned(size_t minBytes)
{
    std::lock_guard lock(m_mutex);

    if (m_lru.empty() || minBytes == 0)
        return 0;

    size_t freed = 0;

    // Walk from LRU back (least recently used) toward front.
    // Evict frames that have a non-null gpuTextureOwner (co-own GPU
    // memory with GpuTextureCache via putShared).
    // Never evict pinned frames (static images).
    auto it = m_lru.end();
    while (freed < minBytes && it != m_lru.begin()) {
        --it;
        auto mapIt = m_map.find(*it);
        if (mapIt == m_map.end()) {
            it = m_lru.erase(it);
            continue;
        }

        auto& frame = mapIt->second.frame;
        if (!frame || frame->pinned) {
            continue;  // never evict pinned
        }
        if (!frame->gpuTextureOwner) {
            continue;  // no GPU co-ownership — skip
        }

        // This frame co-owns GPU memory.  Evicting it releases the
        // shared_ptr, which may trigger GPU texture destruction in
        // the shared_ptrs aliased by GpuTextureCache::putShared().
        size_t frameBytes = mapIt->second.sizeBytes;
        m_used -= frameBytes;
        freed += frameBytes;
        m_map.erase(mapIt);
        it = m_lru.erase(it);
        ++m_evictions;
    }

    return freed;
}

void FrameCache::removePlayhead(uint64_t mediaId)
{
    std::lock_guard lock(m_mutex);
    m_playheads.erase(mediaId);
}

void FrameCache::clear()
{
    std::lock_guard lock(m_mutex);
    m_map.clear();
    m_lru.clear();
    m_playheads.clear();
    m_used = 0;
}

void FrameCache::setCapacity(size_t capacityBytes)
{
    std::lock_guard lock(m_mutex);
    m_capacity = capacityBytes;

    // Evict if shrunk
    while (m_used > m_capacity && !m_lru.empty()) {
        auto& backKey = m_lru.back();
        auto mapIt = m_map.find(backKey);
        if (mapIt != m_map.end()) {
            m_used -= mapIt->second.sizeBytes;
            m_map.erase(mapIt);
            ++m_evictions;
        }
        m_lru.pop_back();
    }

    spdlog::info("FrameCache capacity set to {:.1f} MB, currently {:.1f} MB used",
                 m_capacity / (1024.0 * 1024.0), m_used / (1024.0 * 1024.0));
}

void FrameCache::setMaxEntries(size_t maxEntries)
{
    std::lock_guard lock(m_mutex);
    m_maxEntries = maxEntries;

    // Trim if the new cap is below the current entry count.  Walk the
    // LRU from the back (least-recently-used) toward the front and
    // erase non-pinned entries until the count fits.  Pinned entries
    // (e.g. static-image clips whose re-decode is expensive) are
    // skipped — the trim stops early if every remaining entry is
    // pinned.  This is a one-shot adjustment; the in-line check inside
    // evictUntilFits drives steady-state behaviour.
    if (m_maxEntries == 0) return;
    if (m_map.size() <= m_maxEntries) {
        spdlog::info("FrameCache max entries set to {}, currently {} entries",
                     m_maxEntries, m_map.size());
        return;
    }

    auto it = m_lru.end();
    while (m_map.size() > m_maxEntries && it != m_lru.begin()) {
        --it;
        auto mapIt = m_map.find(*it);
        if (mapIt == m_map.end()) {
            it = m_lru.erase(it);
            continue;
        }
        if (mapIt->second.frame && mapIt->second.frame->pinned) {
            // skip — keep iterating toward the front
            continue;
        }
        m_used -= mapIt->second.sizeBytes;
        m_map.erase(mapIt);
        it = m_lru.erase(it);
        ++m_evictions;
    }

    if (m_map.size() > m_maxEntries) {
        static int s_warned = 0;
        if (++s_warned <= 3) {
            spdlog::warn("FrameCache::setMaxEntries: cap={} but {} "
                         "entries remain (all pinned)",
                         m_maxEntries, m_map.size());
        }
    }

    spdlog::info("FrameCache max entries set to {}, currently {} entries",
                 m_maxEntries, m_map.size());
}

size_t FrameCache::maxEntries() const noexcept
{
    return m_maxEntries;
}

size_t FrameCache::gpuCoOwnedCount() const
{
    std::lock_guard lock(m_mutex);
    size_t n = 0;
    for (const auto& [key, entry] : m_map) {
        (void)key;
        if (entry.frame && entry.frame->gpuTextureOwner) ++n;
    }
    return n;
}


// ─── Statistics ─────────────────────────────────────────────────────────────

CacheStats FrameCache::stats() const
{
    std::lock_guard lock(m_mutex);
    return CacheStats{
        .hitCount       = m_hits,
        .missCount      = m_misses,
        .evictionCount  = m_evictions,
        .frameCount     = m_map.size(),
        .memoryUsed     = m_used,
        .memoryCapacity = m_capacity,
    };
}

size_t FrameCache::frameCount() const
{
    std::lock_guard lock(m_mutex);
    return m_map.size();
}

size_t FrameCache::memoryUsed() const
{
    std::lock_guard lock(m_mutex);
    return m_used;
}

// ─── Eviction ───────────────────────────────────────────────────────────────

void FrameCache::evictUntilFits(size_t neededBytes)
{
    // Caller holds m_mutex.
    //
    // Two-pass eviction strategy:
    //   Pass 1: Evict frames that are BEHIND all known playheads first.
    //           These are "stale" frames that playback will never revisit.
    //   Pass 2: Fall back to pure LRU eviction (original behavior).
    //
    // This protects prefetched future frames from being evicted by
    // near-playhead accesses that would otherwise promote old frames
    // in the LRU and push prefetch to the back of the queue.
    //
    // Eviction trigger combines two signals:
    //   - byte pressure:  m_used + neededBytes > m_capacity
    //   - entry pressure: m_map.size() > m_maxEntries (when m_maxEntries != 0)
    //
    // The entry-pressure signal is what catches the GPU-resident leak:
    // a frame with gpuReady=true and pixels.empty() has memoryUsage() ≈
    // 200 bytes but pins an 8 MB VkImage via gpuTextureOwner.  Without
    // an entry cap, FrameCache fills with orphan-textured CachedFrames
    // for ~45 s before VRAM exhausts and ZC collapses to 0.

    auto overCapacity = [&]() {
        if (m_used + neededBytes > m_capacity) return true;
        if (m_maxEntries != 0 && m_map.size() > m_maxEntries) return true;
        return false;
    };

    if (!overCapacity())
        return;

    size_t pass1Evicted = 0, pass2Evicted = 0;
    size_t pass1Bytes = 0, pass2Bytes = 0;

    // Pass 1: Scan from LRU back and evict frames behind playheads.
    // Only do this when we have playhead info.
    if (!m_playheads.empty()) {
        auto it = m_lru.end();
        while (it != m_lru.begin() && overCapacity()) {
            --it;
            auto& key = *it;

            // Never evict pinned frames (e.g. static images).
            {
                auto pinIt = m_map.find(key);
                if (pinIt != m_map.end() && pinIt->second.frame &&
                    pinIt->second.frame->pinned) {
                    continue;
                }
            }

            // Never pass-1 evict looping-clip frames.  The media playhead
            // marches forward linearly through loop iterations, so in
            // isolation every loop frame looks "behind playhead" the
            // moment we pass the end of the first iteration — but the
            // clip will wrap and need every frame again.  Let pass-2
            // pure-LRU evict them only under genuine memory pressure.
            {
                auto loopIt = m_map.find(key);
                if (loopIt != m_map.end() && loopIt->second.frame &&
                    loopIt->second.frame->isLoopFrame) {
                    continue;
                }
            }

            // Check if this frame is OUTSIDE the protected playback window
            // for its media.  Default extent is 5 behind / 0 ahead (matches
            // pre-Phase-B behavior).  UnifiedCache::setPlayheadWindow can
            // widen the window per media via FrameCache::setPlayheadWindow.
            auto phIt = m_playheads.find(key.mediaId);
            if (phIt != m_playheads.end()) {
                int64_t playhead = phIt->second;
                int behindExt = 5;
                int aheadExt  = 0;
                auto winIt = m_windows.find(key.mediaId);
                if (winIt != m_windows.end()) {
                    behindExt = winIt->second.behind;
                    aheadExt  = winIt->second.ahead;
                }
                const int64_t lo = playhead - behindExt;
                const int64_t hi = playhead + aheadExt;
                const bool outsideWindow = (key.frameNumber < lo) ||
                                           (aheadExt > 0 && key.frameNumber > hi);
                if (outsideWindow) {
                    auto mapIt = m_map.find(key);
                    if (mapIt != m_map.end()) {
                        size_t freed = mapIt->second.sizeBytes;
                        m_used -= freed;
                        pass1Bytes += freed;
                        m_map.erase(mapIt);
                        ++m_evictions;
                        ++pass1Evicted;
                    }
                    it = m_lru.erase(it);
                    continue;
                }
            }
            // Inside protected window or no playhead info — skip in pass 1
        }
    }

    // Pass 2: Regular LRU eviction from the back (fallback).
    // Walk from back to front, evicting non-pinned frames.  Pinned frames
    // (e.g. static images) are skipped and stay in the cache.
    //
    // Two sub-passes:
    //   2a) Evict non-loop, non-pinned frames first.  These are expensive
    //       to decode again but the clip is NOT a short recurring loop,
    //       so re-prefetch on demand is acceptable.
    //   2b) Only if 2a didn't free enough, evict loop frames.  Those are
    //       very expensive to lose — the character clip is short and
    //       guaranteed to be needed again every 1-3 seconds.
    auto evictPass = [&](bool includeLoopFrames) {
        if (m_lru.empty()) return;
        auto it = m_lru.end();
        while (overCapacity() && it != m_lru.begin()) {
            --it;
            auto mapIt = m_map.find(*it);
            // Stale list entry with no map entry — drop the list node.
            if (mapIt == m_map.end()) {
                auto next = m_lru.erase(it);
                it = next;
                continue;
            }
            if (mapIt->second.frame && mapIt->second.frame->pinned) {
                continue;  // skip pinned, keep walking toward front
            }
            if (!includeLoopFrames && mapIt->second.frame &&
                mapIt->second.frame->isLoopFrame) {
                continue;  // protect loop frames in 2a
            }
            size_t freed = mapIt->second.sizeBytes;
            m_map.erase(mapIt);
            m_used -= freed;
            pass2Bytes += freed;
            ++m_evictions;
            ++pass2Evicted;
            auto next = m_lru.erase(it);
            it = next;
        }
    };
    evictPass(/*includeLoopFrames=*/false);
    if (overCapacity())
        evictPass(/*includeLoopFrames=*/true);

    if (pass1Evicted + pass2Evicted > 0) {
        static int s_evictLog = 0;
        if (++s_evictLog % 50 == 0) {
            spdlog::info("[PERF] FrameCache evict: pass1={} ({:.1f}MB) pass2={} ({:.1f}MB) "
                         "playheads={} remaining={} used={:.1f}MB",
                         pass1Evicted, pass1Bytes / 1048576.0,
                         pass2Evicted, pass2Bytes / 1048576.0,
                         m_playheads.size(), m_map.size(), m_used / 1048576.0);
        }
    }
}

} // namespace rt

