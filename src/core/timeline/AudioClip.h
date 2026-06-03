/*
 * AudioClip — a timeline clip referencing an audio media file.
 *
 * Contains the asset path, audio properties (channels, sample rate),
 * volume/pan keyframes, and fade in/out durations.
 */

#pragma once

#include "timeline/Clip.h"
#include "timeline/KeyframeTrack.h"
#include "audiofx/FxChain.h"
#include <string>

namespace rt {

class AudioClip : public Clip
{
public:
    AudioClip();
    /// Convenience constructor: sets media path.
    explicit AudioClip(const std::string& mediaPath);
    ~AudioClip() override = default;

    // ── Media reference ─────────────────────────────────────────────────
    [[nodiscard]] const std::string& mediaPath() const noexcept { return m_mediaPath; }
    void setMediaPath(const std::string& path) { m_mediaPath = path; }

    [[nodiscard]] uint64_t mediaId() const noexcept { return m_mediaId; }
    void setMediaId(uint64_t id) noexcept { m_mediaId = id; }

    // ── Audio properties ────────────────────────────────────────────────
    [[nodiscard]] uint32_t sampleRate() const noexcept { return m_sampleRate; }
    void setSampleRate(uint32_t sr) noexcept { m_sampleRate = sr; }

    [[nodiscard]] uint16_t channels() const noexcept { return m_channels; }
    void setChannels(uint16_t ch) noexcept { m_channels = ch; }

    [[nodiscard]] int64_t sourceDuration() const noexcept { return m_sourceDuration; }
    void setSourceDuration(int64_t d) noexcept { m_sourceDuration = d; }

    // ── Keyframeable audio properties ───────────────────────────────────
    KeyframeTrack<float>& volume() noexcept { return m_volume; }
    KeyframeTrack<float>& pan()    noexcept { return m_pan; }

    const KeyframeTrack<float>& volume() const noexcept { return m_volume; }
    const KeyframeTrack<float>& pan()    const noexcept { return m_pan; }

    // ── Audio effect chain (EQ, dynamics) ───────────────────────────────
    /// Per-clip DSP chain applied to this clip's audio (see core/audiofx).
    /// Distinct from the GPU EffectStack (effects()): this is sample-domain
    /// audio processing, not a video effect.
    audiofx::FxChain&       audioFx()       noexcept { return m_audioFx; }
    const audiofx::FxChain& audioFx() const noexcept { return m_audioFx; }

    // ── Fades ───────────────────────────────────────────────────────────
    [[nodiscard]] int64_t fadeInDuration()  const noexcept { return m_fadeIn; }
    [[nodiscard]] int64_t fadeOutDuration() const noexcept { return m_fadeOut; }
    void setFadeInDuration(int64_t d)  noexcept { m_fadeIn = d; }
    void setFadeOutDuration(int64_t d) noexcept { m_fadeOut = d; }

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
    std::string m_mediaPath;
    uint64_t    m_mediaId{0};
    uint32_t    m_sampleRate{48000};
    uint16_t    m_channels{2};
    int64_t     m_sourceDuration{0};

    KeyframeTrack<float> m_volume{1.0f};   // 0.0 = silence, 1.0 = unity
    KeyframeTrack<float> m_pan{0.0f};      // -1.0 = left, 0.0 = center, 1.0 = right

    int64_t m_fadeIn{0};   // In ticks
    int64_t m_fadeOut{0};

    audiofx::FxChain m_audioFx;
};

} // namespace rt
