/*
 * AudioClip.cpp — Audio clip implementation.
 * Step 3: Core Data Model
 */

#include "timeline/AudioClip.h"

namespace rt {

AudioClip::AudioClip()
    : Clip(ClipType::Audio)
{
    m_label = "Audio Clip";
    // Color is left at the Clip default sentinel (0xFF888888) so that
    // TimelineTrackWidget applies the audio theme tint.
}

AudioClip::AudioClip(const std::string& mediaPath)
    : AudioClip()
{
    m_mediaPath = mediaPath;
}

std::unique_ptr<Clip> AudioClip::createEmpty() const
{
    return std::make_unique<AudioClip>();
}

void AudioClip::copyOwnStateFrom(const Clip& src, CloneMode /*mode*/)
{
    const auto& o = static_cast<const AudioClip&>(src);
    // AudioClip-specific
    m_mediaPath        = o.m_mediaPath;
    m_mediaId          = o.m_mediaId;
    m_sampleRate       = o.m_sampleRate;
    m_channels         = o.m_channels;
    m_sourceDuration   = o.m_sourceDuration;
    m_audioStreamIndex = o.m_audioStreamIndex;
    m_volume         = o.m_volume;
    m_pan            = o.m_pan;
    m_fadeIn         = o.m_fadeIn;
    m_fadeOut        = o.m_fadeOut;
    m_audioFx        = o.m_audioFx.clone();
}

} // namespace rt
