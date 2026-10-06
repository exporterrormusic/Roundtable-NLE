/*
 * test_audio_mix_stream.cpp — block-wise audio export mixing.
 *
 * AudioMixStream mixes a block at a time, AudioRegionReader reads a source
 * region a block at a time and AudioFileWriter writes a file a block at a
 * time. Each must give exactly the same output as the one-shot path
 * whatever the block size: these tests use several block sizes, including
 * ones that cut through fades, transitions and resampler, decoder and
 * encoder frame boundaries.
 */

#include <gtest/gtest.h>

#include "AudioMixdown.h"
#include "Encoder.h"
#include "Muxer.h"

#include "audio/AudioFile.h"
#include "audio/AudioRegionReader.h"
#include "audiofx/Dynamics.h"
#include "audiofx/FxChain.h"
#include "audiofx/ParametricEQ.h"
#include "effects/Effect.h"
#include "effects/EffectStack.h"
#include "timeline/AudioClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "Constants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}
#endif

using namespace rt;

namespace {

namespace fs = std::filesystem;

/// Per-test temp directory (ctest runs tests in parallel processes).
class TempDir
{
public:
    TempDir()
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        m_path = fs::temp_directory_path() /
                 (std::string("rt_mixstream_") + info->test_suite_name() + "_" + info->name());
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    [[nodiscard]] const fs::path& path() const noexcept { return m_path; }

private:
    fs::path m_path;
};

/// A 32-bit float WAV of two tones plus a little noise, different per
/// channel, so resampling and filters all have something to chew on.
fs::path writeTestWav(const fs::path& path, uint32_t rate, uint16_t channels,
                      double seconds, uint32_t seed)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return {};

    const auto frames = static_cast<uint32_t>(seconds * rate);
    const uint32_t dataSize = frames * channels * 4;
    auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); w32(36 + dataSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3 /* IEEE float */); w16(channels);
    w32(rate); w32(rate * channels * 4); w16(static_cast<uint16_t>(channels * 4)); w16(32);
    f.write("data", 4); w32(dataSize);

    uint32_t lcg = seed;
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        for (uint16_t c = 0; c < channels; ++c) {
            lcg = lcg * 1664525u + 1013904223u;
            const float noise = (static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f) * 0.05f;
            const float v = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * (220.0 + 55.0 * c) * t)) +
                            0.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1375.0 * t)) + noise;
            f.write(reinterpret_cast<const char*>(&v), 4);
        }
    }
    return f ? path : fs::path{};
}

AudioClip* addAudioClip(Track* track, const fs::path& media, uint32_t rate, uint16_t channels,
                        double startSec, double durationSec, double sourceInSec,
                        double sourceSeconds)
{
    auto clip = std::make_unique<AudioClip>();
    clip->setMediaPath(media.string());
    clip->setSampleRate(rate);
    clip->setChannels(channels);
    clip->setTimelineIn(secondsToTicks(startSec));
    clip->setDuration(secondsToTicks(durationSec));
    clip->setSourceIn(secondsToTicks(sourceInSec));
    clip->setSourceDuration(secondsToTicks(sourceSeconds));
    AudioClip* raw = clip.get();
    track->addClip(std::move(clip));
    return raw;
}

/// Three audio tracks: fades, a cross-dissolve, EQ + dynamics, a channel
/// fill, pan/volume, 44.1 kHz sources in a 48 kHz mix, and a muted track.
struct MixFixture
{
    std::unique_ptr<Timeline> timeline = std::make_unique<Timeline>();
    Track* mutedTrack{nullptr};
    bool ok{false};

