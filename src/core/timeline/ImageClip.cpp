/*
 * ImageClip.cpp — Still image clip implementation.
 */

#include "timeline/ImageClip.h"

namespace rt {

ImageClip::ImageClip()
    : Clip(ClipType::Image)
{
    m_label = "Image Clip";
    // Color left at sentinel — TimelineTrackWidget applies the image theme tint.
}

ImageClip::ImageClip(const std::string& mediaPath)
    : ImageClip()
{
    m_mediaPath = mediaPath;
}

std::unique_ptr<Clip> ImageClip::clone() const
{
    auto copy = std::make_unique<ImageClip>();

    // Base clip properties
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
    copy->m_timeInterpolation = m_timeInterpolation;
    copy->m_speedRamp  = m_speedRamp;
    copy->m_blendMode  = m_blendMode;
    copy->m_opacity    = m_opacity;
    copy->m_posX       = m_posX;
    copy->m_posY       = m_posY;
    copy->m_scaleX     = m_scaleX;
    copy->m_scaleY     = m_scaleY;
    copy->m_rotation   = m_rotation;
    copy->m_shutterAngle = m_shutterAngle;
    copy->m_anchorX    = m_anchorX;
    copy->m_anchorY    = m_anchorY;

    // Shot group / layer metadata
    copy->m_groupId    = m_groupId;
    copy->m_syncLine   = m_syncLine;
    copy->m_linkId     = m_linkId;
    copy->m_shotName   = m_shotName;
    copy->m_layerId    = m_layerId;

    // Effect stack
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

    // ImageClip-specific
    copy->m_mediaPath    = m_mediaPath;
    copy->m_mediaId      = m_mediaId;
    copy->m_sourceWidth  = m_sourceWidth;
    copy->m_sourceHeight = m_sourceHeight;
    copy->m_cropL        = m_cropL;
    copy->m_cropR        = m_cropR;
    copy->m_cropT        = m_cropT;
    copy->m_cropB        = m_cropB;

    return copy;
}

} // namespace rt
