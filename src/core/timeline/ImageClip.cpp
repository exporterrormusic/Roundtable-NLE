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
    copyBaseStateTo(*copy);

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
