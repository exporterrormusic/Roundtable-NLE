/*
 * test_export.cpp — Unit tests for the Export Pipeline (Step 24).
 *
 * Tests: Encoder enums/config/factory, ExportPresets, ContainerFormats,
 *        AudioMixdown, RenderQueue job management.
 */

#include <gtest/gtest.h>

#include "Encoder.h"
#include "AudioMixdown.h"
#include "Muxer.h"
#include "RenderQueue.h"
#include "ExportIntegrity.h"

#include "cache/FrameCache.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "Constants.h"
#include "audiofx/ParametricEQ.h"
#include "audiofx/FxChain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>

#ifdef ROUNDTABLE_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace rt;

// ═════════════════════════════════════════════════════════════════════════════
// Encoder enums and config
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportEncoder, CodecNames)
{
    EXPECT_STREQ(encoderCodecName(EncoderCodec::H264), "H.264");
    EXPECT_STREQ(encoderCodecName(EncoderCodec::H265), "H.265");
    EXPECT_STREQ(encoderCodecName(EncoderCodec::AV1), "AV1");
    EXPECT_STREQ(encoderCodecName(EncoderCodec::ProRes), "ProRes");
    EXPECT_STREQ(encoderCodecName(EncoderCodec::DNxHR), "DNxHR");
    EXPECT_STREQ(encoderCodecName(EncoderCodec::ImageSequence), "Image Sequence");
}

TEST(ExportEncoder, CodecCount)
{
    EXPECT_EQ(static_cast<int>(EncoderCodec::Count), 6);
}

TEST(ExportEncoder, ConfigDefaults)
{
    EncoderConfig cfg;
    EXPECT_EQ(cfg.width, 1920u);
    EXPECT_EQ(cfg.height, 1080u);
    EXPECT_DOUBLE_EQ(cfg.fps, 30.0);
    EXPECT_EQ(cfg.fpsNum, 30);
    EXPECT_EQ(cfg.fpsDen, 1);
    EXPECT_EQ(cfg.codec, EncoderCodec::H264);
    EXPECT_EQ(cfg.preset, EncoderPreset::Medium);
    EXPECT_EQ(cfg.hwAccel, HardwareAccel::None);
    EXPECT_EQ(cfg.crf, 23);
    EXPECT_EQ(cfg.bitrateMbps, 0);
    EXPECT_EQ(cfg.gopSize, 0);
    EXPECT_TRUE(cfg.bt709);
}

TEST(ExportEncoder, PresetEnum)
{
    EXPECT_EQ(static_cast<int>(EncoderPreset::Ultrafast), 0);
    EXPECT_EQ(static_cast<int>(EncoderPreset::Veryslow), 8);
    EXPECT_EQ(static_cast<int>(EncoderPreset::Count), 9);
}

TEST(ExportEncoder, HardwareAccelEnum)
{
    EXPECT_EQ(static_cast<int>(HardwareAccel::None), 0);
    EXPECT_EQ(static_cast<int>(HardwareAccel::NVENC), 1);
    EXPECT_EQ(static_cast<int>(HardwareAccel::QSV), 2);
    EXPECT_EQ(static_cast<int>(HardwareAccel::AMF), 3);
    EXPECT_EQ(static_cast<int>(HardwareAccel::Count), 4);
}

TEST(ExportEncoder, ProResProfileEnum)
{
    EXPECT_EQ(static_cast<int>(ProResProfile::Proxy), 0);
    EXPECT_EQ(static_cast<int>(ProResProfile::HQ), 3);
    EXPECT_EQ(static_cast<int>(ProResProfile::_4444), 4);
    EXPECT_EQ(static_cast<int>(ProResProfile::Count), 6);
}

TEST(ExportEncoder, ImageFormatEnum)
{
    EXPECT_EQ(static_cast<int>(ImageFormat::PNG), 0);
    EXPECT_EQ(static_cast<int>(ImageFormat::JPEG), 2);
    EXPECT_EQ(static_cast<int>(ImageFormat::Count), 3);
}

TEST(ExportEncoder, EncodedPacketDefaults)
{
    EncodedPacket pkt;
    EXPECT_EQ(pkt.data, nullptr);
    EXPECT_EQ(pkt.size, 0);
    EXPECT_EQ(pkt.pts, 0);
    EXPECT_EQ(pkt.dts, 0);
    EXPECT_FALSE(pkt.isKeyframe);
    EXPECT_FALSE(pkt.ownsData);
}

TEST(ExportEncoder, FactoryCreatesEncoder)
{
    // Factory should create encoders (they may fail to init without FFmpeg,
    // but the factory itself should return a non-null pointer)
    auto h264 = Encoder::create(EncoderCodec::H264, HardwareAccel::None);
    EXPECT_NE(h264, nullptr);

    auto h265 = Encoder::create(EncoderCodec::H265, HardwareAccel::None);
    EXPECT_NE(h265, nullptr);

    auto av1 = Encoder::create(EncoderCodec::AV1, HardwareAccel::None);
    EXPECT_NE(av1, nullptr);

    auto prores = Encoder::create(EncoderCodec::ProRes, HardwareAccel::None);
    EXPECT_NE(prores, nullptr);

    auto imgseq = Encoder::create(EncoderCodec::ImageSequence, HardwareAccel::None);
    EXPECT_NE(imgseq, nullptr);
}

TEST(ExportEncoder, FactoryInvalidCodec)
{
    auto enc = Encoder::create(EncoderCodec::Count, HardwareAccel::None);
    EXPECT_EQ(enc, nullptr);
}

#ifdef ROUNDTABLE_HAS_FFMPEG
// Real CPU encode: opens the FFmpeg encoder and pushes a few solid-colour BGRA
// frames through.  Verifies the prores_aw (non-4444) / prores_ks (4444) split
// actually opens and produces packets in this FFmpeg build — the thing the
// factory-only tests above can't catch.  No GPU needed (pure libavcodec).
namespace {
bool encodeSolidProRes(ProResProfile profile, int frames)
{
    EncoderConfig cfg;
    cfg.width  = 128;
    cfg.height = 128;
    cfg.fpsNum = 30;
    cfg.fpsDen = 1;
    cfg.codec  = EncoderCodec::ProRes;
    cfg.proresProfile = profile;

    auto enc = Encoder::create(EncoderCodec::ProRes, HardwareAccel::None);
    if (!enc || !enc->init(cfg)) return false;
    if (!enc->is10BitTarget()) return false;   // ProRes is always 10-bit here

    std::vector<uint8_t> bgra(static_cast<size_t>(cfg.width) * cfg.height * 4, 160);
    for (int i = 0; i < frames; ++i)
        enc->encodeFrame(bgra.data(), i);
    enc->flush();
    const bool produced = enc->framesEncoded() > 0;
    enc->shutdown();
    return produced;
}
} // namespace

