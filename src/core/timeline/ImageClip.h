/*
 * ImageClip — a timeline clip referencing a still image file.
 *
 * Decodes a single frame from the image path and holds it for the
 * clip's entire duration on the timeline.
 */

#pragma once

#include "timeline/Clip.h"
#include <string>

namespace rt {

class ImageClip : public Clip
{
public:
    ImageClip();
    explicit ImageClip(const std::string& mediaPath);
    ~ImageClip() override = default;

    // ── Media reference ─────────────────────────────────────────────────
    [[nodiscard]] const std::string& mediaPath() const noexcept { return m_mediaPath; }
    void setMediaPath(const std::string& path) { m_mediaPath = path; }

    [[nodiscard]] uint64_t mediaId() const noexcept { return m_mediaId; }
    void setMediaId(uint64_t id) noexcept { m_mediaId = id; }

    // ── Image properties ────────────────────────────────────────────────
    [[nodiscard]] uint32_t sourceWidth()  const noexcept { return m_sourceWidth; }
    [[nodiscard]] uint32_t sourceHeight() const noexcept { return m_sourceHeight; }
    void setSourceResolution(uint32_t w, uint32_t h) noexcept { m_sourceWidth = w; m_sourceHeight = h; }

    // ── Crop (percentage 0–100) ─────────────────────────────────────────
    void setCrop(float l, float r, float t, float b) {
        m_cropL = l; m_cropR = r; m_cropT = t; m_cropB = b;
    }
    [[nodiscard]] float cropLeft()   const noexcept { return m_cropL; }
    [[nodiscard]] float cropRight()  const noexcept { return m_cropR; }
    [[nodiscard]] float cropTop()    const noexcept { return m_cropT; }
    [[nodiscard]] float cropBottom() const noexcept { return m_cropB; }

    // ── Clone ───────────────────────────────────────────────────────────
protected:
    [[nodiscard]] std::unique_ptr<Clip> createEmpty() const override;
    void copyOwnStateFrom(const Clip& src, CloneMode mode) override;
public:

private:
    std::string m_mediaPath;
    uint64_t    m_mediaId{0};
    uint32_t    m_sourceWidth{0};
    uint32_t    m_sourceHeight{0};
    float       m_cropL{0.0f};
    float       m_cropR{0.0f};
    float       m_cropT{0.0f};
    float       m_cropB{0.0f};
};

} // namespace rt
