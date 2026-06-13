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

    // Base clip properties
    copy->m_label      = m_label;
    copy->m_color      = m_color;
    copy->m_enabled    = m_enabled;
    copy->m_timelineIn = m_timelineIn;
    copy->m_duration   = m_duration;
    copy->m_sourceIn   = m_sourceIn;
    copy->m_speed      = m_speed;
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
    copy->m_linkId     = m_linkId;
    copy->m_shotName   = m_shotName;
    copy->m_layerId    = m_layerId;

    // Effect stack
    if (!m_effects.isEmpty()) {
        auto clonedEffects = m_effects.clone();
        if (clonedEffects)
            copy->m_effects = std::move(*clonedEffects);
    }

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