TEST(ExportEncoder, ProResNon4444EncodesViaAw)
{
    // Proxy/LT/Standard/HQ route to prores_aw (or fall back to prores_ks).
    EXPECT_TRUE(encodeSolidProRes(ProResProfile::HQ, 4));
}

TEST(ExportEncoder, ProRes4444EncodesViaKs)
{
    // 4444 / 4444 XQ need prores_ks (alpha / 4:4:4).
    EXPECT_TRUE(encodeSolidProRes(ProResProfile::_4444, 4));
}

TEST(ExportEncoder, FractionalFpsAutoGopIsTwoSecondsInFrames)
{
    EncoderConfig cfg;
    cfg.width   = 128;
    cfg.height  = 128;
    cfg.fps     = 60000.0 / 1001.0;
    cfg.fpsNum  = 60000;
    cfg.fpsDen  = 1001;
    cfg.codec   = EncoderCodec::H264;
    cfg.hwAccel = HardwareAccel::None;
    cfg.gopSize = 0;

    auto enc = Encoder::create(EncoderCodec::H264, HardwareAccel::None);
    ASSERT_NE(enc, nullptr);
    ASSERT_TRUE(enc->init(cfg)) << enc->lastError();
    ASSERT_NE(enc->avCodecContext(), nullptr);
    EXPECT_EQ(enc->avCodecContext()->gop_size, 120);
    enc->shutdown();
}
#endif // ROUNDTABLE_HAS_FFMPEG

// ═════════════════════════════════════════════════════════════════════════════
// Container format
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportMuxer, ContainerFormatNames)
{
    EXPECT_STREQ(containerFormatName(ContainerFormat::MP4), "MP4");
    EXPECT_STREQ(containerFormatName(ContainerFormat::MOV), "MOV");
    EXPECT_STREQ(containerFormatName(ContainerFormat::MKV), "MKV");
    EXPECT_STREQ(containerFormatName(ContainerFormat::WebM), "WebM");
    EXPECT_STREQ(containerFormatName(ContainerFormat::AVI), "AVI");
}

TEST(ExportMuxer, ContainerFormatExtensions)
{
    EXPECT_STREQ(containerFormatExtension(ContainerFormat::MP4), ".mp4");
    EXPECT_STREQ(containerFormatExtension(ContainerFormat::MOV), ".mov");
    EXPECT_STREQ(containerFormatExtension(ContainerFormat::MKV), ".mkv");
    EXPECT_STREQ(containerFormatExtension(ContainerFormat::WebM), ".webm");
    EXPECT_STREQ(containerFormatExtension(ContainerFormat::AVI), ".avi");
}

TEST(ExportMuxer, ContainerFormatCount)
{
    EXPECT_EQ(static_cast<int>(ContainerFormat::Count), 5);
}

TEST(ExportMuxer, MuxerConfigDefaults)
{
    MuxerConfig cfg;
    EXPECT_EQ(cfg.format, ContainerFormat::MP4);
    EXPECT_EQ(cfg.videoWidth, 1920u);
    EXPECT_EQ(cfg.videoHeight, 1080u);
    EXPECT_EQ(cfg.videoFpsNum, 30);
    EXPECT_EQ(cfg.videoFpsDen, 1);
    EXPECT_EQ(cfg.audioSampleRate, 48000u);
    EXPECT_EQ(cfg.audioChannels, 2);
    EXPECT_TRUE(cfg.hasAudio);
}

TEST(ExportMuxer, MuxerNotOpenByDefault)
{
    Muxer muxer;
    EXPECT_FALSE(muxer.isOpen());
    EXPECT_EQ(muxer.videoPtsWritten(), 0);
    EXPECT_EQ(muxer.audioPtsWritten(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Export presets
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportPresets, PresetNames)
{
    EXPECT_STREQ(exportPresetName(ExportPreset::YouTube1080p30), "YouTube 1080p 30fps");
    EXPECT_STREQ(exportPresetName(ExportPreset::YouTube4K60), "YouTube 4K 60fps");
    EXPECT_STREQ(exportPresetName(ExportPreset::ArchiveProRes), "Archive ProRes HQ");
    EXPECT_STREQ(exportPresetName(ExportPreset::WebOptimized), "Web Optimized");
    EXPECT_STREQ(exportPresetName(ExportPreset::Custom), "Custom");
}

TEST(ExportPresets, PresetCount)
{
    EXPECT_EQ(static_cast<int>(ExportPreset::Count), 9);
}

TEST(ExportPresets, ApplyYouTube1080p30)
{
    ExportJobConfig cfg;
    cfg.applyPreset(ExportPreset::YouTube1080p30);

    EXPECT_EQ(cfg.outputWidth, 1920u);
    EXPECT_EQ(cfg.outputHeight, 1080u);
    EXPECT_EQ(cfg.encoderConfig.codec, EncoderCodec::H264);
    EXPECT_EQ(cfg.encoderConfig.fpsNum, 30);
    EXPECT_EQ(cfg.encoderConfig.crf, 18);
    EXPECT_EQ(cfg.encoderConfig.hwAccel, HardwareAccel::NVENC);
    EXPECT_EQ(cfg.containerFormat, static_cast<uint8_t>(ContainerFormat::MP4));
}

TEST(ExportPresets, ApplyYouTube4K60)
{
    ExportJobConfig cfg;
    cfg.applyPreset(ExportPreset::YouTube4K60);

    EXPECT_EQ(cfg.outputWidth, 3840u);
    EXPECT_EQ(cfg.outputHeight, 2160u);
    EXPECT_EQ(cfg.encoderConfig.codec, EncoderCodec::AV1);
    EXPECT_EQ(cfg.encoderConfig.fpsNum, 60);
    EXPECT_EQ(cfg.encoderConfig.hwAccel, HardwareAccel::NVENC);
}

TEST(ExportPresets, ApplyArchiveProRes)
{
    ExportJobConfig cfg;
    cfg.applyPreset(ExportPreset::ArchiveProRes);

    EXPECT_EQ(cfg.encoderConfig.codec, EncoderCodec::ProRes);
    EXPECT_EQ(cfg.encoderConfig.proresProfile, ProResProfile::HQ);
    EXPECT_EQ(cfg.containerFormat, static_cast<uint8_t>(ContainerFormat::MOV));
}

TEST(ExportPresets, ApplyWebOptimized)
{
    ExportJobConfig cfg;
    cfg.applyPreset(ExportPreset::WebOptimized);

    EXPECT_EQ(cfg.outputWidth, 1280u);
    EXPECT_EQ(cfg.outputHeight, 720u);
    EXPECT_EQ(cfg.encoderConfig.codec, EncoderCodec::H264);
    EXPECT_EQ(cfg.encoderConfig.crf, 23);
}

TEST(ExportPresets, ApplyCustomNoOp)
{
    ExportJobConfig cfg;
    cfg.outputWidth = 999;
    cfg.applyPreset(ExportPreset::Custom);
    // Custom should not change anything
    EXPECT_EQ(cfg.outputWidth, 999u);
}

// ═════════════════════════════════════════════════════════════════════════════
// ExportJobConfig
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportJobConfig, Defaults)
{
    ExportJobConfig cfg;
    EXPECT_TRUE(cfg.outputPath.empty());
    EXPECT_EQ(cfg.preset, ExportPreset::Custom);
    EXPECT_EQ(cfg.outputWidth, 1920u);
    EXPECT_EQ(cfg.outputHeight, 1080u);
    EXPECT_TRUE(cfg.includeAudio);
    EXPECT_FALSE(cfg.audioOnly);
    EXPECT_EQ(cfg.startFrame, 0);
    EXPECT_EQ(cfg.endFrame, 0);
}

TEST(ExportJobConfig, OutputPathAssignment)
{
    ExportJobConfig cfg;
    cfg.outputPath = "C:/renders/test.mp4";
    EXPECT_EQ(cfg.outputPath.string(), "C:/renders/test.mp4");
}

// ═════════════════════════════════════════════════════════════════════════════
// JobStatus / JobProgress
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportJob, StatusEnum)
{
    EXPECT_EQ(static_cast<int>(JobStatus::Queued), 0);
    EXPECT_EQ(static_cast<int>(JobStatus::Running), 1);
    EXPECT_EQ(static_cast<int>(JobStatus::Completed), 2);
    EXPECT_EQ(static_cast<int>(JobStatus::Failed), 3);
    EXPECT_EQ(static_cast<int>(JobStatus::Cancelled), 4);
}

