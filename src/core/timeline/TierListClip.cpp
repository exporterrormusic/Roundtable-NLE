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

    // Base clip properties
    copy->m_label         = m_label;
    copy->m_color         = m_color;
    copy->m_enabled       = m_enabled;
    copy->m_offline       = m_offline;
    copy->m_renderStatus  = m_renderStatus;
    copy->m_timelineIn    = m_timelineIn;
    copy->m_duration      = m_duration;
    copy->m_sourceIn      = m_sourceIn;
    copy->m_speed         = m_speed;
    copy->m_maintainPitch = m_maintainPitch;
    copy->m_timeInterpolation = m_timeInterpolation;
    copy->m_speedRamp     = m_speedRamp;
    copy->m_blendMode     = m_blendMode;
    copy->m_opacity       = m_opacity;
    copy->m_posX          = m_posX;
    copy->m_posY          = m_posY;
    copy->m_scaleX        = m_scaleX;
    copy->m_scaleY        = m_scaleY;
    copy->m_rotation      = m_rotation;
    copy->m_shutterAngle  = m_shutterAngle;
    copy->m_anchorX       = m_anchorX;
    copy->m_anchorY       = m_anchorY;

    // Shot group / layer metadata
    copy->m_groupId  = m_groupId;
    copy->m_syncLine = m_syncLine;
    copy->m_linkId   = m_linkId;
    copy->m_shotName = m_shotName;
    copy->m_layerId  = m_layerId;

    // Effect stack
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

    // Opacity masks and markers
    copy->m_masks   = m_masks;
    copy->m_markers = m_markers;

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
