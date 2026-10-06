/*
 * test_audio_mix_stream.cpp — block-wise audio export mixing.
 *
 * AudioMixStream mixes a block at a time and AudioRegionReader reads a
 * source region a block at a time. Both must give exactly the same samples
 * as the one-shot paths whatever the block size: these tests mix and read with
 * several block sizes, including ones that cut through fades, transitions
 * and resampler/decoder frame boundaries.
 */

#include <gtest/gtest.h>

#include "AudioMixdown.h"

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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
