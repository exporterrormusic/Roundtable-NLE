#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rt {

struct GpuGenerationCacheStats
{
    size_t residentEntries{0};
    size_t estimatedBytes{0};
};

struct GpuGenerationWorkingSetStats
{
    GpuGenerationCacheStats compositors;
    GpuGenerationCacheStats effectProcessors;
    GpuGenerationCacheStats spineRenderers;
    GpuGenerationCacheStats transitionRenderers;
    GpuGenerationCacheStats nv12Converters;

    [[nodiscard]] size_t totalResidentEntries() const noexcept
    {
        return compositors.residentEntries
             + effectProcessors.residentEntries
             + spineRenderers.residentEntries
             + transitionRenderers.residentEntries
             + nv12Converters.residentEntries;
    }

    [[nodiscard]] size_t totalEstimatedBytes() const noexcept
    {
        return compositors.estimatedBytes
             + effectProcessors.estimatedBytes
             + spineRenderers.estimatedBytes
             + transitionRenderers.estimatedBytes
             + nv12Converters.estimatedBytes;
    }
};

/// Resolution-keyed LRU for mutable GPU helper generations.
///
/// The cache owns one shared lease per resident generation. Eviction only
/// drops that lease: command submissions and delayed readbacks may retain
/// their own leases until the GPU has finished with the generation.
template <typename T, typename Key = uint64_t,
          typename Hash = std::hash<Key>>
class GpuGenerationCache
{
public:
    using Ptr = std::shared_ptr<T>;

    struct Limits {
        size_t maxEntries{4};
        size_t maxEstimatedBytes{256ull * 1024ull * 1024ull};
    };

    explicit GpuGenerationCache(Limits limits = {}) : m_limits(limits) {}

    [[nodiscard]] Ptr find(const Key& key)
    {
        const auto it = m_entries.find(key);
        if (it == m_entries.end())
            return {};
        it->second.lastUse = ++m_clock;
        return it->second.resource;
    }

    /// Insert/replace one generation and return leases retired by the LRU.
    /// Callers should let the returned vector destruct outside their mutex.
    [[nodiscard]] std::vector<Ptr> insert(const Key& key, Ptr resource,
                                          size_t estimatedBytes)
    {
        std::vector<Ptr> retired;
        if (!resource)
            return retired;

        if (const auto old = m_entries.find(key); old != m_entries.end()) {
            m_estimatedBytes -= old->second.estimatedBytes;
            retired.push_back(std::move(old->second.resource));
            m_entries.erase(old);
        }

        m_estimatedBytes += estimatedBytes;
        m_entries.emplace(key, Entry{std::move(resource), ++m_clock,
                                     estimatedBytes});

        while (overLimit()) {
            auto victim = m_entries.end();
            uint64_t oldest = std::numeric_limits<uint64_t>::max();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->first == key)
                    continue; // Never evict the generation just requested.
                if (it->second.lastUse < oldest) {
                    oldest = it->second.lastUse;
                    victim = it;
                }
            }
            if (victim == m_entries.end())
                break; // One oversized active generation is permitted.

            m_estimatedBytes -= victim->second.estimatedBytes;
            retired.push_back(std::move(victim->second.resource));
            m_entries.erase(victim);
        }
        return retired;
    }

    [[nodiscard]] std::vector<Ptr> clear()
    {
        std::vector<Ptr> retired;
        retired.reserve(m_entries.size());
        for (auto& [key, entry] : m_entries) {
            (void)key;
            retired.push_back(std::move(entry.resource));
        }
        m_entries.clear();
        m_estimatedBytes = 0;
        return retired;
    }

    [[nodiscard]] size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] size_t estimatedBytes() const noexcept
    {
        return m_estimatedBytes;
    }

    [[nodiscard]] GpuGenerationCacheStats stats() const noexcept
    {
        return {m_entries.size(), m_estimatedBytes};
    }

    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (const auto& [key, entry] : m_entries)
            fn(key, entry.resource);
    }

private:
    struct Entry {
        Ptr resource;
        uint64_t lastUse{0};
        size_t estimatedBytes{0};
    };

    [[nodiscard]] bool overLimit() const noexcept
    {
        return m_entries.size() > m_limits.maxEntries ||
               m_estimatedBytes > m_limits.maxEstimatedBytes;
    }

    Limits m_limits;
    std::unordered_map<Key, Entry, Hash> m_entries;
    uint64_t m_clock{0};
    size_t m_estimatedBytes{0};
};

[[nodiscard]] inline size_t estimatedGpuImageBytes(
    uint32_t width, uint32_t height, size_t bytesPerPixel,
    size_t imageCount = 1) noexcept
{
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    if (width == 0 || height == 0 || bytesPerPixel == 0 || imageCount == 0)
        return 0;
    const size_t pixels = static_cast<size_t>(width) * height;
    if (pixels > kMax / bytesPerPixel)
        return kMax;
    const size_t oneImage = pixels * bytesPerPixel;
    return oneImage > kMax / imageCount ? kMax : oneImage * imageCount;
}

} // namespace rt
