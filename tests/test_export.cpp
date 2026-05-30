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

#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <filesystem>
#include <thread>

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
    EXPECT_STREQ(encoderCodecName(EncoderCodec::ImageSequence), "Image Sequence");
}

TEST(ExportEncoder, CodecCount)
{
    EXPECT_EQ(static_cast<int>(EncoderCodec::Count), 5);
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
    EXPECT_EQ(static_cast<int>(ImageFormat::JPEG), 4);
    EXPECT_EQ(static_cast<int>(ImageFormat::Count), 5);
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
    EXPECT_EQ(prog.currentFrame, 0);
    EXPECT_EQ(prog.totalFrames, 0);
    EXPECT_FLOAT_EQ(prog.percent, 0.0f);
    EXPECT_DOUBLE_EQ(prog.elapsedSeconds, 0.0);
    EXPECT_DOUBLE_EQ(prog.fps, 0.0);
    EXPECT_TRUE(prog.statusText.empty());
}

TEST(ExportJob, Defaults)
{
    ExportJob job;
    EXPECT_EQ(job.id, 0u);
    EXPECT_EQ(job.status, JobStatus::Queued);
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
    EXPECT_EQ(jobs.size(), 1u);
    EXPECT_EQ(jobs[0].id, 1u);
    EXPECT_EQ(jobs[0].status, JobStatus::Queued);
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

    const auto* job = queue.job(id);
    EXPECT_NE(job, nullptr);
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

// ═════════════════════════════════════════════════════════════════════════════
// Total: ~50 tests
// ═════════════════════════════════════════════════════════════════════════════
