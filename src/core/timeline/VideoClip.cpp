/*
 * VideoClip.cpp — Video clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/VideoClip.h"

namespace rt {

VideoClip::VideoClip()
    : Clip(ClipType::Video)
{
    m_label = "Video Clip";
    // Color is left at the Clip default sentinel (0xFF888888) so that
    // TimelineTrackWidget can apply per-type theme tints (blue/orange/purple).
    // Users can still override via the clip color picker.
}

VideoClip::VideoClip(const std::string& mediaPath)
    : VideoClip()
{
    m_mediaPath = mediaPath;
}

std::unique_ptr<Clip> VideoClip::clone() const
{
    auto copy = std::make_unique<VideoClip>();

    // Base clip properties
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_offline    = m_offline;
    copy->m_renderStatus = m_renderStatus;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
    copy->m_maintainPitch = m_maintainPitch;
    copy->m_speedRamp  = m_speedRamp;
    copy->m_blendMode  = m_blendMode;
    copy->m_opacity    = m_opacity;
    copy->m_posX       = m_posX;
    copy->m_posY       = m_posY;
    copy->m_scaleX     = m_scaleX;
    copy->m_scaleY     = m_scaleY;
    copy->m_rotation   = m_rotation;
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

    // Opacity masks and markers (deep copy vectors)
    copy->m_masks    = m_masks;
    copy->m_markers  = m_markers;

    // VideoClip-specific
    copy->m_mediaPath      = m_mediaPath;
    copy->m_mediaId        = m_mediaId;
    copy->m_sourceWidth    = m_sourceWidth;
    copy->m_sourceHeight   = m_sourceHeight;
    copy->m_sourceFps      = m_sourceFps;
    copy->m_sourceDuration = m_sourceDuration;
    copy->m_hasAudio       = m_hasAudio;
    copy->m_volume         = m_volume;
    copy->m_cropL          = m_cropL;
    copy->m_cropR          = m_cropR;
    copy->m_cropT          = m_cropT;
    copy->m_cropB          = m_cropB;
    copy->m_characterName  = m_characterName;
    copy->m_isTalking      = m_isTalking;
    copy->m_videoMutePath  = m_videoMutePath;
    copy->m_videoTalkPath  = m_videoTalkPath;
    copy->m_outfit         = m_outfit;
    copy->m_animationName  = m_animationName;

    return copy;
}

} // namespace rt
