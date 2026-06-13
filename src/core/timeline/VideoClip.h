/*
 * VideoClip — a timeline clip referencing a video media file.
 *
 * Contains the asset path, codec info, and video-specific properties.
 * Actual decode is handled by VideoDecoder (core/media/).
 */

#pragma once

#include "timeline/Clip.h"
#include <string>

namespace rt {

class VideoClip : public Clip
{
public:
    VideoClip();
    /// Convenience constructor: sets media path.
    explicit VideoClip(const std::string& mediaPath);
    ~VideoClip() override = default;

    // ── Media reference ─────────────────────────────────────────────────
    [[nodiscard]] const std::string& mediaPath() const noexcept { return m_mediaPath; }
    void setMediaPath(const std::string& path) { m_mediaPath = path; }

    /// Asset database ID for the source media
    [[nodiscard]] uint64_t mediaId() const noexcept { return m_mediaId; }
    void setMediaId(uint64_t id) noexcept { m_mediaId = id; }

    // ── Video properties ────────────────────────────────────────────────
    [[nodiscard]] uint32_t sourceWidth()  const noexcept { return m_sourceWidth; }
    [[nodiscard]] uint32_t sourceHeight() const noexcept { return m_sourceHeight; }
    void setSourceResolution(uint32_t w, uint32_t h) noexcept { m_sourceWidth = w; m_sourceHeight = h; }

    [[nodiscard]] double sourceFps() const noexcept { return m_sourceFps; }
    void setSourceFps(double fps) noexcept { m_sourceFps = fps; }

    /// Source codec name (e.g. "h264", "hevc", "av1") for smart render matching.
    [[nodiscard]] const std::string& sourceCodecName() const noexcept { return m_sourceCodecName; }
    void setSourceCodecName(const std::string& name) { m_sourceCodecName = name; }

    /// Total source duration in ticks (before speed adjustment)
    [[nodiscard]] int64_t sourceDuration() const noexcept { return m_sourceDuration; }
    void setSourceDuration(int64_t d) noexcept { m_sourceDuration = d; }

    // ── Audio from video ────────────────────────────────────────────────
    [[nodiscard]] bool hasAudio() const noexcept { return m_hasAudio; }
    void setHasAudio(bool v) noexcept { m_hasAudio = v; }

    [[nodiscard]] float volume() const noexcept { return m_volume; }
    void setVolume(float v) noexcept { m_volume = v; }

    // ── Crop (percentage 0–100) ─────────────────────────────────────────
    void setCrop(float l, float r, float t, float b) {
        m_cropL = l; m_cropR = r; m_cropT = t; m_cropB = b;
    }
    [[nodiscard]] float cropLeft()   const noexcept { return m_cropL; }
    [[nodiscard]] float cropRight()  const noexcept { return m_cropR; }
    [[nodiscard]] float cropTop()    const noexcept { return m_cropT; }
    [[nodiscard]] float cropBottom() const noexcept { return m_cropB; }

    // ── Character properties (video characters from ShotComposer) ───────
    [[nodiscard]] const std::string& characterName() const noexcept { return m_characterName; }
    void setCharacterName(const std::string& name) { m_characterName = name; }

    [[nodiscard]] bool isTalking() const noexcept { return m_isTalking; }
    /// Toggle talking state and switch mediaPath to the appropriate video.
    void setTalking(bool v) {
        m_isTalking = v;
        if (!m_videoMutePath.empty() && !m_videoTalkPath.empty())
            m_mediaPath = v ? m_videoTalkPath : m_videoMutePath;
    }

    [[nodiscard]] const std::string& videoMutePath() const noexcept { return m_videoMutePath; }
    void setVideoMutePath(const std::string& p) { m_videoMutePath = p; }

    [[nodiscard]] const std::string& videoTalkPath() const noexcept { return m_videoTalkPath; }
    void setVideoTalkPath(const std::string& p) { m_videoTalkPath = p; }

    [[nodiscard]] const std::string& outfit() const noexcept { return m_outfit; }
    void setOutfit(const std::string& o) { m_outfit = o; }

    [[nodiscard]] const std::string& animationName() const noexcept { return m_animationName; }
    void setAnimationName(const std::string& n) { m_animationName = n; }

    /// True when this VideoClip represents a character from ShotComposer.
    [[nodiscard]] bool isVideoCharacter() const noexcept { return !m_characterName.empty(); }

    /// Video characters get the character capability (animation/costume
    /// controls) on top of the base Spine/PngPuppet set.
    [[nodiscard]] bool isCharacter() const noexcept override {
        return Clip::isCharacter() || isVideoCharacter();
    }

    // ── Clone ───────────────────────────────────────────────────────────
    [[nodiscard]] std::unique_ptr<Clip> clone() const override;

private:
    std::string m_mediaPath;
    uint64_t    m_mediaId{0};
    uint32_t    m_sourceWidth{0};
    uint32_t    m_sourceHeight{0};
    double      m_sourceFps{0.0};
    std::string m_sourceCodecName;
    int64_t     m_sourceDuration{0};
    bool        m_hasAudio{false};
    float       m_volume{1.0f};
    float       m_cropL{0.0f};
    float       m_cropR{0.0f};
    float       m_cropT{0.0f};
    float       m_cropB{0.0f};
    std::string m_characterName;
    bool        m_isTalking{false};
    std::string m_videoMutePath;
    std::string m_videoTalkPath;
    std::string m_outfit;
    std::string m_animationName;
};

} // namespace rt
