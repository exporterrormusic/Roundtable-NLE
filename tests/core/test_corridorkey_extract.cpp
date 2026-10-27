/*
 * test_corridorkey_extract.cpp — CorridorKey's frame extraction gives the
 * render, frame for frame, what the app shows (ai/CorridorKey.h extractArgs).
 *
 * The truth is the app's own VideoDecoder: source frame f is the picture it
 * returns after seekToFrame(f). Files often start a frame or two in (B-frame
 * delay), and then app frame f is NOT the f-th picture of the file — that is
 * what this guards. The fixture shows each picture's index as 7 luma stripes
 * (as test_video_decoder_seek does) at 29.97 fps with B-frames, its first
 * picture 0, 1 or 2 frames in; the bundled ffmpeg runs extractArgs; render
 * frame k must be the app's source frame first + k.
 */
#include <gtest/gtest.h>

#include "AppPaths.h"
#include "PathUtils.h"
#include "ai/CorridorKey.h"
#include "decode/VideoDecoder.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

#ifdef _WIN32
#include <process.h>
#endif

using namespace rt;
namespace ck = rt::corridorkey;
namespace fs = std::filesystem;

namespace {

constexpr int kW = 64, kH = 64, kFrames = 120, kBits = 7, kGop = 30;

#ifdef ROUNDTABLE_HAS_FFMPEG
/// 64x64 MPEG-4 mp4 at 30000/1001 with 2 B-frames; picture i shows i in
/// binary and is stamped at pts i + offset.
bool writeIndexedVideo(const std::string& path, int offset)
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
    enc->time_base = AVRational{1001, 30000};
    enc->framerate = AVRational{30000, 1001};
    enc->gop_size = kGop;
    enc->max_b_frames = 2;
    enc->bit_rate = 2'000'000;
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    bool ok = avcodec_open2(enc, codec, nullptr) >= 0 &&
              avcodec_parameters_from_context(st->codecpar, enc) >= 0;
    st->time_base = enc->time_base;
    st->avg_frame_rate = enc->framerate;
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
        frame->pts = i + offset;
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

int indexFromRow(const uint8_t* row)
{
    const int stripe = kW / kBits;
    int index = 0;
    for (int bit = 0; bit < kBits; ++bit)
        if (row[bit * stripe + stripe / 2] > 128) index |= (1 << bit);
    return index;
}

bool runFfmpeg(const std::vector<std::string>& args)
{
#ifdef _WIN32
    const std::string exe = pathToUtf8(AppPaths::ffmpegExecutable());
    std::vector<std::string> quoted{"\"" + exe + "\""};
    for (const auto& a : args) quoted.push_back("\"" + a + "\"");
    std::vector<const char*> argv;
    for (const auto& q : quoted) argv.push_back(q.c_str());
    argv.push_back(nullptr);
    return _spawnv(_P_WAIT, exe.c_str(), argv.data()) == 0;
#else
    (void)args;
    return false;
#endif
}

struct Case
{
    int offset;   ///< frames before the file's first picture
    int first;    ///< render range start (app frame)
};

class CorridorKeyExtract : public ::testing::TestWithParam<Case>
{
};

} // namespace

TEST_P(CorridorKeyExtract, RenderFrameKIsTheAppsSourceFrameFirstPlusK)
{
#ifndef ROUNDTABLE_HAS_FFMPEG
    GTEST_SKIP() << "built without FFmpeg";
#else
    if (AppPaths::ffmpegExecutable().empty()) GTEST_SKIP() << "no ffmpeg.exe beside the tests";
    const Case c = GetParam();
    const fs::path dir = fs::temp_directory_path()
        / ("rt_ck_extract_" + std::to_string(c.offset) + "_" + std::to_string(c.first));
    fs::remove_all(dir);
    fs::create_directories(dir / "frames");
    const std::string video = pathToUtf8(dir / "indexed.mp4");
    if (!writeIndexedVideo(video, c.offset)) GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";

    ck::RenderPlan plan;
    plan.sourcePath = video;
    plan.fps = {30000, 1001};
    plan.firstFrame = c.first;
    plan.lastFrame = c.first + 11;
    plan.sourceWidth = kW;
    plan.sourceHeight = kH;
    plan.keyRect = {0, 0, kW, kH};
    plan.matrix = ColorMatrix::BT709;
    ASSERT_TRUE(runFfmpeg(ck::extractArgs(plan, dir / "frames")));
    ASSERT_TRUE(runFfmpeg({"-hide_banner", "-loglevel", "error", "-y", "-i",
                           pathToUtf8(dir / "frames" / "%06d.png"), "-f", "rawvideo",
                           "-pix_fmt", "gray", pathToUtf8(dir / "frames.gray")}));
    std::ifstream in(dir / "frames.gray", std::ios::binary);
    const std::vector<uint8_t> gray((std::istreambuf_iterator<char>(in)), {});
    ASSERT_EQ(gray.size(), size_t(kW * kH) * size_t(plan.frameCount()));

    VideoDecoder decoder;
    ASSERT_TRUE(decoder.open(video, /*forceSoftware=*/true, 1));
    for (int64_t k = 0; k < plan.frameCount(); ++k) {
        ASSERT_TRUE(decoder.seekToFrame(plan.firstFrame + k, SeekMode::Precise));
        DecodedFrame shown;
        ASSERT_TRUE(decoder.decodeNext(shown));
        const int app = indexFromRow(shown.data[0] + (kH / 2) * shown.linesize[0]);
        const int render = indexFromRow(gray.data() + size_t(k) * kW * kH + size_t(kH / 2) * kW);
        EXPECT_EQ(render, app) << "offset " << c.offset << ", app frame " << plan.firstFrame + k;
        // The fixture really starts late: away from the start, app frame f
        // is picture f - offset (else this test would prove nothing).
        if (plan.firstFrame + k >= c.offset + 1)
            EXPECT_EQ(app, plan.firstFrame + k - c.offset) << "fixture offset not in the file";
    }
    decoder.close();
    in.close();
    fs::remove_all(dir);
#endif
}

INSTANTIATE_TEST_SUITE_P(StartOffsets, CorridorKeyExtract,
                         ::testing::Values(Case{0, 0}, Case{0, 2}, Case{0, 45},
                                           Case{1, 0}, Case{1, 2}, Case{1, 45},
                                           Case{2, 0}, Case{2, 30}, Case{2, 61}),
                         [](const auto& info) {
                             return "offset" + std::to_string(info.param.offset) + "_first"
                                    + std::to_string(info.param.first);
                         });