    explicit MixFixture(const fs::path& dir)
    {
        const auto stereo48 = writeTestWav(dir / "stereo48.wav", 48000, 2, 3.0, 1);
        const auto mono44   = writeTestWav(dir / "mono44.wav", 44100, 1, 3.0, 2);
        const auto stereo44 = writeTestWav(dir / "stereo44.wav", 44100, 2, 3.0, 3);
        if (stereo48.empty() || mono44.empty() || stereo44.empty()) return;

        Track* a1 = timeline->addAudioTrack("A1");
        Track* a2 = timeline->addAudioTrack("A2");
        Track* a3 = timeline->addAudioTrack("A3");
        if (!a1 || !a2 || !a3) return;

        AudioClip* c1 = addAudioClip(a1, stereo48, 48000, 2, 0.0, 1.5, 0.0, 3.0);
        c1->setFadeInDuration(secondsToTicks(0.2));
        c1->pan().setDefaultValue(-0.3f);
        c1->volume().setDefaultValue(0.8f);
        c1->effects().addEffect(createEffect(EffectType::FillLeftWithRight));
        static_cast<audiofx::ParametricEQ*>(
            c1->audioFx().add(audiofx::ProcessorKind::ParametricEQ))->loadVoicePreset();
        static_cast<audiofx::Dynamics*>(
            c1->audioFx().add(audiofx::ProcessorKind::Dynamics))->loadVoicePreset();

        AudioClip* c2 = addAudioClip(a1, mono44, 44100, 1, 1.5, 1.5, 0.25, 3.0);

        Transition dissolve;
        dissolve.type          = TransitionType::CrossDissolve;
        dissolve.duration      = secondsToTicks(0.4);
        dissolve.leftClipId    = c1->id();
        dissolve.rightClipId   = c2->id();
        dissolve.editPointTick = secondsToTicks(1.5);
        a1->addTransition(dissolve);

        AudioClip* c3 = addAudioClip(a2, stereo44, 44100, 2, 0.7, 1.9, 0.1, 3.0);
        c3->setFadeOutDuration(secondsToTicks(0.3));
        c3->pan().setDefaultValue(0.5f);
        c3->volume().setDefaultValue(1.2f);

        addAudioClip(a3, stereo48, 48000, 2, 0.0, 3.0, 0.0, 3.0);
        a3->setMuted(true);
        mutedTrack = a3;
        ok = true;
    }
};

float rms(const std::vector<float>& s)
{
    if (s.empty()) return 0.0f;
    double acc = 0.0;
    for (float v : s) acc += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(acc / s.size()));
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return INFINITY;
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

std::vector<float> streamAll(AudioMixStream& stream, int64_t blockFrames)
{
    std::vector<float> out(static_cast<size_t>(stream.totalFrames()) * stream.channels());
    int64_t pos = 0;
    while (pos < stream.totalFrames()) {
        const int64_t got = stream.read(out.data() + static_cast<size_t>(pos) * stream.channels(),
                                        std::min(blockFrames, stream.totalFrames() - pos));
        if (got <= 0) break;
        pos += got;
    }
    out.resize(static_cast<size_t>(pos) * stream.channels());
    return out;
}

std::vector<float> readerAll(AudioFile& file, int64_t start, int64_t frames, uint32_t rate,
                             int64_t blockFrames)
{
    AudioRegionReader reader;
    std::vector<float> all, block;
    if (!reader.begin(file, start, frames, rate)) return all;
    while (reader.read(blockFrames, block) > 0) all.insert(all.end(), block.begin(), block.end());
    return all;
}

} // namespace

// ── AudioRegionReader ───────────────────────────────────────────────────────

TEST(AudioRegionReader, SameRateMatchesOneShotRead)
{
    TempDir dir;
    const auto wav = writeTestWav(dir.path() / "a.wav", 48000, 2, 2.0, 7);
    AudioFile file;
    ASSERT_TRUE(file.open(wav));

    std::vector<float> expected;
    ASSERT_GT(file.readRegionResampled(1234, 50000, 48000, expected), 0);
    for (int64_t block : {1, 997, 4096, 200000}) {
        EXPECT_EQ(readerAll(file, 1234, 50000, 48000, block), expected) << "block " << block;
    }
}

TEST(AudioRegionReader, ResampledMatchesOneShotRead)
{
    TempDir dir;
    const auto wav = writeTestWav(dir.path() / "a.wav", 44100, 2, 2.0, 8);
    AudioFile file;
    ASSERT_TRUE(file.open(wav));

    // A region that starts mid-file and one that runs past the end.
    for (const auto [start, frames] : {std::pair<int64_t, int64_t>{1234, 50000},
                                       std::pair<int64_t, int64_t>{60000, 90000}}) {
        std::vector<float> expected;
        ASSERT_GT(file.readRegionResampled(start, frames, 48000, expected), 0);
        for (int64_t block : {1, 997, 4096, 200000}) {
            EXPECT_EQ(readerAll(file, start, frames, 48000, block), expected)
                << "start " << start << " block " << block;
        }
    }
}

