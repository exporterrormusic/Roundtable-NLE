/*
 * test_export_decode.cpp — the export path of MediaPool::getFrame
 * (forceExact) returns exactly the requested source frame through the
 * patterns an export produces:
 *   - in-order frames with repeats (a 24p clip in a 30p sequence, slow
 *     motion) — a repeat is answered without decoding again;
 *   - forward skips (a fast clip);
 *   - backward jumps (a clip re-used later, a reversed clip), which reopen
 *     the frame-threaded export decoder instead of flushing it, and after
 *     enough of them fall back to slice threads.
 *
 * The fixture is a software-decoded MPEG-4 file, so the export decoder runs
 * on frame threads. Each frame's index is drawn as 7 black/white stripes.
 */
#include <gtest/gtest.h>

#include <cache/FrameCache.h>
#include <playback/MediaPool.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

using namespace rt;

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

constexpr int kW = 128, kH = 64, kFps = 30, kFrames = 150, kBits = 8, kGop = 45;

/// 128x64 MPEG-4 mp4: GOP 45 with 2 B-frames; frame i shows i in binary.
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
int frameIndexOf(const CachedFrame* frame)
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

class ExportDecodeTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = std::filesystem::temp_directory_path() /
            (std::string("rt_export_decode_") + info->name() + ".mp4");
        if (!writeIndexedVideo(m_path.string()))
            GTEST_SKIP() << "no MPEG-4 encoder in this FFmpeg";
        m_cache = std::make_shared<FrameCache>(256ull * 1024 * 1024);
        m_pool = std::make_unique<MediaPool>(m_cache);
        m_handle = m_pool->open(m_path);
        ASSERT_NE(m_handle, InvalidMedia);
    }

    void TearDown() override
    {
        m_pool.reset();
        std::error_code ec;
        std::filesystem::remove(m_path, ec);
    }

    std::shared_ptr<CachedFrame> exact(int frame)
    {
        return m_pool->getFrame(m_handle, frame, ResolutionTier::Full,
                                /*scrubMode=*/true, /*forceExact=*/true);
    }

    std::filesystem::path       m_path;
    std::shared_ptr<FrameCache> m_cache;
    std::unique_ptr<MediaPool>  m_pool;
    MediaHandle                m_handle{InvalidMedia};
};

} // namespace

TEST_F(ExportDecodeTest, InOrderWithRepeatsAndSkips)
{
    // 24p-in-30p cadence (every 4th source frame shown twice), then a 2x
    // stretch across the keyframe at 90.
    std::vector<int> sequence;
    for (int i = 0; i < 60; ++i) {
        sequence.push_back(i);
        if (i % 4 == 3) sequence.push_back(i);
    }
    for (int i = 60; i < 140; i += 2) sequence.push_back(i);

    std::shared_ptr<CachedFrame> previous;
    for (int target : sequence) {
        auto frame = exact(target);
        ASSERT_TRUE(frame) << "frame " << target;
        EXPECT_EQ(frameIndexOf(frame.get()), target);
        if (previous && previous->frameNumber == target)
            EXPECT_EQ(frame.get(), previous.get()) << "repeat " << target << " decoded again";
        previous = frame;
    }
}

TEST_F(ExportDecodeTest, BackwardJumpsLandExactly)
{
    // A clip re-used later in the edit, then a reversed run — well past the
    // number of reopens after which the decoder goes slice-threaded.
    for (int target : {10, 11, 12, 100, 101, 40, 41, 42, 130, 5})
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "frame " << target;
    for (int target = 120; target >= 100; --target)
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "reverse " << target;
    for (int target = 101; target < 110; ++target)
        EXPECT_EQ(frameIndexOf(exact(target).get()), target) << "forward again " << target;
}

TEST_F(ExportDecodeTest, ReleasedDecodersReopen)
{
    EXPECT_EQ(frameIndexOf(exact(30).get()), 30);
    m_pool->releaseExportDecoders();
    EXPECT_EQ(frameIndexOf(exact(31).get()), 31);
    EXPECT_EQ(frameIndexOf(exact(32).get()), 32);
}


TEST_F(ExportDecodeTest, ExactPlaybackHitTopsUpDecodeAhead)
{
    // Steady playback reads exact cache hits. Those used to schedule
    // nothing, so the decode-ahead window drained and was refilled only
    // after a frame had already missed. A hit with a gap ahead must queue
    // decode-ahead on its own.
    m_pool->setProjectFps(kFps);
    auto seeded = std::make_shared<CachedFrame>();
    seeded->mediaId     = m_handle;
    seeded->frameNumber = 100;
    seeded->tier        = ResolutionTier::Full;
    seeded->width       = kW;
    seeded->height      = kH;
    seeded->stride      = kW * 4;
    seeded->pixels.assign(static_cast<size_t>(kW) * kH * 4, 0);
    m_cache->put(seeded);
    ASSERT_FALSE(m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full));

    ASSERT_EQ(m_pool->tryGetFrame(m_handle, 100, ResolutionTier::Full), seeded);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(m_pool->isFrameCached(m_handle, 106, ResolutionTier::Full));
}

#endif