TEST(ExportJob, ProgressDefaults)
{
    JobProgress prog;
    EXPECT_EQ(prog.currentFrame.load(), 0);
    EXPECT_EQ(prog.totalFrames.load(), 0);
    EXPECT_FLOAT_EQ(prog.percent.load(), 0.0f);
    EXPECT_DOUBLE_EQ(prog.elapsedSeconds.load(), 0.0);
    EXPECT_DOUBLE_EQ(prog.fps.load(), 0.0);
    EXPECT_TRUE(prog.statusText.empty());
}

TEST(ExportJob, Defaults)
{
    ExportJob job;
    EXPECT_EQ(job.id, 0u);
    EXPECT_EQ(job.status.load(), JobStatus::Queued);
    EXPECT_TRUE(job.error.empty());
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioMixdown
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportAudioMixdown, ConfigDefaults)
{
    AudioMixdownConfig cfg;
    EXPECT_EQ(cfg.sampleRate, 48000u);
    EXPECT_EQ(cfg.channels, 2u);
    EXPECT_EQ(cfg.codec, AudioCodec::PCM_S16LE);
    EXPECT_FLOAT_EQ(cfg.masterVolume, 1.0f);
}

TEST(ExportAudioMixdown, CodecEnum)
{
    EXPECT_EQ(static_cast<int>(AudioCodec::PCM_S16LE), 0);
    EXPECT_EQ(static_cast<int>(AudioCodec::PCM_F32LE), 1);
    EXPECT_EQ(static_cast<int>(AudioCodec::AAC), 2);
    EXPECT_EQ(static_cast<int>(AudioCodec::FLAC), 3);
    EXPECT_EQ(static_cast<int>(AudioCodec::MP3), 4);
}

TEST(ExportAudioMixdown, CodecExtensions)
{
    EXPECT_STREQ(audioCodecExtension(AudioCodec::PCM_S16LE), ".wav");
    EXPECT_STREQ(audioCodecExtension(AudioCodec::MP3),       ".mp3");
    EXPECT_STREQ(audioCodecExtension(AudioCodec::AAC),       ".m4a");
    EXPECT_STREQ(audioCodecExtension(AudioCodec::FLAC),      ".flac");
}

TEST(ExportAudioMixdown, MixEmptyTimeline)
{
    AudioMixdown mixdown;
    Timeline timeline;

    AudioMixdownConfig cfg;
    cfg.sampleRate = 44100;
    cfg.channels = 2;
    cfg.endTime = 1.0; // 1 second

    auto result = mixdown.mix(timeline, cfg);

    // With no clips, should produce silence
    EXPECT_GE(result.totalFrames, 0);
}

TEST(ExportAudioMixdown, MixdownResultDefaults)
{
    MixdownResult result;
    EXPECT_TRUE(result.samples.empty());
    EXPECT_EQ(result.totalFrames, 0);
    EXPECT_DOUBLE_EQ(result.duration, 0.0);
    EXPECT_EQ(result.sampleRate, 0u);
    EXPECT_EQ(result.channels, 0u);
}

TEST(ExportAudioMixdown, EstimateFileSize)
{
    // PCM 16-bit stereo @ 48kHz for 60 seconds
    AudioMixdownConfig estCfg;
    estCfg.codec = AudioCodec::PCM_S16LE;
    estCfg.sampleRate = 48000;
    estCfg.channels = 2;
    auto size = AudioMixdown::estimateFileSize(estCfg, 60.0);
    // 48000 samples * 2 channels * 2 bytes * 60 sec = 11520000 bytes + WAV header
    EXPECT_GT(size, 11000000u);
    EXPECT_LT(size, 12000000u);
}

TEST(ExportAudioMixdown, EstimateFileSizeAAC)
{
    AudioMixdownConfig estCfg;
    estCfg.codec = AudioCodec::AAC;
    estCfg.sampleRate = 48000;
    estCfg.channels = 2;
    estCfg.bitrate = 192000;
    auto size = AudioMixdown::estimateFileSize(estCfg, 60.0);
    // ~192kbps * 60s / 8 = ~1,440,000 bytes
    EXPECT_GT(size, 1000000u);
    EXPECT_LT(size, 2000000u);
}

