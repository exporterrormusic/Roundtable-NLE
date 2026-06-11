/*
 * MediaPoolQueries.cpp — Queries and internal helpers for MediaPool.
 * Extracted from MediaPool.cpp for maintainability.
 */

#include "MediaPool.h"

namespace rt {

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
