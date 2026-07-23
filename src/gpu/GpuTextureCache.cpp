/*
 * GpuTextureCache.cpp — LRU cache of Vulkan textures in VRAM.
 */

#include "GpuTextureCache.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace rt {

GpuTextureCache::GpuTextureCache(size_t vramBudgetBytes)
    : m_budget(vramBudgetBytes)
{
}

GpuTextureCache::~GpuTextureCache()
{
    // clear() acquires m_mtx — safe because the destructor implies no
    // other thread holds a reference to this object any more.
    clear();
}

// ── Lookup ──────────────────────────────────────────────────────────────────

GpuTextureCache::LookupResult GpuTextureCache::get(
    uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    std::lock_guard lk(m_mtx);
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it == m_map.end()) {
        ++m_misses;
        return {};
    }

    // For shared-ownership entries, verify the texture is still alive.
    // The CachedFrame (FrameCache) may have been evicted, destroying the
    // GPU texture.  In that case, remove the stale entry.
    if (!it->second.texture && it->second.sharedOwner) {
        // sharedOwner is alive — texture is valid
    } else if (!it->second.texture && !it->second.sharedOwner) {
        // Stale entry — remove it
        m_used -= it->second.bytes;
        m_lru.erase(it->second.lruIt);
        m_map.erase(it);
        ++m_misses;
        return {};
    }

    // Promote to front of LRU
    m_lru.splice(m_lru.begin(), m_lru, it->second.lruIt);

    ++m_hits;
    LookupResult r;
    r.found      = true;
    // Use appropriate descriptor based on ownership type
    if (it->second.texture) {
        r.descriptor = it->second.texture->descriptorInfo();
    } else {
        r.descriptor = it->second.sharedDescriptor;
    }
    r.width      = it->second.width;
    r.height     = it->second.height;
    r.isPacked   = it->second.isPacked;
    r.isPMA      = it->second.isPMA;
    r.contentBoundsValid = it->second.contentBoundsValid;
    r.contentLeft = it->second.contentLeft;
    r.contentTop = it->second.contentTop;
    r.contentRight = it->second.contentRight;
    r.contentBottom = it->second.contentBottom;
    return r;
}

// ── Insert ──────────────────────────────────────────────────────────────────

void GpuTextureCache::put(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
                          std::unique_ptr<Texture> tex, size_t textureBytes,
                          bool isPacked, bool isPMA, bool isLoopFrame,
                          bool contentBoundsValid, float contentLeft,
                          float contentTop, float contentRight, float contentBottom)
{
    if (!tex) return;

    std::lock_guard lk(m_mtx);
    CacheKey key{mediaId, frameNumber, tier};

    // If already cached, keep the existing entry (first decode wins).
    // Replacing here causes visible flicker when NVDEC and the SW decoder
    // produce subtly different RGB for the same H.264 frame (BT.709 limited
    // vs sws_scale's default colorspace), because consecutive composites
    // would alternate between the two outputs.  The source pixels don't
    // change, so the first cached version is the source of truth.
    auto existing = m_map.find(key);
    if (existing != m_map.end()) {
        // Promote to front of LRU so we don't evict it just because
        // a re-upload was attempted.
        m_lru.splice(m_lru.begin(), m_lru, existing->second.lruIt);
        return;
    }

    // Evict to make room
    evictUntilFits(textureBytes);

    // Insert
    m_lru.push_front(key);
    Entry entry;
    entry.width       = tex->width();
    entry.height      = tex->height();
    entry.bytes       = textureBytes;
    entry.isPacked    = isPacked;
    entry.isPMA       = isPMA;
    entry.isLoopFrame = isLoopFrame;
    entry.contentBoundsValid = contentBoundsValid;
    entry.contentLeft = contentLeft;
    entry.contentTop = contentTop;
    entry.contentRight = contentRight;
    entry.contentBottom = contentBottom;
    entry.texture     = std::move(tex);
    entry.lruIt       = m_lru.begin();

    m_used += textureBytes;
    m_map.emplace(key, std::move(entry));
}

