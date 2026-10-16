/*
 * test_video_decoder_seek.cpp — VideoDecoder::seek(Precise) lands on the
 * requested frame and leaves the decoder able to continue from it.
 *
 * Regression: the precise seek decoded up to the target, then FLUSHED the
 * codec — the target picture was thrown away and the next decodeNext() ran
 * on a decoder with no reference frames (long-GOP H.264 then skips to the
 * next keyframe). MediaPool uses it for playback-start jumps and the
 * prefetch loop, so the first frames after a jump could be wrong.
 *
 * Also covers the catch-up decode (idea from FilmCraft's GOP cache): frames
 * nothing references are skipped on the way to the target, which must not
 * change the target picture.
 *
 * The fixture encodes each frame's index as 7 black/white luma stripes.
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
}
#endif

using namespace rt;

namespace {

constexpr int kW = 64, kH = 64, kFps = 30, kFrames = 120, kBits = 7, kGop = 45;

#ifdef ROUNDTABLE_HAS_FFMPEG
/// 64x64 MPEG-4 mp4: GOP 45 with 2 B-frames; frame i shows i in binary.
bool writeIndexedVideo(const std::string& path)
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
#endif

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

class VideoDecoderSeekTest : public ::testing::TestWithParam<int>
{
protected:
    void SetUp() override
    {
#ifndef ROUNDTABLE_HAS_FFMPEG
        GTEST_SKIP() << "built without FFmpeg";
#else
        // ctest runs every test as its own process in parallel (-j8), so the
        // file must be unique per test, not just per thread count.
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = std::string(info->test_suite_name()) + "_" + info->name();
        for (char& c : name)
            if (c == '/') c = '_';
        m_path = (std::filesystem::temp_directory_path()
                  / ("rt_decoder_seek_" + name + ".mp4")).string();
        if (!writeIndexedVideo(m_path)) GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
        // Param = decoder threads (1 = no frame threading, 4 = frame threads
        // that hold pictures back — the case that exposes ordering bugs).
        ASSERT_TRUE(m_decoder.open(m_path, /*forceSoftware=*/true, GetParam()));
#endif
    }

    void TearDown() override
    {
        m_decoder.close();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    /// Index of the frame decodeNext() returns after a precise seek.
    int preciseSeek(int frame)
    {
        if (!m_decoder.seek((frame + 0.25) / kFps, SeekMode::Precise)) return -2;
        DecodedFrame decoded;
        if (!m_decoder.decodeNext(decoded)) return -3;
        return frameIndexOf(decoded);
    }

    int next()
    {
        DecodedFrame decoded;
        return m_decoder.decodeNext(decoded) ? frameIndexOf(decoded) : -3;
    }

    std::string  m_path;
    VideoDecoder m_decoder;
};

} // namespace

TEST_P(VideoDecoderSeekTest, PreciseSeekReturnsTheRequestedFrame)
{
    for (int target : {40, 62, 0, 44, 45, 46, 89, 7, 100, 119})
        EXPECT_EQ(preciseSeek(target), target) << "target " << target;
}

TEST_P(VideoDecoderSeekTest, DecodingContinuesFromTheTarget)
{
    // After landing, the decoder keeps its references: the following
    // frames come out in order (no skip to the next keyframe, no garbage).
    ASSERT_EQ(preciseSeek(40), 40);
    for (int expected = 41; expected <= 52; ++expected)   // across the GOP at 45
        EXPECT_EQ(next(), expected);
    ASSERT_EQ(preciseSeek(10), 10);                        // backward
    for (int expected = 11; expected <= 14; ++expected)
        EXPECT_EQ(next(), expected);
}

TEST_P(VideoDecoderSeekTest, AdvanceToSkipsForwardWithoutSeeking)
{
    // Scrub steps and prefetch catch-up jump forward without a seek;
    // pictures in between are skipped, the target is exact.
    ASSERT_EQ(preciseSeek(5), 5);
    const auto advance = [&](int frame) {
        if (!m_decoder.advanceTo((frame + 0.25) / kFps)) return -2;
        return next();
    };
    EXPECT_EQ(advance(17), 17);
    EXPECT_EQ(next(), 18);
    EXPECT_EQ(advance(44), 44);   // just before the keyframe at 45
    EXPECT_EQ(advance(70), 70);   // across it
    EXPECT_EQ(next(), 71);
    // Straight after a precise seek, before decodeNext: the parked target.
    ASSERT_TRUE(m_decoder.seek((90 + 0.25) / kFps, SeekMode::Precise));
    EXPECT_EQ(advance(90), 90);
    EXPECT_EQ(advance(100), 100);
}

INSTANTIATE_TEST_SUITE_P(Threads, VideoDecoderSeekTest, ::testing::Values(1, 4));