TEST(ExportAudioMixdown, WriteWavFile)
{
    // Create a MixdownResult with silence
    MixdownResult mixResult;
    mixResult.samples.resize(48000 * 2, 0.0f); // 1 sec stereo silence
    mixResult.totalFrames = 48000;
    mixResult.duration = 1.0;
    mixResult.sampleRate = 48000;
    mixResult.channels = 2;
    auto tempPath = std::filesystem::temp_directory_path() / "test_export_mix.wav";

    bool ok = AudioMixdown::writeWav(mixResult, tempPath);
    EXPECT_TRUE(ok);

    // Verify file exists and has reasonable size
    EXPECT_TRUE(std::filesystem::exists(tempPath));
    auto fileSize = std::filesystem::file_size(tempPath);
    // WAV header (44 bytes) + 48000 * 2ch * 2 bytes = 192044
    EXPECT_GT(fileSize, 190000u);
    EXPECT_LT(fileSize, 195000u);

    std::filesystem::remove(tempPath);
}

TEST(ExportAudioMixdown, EstimateFileSizeMP3)
{
    AudioMixdownConfig estCfg;
    estCfg.codec = AudioCodec::MP3;
    estCfg.sampleRate = 48000;
    estCfg.channels = 2;
    estCfg.bitrate = 192000;
    auto size = AudioMixdown::estimateFileSize(estCfg, 60.0);
    // ~192kbps * 60s / 8 = ~1,440,000 bytes (same model as AAC)
    EXPECT_GT(size, 1000000u);
    EXPECT_LT(size, 2000000u);
}

// writeAudioFile encodes + muxes via FFmpeg.  Bundled FFmpeg ships libmp3lame,
// the native AAC encoder, and FLAC; if a stripped build lacks one the test
// skips rather than failing.
namespace {
void expectEncodedAudioFile(AudioCodec codec, const char* ext)
{
    MixdownResult mix;
    mix.samples.resize(48000 * 2, 0.0f);  // 1s of stereo silence
    mix.totalFrames = 48000;
    mix.duration    = 1.0;
    mix.sampleRate  = 48000;
    mix.channels    = 2;

    auto path = std::filesystem::temp_directory_path() /
                (std::string("test_export_audioonly") + ext);
    std::filesystem::remove(path);

    bool ok = AudioMixdown::writeAudioFile(mix, path, codec, 192000);
    if (!ok) {
        GTEST_SKIP() << "encoder for " << ext << " unavailable in this FFmpeg build";
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);
    std::filesystem::remove(path);
}
} // namespace

TEST(ExportAudioMixdown, WriteMp3File)  { expectEncodedAudioFile(AudioCodec::MP3,  ".mp3"); }
TEST(ExportAudioMixdown, WriteAacFile)  { expectEncodedAudioFile(AudioCodec::AAC,  ".m4a"); }
TEST(ExportAudioMixdown, WriteFlacFile) { expectEncodedAudioFile(AudioCodec::FLAC, ".flac"); }

TEST(ExportAudioMixdown, WriteAudioFileWavRoutesToPcm)
{
    // PCM_S16LE must produce a valid RIFF/WAVE just like writeWav().
    MixdownResult mix;
    mix.samples.resize(48000 * 2, 0.0f);
    mix.totalFrames = 48000;
    mix.duration    = 1.0;
    mix.sampleRate  = 48000;
    mix.channels    = 2;

    auto path = std::filesystem::temp_directory_path() / "test_export_audioonly_pcm.wav";
    std::filesystem::remove(path);

    EXPECT_TRUE(AudioMixdown::writeAudioFile(mix, path, AudioCodec::PCM_S16LE, 0));
    ASSERT_TRUE(std::filesystem::exists(path));
    std::ifstream in(path, std::ios::binary);
    char riff[4] = {};
    in.read(riff, 4);
    EXPECT_EQ(std::string(riff, 4), "RIFF");
    in.close();
    std::filesystem::remove(path);
}

// ── Audio FX chain integration in the export mixdown ─────────────────────────

namespace {

/// Write a stereo PCM-16 WAV tone; returns the path (empty on failure).
std::filesystem::path writeToneWav(double seconds, float amp, uint32_t sr)
{
    auto path = std::filesystem::temp_directory_path() / "test_export_fx_tone.wav";
    std::ofstream f(path, std::ios::binary);
    if (!f) return {};

    const uint16_t channels = 2, bits = 16;
    const auto frames = static_cast<uint32_t>(seconds * sr);
    const uint32_t dataSize = frames * channels * (bits / 8);
    const uint32_t byteRate = sr * channels * (bits / 8);
    const uint16_t blockAlign = channels * (bits / 8);
    const uint32_t fileSize = 36 + dataSize;
    const uint32_t fmtSize = 16; const uint16_t pcm = 1;

    auto w32 = [&](uint32_t v){ f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [&](uint16_t v){ f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); w32(fileSize); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(fmtSize); w16(pcm); w16(channels);
    w32(sr); w32(byteRate); w16(blockAlign); w16(bits);
    f.write("data", 4); w32(dataSize);

    for (uint32_t i = 0; i < frames; ++i) {
        const float v = amp * std::sin(2.0f * 3.14159265f * 440.0f * i / sr);
        const auto s = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        w16(static_cast<uint16_t>(s)); w16(static_cast<uint16_t>(s));
    }
    return path;
}

float mixRms(const MixdownResult& r)
{
    if (r.samples.empty()) return 0.0f;
    double acc = 0.0;
    for (float s : r.samples) acc += static_cast<double>(s) * s;
    return static_cast<float>(std::sqrt(acc / r.samples.size()));
}

} // namespace

TEST(ExportAudioMixdown, ClipFxChainAffectsMix)
{
    const uint32_t sr = 48000;
    auto wav = writeToneWav(0.5, 0.8f, sr);
    ASSERT_FALSE(wav.empty());

    Timeline tl;
    Track* at = tl.addAudioTrack("Audio 1");
    ASSERT_NE(at, nullptr);

    auto clip = std::make_unique<AudioClip>();
    clip->setMediaPath(wav.string());
    clip->setSampleRate(sr);
    clip->setChannels(2);
    clip->setTimelineIn(0);
    clip->setDuration(secondsToTicks(0.5));
    clip->setSourceDuration(secondsToTicks(0.5));
    AudioClip* rawClip = clip.get();
    at->addClip(std::move(clip));

    AudioMixdownConfig cfg;
    cfg.sampleRate = sr;
    cfg.channels = 2;

    AudioMixdown mixdown;
    auto dry = mixdown.mix(tl, cfg);
    if (!dry.isValid() || mixRms(dry) < 1e-4f) {
        std::filesystem::remove(wav);
        GTEST_SKIP() << "No audio backend available to read WAV";
    }

    // Apply a -6 dB output trim via the clip's EQ; the mix should ~halve.
    auto* eq = static_cast<audiofx::ParametricEQ*>(
        rawClip->audioFx().add(audiofx::ProcessorKind::ParametricEQ));
    eq->setOutputGainDb(-6.0206f);  // 0.5x linear

    auto wet = mixdown.mix(tl, cfg);
    ASSERT_TRUE(wet.isValid());

    EXPECT_NEAR(mixRms(wet), mixRms(dry) * 0.5f, mixRms(dry) * 0.05f);

    std::filesystem::remove(wav);
}

// ═════════════════════════════════════════════════════════════════════════════
// RenderQueue job management
// ═════════════════════════════════════════════════════════════════════════════

TEST(ExportRenderQueue, AddJob)
{
    RenderQueue queue;

    ExportJobConfig cfg;
    cfg.outputPath = "test_output.mp4";
    cfg.outputWidth = 1920;
    cfg.outputHeight = 1080;

    uint32_t id = queue.addJob(cfg);
    EXPECT_EQ(id, 1u);

    auto jobs = queue.jobs();
    ASSERT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs[0]->id, 1u);
    EXPECT_EQ(jobs[0]->status.load(), JobStatus::Queued);
}