void GpuTextureCache::putShared(uint64_t mediaId, int64_t frameNumber, uint8_t tier,
                                std::shared_ptr<void> sharedOwner,
                                VkDescriptorImageInfo descriptor,
                                uint32_t width, uint32_t height,
                                size_t textureBytes,
                                bool isPacked, bool isPMA, bool isLoopFrame,
                                bool contentBoundsValid, float contentLeft,
                                float contentTop, float contentRight, float contentBottom)
{
    if (!sharedOwner) return;

    std::lock_guard lk(m_mtx);
    CacheKey key{mediaId, frameNumber, tier};

    // If already cached, skip (don't replace existing entries)
    auto existing = m_map.find(key);
    if (existing != m_map.end()) return;

    // UPGRADE_PLAN 2026-05-22: putShared MUST evict, just like put().
    //
    // The previous comment claimed "Don't evict for shared entries —
    // they don't own VRAM exclusively. The VRAM is owned by the
    // FrameCache's CachedFrame."  That stopped being true after the
    // CompositeServiceLayerBuild "transfer of ownership" change:
    // FrameCache resets gpuTextureOwner immediately after putShared,
    // leaving GpuTextureCache as the SOLE owner of the VkImage.
    // Without eviction here the cache grew unbounded (gpuTexN climbed
    // 2→1469 in 52 s of playback) until VMA-tracked VRAM crossed the
    // OS budget and the driver started paging textures to system RAM.
    evictUntilFits(textureBytes);

    m_lru.push_front(key);
    Entry entry;
    entry.width            = width;
    entry.height           = height;
    entry.bytes            = textureBytes;
    entry.isPacked         = isPacked;
    entry.isPMA            = isPMA;
    entry.isLoopFrame      = isLoopFrame;
    entry.contentBoundsValid = contentBoundsValid;
    entry.contentLeft = contentLeft;
    entry.contentTop = contentTop;
    entry.contentRight = contentRight;
    entry.contentBottom = contentBottom;
    entry.sharedOwner      = std::move(sharedOwner);
    entry.sharedDescriptor = descriptor;
    entry.lruIt            = m_lru.begin();

    m_used += textureBytes;
    m_map.emplace(key, std::move(entry));
}

// ── Maintenance ─────────────────────────────────────────────────────────────

void GpuTextureCache::clear()
{
    std::lock_guard lk(m_mtx);
    m_map.clear();
    m_lru.clear();
    m_used = 0;
}

void GpuTextureCache::evictMedia(uint64_t mediaId)
{
    std::lock_guard lk(m_mtx);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (it->first.mediaId == mediaId) {
            m_used -= it->second.bytes;
            m_lru.erase(it->second.lruIt);
            it = m_map.erase(it);
        } else {
            ++it;
        }
    }
}

void GpuTextureCache::setBudget(size_t bytes)
{
    std::lock_guard lk(m_mtx);
    m_budget = bytes;
    evictUntilFits(0);
}

void GpuTextureCache::setMaxEntries(size_t maxEntries)
{
    std::lock_guard lk(m_mtx);
    m_maxEntries = maxEntries;
    evictUntilFits(0);
}

size_t GpuTextureCache::maxEntries() const
{
    std::lock_guard lk(m_mtx);
    return m_maxEntries;
}

// ── Pinning ─────────────────────────────────────────────────────────────────

void GpuTextureCache::pin(uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    std::lock_guard lk(m_mtx);
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it != m_map.end()) {
        ++it->second.pinCount;
    }
}

void GpuTextureCache::unpin(uint64_t mediaId, int64_t frameNumber, uint8_t tier)
{
    std::lock_guard lk(m_mtx);
    CacheKey key{mediaId, frameNumber, tier};
    auto it = m_map.find(key);
    if (it != m_map.end() && it->second.pinCount > 0) {
        --it->second.pinCount;
    }
}

void GpuTextureCache::unpinAll()
{
    std::lock_guard lk(m_mtx);
    for (auto& [key, entry] : m_map)
        entry.pinCount = 0;
}

