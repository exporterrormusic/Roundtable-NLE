/*
 * VideoDecoderDecode.cpp — Seek, decode, and frame info for VideoDecoder.
 * Extracted from VideoDecoder.cpp for maintainability.
 */

#ifdef ROUNDTABLE_HAS_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include "media/VideoDecoder.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace rt {

// ── Helpers (local to this TU) ──────────────────────────────────────────────

static PixelFormat convertPixFmt(AVPixelFormat fmt)
{
    switch (fmt)
    {
    case AV_PIX_FMT_NV12:    return PixelFormat::NV12;
    case AV_PIX_FMT_YUV420P: return PixelFormat::YUV420P;
    case AV_PIX_FMT_BGRA:    return PixelFormat::BGRA;
    case AV_PIX_FMT_RGBA:    return PixelFormat::RGBA;
    case AV_PIX_FMT_CUDA:    return PixelFormat::NV12;
    case AV_PIX_FMT_QSV:    return PixelFormat::NV12;
    default:                  return PixelFormat::Unknown;
    }
}

// ── Seek ────────────────────────────────────────────────────────────────────

bool VideoDecoder::seek(double timeSeconds, SeekMode mode)
{
    std::unique_lock lock(m_mutex);
    if (!m_isOpen) return false;

    // Still-image fast path (single-frame JPG/PNG/etc.). FFmpeg's image2
    // demuxer can't rewind: after the lone packet is read, av_read_frame
    // returns EOF forever. av_seek_frame "succeeds" but doesn't actually
    // re-emit the packet, and Precise mode then flushes the just-decoded
    // frame — so the subsequent decodeNext fails. Reopening from scratch
    // resets the demuxer cursor so decodeNext can read the JPG packet
    // again. Mirrors what MediaPool::decodeFrame does for its main entry
    // decoder via close()+open().
    const bool isStill = (m_info.frameCount <= 1 || m_info.duration <= 0.0);
    if (isStill && !m_path.empty()) {
        std::filesystem::path path = m_path;
        bool forceSoftware = m_forceSoftware;
        int maxThreads = m_maxThreads;
        bool sliceOnly = m_sliceOnlyThreading;
        lock.unlock();   // open() takes the same mutex
        bool ok = open(path, forceSoftware, maxThreads, sliceOnly);
        return ok;
    }

    AVStream* vs = m_fmtCtx->streams[m_info.videoStreamIndex];

    int64_t targetTs = static_cast<int64_t>(timeSeconds / av_q2d(vs->time_base));

    int flags = AVSEEK_FLAG_BACKWARD;
    int ret = av_seek_frame(m_fmtCtx, m_info.videoStreamIndex, targetTs, flags);
    if (ret < 0)
    {
        int64_t tsAv = static_cast<int64_t>(timeSeconds * AV_TIME_BASE);
        ret = av_seek_frame(m_fmtCtx, -1, tsAv, flags);
        if (ret < 0)
        {
            m_lastError = "Seek failed";
            return false;
        }
    }

    avcodec_flush_buffers(m_codecCtx);
    m_draining = false;

    if (mode == SeekMode::Precise)
    {
        DecodedFrame dummy;
        while (true)
        {
            int rdret = av_read_frame(m_fmtCtx, m_packet);
            if (rdret < 0) break;
            if (m_packet->stream_index != m_info.videoStreamIndex) {
                av_packet_unref(m_packet);
                continue;
            }
            bool ok = decodePacket(m_packet, dummy);
            av_packet_unref(m_packet);
            if (ok && dummy.timestamp >= timeSeconds - (0.5 / m_info.fps))
                break;
        }
        avcodec_flush_buffers(m_codecCtx);
    }

    m_currentFrame = secondsToFrame(timeSeconds);
    return true;
}

bool VideoDecoder::seekToFrame(int64_t frameNumber, SeekMode mode)
{
    double time = frameToSeconds(frameNumber);
    return seek(time, mode);
}

