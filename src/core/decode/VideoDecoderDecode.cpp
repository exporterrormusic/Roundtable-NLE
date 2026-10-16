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

#include "decode/VideoDecoder.h"
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

    const auto seekDemuxer = [&](double seconds) {
        const int64_t targetTs = static_cast<int64_t>(seconds / av_q2d(vs->time_base));
        const int flags = AVSEEK_FLAG_BACKWARD;
        int ret = av_seek_frame(m_fmtCtx, m_info.videoStreamIndex, targetTs, flags);
        if (ret < 0) {
            const int64_t tsAv = static_cast<int64_t>(seconds * AV_TIME_BASE);
            ret = av_seek_frame(m_fmtCtx, -1, tsAv, flags);
            if (ret < 0) {
                m_lastError = "Seek failed";
                return false;
            }
        }
        avcodec_flush_buffers(m_codecCtx);
        m_draining = false;
        av_frame_unref(m_pendingFrame);
        m_hasPendingFrame = false;
        av_packet_unref(m_heldPacket);
        m_hasHeldPacket = false;
        return true;
    };

    if (!seekDemuxer(timeSeconds)) return false;
    if (mode == SeekMode::Precise) {
        // Open GOP: B-frames shown just before a keyframe are coded after
        // it, so the keyframe's DECODE time precedes them and a backward
        // seek can pick a keyframe that is SHOWN after the target (target
        // 44, keyframe 45). If decoding starts past the target, seek
        // further back and decode forward again.
        for (double backoff = 1.0;
             decodeForwardTo(timeSeconds) && backoff <= 16.0; backoff *= 2.0) {
            const double from = std::max(0.0, timeSeconds - backoff);
            if (!seekDemuxer(from)) return false;
            if (from <= 0.0) {
                decodeForwardTo(timeSeconds);
                break;
            }
        }
    }

    m_currentFrame = secondsToFrame(timeSeconds);
    return true;
}

bool VideoDecoder::decodeForwardTo(double timeSeconds)
{
    // A frame "covers" the target from half a frame before it (the rule the
    // callers have always used).
    const double fps = m_info.fps > 0.0 ? m_info.fps : 30.0;
    const double reachedAt = timeSeconds - 0.5 / fps;
    const double pastTarget = timeSeconds + 0.5 / fps;
    bool firstPicture = true;
    bool overshot = false;
    const AVRational timeBase = m_fmtCtx->streams[m_info.videoStreamIndex]->time_base;

    // Catch-up decode (FilmCraft's GOP cache does the same): a picture no
    // other picture references, and which is shown before the target, can
    // be skipped outright — the target decodes bit-identically, and on
    // long-GOP B-frame footage about half the pictures are skippable.
    const AVDiscard defaultSkip = m_codecCtx->skip_frame;

    // Returns true once the target is parked in m_pendingFrame.
    const auto takeFrames = [&]() {
        while (avcodec_receive_frame(m_codecCtx, m_frame) == 0) {
            const double shownAt = ptsToSeconds(m_frame->best_effort_timestamp
                                                != AV_NOPTS_VALUE
                                                    ? m_frame->best_effort_timestamp
                                                    : m_frame->pts);
            if (firstPicture) {
                firstPicture = false;
                overshot = shownAt >= pastTarget;
            }
            // Keep the latest picture either way: past the end of the
            // stream the last frame is the closest we have.
            av_frame_unref(m_pendingFrame);
            av_frame_move_ref(m_pendingFrame, m_frame);
            m_hasPendingFrame = true;
            if (shownAt >= reachedAt) return true;
        }
        return false;
    };

    bool found = false;
    while (!found) {
        int readResult = 0;
        if (m_hasHeldPacket) {
            // A packet an earlier precise seek could not send goes first.
            av_packet_move_ref(m_packet, m_heldPacket);
            m_hasHeldPacket = false;
        } else {
            readResult = av_read_frame(m_fmtCtx, m_packet);
        }
        if (readResult < 0) {
            // End of stream: drain what the decoder still holds.
            m_codecCtx->skip_frame = defaultSkip;
            avcodec_send_packet(m_codecCtx, nullptr);
            found = takeFrames();
            m_draining = true;
            break;
        }
        if (m_packet->stream_index != m_info.videoStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }
        const bool beforeTarget = m_packet->pts != AV_NOPTS_VALUE
            && m_packet->pts * av_q2d(timeBase) < reachedAt;
        m_codecCtx->skip_frame = beforeTarget ? AVDISCARD_NONREF : defaultSkip;
        if (avcodec_send_packet(m_codecCtx, m_packet) == AVERROR(EAGAIN)) {
            // Output queue full (frame threads): collect, then resend.
            found = takeFrames();
            if (found) {
                // Not sent yet — decodeNext() sends it first, or that
                // picture would silently go missing after the target.
                av_packet_unref(m_heldPacket);
                av_packet_move_ref(m_heldPacket, m_packet);
                m_hasHeldPacket = true;
                break;
            }
            avcodec_send_packet(m_codecCtx, m_packet);
        }
        av_packet_unref(m_packet);
        found = takeFrames();
    }
    m_codecCtx->skip_frame = defaultSkip;
    return overshot;
}

bool VideoDecoder::seekToFrame(int64_t frameNumber, SeekMode mode)
{
    double time = frameToSeconds(frameNumber);
    return seek(time, mode);
}

bool VideoDecoder::advanceTo(double timeSeconds)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return false;
    if (m_hasPendingFrame) {
        // A parked picture that already covers the target is the answer.
        const double fps = m_info.fps > 0.0 ? m_info.fps : 30.0;
        const double shownAt = ptsToSeconds(
            m_pendingFrame->best_effort_timestamp != AV_NOPTS_VALUE
                ? m_pendingFrame->best_effort_timestamp : m_pendingFrame->pts);
        if (shownAt >= timeSeconds - 0.5 / fps) return true;
        av_frame_unref(m_pendingFrame);
        m_hasPendingFrame = false;
    }
    decodeForwardTo(timeSeconds);   // consumes a held packet first
    m_currentFrame = secondsToFrame(timeSeconds);
    return m_hasPendingFrame;
}

// ── Decode ──────────────────────────────────────────────────────────────────

bool VideoDecoder::decodeNext(DecodedFrame& outFrame)
{
    std::lock_guard lock(m_mutex);
    if (!m_isOpen) return false;

    // The target of the last precise seek.
    if (m_hasPendingFrame) {
        av_frame_unref(m_frame);
        av_frame_move_ref(m_frame, m_pendingFrame);
        m_hasPendingFrame = false;
        fillFrameInfo(outFrame, m_frame);
        return true;
    }

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

        if (m_hasHeldPacket) {
            av_packet_move_ref(m_packet, m_heldPacket);
            m_hasHeldPacket = false;
            ret = 0;
        } else {
            ret = av_read_frame(m_fmtCtx, m_packet);
        }
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
