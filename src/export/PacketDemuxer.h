/*
 * PacketDemuxer — extracts raw compressed packets from a source file.
 *
 * Used by the smart rendering path to copy source packets directly into the
 * output container without decoding + re-encoding. Opens a source file,
 * seeks to a frame, and reads the compressed video packet.
 */

#pragma once

#include "Encoder.h"  // EncodedPacket

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// FFmpeg forward declarations
struct AVFormatContext;
struct AVPacket;
struct AVCodecContext;

namespace rt {

class PacketDemuxer
{
public:
    PacketDemuxer();
    ~PacketDemuxer();

    PacketDemuxer(const PacketDemuxer&) = delete;
    PacketDemuxer& operator=(const PacketDemuxer&) = delete;

    /// Open a source media file. Returns true on success.
    bool open(const std::string& path);

    /// Close and release resources.
    void close();

    /// Read the compressed video packet for a given source frame number.
    /// The returned packet data is owned by the internal buffer (valid until
    /// the next call to readFrame or close).
    /// @param frameNumber  0-based source frame index
    /// @param outPkt       Filled with the raw packet data on success
    /// @return true if a packet was successfully read
    bool readFrame(int64_t frameNumber, EncodedPacket& outPkt);

    /// Check if the demuxer is open.
    [[nodiscard]] bool isOpen() const noexcept;

    /// Get the video stream index.
    [[nodiscard]] int videoStreamIndex() const noexcept { return m_videoStream; }
    [[nodiscard]] bool strictlyMatches(const AVCodecContext* target,
                                       uint32_t width, uint32_t height,
                                       int fpsNum, int fpsDen) const noexcept;
    [[nodiscard]] AVCodecContext* codecContext() const noexcept { return m_codecContext; }

private:
    AVFormatContext* m_fmtCtx{nullptr};
    AVPacket*        m_pkt{nullptr};
    AVCodecContext*  m_codecContext{nullptr};
    int              m_videoStream{-1};
    double           m_fps{0.0};
    int64_t          m_startPts{0};
    std::string      m_path;

    // Persistent buffer for packet data (avoids per-frame allocation)
    std::vector<uint8_t> m_buffer;
};

} // namespace rt
