/*
 * IndexedVideoFixture.h - a small MPEG-4 test video whose frame i shows i as
 * black/white stripes, and the reader for those stripes. Shared by the
 * MediaPool frame tests (test_export_decode, test_frame_request).
 *
 * Software-decoded MPEG-4, GOP 45 with 2 B-frames, so decoders run the
 * reorder and frame-threaded paths.
 */
#pragma once

#ifdef ROUNDTABLE_HAS_FFMPEG

#include <cache/FrameCache.h>

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace rt_test {

using rt::CachedFrame;

constexpr int kW = 128, kH = 64, kFps = 30, kFrames = 150, kBits = 8, kGop = 45;

/// 128x64 MPEG-4 mp4: GOP 45 with 2 B-frames; frame i shows i in binary.
/// `tagBt709` marks the stream BT.709 limited range; untagged, its 64-line
/// height makes decoders assume SD BT.601 (and skip the GPU converters).
inline bool writeIndexedVideo(const std::string& path, bool tagBt709 = false)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) return false;
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.c_str()) < 0) return false;
    AVStream* st = avformat_new_stream(fmt, nullptr);
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->width = kW;
    enc->height = kH;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, kFps};
    enc->framerate = AVRational{kFps, 1};
    enc->gop_size = kGop;
    enc->max_b_frames = 2;
    if (tagBt709) {
        enc->color_primaries = AVCOL_PRI_BT709;
        enc->color_trc       = AVCOL_TRC_BT709;
        enc->colorspace      = AVCOL_SPC_BT709;
        enc->color_range     = AVCOL_RANGE_MPEG;
    }
    enc->bit_rate = 2'000'000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    bool ok = avcodec_open2(enc, codec, nullptr) >= 0 &&
              avcodec_parameters_from_context(st->codecpar, enc) >= 0;
    st->time_base = enc->time_base;
    ok = ok && avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
         avformat_write_header(fmt, nullptr) >= 0;

    AVFrame* frame = av_frame_alloc();
    frame->format = enc->pix_fmt;
    frame->width = kW;
    frame->height = kH;
    ok = ok && av_frame_get_buffer(frame, 0) >= 0;
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&](AVFrame* in) {
        if (avcodec_send_frame(enc, in) < 0) return false;
        while (avcodec_receive_packet(enc, pkt) == 0) {
            // Without a duration the last frame has none, the file reads as
            // 30.2 fps, and every frame-number → time mapping drifts.
            if (pkt->duration <= 0) pkt->duration = 1;
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(fmt, pkt) < 0) return false;
        }
        return true;
    };
    for (int i = 0; ok && i < kFrames; ++i) {
        ok = av_frame_make_writable(frame) >= 0;
        for (int y = 0; y < kH; ++y)
            for (int x = 0; x < kW; ++x) {
                const int bit = std::min(x / (kW / kBits), kBits - 1);
                frame->data[0][y * frame->linesize[0] + x] = ((i >> bit) & 1) ? 235 : 16;
            }
        for (int y = 0; y < kH / 2; ++y)
            for (int x = 0; x < kW / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 128;
            }
        frame->pts = i;
        ok = ok && drain(frame);
    }
    ok = ok && drain(nullptr) && av_write_trailer(fmt) >= 0;
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    if (fmt->pb) avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return ok;
}

/// Frame index read from the stripes of a BGRA frame (-1 if unreadable).
inline int frameIndexOf(const CachedFrame* frame)
{
    if (!frame || frame->pixels.empty() || frame->width < static_cast<uint32_t>(kW))
        return -1;
    const int stripe = kW / kBits;
    const uint8_t* row = frame->pixels.data() + (kH / 2) * frame->stride;
    int index = 0;
    for (int bit = 0; bit < kBits; ++bit)
        if (row[(bit * stripe + stripe / 2) * 4 + 1] > 128) index |= (1 << bit);
    return index;
}

} // namespace rt_test

#endif // ROUNDTABLE_HAS_FFMPEG
