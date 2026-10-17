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

std::unique_ptr<Clip> ImageClip::createEmpty() const
{
    return std::make_unique<ImageClip>();
}

void ImageClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const ImageClip&>(src);
    // ImageClip-specific
    m_mediaPath    = o.m_mediaPath;
    m_mediaId      = o.m_mediaId;
    m_sourceWidth  = o.m_sourceWidth;
    m_sourceHeight = o.m_sourceHeight;
    m_cropL        = o.m_cropL;
    m_cropR        = o.m_cropR;
    m_cropT        = o.m_cropT;
    m_cropB        = o.m_cropB;
}

} // namespace rt
