/*
 * MediaPoolQueries.cpp — Queries and internal helpers for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"

namespace rt {

void MediaPool::setProjectFps(double fps)
{
    m_projectFps.store(fps > 0.0 ? fps : 30.0, std::memory_order_relaxed);
}

void MediaPool::setPlaybackSpeed(double speed)
{
    m_playbackSpeed.store(speed, std::memory_order_relaxed);
}

void MediaPool::setDiskCache(std::shared_ptr<DiskFrameCache> cache)
{
    m_diskCache = std::move(cache);
}

DiskFrameCache* MediaPool::diskCache() const noexcept
{
    return m_diskCache.get();
}

FrameScheduler& MediaPool::scheduler() noexcept
{
    return m_scheduler;
}

const FrameScheduler& MediaPool::scheduler() const noexcept
{
    return m_scheduler;
}

// ─── Queries ─────────────────────────────────────────────────────────────────

const VideoStreamInfo* MediaPool::getInfo(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? &entry->info : nullptr;
}

std::filesystem::path MediaPool::getPath(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    auto* entry = findEntry(handle);
    return entry ? entry->path : std::filesystem::path{};
}

bool MediaPool::isValid(MediaHandle handle) const
{
    std::lock_guard lock(m_mutex);
    return m_entries.find(handle) != m_entries.end();
}

size_t MediaPool::openCount() const
{
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

// ─── Internal ────────────────────────────────────────────────────────────────

MediaEntry* MediaPool::findEntry(MediaHandle handle)
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

const MediaEntry* MediaPool::findEntry(MediaHandle handle) const
{
    auto it = m_entries.find(handle);
    return it != m_entries.end() ? &it->second : nullptr;
}

} // namespace rt
