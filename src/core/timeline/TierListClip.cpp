/*
 * TierListClip.cpp — tier-list ranking board clip implementation.
 */

#include "timeline/TierListClip.h"

#include <algorithm>

namespace rt {

TierListClip::TierListClip()
    : Clip(ClipType::TierList)
{
    m_label = "Tier List";

    // Default NIKKE-style S–F tiers (placeholder colours until measured from a
    // real sample frame).  0xAARRGGBB.
    m_tiers = {
        { "S", 0xFFFF7F7Fu },  // measured from the sample (pastel S–F palette)
        { "A", 0xFFFFBF7Fu },
        { "B", 0xFFFFDF80u },
        { "C", 0xFFFFFF7Fu },
        { "D", 0xFFBFFF7Fu },
        { "F", 0xFF7FFF7Fu },
    };
}

float TierListClip::entryAspectRatio() const noexcept
{
    switch (m_entryAspect)
    {
        case TierEntryAspect::Banner:   return 16.0f / 9.0f;
        case TierEntryAspect::Square:   return 1.0f;
        case TierEntryAspect::Vertical: return 9.0f / 16.0f;
        case TierEntryAspect::Custom:
            if (m_customAspectH > 0.0f && m_customAspectW > 0.0f)
                return m_customAspectW / m_customAspectH;
            return 16.0f / 9.0f;
    }
    return 16.0f / 9.0f;
}

const TierEntry* TierListClip::entryById(uint64_t id) const noexcept
{
    for (const auto& e : m_entries)
        if (e.id == id)
            return &e;
    return nullptr;
}

int64_t TierListClip::eventTickAt(int64_t timelineTick) const noexcept
{
    const int64_t mapped = sourceIn()
        + static_cast<int64_t>(static_cast<double>(timelineTick - timelineIn())
                               * speed());
    return std::max<int64_t>(0, mapped);
}

std::unique_ptr<Clip> TierListClip::clone() const
{
    auto copy = std::make_unique<TierListClip>();
    copyBaseStateTo(*copy);

    // TierListClip-specific
    copy->m_title          = m_title;
    copy->m_tiers          = m_tiers;
    copy->m_entryAspect    = m_entryAspect;
    copy->m_customAspectW  = m_customAspectW;
    copy->m_customAspectH  = m_customAspectH;
    copy->m_topSafeMargin  = m_topSafeMargin;
    copy->m_rightSafeMargin= m_rightSafeMargin;
    copy->m_bgColor        = m_bgColor;
    copy->m_entries        = m_entries;
    copy->m_subbins        = m_subbins;
    copy->m_events         = m_events;
    copy->m_renderRevision.store(renderRevision(), std::memory_order_relaxed);

    return copy;
}

} // namespace rt