// ── Statistics ──────────────────────────────────────────────────────────────
//
// These lock so that callers see consistent values during concurrent put /
// evict.  Contention is negligible (O(1) reads), but the lock matters for
// memoryUsed() / isUnderPressure() during eviction storms — without it the
// caller could see a transient over-budget value.

size_t GpuTextureCache::entryCount() const
{
    std::lock_guard lk(m_mtx);
    return m_map.size();
}

size_t GpuTextureCache::memoryUsed() const
{
    std::lock_guard lk(m_mtx);
    return m_used;
}

size_t GpuTextureCache::budget() const
{
    std::lock_guard lk(m_mtx);
    return m_budget;
}

size_t GpuTextureCache::hits() const
{
    std::lock_guard lk(m_mtx);
    return m_hits;
}

size_t GpuTextureCache::misses() const
{
    std::lock_guard lk(m_mtx);
    return m_misses;
}

bool GpuTextureCache::isUnderPressure() const
{
    std::lock_guard lk(m_mtx);
    return m_used > (m_budget * 9 / 10);
}

int GpuTextureCache::usagePercent() const
{
    std::lock_guard lk(m_mtx);
    return m_budget > 0 ? static_cast<int>(m_used * 100 / m_budget) : 0;
}

void GpuTextureCache::setRecycleFn(RecycleFn fn)
{
    std::lock_guard lk(m_mtx);
    m_recycleFn = std::move(fn);
}

// ── Eviction ────────────────────────────────────────────────────────────────

// Requires m_mtx held — called from put() and setBudget() which take the
// lock at entry.  Do NOT take m_mtx here; doing so would deadlock on a
// non-recursive mutex.
void GpuTextureCache::evictUntilFits(size_t needed)
{
    // Eviction trigger combines two signals (UPGRADE_PLAN 2026-05-22):
    //   - byte pressure:  m_used + needed > m_budget
    //   - entry pressure: m_map.size() >= m_maxEntries (when set)
    //
    // The entry-count signal is the Premiere-style bounded working
    // set fix.  Before it, gpuOwn climbed from 2 to 1469 entries over
    // 52 s of single-clip playback (>11 GB of GPU textures), because
    // the byte budget was so generous (40-60% of VRAM, multi-GB) that
    // the cache happily hoarded every frame the compositor had ever
    // shown.  With the entry cap, the cache is treated as a small
    // recent-frames pool — anything older lives in DiskFrameCache and
    // re-uploads on demand if scrubbed back.
    //
    // ">=" (not ">") on the entry-count check because we want eviction
    // to leave room for the incoming insertion: if cap is N and we
    // already hold N, we need to evict at least one before the put
    // below pushes us to N+1.

    auto overCapacity = [&]() {
        if (m_used + needed > m_budget) return true;
        if (m_maxEntries != 0 && m_map.size() >= m_maxEntries) return true;
        return false;
    };

    auto walkAndEvict = [&](bool includeLoopFrames) {
        if (m_lru.empty()) return;
        auto it = m_lru.end();
        while (overCapacity() && it != m_lru.begin()) {
            --it;
            auto mapIt = m_map.find(*it);
            if (mapIt == m_map.end()) {
                it = m_lru.erase(it);
                continue;
            }
            // Skip pinned entries — still referenced by in-flight GPU work
            if (mapIt->second.pinCount > 0)
                continue;
            if (!includeLoopFrames && mapIt->second.isLoopFrame) {
                continue;
            }
            m_used -= mapIt->second.bytes;
            // A4: recycle the Texture instead of destroying it, if a
            // recycle callback is installed (typically by GpuUploadManager).
            if (m_recycleFn && mapIt->second.texture) {
                m_recycleFn(mapIt->second.texture);
            }
            m_map.erase(mapIt);
            it = m_lru.erase(it);
        }
    };
    walkAndEvict(/*includeLoopFrames=*/false);
    if (overCapacity())
        walkAndEvict(/*includeLoopFrames=*/true);
}

} // namespace rt