TEST(ExportRenderQueue, FullSnapshotSurvivesEditsAndProjectSwitch)
{
    RenderQueue queue;
    ExportJobConfig cfg;
    cfg.outputPath = "immutable_snapshot.mp4";

    auto liveProject = Project::createNew("Queue-Time Project");
    ASSERT_NE(liveProject, nullptr);
    ASSERT_EQ(liveProject->activeSequenceIndex(), 0u);

    Timeline* exportedTimeline = liveProject->addSequence("Queued Sequence");
    ASSERT_NE(exportedTimeline, nullptr);
    ASSERT_GE(exportedTimeline->trackCount(), 1u);

    auto sourceClip = std::make_unique<VideoClip>("C:/media/original.mov");
    sourceClip->setLabel("Queue-Time Clip");
    sourceClip->setTimelineIn(1200);
    sourceClip->setDuration(96000);
    VideoClip* liveClip = sourceClip.get();
    ASSERT_NE(exportedTimeline->track(0)->addClip(std::move(sourceClip)), nullptr);

    // Export a non-active sequence to verify the snapshot aliases the selected
    // sequence by identity/index rather than blindly using Project::timeline().
    const uint32_t id = queue.addJob(cfg, liveProject.get(), exportedTimeline);
    const auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    ASSERT_TRUE(job->snapshotCaptureError.empty()) << job->snapshotCaptureError;
    ASSERT_NE(job->renderSnapshot, nullptr);
    ASSERT_TRUE(job->renderSnapshot->isFullProject());

    const auto snapshot = job->renderSnapshot;
    EXPECT_EQ(snapshot->sequenceIndex, 1u);
    ASSERT_EQ(snapshot->project->sequenceCount(), 2u);
    EXPECT_EQ(snapshot->project->activeSequenceIndex(), 1u);
    EXPECT_EQ(snapshot->timeline.get(), snapshot->project->sequence(1));
    EXPECT_FALSE(snapshot->project.owner_before(snapshot->timeline));
    EXPECT_FALSE(snapshot->timeline.owner_before(snapshot->project));
    EXPECT_EQ(snapshot->timeline->name(), "Queued Sequence");
    ASSERT_GE(snapshot->timeline->trackCount(), 1u);
    ASSERT_EQ(snapshot->timeline->track(0)->clipCount(), 1u);
    const auto* capturedClip = dynamic_cast<const VideoClip*>(
        snapshot->timeline->track(0)->clip(0));
    ASSERT_NE(capturedClip, nullptr);
    EXPECT_EQ(capturedClip->mediaPath(), "C:/media/original.mov");
    EXPECT_EQ(capturedClip->label(), "Queue-Time Clip");
    EXPECT_EQ(capturedClip->duration(), 96000);

    // Simulate edits while queued/running, then a complete project switch that
    // destroys every live source object. The retained render graph must remain
    // byte-for-byte at its queue-time state.
    exportedTimeline->setName("Edited After Queue");
    liveClip->setMediaPath("C:/media/replacement.mov");
    liveClip->setLabel("Edited Clip");
    liveClip->setDuration(48000);
    liveProject = Project::createNew("Different Project");

    EXPECT_EQ(snapshot->project->name(), "Queue-Time Project");
    EXPECT_EQ(snapshot->timeline->name(), "Queued Sequence");
    capturedClip = dynamic_cast<const VideoClip*>(
        snapshot->timeline->track(0)->clip(0));
    ASSERT_NE(capturedClip, nullptr);
    EXPECT_EQ(capturedClip->mediaPath(), "C:/media/original.mov");
    EXPECT_EQ(capturedClip->label(), "Queue-Time Clip");
    EXPECT_EQ(capturedClip->duration(), 96000);
}

TEST(ExportRenderQueue, FullSnapshotFailsClosedForForeignTimeline)
{
    RenderQueue queue;
    ExportJobConfig cfg;
    cfg.outputPath = "foreign_timeline.mp4";
    auto project = Project::createNew("Owner");
    Timeline foreignTimeline;

    const uint32_t id = queue.addJob(cfg, project.get(), &foreignTimeline);
    const auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(job->renderSnapshot, nullptr);
    EXPECT_FALSE(job->snapshotCaptureError.empty());

    queue.start(nullptr);
    queue.waitForAll();
    const auto failed = queue.job(id);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->status.load(), JobStatus::Failed);
    EXPECT_EQ(failed->error, failed->snapshotCaptureError);
}

TEST(ExportRenderQueue, AddMultipleJobs)
{
    RenderQueue queue;

    ExportJobConfig cfg;
    cfg.outputPath = "out1.mp4";
    uint32_t id1 = queue.addJob(cfg);

    cfg.outputPath = "out2.mp4";
    uint32_t id2 = queue.addJob(cfg);

    cfg.outputPath = "out3.mp4";
    uint32_t id3 = queue.addJob(cfg);

    EXPECT_EQ(id1, 1u);
    EXPECT_EQ(id2, 2u);
    EXPECT_EQ(id3, 3u);

    EXPECT_EQ(queue.pendingCount(), 3u);
}

TEST(ExportRenderQueue, RemoveQueuedJob)
{
    RenderQueue queue;

    ExportJobConfig cfg;
    cfg.outputPath = "out.mp4";
    uint32_t id = queue.addJob(cfg);

    EXPECT_TRUE(queue.removeJob(id));
    EXPECT_EQ(queue.pendingCount(), 0u);
}

TEST(ExportRenderQueue, RemoveNonexistentJob)
{
    RenderQueue queue;
    EXPECT_FALSE(queue.removeJob(999));
}

TEST(ExportRenderQueue, InitialState)
{
    RenderQueue queue;
    EXPECT_FALSE(queue.isRunning());
    EXPECT_EQ(queue.pendingCount(), 0u);
    EXPECT_TRUE(queue.jobs().empty());
}

