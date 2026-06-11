/*
 * ChromaKeyEncoder — Standard H.264 encoder for chroma-key output.
 *
 * Takes RGBA pixels (with alpha already composited over a solid background
 * color by the caller), and encodes them as a standard YUV420P H.264 MP4
 * file — no packed-alpha, no alpha channel, just a normal video with a
 * solid-color background ready for keying in external editing software.
 *
 * Uses NVENC (h264_nvenc) for GPU acceleration when available, falling
 * back to libx264 software encoding.
 *
 * Thread-safe: one instance per thread.
 */

#pragma once

#include "convert/MediaFileEncoderBase.h"

namespace rt {

class ChromaKeyEncoder : public MediaFileEncoderBase
{
public:
    ChromaKeyEncoder() = default;
    ~ChromaKeyEncoder() override;

    /// Check whether NVENC H.264 encoding is available on this system.
    static bool isNvencAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mp4)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param crf      Quality (0-51; default 22)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 22);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    /// The alpha channel is ignored — only RGB is encoded.
    bool writeFrame(const uint8_t* rgbaPixels);

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "ChromaKey"; }
};

} // namespace rt