TEST(AudioRegionReader, SequentialCompressedReadsDecodeContinuously)
{
    // AAC decodes with an inter-frame overlap: a seek per read would put a
    // priming transient at every boundary. Sequential reads must continue
    // the decode and so match one long read exactly.
    TempDir dir;
    MixdownResult tone;
    tone.sampleRate = 48000;
    tone.channels = 2;
    tone.totalFrames = 48000 * 2;
    tone.duration = 2.0;
    tone.samples.resize(static_cast<size_t>(tone.totalFrames) * 2);
    for (int64_t i = 0; i < tone.totalFrames; ++i) {
        const float v = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0));
        tone.samples[static_cast<size_t>(i) * 2] = v;
        tone.samples[static_cast<size_t>(i) * 2 + 1] = -v;
    }
    const auto m4a = dir.path() / "tone.m4a";
    if (!AudioMixdown::writeAudioFile(tone, m4a, AudioCodec::AAC, 192000))
        GTEST_SKIP() << "AAC encoder unavailable in this FFmpeg build";

    AudioFile oneShot;
    ASSERT_TRUE(oneShot.open(m4a));
    ASSERT_EQ(oneShot.backend(), AudioBackend::FFmpeg);
    std::vector<float> expected;
    const int64_t frames = oneShot.readRegion(1000, 80000, expected);
    ASSERT_EQ(frames, 80000);

    AudioFile sequential;
    ASSERT_TRUE(sequential.open(m4a));
    std::vector<float> got, block;
    for (int64_t pos = 1000; pos < 81000; pos += 3001) {
        const int64_t n = std::min<int64_t>(3001, 81000 - pos);
        ASSERT_EQ(sequential.readRegion(pos, n, block), n) << "at " << pos;
        got.insert(got.end(), block.begin(), block.end());
    }
    EXPECT_EQ(got, expected);

    // A non-sequential read afterwards seeks instead of continuing. An AAC
    // seek has no decoder pre-roll, so only compare once a few frames have
    // settled the overlap: a stale continuation would be off by ~40000 frames.
    AudioFile fresh;
    ASSERT_TRUE(fresh.open(m4a));
    std::vector<float> again, freshRead;
    ASSERT_EQ(sequential.readRegion(40000, 8192, again), 8192);
    ASSERT_EQ(fresh.readRegion(40000, 8192, freshRead), 8192);
    const std::vector<float> settledAgain(again.begin() + 4096 * 2, again.end());
    const std::vector<float> settledFresh(freshRead.begin() + 4096 * 2, freshRead.end());
    EXPECT_LE(maxAbsDiff(settledAgain, settledFresh), 1e-3f);
}

TEST(AudioFileStreaming, CompressedReadAllStreamingMatchesReadAll)
{
    TempDir dir;
    MixdownResult tone;
    tone.sampleRate = 48000;
    tone.channels = 2;
    tone.totalFrames = 48000 * 2 + 333;
    tone.duration = static_cast<double>(tone.totalFrames) / 48000.0;
    tone.samples.resize(static_cast<size_t>(tone.totalFrames) * 2);
    for (size_t i = 0; i < tone.samples.size(); ++i)
        tone.samples[i] = 0.3f * static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
    const auto m4a = dir.path() / "tone.m4a";
    if (!AudioMixdown::writeAudioFile(tone, m4a, AudioCodec::AAC, 128000))
        GTEST_SKIP() << "AAC encoder unavailable in this FFmpeg build";

    // Fresh files for both: an AAC seek back to the start does not decode
    // exactly like a fresh open (no decoder pre-roll).
    AudioFile whole, streaming;
    ASSERT_TRUE(whole.open(m4a));
    ASSERT_TRUE(streaming.open(m4a));
    ASSERT_EQ(streaming.backend(), AudioBackend::FFmpeg);
    const auto all = whole.readAll();
    ASSERT_FALSE(all.empty());

    std::vector<float> streamed;
    const int64_t frames = streaming.readAllStreaming([&](const float* samples, int64_t n) {
        streamed.insert(streamed.end(), samples, samples + n * 2);
        return true;
    });
    EXPECT_EQ(frames, static_cast<int64_t>(all.size() / 2));
    EXPECT_EQ(streamed, all);
}

// ── AudioMixStream ──────────────────────────────────────────────────────────