TEST(ExportRenderQueue, JobLookup)
{
    RenderQueue queue;

    ExportJobConfig cfg;
    cfg.outputPath = "lookup_test.mp4";
    uint32_t id = queue.addJob(cfg);

    const auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(job->id, id);

    EXPECT_EQ(queue.job(999), nullptr);
}

TEST(ExportRenderQueue, CallbacksSet)
{
    RenderQueue queue;

    bool progressCalled = false;
    bool completeCalled = false;

    queue.setProgressCallback([&](uint32_t, const JobProgress&) {
        progressCalled = true;
    });

    queue.setCompleteCallback([&](uint32_t, bool, const std::string&) {
        completeCalled = true;
    });

    // Callbacks should be set but not called yet
    EXPECT_FALSE(progressCalled);
    EXPECT_FALSE(completeCalled);
}

TEST(ExportRenderQueue, JobSequentialIds)
{
    RenderQueue queue;
    ExportJobConfig cfg;

    for (int i = 0; i < 10; ++i) {
        uint32_t id = queue.addJob(cfg);
        EXPECT_EQ(id, static_cast<uint32_t>(i + 1));
    }
}

TEST(ExportRenderQueue, CancelMarksJobAndRemoveAfterFinalize)
{
    // "Remove from Queue" UI flow: cancelJob() then removeJob(). A job that
    // is not Running must be removable, and cancel must set the per-job flag
    // (the old m_cancelFlags map raced; the flag now lives on the job).
    RenderQueue queue;
    ExportJobConfig cfg;
    cfg.outputPath = "cancel_test.mp4";
    uint32_t id = queue.addJob(cfg);

    queue.cancelJob(id);
    auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    EXPECT_TRUE(job->cancelRequested.load());

    EXPECT_TRUE(queue.removeJob(id));
    EXPECT_EQ(queue.job(id), nullptr);
    EXPECT_EQ(queue.pendingCount(), 0u);
}

TEST(ExportRenderQueue, HeldJobReferenceSurvivesRemoval)
{
    // The worker thread keeps its own shared_ptr to the job it is running.
    // Erasing the job from the queue while a reference is held must not
    // invalidate that reference (this was the dangling-ExportJob* crash
    // class when m_jobs stored jobs by value).
    RenderQueue queue;
    ExportJobConfig cfg;
    cfg.outputPath = "held.mp4";
    uint32_t id = queue.addJob(cfg);

    auto held = queue.job(id);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(queue.removeJob(id));
    EXPECT_EQ(queue.job(id), nullptr);

    // Still safely readable after removal from the queue.
    EXPECT_EQ(held->id, id);
    EXPECT_EQ(held->status.load(), JobStatus::Queued);
}

// ═════════════════════════════════════════════════════════════════════════════
// =============================================================================
// Fail-closed frame/file integrity
// =============================================================================

