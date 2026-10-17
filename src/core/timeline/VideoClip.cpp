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

std::unique_ptr<Clip> VideoClip::createEmpty() const
{
    return std::make_unique<VideoClip>();
}

void VideoClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const VideoClip&>(src);
    // VideoClip-specific
    m_mediaPath      = o.m_mediaPath;
    m_mediaId        = o.m_mediaId;
    m_sourceWidth    = o.m_sourceWidth;
    m_sourceHeight   = o.m_sourceHeight;
    m_sourceRotation = o.m_sourceRotation;
    m_sourceMetadataAuthoritative = o.m_sourceMetadataAuthoritative;
    m_sourceFps      = o.m_sourceFps;
    m_sourceDuration = o.m_sourceDuration;
    m_hasAudio       = o.m_hasAudio;
    m_volume         = o.m_volume;
    m_cropL          = o.m_cropL;
    m_cropR          = o.m_cropR;
    m_cropT          = o.m_cropT;
    m_cropB          = o.m_cropB;
    m_characterName  = o.m_characterName;
    m_isTalking      = o.m_isTalking;
    m_videoMutePath  = o.m_videoMutePath;
    m_videoTalkPath  = o.m_videoTalkPath;
    m_outfit         = o.m_outfit;
    m_animationName  = o.m_animationName;
}

} // namespace rt