TEST(AudioMixStream, BlockSizeDoesNotChangeTheMix)
{
    TempDir dir;
    MixFixture fx(dir.path());
    ASSERT_TRUE(fx.ok);

    for (const auto [startSec, endSec] : {std::pair<double, double>{0.0, 0.0},
                                          std::pair<double, double>{0.9, 2.2}}) {
        AudioMixdownConfig cfg;
        cfg.sampleRate = 48000;
        cfg.channels = 2;
        cfg.startTime = startSec;
        cfg.endTime = endSec;
        cfg.masterVolume = 0.9f;

        AudioMixdown mixdown;
        const MixdownResult whole = mixdown.mix(*fx.timeline, cfg);
        ASSERT_TRUE(whole.isValid());
        if (rms(whole.samples) < 1e-3f) GTEST_SKIP() << "No audio backend available to read WAV";

        for (int64_t block : {1, 7, 997, 4096, 1 << 20}) {
            AudioMixStream stream(*fx.timeline, cfg);
            ASSERT_TRUE(stream.isValid());
            EXPECT_EQ(streamAll(stream, block), whole.samples)
                << "range " << startSec << "-" << endSec << " block " << block;
        }
    }
}

TEST(AudioMixStream, MutedTrackAndChannelCountsHonored)
{
    TempDir dir;
    MixFixture fx(dir.path());
    ASSERT_TRUE(fx.ok);

    AudioMixdownConfig cfg;
    cfg.sampleRate = 48000;
    cfg.channels = 2;
    AudioMixStream stream(*fx.timeline, cfg);
    ASSERT_TRUE(stream.isValid());
    EXPECT_EQ(stream.audioTrackCount(), 3u);
    EXPECT_EQ(stream.totalFrames(), static_cast<int64_t>(3.0 * 48000));

    // Unmuting the third track (a full-length clip) changes the mix.
    const auto withMute = streamAll(stream, 4096);
    fx.mutedTrack->setMuted(false);
    AudioMixStream unmuted(*fx.timeline, cfg);
    const auto withoutMute = streamAll(unmuted, 4096);
    if (rms(withMute) < 1e-3f) GTEST_SKIP() << "No audio backend available to read WAV";
    EXPECT_GT(maxAbsDiff(withMute, withoutMute), 0.01f);
}

TEST(AudioMixStream, TimelineMayGoAwayAfterConstruction)
{
    TempDir dir;
    MixFixture fx(dir.path());
    ASSERT_TRUE(fx.ok);

    AudioMixdownConfig cfg;
    AudioMixdown mixdown;
    const MixdownResult whole = mixdown.mix(*fx.timeline, cfg);
    ASSERT_TRUE(whole.isValid());

    AudioMixStream stream(*fx.timeline, cfg);
    fx.timeline.reset();
    EXPECT_EQ(streamAll(stream, 4096), whole.samples);
}

TEST(AudioMixStream, EmptyTimelineIsInvalid)
{
    Timeline timeline;
    AudioMixStream stream(timeline, {});
    EXPECT_FALSE(stream.isValid());
    EXPECT_FALSE(stream.lastError().empty());
    std::vector<float> out(64);
    EXPECT_EQ(stream.read(out.data(), 32), 0);
}

// ── AudioFileWriter ─────────────────────────────────────────────────────────

namespace {

MixdownResult testSignal(double seconds, uint32_t rate, uint16_t channels)
{
    MixdownResult mix;
    mix.sampleRate = rate;
    mix.channels = channels;
    mix.totalFrames = static_cast<int64_t>(seconds * rate);
    mix.duration = seconds;
    mix.samples.resize(static_cast<size_t>(mix.totalFrames) * channels);
    uint32_t lcg = 99;
    for (size_t i = 0; i < mix.samples.size(); ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const double t = static_cast<double>(i / channels) / rate;
        // Peaks past +-1 exercise the PCM-16 clamp.
        mix.samples[i] = 1.2f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 330.0 * t)) +
                         (static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f) * 0.1f;
    }
    return mix;
}

