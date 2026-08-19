/*
 * PacketDemuxer.cpp — FFmpeg-based raw packet extraction.
 */

#include "PacketDemuxer.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

namespace rt {

PacketDemuxer::PacketDemuxer()
{
    m_pkt = av_packet_alloc();
}

PacketDemuxer::~PacketDemuxer()
{
    close();
    if (m_pkt) av_packet_free(&m_pkt);
}

bool PacketDemuxer::open(const std::string& path)
{
    close();
    m_path = path;

    if (avformat_open_input(&m_fmtCtx, path.c_str(), nullptr, nullptr) < 0) {
        spdlog::error("PacketDemuxer: Failed to open '{}'", path);
        return false;
    }

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        spdlog::error("PacketDemuxer: Failed to find stream info for '{}'", path);
        close();
        return false;
    }

    // Find best video stream
    m_videoStream = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStream < 0) {
        spdlog::error("PacketDemuxer: No video stream in '{}'", path);
        close();
        return false;
    }

    auto* st = m_fmtCtx->streams[m_videoStream];
    m_codecContext = avcodec_alloc_context3(nullptr);
    if (!m_codecContext ||
        avcodec_parameters_to_context(m_codecContext, st->codecpar) < 0) {
        spdlog::error("PacketDemuxer: Cannot copy codec parameters for '{}'", path);
        close();
        return false;
    }
    m_startPts = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;

    // Compute FPS from stream time_base and codec framerate
    if (st->avg_frame_rate.den > 0 && st->avg_frame_rate.num > 0) {
        m_fps = av_q2d(st->avg_frame_rate);
    } else if (st->r_frame_rate.den > 0 && st->r_frame_rate.num > 0) {
        m_fps = av_q2d(st->r_frame_rate);
    } else {
        m_fps = 30.0; // fallback
    }

    spdlog::debug("PacketDemuxer: Opened '{}' stream={} fps={:.3f}", path, m_videoStream, m_fps);
    return true;
}

void PacketDemuxer::close()
{
    if (m_codecContext)
        avcodec_free_context(&m_codecContext);
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStream = -1;
    m_fps = 0.0;
    m_startPts = 0;
    m_path.clear();
    m_buffer.clear();
}

bool PacketDemuxer::strictlyMatches(const AVCodecContext* target,
                                    uint32_t width, uint32_t height,
                                    int fpsNum, int fpsDen) const noexcept
{
    if (!isOpen() || !m_codecContext || !target || fpsNum <= 0 || fpsDen <= 0)
        return false;
    const AVStream* st = m_fmtCtx->streams[m_videoStream];
    const AVRational sourceRate = st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0
        ? st->avg_frame_rate : st->r_frame_rate;
    const AVRational targetRate{fpsNum, fpsDen};
    return sourceRate.num > 0 && sourceRate.den > 0 &&
           av_cmp_q(sourceRate, targetRate) == 0 &&
           m_codecContext->codec_id == target->codec_id &&
           m_codecContext->width == static_cast<int>(width) &&
           m_codecContext->height == static_cast<int>(height) &&
           m_codecContext->pix_fmt == target->pix_fmt &&
           m_codecContext->profile == target->profile &&
           m_codecContext->level == target->level &&
           m_codecContext->color_range == target->color_range &&
           m_codecContext->color_primaries == target->color_primaries &&
           m_codecContext->color_trc == target->color_trc &&
           m_codecContext->colorspace == target->colorspace &&
           m_codecContext->chroma_sample_location == target->chroma_sample_location &&
           av_cmp_q(m_codecContext->sample_aspect_ratio,
                    target->sample_aspect_ratio) == 0 &&
           m_codecContext->extradata && m_codecContext->extradata_size > 0;
}

bool PacketDemuxer::isOpen() const noexcept
{
    return m_fmtCtx != nullptr && m_videoStream >= 0;
}

bool PacketDemuxer::readFrame(int64_t frameNumber, EncodedPacket& outPkt)
{
    if (!isOpen()) return false;

    auto* st = m_fmtCtx->streams[m_videoStream];

    // Convert frame number to timestamp in stream time_base
    // target_seconds = frameNumber / fps
    // target_pts = startPts + target_seconds / av_q2d(time_base)
    double targetSeconds = static_cast<double>(frameNumber) / m_fps;
    int64_t targetPts = m_startPts + static_cast<int64_t>(
        targetSeconds / av_q2d(st->time_base));

    // Seek to the nearest keyframe at or before the target
    int ret = av_seek_frame(m_fmtCtx, m_videoStream, targetPts,
                            AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        spdlog::warn("PacketDemuxer: Seek failed for frame {} in '{}'",
                     frameNumber, m_path);
        return false;
    }

    // Read packets until we find the video packet closest to our target PTS.
    // After seeking to a keyframe, we need to skip forward to the exact frame.
    bool found = false;
    int maxAttempts = 500; // Safety limit

    while (maxAttempts-- > 0) {
        av_packet_unref(m_pkt);
        ret = av_read_frame(m_fmtCtx, m_pkt);
        if (ret < 0) break; // EOF or error

        if (m_pkt->stream_index != m_videoStream)
            continue;

        // Check if this packet's PTS matches our target (within 1 frame tolerance)
        int64_t frameDurationPts = static_cast<int64_t>(
            1.0 / (m_fps * av_q2d(st->time_base)));
        if (frameDurationPts <= 0) frameDurationPts = 1;

        if (m_pkt->pts >= targetPts - frameDurationPts / 2) {
            found = true;
            break;
        }
    }

    if (!found) {
        spdlog::warn("PacketDemuxer: Could not find frame {} in '{}'",
                     frameNumber, m_path);
        return false;
    }

    // Copy packet data into our persistent buffer
    m_buffer.assign(m_pkt->data, m_pkt->data + m_pkt->size);

    outPkt.data = m_buffer.data();
    outPkt.size = static_cast<int>(m_buffer.size());
    outPkt.pts = m_pkt->pts;
    outPkt.dts = m_pkt->dts;
    outPkt.duration = m_pkt->duration;
    outPkt.isKeyframe = (m_pkt->flags & AV_PKT_FLAG_KEY) != 0;
    outPkt.ownsData = false; // m_buffer owns it

    return true;
}

} // namespace rt