// ── Decode ──────────────────────────────────────────────────────────────────

bool VideoDecoder::decodeNext(DecodedFrame& outFrame)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return false;

    while (true)
    {
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == 0) {
            fillFrameInfo(outFrame, m_frame);
            return true;
        }
        if (ret == AVERROR_EOF) {
            m_draining = false;
            return false;
        }

        if (m_draining) {
            m_draining = false;
            return false;
        }

        ret = av_read_frame(m_fmtCtx, m_packet);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF) {
                avcodec_send_packet(m_codecCtx, nullptr);
                m_draining = true;
                continue;
            }
            return false;
        }

        if (m_packet->stream_index != m_info.videoStreamIndex)
        {
            av_packet_unref(m_packet);
            continue;
        }

        ret = avcodec_send_packet(m_codecCtx, m_packet);
        av_packet_unref(m_packet);

        if (ret == AVERROR(EAGAIN)) {
            spdlog::warn("VideoDecoder: send_packet EAGAIN after receive EAGAIN "
                         "— possible packet loss");
        } else if (ret < 0 && ret != AVERROR_EOF) {
            return false;
        }
    }
}

bool VideoDecoder::decodeAt(double timeSeconds, DecodedFrame& outFrame)
{
    if (!seek(timeSeconds, SeekMode::Precise))
        return false;
    return decodeNext(outFrame);
}

bool VideoDecoder::decodePacket(AVPacket* packet, DecodedFrame& outFrame)
{
    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return false;

    ret = avcodec_receive_frame(m_codecCtx, m_frame);
    if (ret < 0)
        return false;

    fillFrameInfo(outFrame, m_frame);
    return true;
}

void VideoDecoder::fillFrameInfo(DecodedFrame& outFrame, AVFrame* frame)
{
    outFrame.width = static_cast<uint32_t>(frame->width);
    outFrame.height = static_cast<uint32_t>(frame->height);
    outFrame.pts = frame->pts;
    outFrame.timestamp = ptsToSeconds(frame->pts);
    outFrame.frameIndex = m_currentFrame++;
    outFrame.isKeyframe = (frame->flags & AV_FRAME_FLAG_KEY) != 0;
    outFrame.avFrame = frame;

    if (frame->format == AV_PIX_FMT_CUDA || frame->format == AV_PIX_FMT_D3D11)
    {
        outFrame.isHardware = true;
        outFrame.format = PixelFormat::NV12;
        outFrame.rawFormat = frame->format;

        if (frame->format == AV_PIX_FMT_CUDA && frame->data[0]) {
            for (int i = 0; i < 4; ++i) {
                outFrame.data[i] = frame->data[i];
                outFrame.linesize[i] = frame->linesize[i];
            }
        } else {
            std::memset(outFrame.data, 0, sizeof(outFrame.data));
            std::memset(outFrame.linesize, 0, sizeof(outFrame.linesize));
        }
    }
    else
    {
        outFrame.isHardware = false;
        outFrame.format = convertPixFmt(static_cast<AVPixelFormat>(frame->format));
        outFrame.rawFormat = frame->format;
        for (int i = 0; i < 4; ++i)
        {
            outFrame.data[i] = frame->data[i];
            outFrame.linesize[i] = frame->linesize[i];
        }
    }
}

// ── Hardware frame transfer ─────────────────────────────────────────────────

bool VideoDecoder::transferHardwareFrame(const DecodedFrame& hwFrame,
                                          DecodedFrame& cpuFrame)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen || !m_hwAccel) return false;

    // Transfer hardware frame to CPU memory
    int ret = av_hwframe_transfer_data(m_hwFrame, hwFrame.avFrame, 0);
    if (ret < 0)
    {
        m_lastError = "Hardware frame transfer failed";
        return false;
    }

    fillFrameInfo(cpuFrame, m_hwFrame);
    cpuFrame.isHardware = false;
    return true;
}

} // namespace rt
#endif // ROUNDTABLE_HAS_FFMPEG
