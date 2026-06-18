/*
 * VideoDecoder.cpp - Video decoder lifecycle and queries.
 * Init, decode, and seek extracted to VideoDecoderInit.cpp
 * and VideoDecoderDecode.cpp.
 */

#ifdef ROUNDTABLE_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include "decode/VideoDecoder.h"
#include <spdlog/spdlog.h>

namespace rt {

// ---- Global user preference -------------------------------------------------

static bool s_forceSoftwareDecode = false;

void setForceSoftwareDecode(bool force) { s_forceSoftwareDecode = force; }
bool forceSoftwareDecode() { return s_forceSoftwareDecode; }

// ---- Constructor / Destructor -----------------------------------------------

VideoDecoder::VideoDecoder()
{
    m_frame   = av_frame_alloc();
    m_hwFrame = av_frame_alloc();
    m_packet  = av_packet_alloc();

    if (!m_frame || !m_hwFrame || !m_packet) {
        spdlog::error("VideoDecoder: Failed to allocate AVFrame/AVPacket");
    }
}

VideoDecoder::~VideoDecoder()
{
    close();
    av_frame_free(&m_frame);
    av_frame_free(&m_hwFrame);
    av_packet_free(&m_packet);
}

// ---- Queries -----------------------------------------------------------------

bool VideoDecoder::isHardwareAccelerated() const noexcept
{
    return m_hwAccel;
}

const std::string& VideoDecoder::lastError() const noexcept
{
    return m_lastError;
}

int64_t VideoDecoder::secondsToFrame(double seconds) const noexcept
{
    if (m_info.fps <= 0.0) return 0;
    return static_cast<int64_t>(std::floor(seconds * m_info.fps + 0.5));
}

double VideoDecoder::frameToSeconds(int64_t frame) const noexcept
{
    if (m_info.fps <= 0.0) return 0.0;
    return static_cast<double>(frame) / m_info.fps;
}

double VideoDecoder::ptsToSeconds(int64_t pts) const noexcept
{
    if (m_info.timebaseDen <= 0) return 0.0;
    return static_cast<double>(pts) * m_info.timebaseNum / m_info.timebaseDen;
}

HbdPlaneFormat hbdPlaneFormat(const DecodedFrame& f) noexcept
{
    switch (f.rawFormat) {
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P016LE:       return HbdPlaneFormat::P010;
        case AV_PIX_FMT_YUVA444P12LE: return HbdPlaneFormat::Yuva444p12;
        default:                      return HbdPlaneFormat::None;
    }
}

} // namespace rt

#else // !ROUNDTABLE_HAS_FFMPEG

// ---- Stub implementations when FFmpeg is not available -----------------------

#include "decode/VideoDecoder.h"
#include <spdlog/spdlog.h>

namespace rt {

VideoDecoder::VideoDecoder() {}
VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::open(const std::filesystem::path&, bool, int, bool)
{
    m_lastError = "FFmpeg not available - video decode disabled";
    spdlog::error("VideoDecoder: {}", m_lastError);
    return false;
}

void VideoDecoder::close() { m_isOpen = false; }
bool VideoDecoder::isOpen() const noexcept { return false; }
const VideoStreamInfo& VideoDecoder::info() const noexcept { return m_info; }
bool VideoDecoder::seek(double, SeekMode) { return false; }
bool VideoDecoder::seekToFrame(int64_t, SeekMode) { return false; }
bool VideoDecoder::decodeNext(DecodedFrame&) { return false; }
bool VideoDecoder::decodeAt(double, DecodedFrame&) { return false; }
bool VideoDecoder::isHardwareAccelerated() const noexcept { return false; }
bool VideoDecoder::transferHardwareFrame(const DecodedFrame&, DecodedFrame&) { return false; }
const std::string& VideoDecoder::lastError() const noexcept { return m_lastError; }
int64_t VideoDecoder::secondsToFrame(double) const noexcept { return 0; }
double VideoDecoder::frameToSeconds(int64_t) const noexcept { return 0.0; }
double VideoDecoder::ptsToSeconds(int64_t) const noexcept { return 0.0; }

void prewarmHardwareDecoders() {}
void shutdownHardwareDecoders() noexcept {}

HbdPlaneFormat hbdPlaneFormat(const DecodedFrame&) noexcept { return HbdPlaneFormat::None; }

} // namespace rt
#endif // ROUNDTABLE_HAS_FFMPEG