std::vector<char> fileBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(AudioFileWriter, BlockSizesWriteIdenticalFiles)
{
    TempDir dir;
    const MixdownResult mix = testSignal(1.3, 48000, 2);
    for (const AudioCodec codec : {AudioCodec::PCM_S16LE, AudioCodec::PCM_F32LE,
                                   AudioCodec::FLAC, AudioCodec::MP3, AudioCodec::AAC}) {
        const std::string ext = audioCodecExtension(codec);
        const auto whole = dir.path() / ("whole" + ext);
        if (!AudioMixdown::writeAudioFile(mix, whole, codec, 160000)) {
            ADD_FAILURE() << "codec " << static_cast<int>(codec) << " unavailable";
            continue;
        }
        for (int64_t block : {1, 997, 4096}) {
            const auto streamed = dir.path() / ("streamed" + std::to_string(block) + ext);
            AudioFileWriter writer;
            ASSERT_TRUE(writer.open(streamed, codec, 48000, 2, 160000));
            for (int64_t pos = 0; pos < mix.totalFrames; pos += block) {
                const int64_t n = std::min(block, mix.totalFrames - pos);
                ASSERT_TRUE(writer.write(mix.samples.data() + static_cast<size_t>(pos) * 2, n));
            }
            EXPECT_EQ(writer.framesWritten(), mix.totalFrames);
            ASSERT_TRUE(writer.finish());
            EXPECT_EQ(fileBytes(streamed), fileBytes(whole))
                << "codec " << static_cast<int>(codec) << " block " << block;
        }
    }
}

TEST(AudioFileWriter, WavHeaderSizesAreFilledIn)
{
    TempDir dir;
    const MixdownResult mix = testSignal(0.5, 44100, 1);
    const auto path = dir.path() / "mono.wav";
    ASSERT_TRUE(AudioMixdown::writeWav(mix, path));

    const auto bytes = fileBytes(path);
    ASSERT_EQ(bytes.size(), 44u + static_cast<size_t>(mix.totalFrames) * 2);
    uint32_t riffSize = 0, dataSize = 0;
    std::memcpy(&riffSize, bytes.data() + 4, 4);
    std::memcpy(&dataSize, bytes.data() + 40, 4);
    EXPECT_EQ(dataSize, static_cast<uint32_t>(mix.totalFrames * 2));
    EXPECT_EQ(riffSize, 36u + dataSize);

    AudioFile file;
    ASSERT_TRUE(file.open(path));
    EXPECT_EQ(file.info().frames, mix.totalFrames);
    EXPECT_EQ(file.info().sampleRate, 44100u);
}

TEST(AudioFileWriter, UnfinishedWriterCanBeDiscarded)
{
    TempDir dir;
    const auto path = dir.path() / "partial.flac";
    {
        AudioFileWriter writer;
        if (!writer.open(path, AudioCodec::FLAC, 48000, 2))
            GTEST_SKIP() << "FLAC encoder unavailable";
        const MixdownResult mix = testSignal(0.2, 48000, 2);
        ASSERT_TRUE(writer.write(mix.samples.data(), mix.totalFrames));
    }   // destroyed without finish(): the file must be closed, so removable
    std::error_code ec;
    EXPECT_TRUE(fs::remove(path, ec)) << ec.message();
}

// ── Muxer: audio pulled while the video is written ──────────────────────────

#ifdef ROUNDTABLE_HAS_FFMPEG
namespace {

/// A few tiny MPEG-4 Part 2 frames, so the muxer has real video to write.
struct TinyVideo
{
    AVCodecContext* ctx{nullptr};
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<EncodedPacket> packets;

    explicit TinyVideo(int frames)
    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        if (!codec) return;
        ctx = avcodec_alloc_context3(codec);
        ctx->width = 64;
        ctx->height = 64;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->time_base = {1, 30};
        ctx->framerate = {30, 1};
        ctx->gop_size = 10;
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return;
        }
        AVFrame* frame = av_frame_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = 64;
        frame->height = 64;
        av_frame_get_buffer(frame, 0);
        AVPacket* pkt = av_packet_alloc();
        auto collect = [&]() {
            while (avcodec_receive_packet(ctx, pkt) == 0) {
                payloads.emplace_back(pkt->data, pkt->data + pkt->size);
                EncodedPacket ep;
                ep.size = pkt->size;
                ep.pts = pkt->pts;
                ep.dts = pkt->dts;
                ep.duration = 1;
                ep.isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                packets.push_back(ep);
                av_packet_unref(pkt);
            }
        };
        for (int i = 0; i < frames; ++i) {
            av_frame_make_writable(frame);
            for (int p = 0; p < 3; ++p) {
                const int h = p == 0 ? 64 : 32;
                for (int y = 0; y < h; ++y)
                    std::memset(frame->data[p] + y * frame->linesize[p], (i * 7 + p * 40) & 0xFF,
                                static_cast<size_t>(p == 0 ? 64 : 32));
            }
            frame->pts = i;
            avcodec_send_frame(ctx, frame);
            collect();
        }
        avcodec_send_frame(ctx, nullptr);
        collect();
        for (size_t i = 0; i < packets.size(); ++i) packets[i].data = payloads[i].data();
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }
    ~TinyVideo() { if (ctx) avcodec_free_context(&ctx); }
};

} // namespace

