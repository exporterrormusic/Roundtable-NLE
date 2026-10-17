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

std::unique_ptr<Clip> AudioClip::clone() const
{
    auto copy = std::make_unique<AudioClip>();
    copyBaseStateTo(*copy);

    // AudioClip-specific
    copy->m_mediaPath        = m_mediaPath;
    copy->m_mediaId          = m_mediaId;
    copy->m_sampleRate       = m_sampleRate;
    copy->m_channels         = m_channels;
    copy->m_sourceDuration   = m_sourceDuration;
    copy->m_audioStreamIndex = m_audioStreamIndex;
    copy->m_volume         = m_volume;
    copy->m_pan            = m_pan;
    copy->m_fadeIn         = m_fadeIn;
    copy->m_fadeOut        = m_fadeOut;
    copy->m_audioFx        = m_audioFx.clone();

    return copy;
}

} // namespace rt
