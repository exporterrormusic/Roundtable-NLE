/*
 * ProResAlphaEncoder — ProRes 4444 encoder with native alpha channel.
 *
 * Uses FFmpeg's prores_ks encoder with profile 4 (ProRes 4444) to produce
 * MOV files with a native YUVA alpha channel.  ProRes 4444 is the same
 * format used by Premiere Pro and DaVinci Resolve for alpha-bearing media.
 *
 * Key advantages over HEVC packed-alpha:
 *   - Intra-frame codec: every frame is independently decodable.
 *     No B-frame reordering, no drain issues, instant random access.
 *   - Native alpha: no packed-alpha hack, no half-height unpacking.
 *   - Industry standard: well-tested, stable decode in FFmpeg.
 *
 * Disadvantage: ~5-10× larger files than HEVC.  For short character
 * animation loops (2-5 seconds at 1080×1920) this is acceptable —
 * roughly 30-100 MB per clip vs 5-15 MB for HEVC.
 *
 * Thread-safe: one instance per thread.
 */

#pragma once

#include "convert/MediaFileEncoderBase.h"

namespace rt {

class ProResAlphaEncoder : public MediaFileEncoderBase
{
public:
    ProResAlphaEncoder() = default;
    ~ProResAlphaEncoder() override;

    /// Check whether the prores_ks encoder is available in this build.
    static bool isAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mov)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param quality  ProRes profile (0-5; 4 = 4444 with alpha)
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int quality = 4);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    bool writeFrame(const uint8_t* rgbaPixels);

protected:
    [[nodiscard]] const char* logName() const noexcept override { return "ProRes"; }
};

} // namespace rt
