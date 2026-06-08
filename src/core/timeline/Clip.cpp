/*
 * Clip.cpp — Base clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/Clip.h"

#include <atomic>

namespace rt {

// Thread-safe unique ID generation
static std::atomic<uint64_t> s_idCounter{1};

Clip::Clip(ClipType type)
    : m_type(type)
    , m_id(s_idCounter.fetch_add(1, std::memory_order_relaxed))
{
}

Clip::~Clip() = default;

void Clip::reserveId(uint64_t usedId) noexcept
{
    // Bump the shared counter so the next fetch_add yields > usedId.
    uint64_t cur = s_idCounter.load(std::memory_order_relaxed);
    while (cur <= usedId &&
           !s_idCounter.compare_exchange_weak(cur, usedId + 1,
                                              std::memory_order_relaxed)) {
        // retry with refreshed `cur`
    }
}

} // namespace rt

