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

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

namespace rt {

class ProResAlphaEncoder
{
public:
    ProResAlphaEncoder();
    ~ProResAlphaEncoder();

    ProResAlphaEncoder(const ProResAlphaEncoder&) = delete;
    ProResAlphaEncoder& operator=(const ProResAlphaEncoder&) = delete;

    /// Check whether ProRes 4444 encoding is available (prores_ks codec).
    static bool isAvailable();

    /// Open an output file for writing.
    /// @param path     Output file path (.mov)
    /// @param width    Frame width in pixels (must be even)
    /// @param height   Frame height in pixels (must be even)
    /// @param fps      Frames per second
    /// @param quality  ProRes quality profile: 0=Proxy, 1=LT, 2=Standard,
    ///                 3=HQ, 4=4444 (default), 5=4444XQ.
    ///                 Only profiles 4 and 5 support alpha.
    /// @return true on success
    bool open(const std::filesystem::path& path,
              uint32_t width, uint32_t height,
              int fps = 60, int quality = 4);

    /// Write one RGBA frame (top-down, row-major, 4 bytes/pixel).
    bool writeFrame(const uint8_t* rgbaPixels);

    /// Flush encoder, write trailer, close file.
    bool finalize();

    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }
    [[nodiscard]] int64_t framesWritten() const noexcept { return m_framesWritten; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    bool flushEncoder();

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_codecCtx{nullptr};
    AVFrame*         m_frame{nullptr};
    AVPacket*        m_packet{nullptr};
    AVStream*        m_stream{nullptr};
    SwsContext*      m_swsCtx{nullptr};

    uint32_t    m_width{0};
    uint32_t    m_height{0};
    int64_t     m_framesWritten{0};
    bool        m_isOpen{false};
    std::string m_lastError;
};

} // namespace rt