TEST(MuxerAudioPull, PulledAudioMatchesDirectEncodeAndInterleaves)
{
    TempDir dir;
    // Longer than FFmpeg's 10 s interleave window, so writing all the video
    // before the audio would show up in the file layout.
    TinyVideo video(450);   // 15 s at 30 fps
    if (!video.ctx || video.packets.empty()) GTEST_SKIP() << "MPEG-4 encoder unavailable";

    const MixdownResult mix = testSignal(15.0, 48000, 2);

    // Reference: the same samples through the standalone AAC writer.
    const auto reference = dir.path() / "reference.m4a";
    ASSERT_TRUE(AudioMixdown::writeAudioFile(mix, reference, AudioCodec::AAC, 160000));

    MuxerConfig cfg;
    cfg.format = ContainerFormat::MP4;
    cfg.videoWidth = 64;
    cfg.videoHeight = 64;
    cfg.videoFpsNum = 30;
    cfg.videoFpsDen = 1;
    cfg.videoCodecId = AV_CODEC_ID_MPEG4;
    cfg.videoCodecContext = video.ctx;
    cfg.audioSampleRate = 48000;
    cfg.audioChannels = 2;
    cfg.audioBitrate = 160000;

    // Pull in small, odd blocks: none lines up with an AAC frame.
    int64_t pos = 0;
    MuxAudioSource source;
    source.sampleRate = 48000;
    source.channels = 2;
    source.pull = [&](float* out, int64_t maxFrames) {
        const int64_t n = std::min({maxFrames, int64_t(777), mix.totalFrames - pos});
        std::copy_n(mix.samples.data() + static_cast<size_t>(pos) * 2, n * 2, out);
        pos += n;
        return n;
    };
    const auto muxed = dir.path() / "muxed.mp4";
    ASSERT_TRUE(Muxer::muxFile(muxed, video.packets, &source, cfg));
    EXPECT_EQ(pos, mix.totalFrames);

    // Same AAC frames → the same decoded audio.
    AudioFile fromMux, fromWriter;
    ASSERT_TRUE(fromMux.open(muxed));
    ASSERT_TRUE(fromWriter.open(reference));
    const auto muxAudio = fromMux.readAll();
    const auto refAudio = fromWriter.readAll();
    ASSERT_FALSE(refAudio.empty());
    EXPECT_EQ(muxAudio, refAudio);

    // Interleaved: in file order, well under a second of video comes
    // before the first audio packet.
    AVFormatContext* fmt = nullptr;
    ASSERT_EQ(avformat_open_input(&fmt, muxed.string().c_str(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(fmt, nullptr), 0);
    struct Placed { int64_t pos; bool audio; };
    std::vector<Placed> order;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        const auto type = fmt->streams[pkt->stream_index]->codecpar->codec_type;
        order.push_back({pkt->pos, type == AVMEDIA_TYPE_AUDIO});
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    std::sort(order.begin(), order.end(),
              [](const Placed& a, const Placed& b) { return a.pos < b.pos; });
    const auto firstAudio = std::find_if(order.begin(), order.end(),
                                         [](const Placed& p) { return p.audio; });
    ASSERT_NE(firstAudio, order.end());
    EXPECT_LT(std::distance(order.begin(), firstAudio), 30);
}

TEST(MuxerAudioPull, NoAudioSourceWritesVideoOnly)
{
    TempDir dir;
    TinyVideo video(10);
    if (!video.ctx || video.packets.empty()) GTEST_SKIP() << "MPEG-4 encoder unavailable";

    MuxerConfig cfg;
    cfg.format = ContainerFormat::MP4;
    cfg.videoWidth = 64;
    cfg.videoHeight = 64;
    cfg.videoCodecId = AV_CODEC_ID_MPEG4;
    cfg.videoCodecContext = video.ctx;
    const auto muxed = dir.path() / "video_only.mp4";
    ASSERT_TRUE(Muxer::muxFile(muxed, video.packets, nullptr, cfg));

    AVFormatContext* fmt = nullptr;
    ASSERT_EQ(avformat_open_input(&fmt, muxed.string().c_str(), nullptr, nullptr), 0);
    EXPECT_EQ(fmt->nb_streams, 1u);
    avformat_close_input(&fmt);
}
#endif // ROUNDTABLE_HAS_FFMPEG
