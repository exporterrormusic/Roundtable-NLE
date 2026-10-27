/*
 * test_corridorkey.cpp — CorridorKey render planning and the clip swap
 * (ai/CorridorKey.h).
 *
 * The contract that matters most: after useRender() the clip shows the SAME
 * source frame at every timeline tick (render frame + firstFrame == source
 * frame), including at 29.97 fps where one frame is 1601.6 ticks, at 2x speed
 * and under a speed ramp — and useSource() puts every field back.
 */

#include <gtest/gtest.h>

#include "ai/CorridorKey.h"
#include "PathUtils.h"
#include "decode/VideoFrameMapping.h"
#include "effects/ChromaKey.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <algorithm>
#include <fstream>
#include <memory>

namespace rt {
namespace {

namespace ck = corridorkey;
namespace fs = std::filesystem;

constexpr double kNtsc = 30000.0 / 1001.0;
constexpr int64_t kSeqFrame = 1600;   // one 30 fps sequence frame

VideoStreamInfo cameraInfo(int64_t frames = 27990, double fps = kNtsc)
{
    VideoStreamInfo info;
    info.width = 1920;
    info.height = 1080;
    info.fps = fps;
    info.frameCount = frames;
    return info;
}

std::unique_ptr<VideoClip> cameraClip(int64_t timelineIn, int64_t sourceIn, int64_t duration,
                                      double speed = 1.0, double fps = kNtsc)
{
    auto clip = std::make_unique<VideoClip>("C:/footage/take one.mp4");
    clip->setTimelineIn(timelineIn);
    clip->setSourceIn(sourceIn);
    clip->setDuration(duration);
    clip->setSpeed(speed);
    clip->setSourceFps(fps);
    clip->setSourceDuration(27990 * 1601);
    clip->setSourceCodecName("h264");
    clip->effects().addEffect(std::make_unique<ChromaKey>());
    return clip;
}

uint64_t ultraKeyId(const VideoClip& clip) { return clip.effects().effect(0).id(); }

/// Render the plan "for real" as far as the mapping is concerned: the render
/// file has exactly frameCount() frames at the same rate.
VideoStreamInfo renderInfo(const ck::RenderPlan& plan)
{
    VideoStreamInfo info = cameraInfo(plan.frameCount(), plan.fps.value());
    info.hasAlpha = true;
    return info;
}

/// Every tick the clip is shown (its transitions across the cuts included)
/// shows the same source frame before and after the swap.
void expectSameFramesAfterSwap(VideoClip& clip, int64_t step, ck::Coverage coverage = {})
{
    const VideoStreamInfo source = cameraInfo();
    const auto result = ck::planRender(clip, source, kSeqFrame, coverage);
    ASSERT_TRUE(result.plan) << result.problem;
    const ck::RenderPlan plan = *result.plan;
    const int64_t from = clip.timelineIn() - coverage.lead;
    const int64_t to = clip.timelineOut() + coverage.tail;

    std::vector<MappedSourceFrame> before;
    for (int64_t t = from; t < to; t += step)
        before.push_back(mapTickToSourceFrame(clip, t, &source));

    const ck::RenderRecord record = ck::makeRecord(plan, clip, "abcd1234");
    ck::useRender(clip, "C:/renders/take one CK.mov", record, ultraKeyId(clip));
    EXPECT_TRUE(ck::renderCovers(clip, record, coverage, kSeqFrame));
    const VideoStreamInfo render = renderInfo(plan);
    size_t i = 0;
    for (int64_t t = from; t < to; t += step, ++i) {
        const MappedSourceFrame after = mapTickToSourceFrame(clip, t, &render);
        EXPECT_EQ(after.frame + plan.firstFrame, before[i].frame) << "tick " << t;
        EXPECT_EQ(after.lowerFrame + plan.firstFrame, before[i].lowerFrame) << "tick " << t;
        EXPECT_EQ(after.upperFrame + plan.firstFrame, before[i].upperFrame) << "tick " << t;
        EXPECT_NEAR(after.blendPhase, before[i].blendPhase, 1e-6) << "tick " << t;
    }
}

// ── Frame rates ─────────────────────────────────────────────────────────────

TEST(CorridorKey, ExactFrameRateRecognisesBroadcastRates)
{
    EXPECT_EQ(ck::exactFrameRate(kNtsc).num, 30000);
    EXPECT_EQ(ck::exactFrameRate(kNtsc).den, 1001);
    EXPECT_EQ(ck::exactFrameRate(24000.0 / 1001.0).num, 24000);
    EXPECT_EQ(ck::exactFrameRate(60000.0 / 1001.0).num, 60000);
    EXPECT_EQ(ck::exactFrameRate(25.0).num, 25);
    EXPECT_EQ(ck::exactFrameRate(25.0).den, 1);
    EXPECT_EQ(ck::exactFrameRate(12.5).num, 25);
    EXPECT_EQ(ck::exactFrameRate(12.5).den, 2);
    EXPECT_EQ(ck::exactFrameRate(0.0).num, 24);
}

TEST(CorridorKey, TickAlignedStepMakesWholeTicks)
{
    EXPECT_EQ(ck::tickAlignedFrameStep({30000, 1001}), 5);   // 1601.6 ticks/frame
    EXPECT_EQ(ck::tickAlignedFrameStep({60000, 1001}), 5);   // 800.8
    EXPECT_EQ(ck::tickAlignedFrameStep({24000, 1001}), 1);   // 2002
    EXPECT_EQ(ck::tickAlignedFrameStep({24, 1}), 1);
    EXPECT_EQ(ck::tickAlignedFrameStep({25, 1}), 1);
    EXPECT_EQ(ck::framesToTicks(5, {30000, 1001}), 8008);
    EXPECT_EQ(ck::framesToTicks(4795, {30000, 1001}), 7679672);
}

// ── Planning ────────────────────────────────────────────────────────────────

TEST(CorridorKey, PlanCoversExactlyTheFramesTheClipShows)
{
    // Source frames 4800..4919 (4 s at 29.97); the last tick reaches into 4920.
    auto clip = cameraClip(10 * kTicksPerSecond, ck::framesToTicks(4800, {30000, 1001}),
                           ck::framesToTicks(120, {30000, 1001}));
    const auto result = ck::planRender(*clip, cameraInfo(), kSeqFrame);
    ASSERT_TRUE(result.plan) << result.problem;
    const auto& plan = *result.plan;
    EXPECT_EQ(plan.firstFrame, 4800);   // already on a 5-frame (whole-tick) boundary
    EXPECT_EQ(plan.lastFrame, 4920);
    EXPECT_EQ(plan.tickShift, ck::framesToTicks(plan.firstFrame, plan.fps));
    EXPECT_EQ(plan.keyRect, (ck::Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(plan.matrix, ColorMatrix::BT709);   // untagged HD

    // Off a boundary the start moves back to the previous whole tick only.
    clip->setSourceIn(ck::framesToTicks(4803, {30000, 1001}));
    EXPECT_EQ(ck::planRender(*clip, cameraInfo(), kSeqFrame).plan->firstFrame, 4800);
}

TEST(CorridorKey, PlanCoversTransitionsAcrossTheCutsClampedToTheSource)
{
    // Half-second dissolves on both cuts: 15 more frames each side.
    auto clip = cameraClip(10 * kTicksPerSecond, ck::framesToTicks(3000, {30000, 1001}),
                           ck::framesToTicks(90, {30000, 1001}));
    const ck::Coverage dissolves{kTicksPerSecond / 2, kTicksPerSecond / 2};
    const auto plan = ck::planRender(*clip, cameraInfo(), kSeqFrame, dissolves).plan;
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->firstFrame, 2985);   // 3000 - 15, a whole-tick boundary
    EXPECT_EQ(plan->lastFrame, 3000 + 90 + 15);

    // Near the ends of the media the coverage stops at the first/last frame.
    auto start = cameraClip(0, 0, ck::framesToTicks(80, {30000, 1001}));
    const auto clamped = ck::planRender(*start, cameraInfo(100), kSeqFrame,
                                        {2 * kTicksPerSecond, 2 * kTicksPerSecond}).plan;
    ASSERT_TRUE(clamped);
    EXPECT_EQ(clamped->firstFrame, 0);
    EXPECT_EQ(clamped->lastFrame, 99);
}

TEST(CorridorKey, TransitionCoverageCountsTwoClipTransitionsOnly)
{
    Track track(TrackType::Video, "V1");
    auto a = cameraClip(0, 0, 4 * kTicksPerSecond);
    auto b = cameraClip(4 * kTicksPerSecond, 0, 4 * kTicksPerSecond);
    auto c = cameraClip(8 * kTicksPerSecond, 0, 4 * kTicksPerSecond);
    VideoClip& A = *a;
    VideoClip& B = *b;
    VideoClip& C = *c;
    track.addClip(std::move(a));
    track.addClip(std::move(b));
    track.addClip(std::move(c));
    EXPECT_EQ(ck::transitionCoverage(track, B).lead, 0);

    Transition dissolve;   // 1 s cross-dissolve on the A|B cut
    dissolve.type = TransitionType::CrossDissolve;
    dissolve.duration = kTicksPerSecond;
    dissolve.leftClipId = A.id();
    dissolve.rightClipId = B.id();
    dissolve.editPointTick = 4 * kTicksPerSecond;
    track.addTransition(dissolve);
    Transition wipe;       // 2 s wipe on the B|C cut
    wipe.type = TransitionType::WipeLeft;
    wipe.duration = 2 * kTicksPerSecond;
    wipe.leftClipId = B.id();
    wipe.rightClipId = C.id();
    wipe.editPointTick = 8 * kTicksPerSecond;
    track.addTransition(wipe);
    Transition fade;       // fade-in at A's head: inside A, no extra frames
    fade.type = TransitionType::FadeFromBlack;
    fade.duration = kTicksPerSecond;
    fade.rightClipId = A.id();
    fade.editPointTick = 0;
    track.addTransition(fade);

    EXPECT_EQ(ck::transitionCoverage(track, A).lead, 0);
    EXPECT_EQ(ck::transitionCoverage(track, A).tail, kTicksPerSecond / 2);
    EXPECT_EQ(ck::transitionCoverage(track, B).lead, kTicksPerSecond / 2);
    EXPECT_EQ(ck::transitionCoverage(track, B).tail, kTicksPerSecond);
    EXPECT_EQ(ck::transitionCoverage(track, C).lead, kTicksPerSecond);
    EXPECT_EQ(ck::transitionCoverage(track, C).tail, 0);
}

TEST(CorridorKey, PlanKeysOnlyTheCroppedAreaWithAMargin)
{
    auto clip = cameraClip(0, 0, ck::framesToTicks(30, {30000, 1001}));
    clip->setCrop(25.0f, 25.0f, 0.0f, 10.0f);   // 480..1440 x 0..972
    const auto plan = ck::planRender(*clip, cameraInfo(), kSeqFrame).plan;
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->keyRect, (ck::Rect{464, 0, 992, 988}));
    EXPECT_EQ(plan->keyRect.x % 2, 0);
    EXPECT_EQ(plan->keyRect.w % 2, 0);
}

TEST(CorridorKey, PlanRefusesFootageItCannotSwapExactly)
{
    auto clip = cameraClip(0, 0, kTicksPerSecond);
    VideoStreamInfo info = cameraInfo();
    info.rotation = 90;
    EXPECT_FALSE(ck::planRender(*clip, info, kSeqFrame).plan);
    info = cameraInfo();
    info.isVFR = true;
    EXPECT_FALSE(ck::planRender(*clip, info, kSeqFrame).plan);
    info = cameraInfo();
    info.hasAlpha = true;
    EXPECT_FALSE(ck::planRender(*clip, info, kSeqFrame).plan);
    EXPECT_FALSE(ck::planRender(*clip, cameraInfo(1), kSeqFrame).plan);
    info = cameraInfo();
    info.colorTransfer = ColorTransfer::PQ;
    const auto hdr = ck::planRender(*clip, info, kSeqFrame);
    EXPECT_FALSE(hdr.plan);
    EXPECT_FALSE(hdr.problem.empty());
}

// ── The swap ────────────────────────────────────────────────────────────────

TEST(CorridorKey, SwapShowsTheSameFrameEverywhereAtNtsc)
{
    auto clip = cameraClip(3 * kTicksPerSecond, 7'654'321, 5 * kTicksPerSecond);
    expectSameFramesAfterSwap(*clip, 160);   // tenth-of-a-frame steps
}

TEST(CorridorKey, SwapShowsTheSameFrameAtDoubleSpeed)
{
    auto clip = cameraClip(0, ck::framesToTicks(900, {30000, 1001}), 2 * kTicksPerSecond, 2.0);
    expectSameFramesAfterSwap(*clip, 400);
}

TEST(CorridorKey, SwapShowsTheSameFrameUnderASpeedRamp)
{
    auto clip = cameraClip(0, ck::framesToTicks(3000, {30000, 1001}), 3 * kTicksPerSecond);
    clip->speedRamp().addKeyframe(0, 0.5f);
    clip->speedRamp().addKeyframe(3 * kTicksPerSecond, 2.0f);
    expectSameFramesAfterSwap(*clip, 800);
}

TEST(CorridorKey, SwapShowsTheSameFrameThroughDissolvesAcrossTheCuts)
{
    auto clip = cameraClip(6 * kTicksPerSecond, 7'654'321, 4 * kTicksPerSecond);
    expectSameFramesAfterSwap(*clip, 160, {kTicksPerSecond / 2, kTicksPerSecond});
}

TEST(CorridorKey, RenderCoversNoticesWhatTheRenderLacks)
{
    auto clip = cameraClip(6 * kTicksPerSecond, ck::framesToTicks(3000, {30000, 1001}),
                           4 * kTicksPerSecond);
    const ck::Coverage half{kTicksPerSecond / 2, kTicksPerSecond / 2};
    const auto plan = ck::planRender(*clip, cameraInfo(), kSeqFrame, half).plan;
    ASSERT_TRUE(plan);
    const ck::RenderRecord record = ck::makeRecord(*plan, *clip, "k");
    ck::useRender(*clip, "r.mov", record, ultraKeyId(*clip));
    EXPECT_TRUE(ck::renderCovers(*clip, record, half, kSeqFrame));
    EXPECT_TRUE(ck::renderCovers(*clip, record, {}, kSeqFrame));          // dissolve removed: fine
    EXPECT_FALSE(ck::renderCovers(*clip, record, {kTicksPerSecond, 0}, kSeqFrame));  // longer one
    EXPECT_FALSE(ck::renderCovers(*clip, record, {0, kTicksPerSecond}, kSeqFrame));
    clip->setDuration(clip->duration() + kTicksPerSecond);               // trimmed longer
    EXPECT_FALSE(ck::renderCovers(*clip, record, half, kSeqFrame));
    clip->setDuration(clip->duration() - kTicksPerSecond);
    clip->setCrop(10.0f, 0.0f, 0.0f, 0.0f);                               // cropping in: fine
    EXPECT_TRUE(ck::renderCovers(*clip, record, half, kSeqFrame));
    auto narrow = cameraClip(0, 0, kTicksPerSecond);
    narrow->setCrop(25.0f, 25.0f, 0.0f, 0.0f);
    const auto narrowPlan = ck::planRender(*narrow, cameraInfo(), kSeqFrame).plan;
    ASSERT_TRUE(narrowPlan);
    const ck::RenderRecord narrowRecord = ck::makeRecord(*narrowPlan, *narrow, "n");
    ck::useRender(*narrow, "n.mov", narrowRecord, ultraKeyId(*narrow));
    narrow->setCrop(0.0f, 0.0f, 0.0f, 0.0f);                              // crop opened up
    EXPECT_FALSE(ck::renderCovers(*narrow, narrowRecord, {}, kSeqFrame));
}

TEST(CorridorKey, SwapShowsTheSameFrameAt24)
{
    auto clip = cameraClip(0, 48'000, 4 * kTicksPerSecond, 1.0, 24.0);
    const VideoStreamInfo source = cameraInfo(27990, 24.0);
    const auto plan = ck::planRender(*clip, source, 2000).plan;
    ASSERT_TRUE(plan);
    const int64_t before = mapTickToSourceFrame(*clip, 50'000, &source).frame;
    ck::useRender(*clip, "r.mov", ck::makeRecord(*plan, *clip, "k"), ultraKeyId(*clip));
    VideoStreamInfo render = cameraInfo(plan->frameCount(), 24.0);
    EXPECT_EQ(mapTickToSourceFrame(*clip, 50'000, &render).frame + plan->firstFrame, before);
}

TEST(CorridorKey, UseRenderThenUseSourceRestoresTheClip)
{
    auto clip = cameraClip(0, 7'654'321, 5 * kTicksPerSecond);
    const auto plan = ck::planRender(*clip, cameraInfo(), kSeqFrame).plan;
    ASSERT_TRUE(plan);
    const ck::RenderRecord record = ck::makeRecord(*plan, *clip, "k");
    const uint64_t fx = ultraKeyId(*clip);

    ck::useRender(*clip, "C:/renders/r.mov", record, fx);
    EXPECT_EQ(clip->mediaPath(), "C:/renders/r.mov");
    EXPECT_EQ(clip->sourceIn(), 7'654'321 - plan->tickShift);
    EXPECT_EQ(clip->sourceDuration(), ck::framesToTicks(plan->frameCount(), plan->fps));
    EXPECT_EQ(clip->sourceCodecName(), "prores");
    EXPECT_FALSE(clip->effects().effectById(fx)->isEnabled());

    ck::useSource(*clip, record, fx);
    EXPECT_EQ(clip->mediaPath(), "C:/footage/take one.mp4");
    EXPECT_EQ(clip->sourceIn(), 7'654'321);
    EXPECT_EQ(clip->sourceDuration(), 27990 * 1601);
    EXPECT_EQ(clip->sourceCodecName(), "h264");
    EXPECT_TRUE(clip->effects().effectById(fx)->isEnabled());
}

// ── Command lines ───────────────────────────────────────────────────────────

TEST(CorridorKey, ExtractSeeksHalfAFrameEarlyAndConvertsLikeTheApp)
{
    ck::RenderPlan plan;
    plan.sourcePath = "C:/footage/a.mp4";
    plan.fps = {30000, 1001};
    plan.firstFrame = 4770;
    plan.lastFrame = 4949;
    plan.sourceWidth = 3840;
    plan.sourceHeight = 2160;
    plan.keyRect = {744, 0, 2224, 2160};
    plan.matrix = ColorMatrix::BT709;
    const auto args = ck::extractArgs(plan, "C:/jobs/1/frames");
    auto after = [&](const std::string& flag) {
        auto it = std::find(args.begin(), args.end(), flag);
        return it == args.end() || it + 1 == args.end() ? std::string() : *(it + 1);
    };
    // Stream timestamps (the app's frame grid), seeking three frames early.
    EXPECT_NE(std::find(args.begin(), args.end(), "-copyts"), args.end());
    EXPECT_EQ(after("-seek_timestamp"), "1");
    EXPECT_EQ(after("-ss"), "159.058900");   // (4770 - 3) * 1001 / 30000
    EXPECT_EQ(after("-vf"),
              "fps=30000/1001:round=near,trim=start_pts=4770:end_pts=4950,"
              "crop=2224:2160:744:0,scale=in_color_matrix=bt709:in_range=tv:out_range=pc,format=rgb24");
    EXPECT_EQ(after("-fps_mode"), "passthrough");
    EXPECT_EQ(after("-start_number"), "0");

    plan.firstFrame = 0;
    plan.keyRect = {0, 0, 3840, 2160};
    plan.matrix = ColorMatrix::BT601;
    plan.fullRange = true;
    const auto first = ck::extractArgs(plan, "C:/jobs/1/frames");
    EXPECT_EQ(std::find(first.begin(), first.end(), "-ss"), first.end());
    const auto vf = std::find(first.begin(), first.end(), "-vf");
    ASSERT_NE(vf, first.end());
    EXPECT_EQ(*(vf + 1), "fps=30000/1001:round=near:start_time=0,trim=start_pts=0:end_pts=4950,"
                         "scale=in_color_matrix=bt601:in_range=pc:out_range=pc,format=rgb24");
}

TEST(CorridorKey, WorkerArgsCarryTheCanvasAndExactRate)
{
    const ck::Layout layout = ck::layoutFor("C:/app");
    ck::RenderPlan plan;
    plan.fps = {30000, 1001};
    plan.sourceWidth = 3840;
    plan.sourceHeight = 2160;
    plan.keyRect = {744, 16, 2224, 2000};
    const auto args = ck::workerArgs(layout, plan, "C:/j/frames", "C:/r/out.mov", "C:/j/tmp",
                                     "C:/app/ffmpeg.exe", 8.0);
    auto after = [&](const std::string& flag) {
        auto it = std::find(args.begin(), args.end(), flag);
        return it == args.end() ? std::string() : *(it + 1);
    };
    EXPECT_EQ(args[0], "-I");
    EXPECT_EQ(after("--fps-num"), "30000");
    EXPECT_EQ(after("--fps-den"), "1001");
    EXPECT_EQ(after("--canvas"), "3840x2160");
    EXPECT_EQ(after("--offset"), "744,16");
    EXPECT_EQ(after("--vram-gb"), "8.0");
    EXPECT_EQ(utf8ToPath(after("--models")), layout.modelsDir());
}

TEST(CorridorKey, ParsesWorkerProtocol)
{
    auto e = ck::parseWorkerLine("RT_PROGRESS 0.4250 Keying\r\n");
    EXPECT_EQ(e.kind, ck::WorkerEvent::Kind::Progress);
    EXPECT_DOUBLE_EQ(e.progress, 0.425);
    EXPECT_EQ(e.text, "Keying");
    e = ck::parseWorkerLine("RT_PROGRESS 1.7 Done");
    EXPECT_DOUBLE_EQ(e.progress, 1.0);
    e = ck::parseWorkerLine("RT_DONE C:/r/out.mov");
    EXPECT_EQ(e.kind, ck::WorkerEvent::Kind::Done);
    EXPECT_EQ(e.text, "C:/r/out.mov");
    e = ck::parseWorkerLine("RT_ERROR CUDA out of memory");
    EXPECT_EQ(e.kind, ck::WorkerEvent::Kind::Error);
    e = ck::parseWorkerLine("  window 3/9 [8:16]");
    EXPECT_EQ(e.kind, ck::WorkerEvent::Kind::Log);
    e = ck::parseWorkerLine("RT_PROGRESS nonsense");
    EXPECT_EQ(e.kind, ck::WorkerEvent::Kind::Log);
}

// ── Record ──────────────────────────────────────────────────────────────────

TEST(CorridorKey, RecordRoundTripsBesideAUnicodeRender)
{
    const fs::path dir = fs::temp_directory_path() / utf8ToPath("rt_ck_test_\xEF\xBD\x9C");
    fs::create_directories(dir);
    const fs::path render = dir / utf8ToPath("take \xEF\xBD\x9C one CK 0-99 abcd.mov");
    std::ofstream(render) << "x";

    ck::RenderRecord r;
    r.sourcePath = "G:/footage/C0001 \xEF\xBD\x9C.MP4";
    r.sourceCodec = "h264";
    r.sourceDuration = 44828784;
    r.fps = {30000, 1001};
    r.firstFrame = 4770;
    r.lastFrame = 4949;
    r.tickShift = 7639632;
    r.keyRect = {744, 0, 2224, 2160};
    r.sourceWidth = 3840;
    r.sourceHeight = 2160;
    r.steadiness = ck::Steadiness::Steadier;
    r.key = "abcd1234";
    r.codeCommit = std::string(ck::kCodeCommit);
    ASSERT_TRUE(ck::writeRecord(ck::recordPathFor(render), r));

    const auto back = ck::recordForMedia(pathToUtf8(render));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->sourcePath, r.sourcePath);
    EXPECT_EQ(back->sourceCodec, "h264");
    EXPECT_EQ(back->sourceDuration, r.sourceDuration);
    EXPECT_EQ(back->fps.num, 30000);
    EXPECT_EQ(back->fps.den, 1001);
    EXPECT_EQ(back->firstFrame, 4770);
    EXPECT_EQ(back->lastFrame, 4949);
    EXPECT_EQ(back->tickShift, 7639632);
    EXPECT_EQ(back->keyRect, r.keyRect);
    EXPECT_EQ(back->sourceWidth, 3840);
    EXPECT_EQ(back->sourceHeight, 2160);
    EXPECT_EQ(back->steadiness, ck::Steadiness::Steadier);
    EXPECT_EQ(back->key, "abcd1234");
    EXPECT_FALSE(ck::recordForMedia("C:/footage/not-a-render.mp4"));
    fs::remove_all(dir);
}

// ── Install + GPU ───────────────────────────────────────────────────────────

TEST(CorridorKey, InstalledOnlyWithStampVenvAndEveryModel)
{
    const fs::path root = fs::temp_directory_path() / "rt_ck_layout_test";
    fs::remove_all(root);
    const ck::Layout layout = ck::layoutFor(root);
    auto touch = [](const fs::path& p) { fs::create_directories(p.parent_path()); std::ofstream(p) << "x"; };
    touch(layout.worker());
    touch(layout.codeDir / "corridorkey" / "runtime.py");
    touch(layout.venvPython());
    for (const auto& f : ck::modelFiles(layout)) touch(f);
    EXPECT_FALSE(ck::isInstalled(layout));   // no stamp yet
    std::ofstream(layout.stampFile()) << ck::stampText();
    EXPECT_TRUE(ck::isInstalled(layout));
    fs::remove(ck::modelFiles(layout).back());
    EXPECT_FALSE(ck::isInstalled(layout));
    touch(ck::modelFiles(layout).back());
    std::ofstream(layout.stampFile()) << "setup=0\n";
    EXPECT_FALSE(ck::isInstalled(layout));   // older setup version
    fs::remove_all(root);
}

TEST(CorridorKey, GpuCheck)
{
    const auto rtx = ck::parseNvidiaSmiLine("NVIDIA GeForce RTX 4090, 8.9, 616.56, 24564");
    ASSERT_TRUE(rtx);
    EXPECT_EQ(rtx->name, "NVIDIA GeForce RTX 4090");
    EXPECT_DOUBLE_EQ(rtx->computeCapability, 8.9);
    EXPECT_EQ(rtx->driverMajor, 616);
    EXPECT_NEAR(rtx->memoryGB, 23.99, 0.01);
    EXPECT_TRUE(ck::gpuProblem(*rtx).empty());

    const auto gtx = ck::parseNvidiaSmiLine("NVIDIA GeForce GTX 1080, 6.1, 560.94, 8192");
    ASSERT_TRUE(gtx);
    EXPECT_NE(ck::gpuProblem(*gtx).find("RTX 30-series"), std::string::npos);
    const auto oldDriver = ck::parseNvidiaSmiLine("NVIDIA GeForce RTX 3060, 8.6, 552.22, 12288");
    ASSERT_TRUE(oldDriver);
    EXPECT_NE(ck::gpuProblem(*oldDriver).find("driver"), std::string::npos);
    EXPECT_FALSE(ck::parseNvidiaSmiLine("No devices were found"));

    EXPECT_DOUBLE_EQ(ck::vramCapGB(24.0), 8.0);
    EXPECT_NEAR(ck::vramCapGB(12.0), 4.2, 1e-9);
    EXPECT_DOUBLE_EQ(ck::vramCapGB(8.0), 4.0);
    EXPECT_DOUBLE_EQ(ck::vramCapGB(6.0), 4.0);
    using S = ck::Steadiness;
    EXPECT_DOUBLE_EQ(ck::vramCapGB(24.0, S::Steadier), 15.6);
    EXPECT_DOUBLE_EQ(ck::vramCapGB(32.0, S::Steadier), 16.0);
    EXPECT_NEAR(ck::vramCapGB(12.0, S::Steadier), 7.8, 1e-9);
    EXPECT_NEAR(ck::vramCapGB(8.0, S::Steadier), 5.2, 1e-9);
    EXPECT_DOUBLE_EQ(ck::vramCapGB(6.0, S::Steadier), 4.0);
}

TEST(CorridorKey, EstimatesAndFormatsDuration)
{
    ck::RenderPlan plan;
    plan.firstFrame = 0;
    plan.lastFrame = 119;
    plan.keyRect = {0, 0, 1920, 1080};
    EXPECT_NEAR(ck::estimateSeconds(plan), 25.0 + 120 * 2.0736 * 0.67, 1e-6);
    EXPECT_EQ(ck::formatDuration(42.0), "about 45 s");
    EXPECT_EQ(ck::formatDuration(200.0), "about 4 min");
    EXPECT_EQ(ck::formatDuration(2.5 * 3600.0), "about 2.5 h");
}

TEST(CorridorKey, RenderNamesAreStableAndSafe)
{
    ck::RenderPlan plan;
    plan.sourcePath = "G:/footage/C0001 \xEF\xBD\x9C take.MP4";
    plan.firstFrame = 10;
    plan.lastFrame = 99;
    const std::string key = ck::renderKey(plan, 1000, 42);
    EXPECT_EQ(key.size(), 8u);
    EXPECT_EQ(key, ck::renderKey(plan, 1000, 42));
    EXPECT_NE(key, ck::renderKey(plan, 1000, 43));   // source changed
    plan.steadiness = ck::Steadiness::Steadier;
    EXPECT_NE(key, ck::renderKey(plan, 1000, 42));   // Steadier is a different render
    plan.steadiness = ck::Steadiness::Standard;
    EXPECT_EQ(ck::renderFileName(plan, key), "C0001 \xEF\xBD\x9C take CK 10-99 " + key + ".mov");
}

} // namespace
} // namespace rt
