/*
 * test_av1_decode.cpp — an AV1 file opened the normal (hardware-first) way
 * decodes the right pictures.
 *
 * Regression: FFmpeg prefers libdav1d for AV1, which has no hardware
 * support, yet the hardware init started from the device's pixel format and
 * "initialized" it as a hardware decoder. Playback then treated software
 * pictures as GPU surfaces and almost no frame arrived (0.2% exact in the
 * format profile). AV1 now goes to the native decoder, which hands it to
 * NVDEC / D3D11VA / QSV when the GPU has AV1 — and decodes in software when
 * it does not — so this test passes with or without a GPU.
 *
 * The fixture draws each frame's index as 8 black/white luma stripes.
 */
#include <gtest/gtest.h>

#include "decode/VideoDecoder.h"

#include <algorithm>
#include <filesystem>
#include <string>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#endif

using namespace rt;

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

constexpr int kW = 128, kH = 64, kFps = 30, kFrames = 30, kBits = 8;

bool writeIndexedAv1(const std::string& path)
{
    const AVCodec* codec = avcodec_find_encoder_by_name("libsvtav1");
    if (!codec) codec = avcodec_find_encoder_by_name("libaom-av1");
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
    enc->gop_size = kFrames;
    enc->bit_rate = 4'000'000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (std::string(codec->name) == "libaom-av1")
        av_opt_set_int(enc->priv_data, "cpu-used", 8, 0);
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

/// Frame index read from the luma stripes (-1 if unreadable).
int frameIndexOf(const DecodedFrame& frame)
{
    if (!frame.data[0] || frame.width < static_cast<uint32_t>(kW)) return -1;
    const int stripe = kW / kBits;
    const uint8_t* row = frame.data[0] + (kH / 2) * frame.linesize[0];
    int index = 0;
    for (int bit = 0; bit < kBits; ++bit)
        if (row[bit * stripe + stripe / 2] > 128) index |= (1 << bit);
    return index;
}

} // namespace

TEST(Av1Decode, HardwareFirstOpenDecodesTheRightPictures)
{
    const auto path = std::filesystem::temp_directory_path() / "rt_av1_decode_test.mp4";
    if (!writeIndexedAv1(path.string()))
        GTEST_SKIP() << "no AV1 encoder in this FFmpeg";

    {
        VideoDecoder decoder;
        ASSERT_TRUE(decoder.open(path, /*forceSoftware=*/false));
        for (int expected = 0; expected < kFrames; ++expected) {
            DecodedFrame decoded;
            ASSERT_TRUE(decoder.decodeNext(decoded)) << "frame " << expected;
            if (decoded.isHardware) {
                DecodedFrame cpu;
                ASSERT_TRUE(decoder.transferHardwareFrame(decoded, cpu)) << "frame " << expected;
                EXPECT_EQ(frameIndexOf(cpu), expected);
            } else {
                EXPECT_EQ(frameIndexOf(decoded), expected);
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
#endif