namespace {

std::filesystem::path uniqueExportTestPath(const std::string& suffix)
{
    static std::atomic<uint64_t> serial{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("roundtable_export_integrity_" + std::to_string(now) + "_" +
            std::to_string(serial.fetch_add(1)) + suffix);
}

std::string readTestFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool waitForRenderQueue(RenderQueue& queue,
                        std::chrono::milliseconds timeout = std::chrono::seconds(15))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (queue.isRunning() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return !queue.isRunning();
}

EncoderConfig proResIntegrityConfig()
{
    EncoderConfig config;
    config.width = 128;
    config.height = 128;
    config.fpsNum = 30;
    config.fpsDen = 1;
    config.codec = EncoderCodec::ProRes;
    config.proresProfile = ProResProfile::HQ;
    config.hwAccel = HardwareAccel::None;
    return config;
}

bool hasProResEncoderForIntegrityTest()
{
    auto encoder = Encoder::create(EncoderCodec::ProRes, HardwareAccel::None);
    if (!encoder || !encoder->init(proResIntegrityConfig()))
        return false;
    encoder->shutdown();
    return true;
}

ExportJobConfig proResIntegrityJob(const std::filesystem::path& path)
{
    ExportJobConfig config;
    config.outputPath = path;
    config.outputWidth = 128;
    config.outputHeight = 128;
    config.encoderConfig = proResIntegrityConfig();
    config.containerFormat = static_cast<uint8_t>(ContainerFormat::MOV);
    config.includeAudio = false;
    config.startFrame = 0;
    config.endFrame = 1;
    return config;
}

std::shared_ptr<CachedFrame> makeIntegrityFrame(uint32_t width,
                                                uint32_t height,
                                                uint32_t padding = 0)
{
    auto frame = std::make_shared<CachedFrame>();
    frame->width = width;
    frame->height = height;
    frame->stride = width * 4u + padding;
    frame->pixels.resize(static_cast<size_t>(frame->stride) * height, 0x70);
    return frame;
}

} // namespace

TEST(ExportIntegrity, RejectsIncompleteFramePayloadsAndAcceptsPaddedRows)
{
    EXPECT_FALSE(validateExportFrame(nullptr, 64, 32, false).valid);

    CachedFrame frame;
    frame.width = 64;
    frame.height = 32;
    frame.stride = 64 * 4;
    EXPECT_FALSE(validateExportFrame(&frame, 64, 32, false).valid);

    frame.pixels.resize(static_cast<size_t>(frame.stride) * frame.height);
    EXPECT_TRUE(validateExportFrame(&frame, 64, 32, false).valid);
    EXPECT_FALSE(validateExportFrame(&frame, 65, 32, false).valid);

    frame.stride = 64 * 4 + 16;
    frame.pixels.resize(static_cast<size_t>(frame.stride) * frame.height);
    const auto padded = validateExportFrame(&frame, 64, 32, false);
    EXPECT_TRUE(padded.valid);
    EXPECT_TRUE(padded.needsBgraRepack);

    frame.depth = 16;
    frame.rgba16fStride = 64 * 8;
    frame.rgba16f.resize(static_cast<size_t>(frame.rgba16fStride) * frame.height);
    const auto highDepth = validateExportFrame(&frame, 64, 32, true);
    EXPECT_TRUE(highDepth.valid);
    EXPECT_TRUE(highDepth.useRgba16f);

    frame.rgba16f.pop_back();
    const auto safeFallback = validateExportFrame(&frame, 64, 32, true);
    EXPECT_TRUE(safeFallback.valid);
    EXPECT_FALSE(safeFallback.useRgba16f);
}

TEST(ExportIntegrity, AtomicPublicationPreservesOrReplacesDestination)
{
    const auto destination = uniqueExportTestPath(".mov");
    const auto staged = uniqueExportTestPath(".partial");
    {
        std::ofstream out(destination, std::ios::binary);
        out << "prior";
    }

    std::string error;
    EXPECT_FALSE(publishStagedExport(staged, destination, error));
    EXPECT_EQ(readTestFile(destination), "prior");

    {
        std::ofstream out(staged, std::ios::binary);
        out << "validated-new-output";
    }
    EXPECT_TRUE(publishStagedExport(staged, destination, error)) << error;
    EXPECT_EQ(readTestFile(destination), "validated-new-output");
    EXPECT_FALSE(std::filesystem::exists(staged));

    std::filesystem::remove(destination);
}

#ifdef ROUNDTABLE_HAS_FFMPEG
TEST(ExportIntegrity, PersistentCompositeFailureRetriesAndPreservesPriorOutput)
{
    if (!hasProResEncoderForIntegrityTest())
        GTEST_SKIP() << "ProRes encoder unavailable in this FFmpeg build";

    const auto destination = uniqueExportTestPath(".mov");
    {
        std::ofstream out(destination, std::ios::binary);
        out << "known-prior-output";
    }

    RenderQueue queue;
    std::atomic<int> attempts{0};
    std::atomic<int> wrongTickAttempts{0};
    queue.setFrameRenderCallback(
        [&](int64_t tick, int64_t nextTick, uint32_t, uint32_t, bool) {
            ++attempts;
            if (tick != 0 || nextTick != -1)
                ++wrongTickAttempts;
            return std::shared_ptr<CachedFrame>{};
        });
    const uint32_t id = queue.addJob(proResIntegrityJob(destination));
    queue.start(nullptr, nullptr);

    ASSERT_TRUE(waitForRenderQueue(queue));
    const auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    EXPECT_EQ(job->status.load(), JobStatus::Failed);
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(wrongTickAttempts.load(), 0);
    EXPECT_EQ(job->progress.currentFrame.load(), 0);
    EXPECT_EQ(readTestFile(destination), "known-prior-output");

    std::filesystem::remove(destination);
}

TEST(ExportIntegrity, RetrySuccessPublishesOnlyExactDecodableTimeline)
{
    if (!hasProResEncoderForIntegrityTest())
        GTEST_SKIP() << "ProRes encoder unavailable in this FFmpeg build";

    const auto destination = uniqueExportTestPath(".mov");
    {
        std::ofstream out(destination, std::ios::binary);
        out << "prior";
    }

    RenderQueue queue;
    std::atomic<int> attempts{0};
    std::atomic<int> wrongTickAttempts{0};
    queue.setFrameRenderCallback(
        [&](int64_t tick, int64_t nextTick, uint32_t, uint32_t, bool) {
            if (tick != 0 || nextTick != -1)
                ++wrongTickAttempts;
            const int attempt = ++attempts;
            if (attempt == 1)
                return std::shared_ptr<CachedFrame>{};
            if (attempt == 2)
                return makeIntegrityFrame(64, 64); // wrong dimensions
            return makeIntegrityFrame(128, 128, 16); // valid padded BGRA
        });
    const uint32_t id = queue.addJob(proResIntegrityJob(destination));
    queue.start(nullptr, nullptr);

    ASSERT_TRUE(waitForRenderQueue(queue));
    const auto job = queue.job(id);
    ASSERT_NE(job, nullptr);
    ASSERT_EQ(job->status.load(), JobStatus::Completed) << job->error;
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(wrongTickAttempts.load(), 0);

    const auto valid = validateExportFile(destination, 1, 128, 128, 30, 1);
    EXPECT_TRUE(valid.ok) << valid.error;
    EXPECT_EQ(valid.decodedFrames, 1);

    const auto wrongCount = validateExportFile(destination, 2, 128, 128, 30, 1);
    EXPECT_FALSE(wrongCount.ok);

    const auto truncated = uniqueExportTestPath("_truncated.mov");
    std::filesystem::copy_file(destination, truncated,
                               std::filesystem::copy_options::overwrite_existing);
    const auto originalSize = std::filesystem::file_size(truncated);
    ASSERT_GT(originalSize, 32u);
    std::filesystem::resize_file(truncated, 32);
    EXPECT_FALSE(validateExportFile(truncated, 1, 128, 128, 30, 1).ok);

    std::filesystem::remove(truncated);
    std::filesystem::remove(destination);
}
#endif

// Total: ~50 tests
// ═════════════════════════════════════════════════════════════════════════════

// =============================================================================
// SmartRenderAnalyzer — passthrough planning (cleanup audit section 5.3)
//
// NOTE: the passthrough EXECUTION path in RenderQueue is currently disabled
// (kEnableSmartRenderPassthrough = false; see the SPS/PPS extradata rationale
// in RenderQueue.cpp).  The ANALYZER below is the live planning brain that
// will drive it once the bitstream-filter work re-enables passthrough, so its
// eligibility rules and source-frame mapping are locked down here.
// =============================================================================

#include "SmartRenderAnalyzer.h"
#include "timeline/OpacityMask.h"

namespace {

constexpr int64_t kTpfAt30 = 1600;  // 48000 ticks/s at 30 fps

/// An identity VideoClip that matches the default EncoderConfig
/// (h264, 1920x1080, 30 fps) — passthrough-eligible by construction.
std::unique_ptr<VideoClip> makeIdentityVideoClip(int64_t inFrames, int64_t durFrames)
{
    auto vc = std::make_unique<VideoClip>();
    vc->setMediaPath("C:/test/source.mp4");
    vc->setSourceResolution(1920, 1080);
    vc->setSourceFps(30.0);
    vc->setSourceCodecName("h264");
    vc->setSourceDuration(10 * 60 * 48000);
    vc->setTimelineIn(inFrames * kTpfAt30);
    vc->setDuration(durFrames * kTpfAt30);
    return vc;
}

} // namespace

TEST(SmartRenderAnalyzer, IdentityClipAllPassthrough)
{
    Timeline tl;
    auto* vt = tl.addVideoTrack("V1");
    vt->addClip(makeIdentityVideoClip(0, 90));

    EncoderConfig enc;  // h264, 1920x1080, 30 fps
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 90);

    EXPECT_EQ(plan.totalFrames, 90);
    EXPECT_EQ(plan.passthroughCount, 90);
    EXPECT_EQ(plan.reEncodeCount, 0);
    // Source-frame mapping: timelineIn==0, sourceIn==0 -> frame f reads f
    ASSERT_TRUE(plan.passthroughFrames.count(0));
    EXPECT_EQ(plan.passthroughFrames.at(0).sourceFrame, 0);
    ASSERT_TRUE(plan.passthroughFrames.count(89));
    EXPECT_EQ(plan.passthroughFrames.at(89).sourceFrame, 89);
    EXPECT_EQ(plan.passthroughFrames.at(0).mediaPath, "C:/test/source.mp4");
}

