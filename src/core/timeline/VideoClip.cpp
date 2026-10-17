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
    copyBaseStateTo(*copy);

    // VideoClip-specific
    copy->m_mediaPath      = m_mediaPath;
    copy->m_mediaId        = m_mediaId;
    copy->m_sourceWidth    = m_sourceWidth;
    copy->m_sourceHeight   = m_sourceHeight;
    copy->m_sourceRotation = m_sourceRotation;
    copy->m_sourceMetadataAuthoritative = m_sourceMetadataAuthoritative;
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
