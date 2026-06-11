/*
 * VP9AlphaEncoder — self-contained VP9+alpha WebM writer.
 *
 * Encodes RGBA frames to VP9 with full alpha channel support using
 * FFmpeg's libvpx-vp9 codec in YUVA420P pixel format, muxed into
 * a WebM container.
 *
 * This is NOT part of the export pipeline's Encoder hierarchy — it is
 * a standalone utility for the pre-rendered animation cache system
 * (AnimationVideoCache / SpinePrerenderer).
 *
 * Usage:
 *   VP9AlphaEncoder enc;
 *   enc.open("out.webm", 1024, 2048, 60);
 *   for (each frame) enc.writeFrame(rgbaPixels);
 *   enc.finalize();
 */

#pragma once

#include "convert/MediaFileEncoderBase.h"

namespace rt {

class VP9AlphaEncoder : public MediaFileEncoderBase
{
public:
    VP9AlphaEncoder() = default;
    ~VP9AlphaEncoder() override;

    /// Open a WebM file for writing.
    /// @param path     Output file path (.webm)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param crf      Constant Rate Factor (0-63, lower = better; default 18)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 18);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    /// Frames must be written sequentially.
    bool writeFrame(const uint8_t* rgbaPixels);

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "VP9Alpha"; }
};

} // namespace rt