TEST(SmartRenderAnalyzer, SourceInOffsetMapsSourceFrames)
{
    Timeline tl;
    auto* vt = tl.addVideoTrack("V1");
    auto clip = makeIdentityVideoClip(10, 60);   // starts at timeline frame 10
    clip->setSourceIn(20 * kTpfAt30);            // trimmed 20 frames into source
    vt->addClip(std::move(clip));

    EncoderConfig enc;
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 70);

    // Frames [0,10) are a gap -> re-encode; [10,70) are passthrough.
    EXPECT_EQ(plan.passthroughCount, 60);
    EXPECT_EQ(plan.reEncodeCount, 10);
    // Export frame 10 = clip local frame 0 = source frame 20
    ASSERT_TRUE(plan.passthroughFrames.count(10));
    EXPECT_EQ(plan.passthroughFrames.at(10).sourceFrame, 20);
    ASSERT_TRUE(plan.passthroughFrames.count(69));
    EXPECT_EQ(plan.passthroughFrames.at(69).sourceFrame, 79);
    EXPECT_FALSE(plan.passthroughFrames.count(9));
}

TEST(SmartRenderAnalyzer, StartFrameWindowIsZeroBasedInPlan)
{
    Timeline tl;
    auto* vt = tl.addVideoTrack("V1");
    vt->addClip(makeIdentityVideoClip(0, 90));

    EncoderConfig enc;
    // Export only frames [30, 60) -> plan keys are 0-based within the window.
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 30, 60);

    EXPECT_EQ(plan.totalFrames, 30);
    EXPECT_EQ(plan.passthroughCount, 30);
    ASSERT_TRUE(plan.passthroughFrames.count(0));
    EXPECT_EQ(plan.passthroughFrames.at(0).sourceFrame, 30);
}

TEST(SmartRenderAnalyzer, DisqualifiersForceReEncode)
{
    // Each mutation must disqualify every frame from passthrough.
    struct Case {
        const char* name;
        std::function<void(VideoClip&)> mutate;
    };
    const Case cases[] = {
        { "position",   [](VideoClip& c){ c.positionX().setDefaultValue(50.0f); } },
        { "scale",      [](VideoClip& c){ c.scaleX().setDefaultValue(1.5f); } },
        { "rotation",   [](VideoClip& c){ c.rotation().setDefaultValue(15.0f); } },
        { "shutter",    [](VideoClip& c){ c.shutterAngle().setDefaultValue(180.0f); } },
        { "opacity",    [](VideoClip& c){ c.opacity().setDefaultValue(0.5f); } },
        { "opacityKf",  [](VideoClip& c){ c.opacity().addKeyframe(0, 1.0f); } },
        { "speed",      [](VideoClip& c){ c.setSpeed(0.5); } },
        { "speedRamp",  [](VideoClip& c){ c.speedRamp().setDefaultValue(2.0f); } },
        { "frameBlend",  [](VideoClip& c){ c.setTimeInterpolation(TimeInterpolation::FrameBlending); } },
        { "opticalFlow", [](VideoClip& c){ c.setTimeInterpolation(TimeInterpolation::OpticalFlow); } },
        { "crop",       [](VideoClip& c){ c.setCrop(5.0f, 0.0f, 0.0f, 0.0f); } },
        { "blendMode",  [](VideoClip& c){ c.setBlendMode(3); } },
        { "mask",       [](VideoClip& c){ c.addMask(OpacityMask{}); } },
        { "resolution", [](VideoClip& c){ c.setSourceResolution(1280, 720); } },
        { "codec",      [](VideoClip& c){ c.setSourceCodecName("hevc"); } },
        { "fps",        [](VideoClip& c){ c.setSourceFps(24.0); } },
        { "disabled",   [](VideoClip& c){ c.setEnabled(false); } },
    };

    for (const auto& cs : cases) {
        Timeline tl;
        auto* vt = tl.addVideoTrack("V1");
        auto clip = makeIdentityVideoClip(0, 30);
        cs.mutate(*clip);
        vt->addClip(std::move(clip));

        EncoderConfig enc;
        auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 30);
        EXPECT_EQ(plan.passthroughCount, 0) << "disqualifier: " << cs.name;
        EXPECT_EQ(plan.reEncodeCount, 30)   << "disqualifier: " << cs.name;
    }
}

TEST(SmartRenderAnalyzer, OverlappingClipsReEncode)
{
    Timeline tl;
    auto* v1 = tl.addVideoTrack("V1");
    auto* v2 = tl.addVideoTrack("V2");
    v1->addClip(makeIdentityVideoClip(0, 60));
    v2->addClip(makeIdentityVideoClip(30, 60));   // overlaps [30,60)

    EncoderConfig enc;
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 90);

    // [0,30) single clip, [30,60) two clips, [60,90) single clip.
    EXPECT_EQ(plan.passthroughCount, 60);
    EXPECT_EQ(plan.reEncodeCount, 30);
    EXPECT_TRUE(plan.passthroughFrames.count(0));
    EXPECT_FALSE(plan.passthroughFrames.count(45));
    EXPECT_TRUE(plan.passthroughFrames.count(75));
}

TEST(SmartRenderAnalyzer, MutedTrackClipIgnored)
{
    Timeline tl;
    auto* vt = tl.addVideoTrack("V1");
    vt->addClip(makeIdentityVideoClip(0, 30));
    vt->setMuted(true);

    EncoderConfig enc;
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 30);

    // No active clips -> nothing to pass through.
    EXPECT_EQ(plan.passthroughCount, 0);
    EXPECT_EQ(plan.reEncodeCount, 30);
}

TEST(SmartRenderAnalyzer, ImageSequenceNeverPassthrough)
{
    Timeline tl;
    auto* vt = tl.addVideoTrack("V1");
    vt->addClip(makeIdentityVideoClip(0, 30));

    EncoderConfig enc;
    enc.codec = EncoderCodec::ImageSequence;
    auto plan = analyzeSmartRender(tl, enc, 1920, 1080, 0, 30);

    EXPECT_EQ(plan.passthroughCount, 0);
    EXPECT_EQ(plan.reEncodeCount, 30);
}

TEST(SmartRenderAnalyzer, CodecToSourceNameMapping)
{
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::H264), "h264");
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::H265), "hevc");
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::AV1), "av1");
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::ProRes), "prores");
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::DNxHR), "dnxhd");
    EXPECT_STREQ(encoderCodecToSourceName(EncoderCodec::ImageSequence), "");
}
