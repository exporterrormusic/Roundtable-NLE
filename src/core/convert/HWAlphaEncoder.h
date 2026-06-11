/*
 * HWAlphaEncoder — GPU-accelerated H.264/HEVC encoder with packed alpha.
 *
 * Uses NVIDIA NVENC (h264_nvenc) for hardware-accelerated video encoding
 * with transparency support.  Because NVENC does not support YUVA pixel
 * formats, we use the "packed-alpha" technique:
 *
 *   ┌───────────────────┐
 *   │   RGB (top half)  │  ← original colour pixels
 *   ├───────────────────┤
 *   │ Alpha (bot half)  │  ← alpha channel as greyscale
 *   └───────────────────┘
 *
 * The output is a standard stream in an MP4 container at double the
 * nominal height.  Full-range (JPEG) color keeps all 256 alpha levels in
 * luma.  On decode, the consumer splits the frame in half and
 * reconstructs RGBA; the layout is auto-detected via the "packed_alpha"
 * container metadata tag.  All-intra (every frame IDR) for O(1) seeking.
 *
 * Falls back to software libx265/libx264 when NVENC is unavailable.
 *
 * Thread-safe: one instance per thread.
 */

#pragma once

#include "convert/MediaFileEncoderBase.h"

#include <vector>

namespace rt {

class HWAlphaEncoder : public MediaFileEncoderBase
{
public:
    HWAlphaEncoder() = default;
    ~HWAlphaEncoder() override;

    /// Check whether NVENC H.264 encoding is available on this system.
    static bool isNvencAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mp4)
    /// @param width    Nominal frame width in pixels (must be even)
    /// @param height   Nominal frame height (encoded height is 2×)
    /// @param fps      Frames per second
    /// @param crf      Quality (0-51)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int crf = 22);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    bool writeFrame(const uint8_t* rgbaPixels);

    /// Flush, write trailer, close file, release the packing buffer.
    bool finalize() override;

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "HWAlpha"; }

private:
    /// Intermediate CPU buffer for the packed (2× height) RGBA frame
    std::vector<uint8_t> m_packedRGBA;
};

} // namespace rt
