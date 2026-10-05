/*
 * test_video_frame_grabber.cpp — Audio Sync's VideoFrameGrabber returns the
 * RIGHT frame on a long-GOP, B-frame video: mid-GOP posters, backward
 * seeks, frame-by-frame playback steps, and past-the-end requests.
 *
 * Regression: the grabber used VideoDecoder::decodeAt(), whose precise seek
 * flushes the codec after reaching the target — the next decode then lacked
 * reference frames, so posters came back black and playback crawled at
 * ~1 fps (every tick re-seeked).
 *
 * The fixture video encodes each frame's index as 7 black/white stripes, so
 * the test can read back exactly which frame it got.
 */
#include <gtest/gtest.h>

#include "panels/audio/VideoFrameGrabber.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <optional>
#include <string>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#endif

using namespace rt;

namespace {

constexpr int kW = 64, kH = 64, kFps = 30, kFrames = 90, kBits = 7;

int g_argc = 1;
char g_arg0[] = "test_video_frame_grabber";
char* g_argv[] = {g_arg0, nullptr};

#ifdef ROUNDTABLE_HAS_FFMPEG
/// Writes a 64x64 MPEG-4 mp4: GOP 45 with B-frames; frame i shows i in binary.
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
    enc->gop_size = 45;
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

/// Frame index read back from the stripes (-1 if unreadable).
int frameIndexOf(const QImage& image)
{
    if (image.isNull()) return -1;
    const int stripe = image.width() / kBits;
    int index = 0;
    for (int bit = 0; bit < kBits; ++bit) {
        const QRgb px = image.pixel(bit * stripe + stripe / 2, image.height() / 2);
        if (qGray(px) > 128) index |= (1 << bit);
    }
    return index;
}

class VideoFrameGrabberTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QCoreApplication::instance())
            s_app = new QCoreApplication(g_argc, g_argv);
    }

    void SetUp() override
    {
#ifndef ROUNDTABLE_HAS_FFMPEG
        GTEST_SKIP() << "built without FFmpeg";
#else
        ASSERT_TRUE(m_dir.isValid());
        m_path = (m_dir.path() + "/indexed.mp4").toStdString();
        if (!writeIndexedVideo(m_path)) GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
#endif
    }

    /// Frame index the grabber delivers for `timeSec`.
    int grab(double timeSec, bool live)
    {
        std::optional<QImage> got;
        QEventLoop loop;
        auto cb = [&](const QImage& image) { got = image; loop.quit(); };
        if (live)
            m_grabber.requestLive(&m_receiver, m_path, timeSec, QSize(kW, kH), cb,
                                  VideoFrameGrabber::Playback);
        else
            m_grabber.requestStill(&m_receiver, m_path, timeSec, QSize(kW, kH), cb);
        if (!got) {
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return got ? frameIndexOf(*got) : -2;
    }

    static double at(int frame) { return (frame + 0.25) / kFps; }

    static inline QCoreApplication* s_app = nullptr;
    QTemporaryDir     m_dir;
    std::string       m_path;
    QObject           m_receiver;
    VideoFrameGrabber m_grabber;
};

} // namespace

TEST_F(VideoFrameGrabberTest, MidGopPosterIsTheRequestedFrame)
{
    // Frame 40: 40 frames past the keyframe at 0, with B-frames around it.
    EXPECT_EQ(grab(at(40), /*live=*/false), 40);
    EXPECT_EQ(grab(at(62), /*live=*/false), 62);   // second GOP
}

TEST_F(VideoFrameGrabberTest, PlaybackStepsForwardFrameByFrame)
{
    for (int i = 10; i <= 30; ++i)
        EXPECT_EQ(grab(at(i), /*live=*/true), i) << "playback tick " << i;
}

TEST_F(VideoFrameGrabberTest, BackwardSeekAndScrubReturnTheRightFrame)
{
    EXPECT_EQ(grab(at(70), true), 70);
    EXPECT_EQ(grab(at(12), true), 12);   // backward across a GOP boundary
    EXPECT_EQ(grab(at(50), true), 50);
    EXPECT_EQ(grab(at(48), true), 48);   // small step back (cache or re-seek)
    EXPECT_EQ(grab(at(3), true), 3);
}

TEST_F(VideoFrameGrabberTest, PastTheEndShowsTheLastFrame)
{
    EXPECT_EQ(grab(at(kFrames + 20), true), kFrames - 1);
}
