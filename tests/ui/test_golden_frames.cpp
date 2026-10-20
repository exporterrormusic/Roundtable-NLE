/*
 * test_golden_frames.cpp — whole-timeline pixel regression harness.
 *
 * Builds synthetic projects in code, renders chosen frames through the real
 * compositor and checks them three ways:
 *   1. frame identity — every source video frame carries its index as a
 *      stripe barcode, read back from the composited output (exact on any
 *      machine: catches clip-start jitter, cadence, trim and cut mistakes);
 *   2. preview == export — each frame is rendered by a preview compositor
 *      (the paused Program Monitor's request) and by an ExportRenderSession
 *      from the same published snapshot, and must be byte-identical;
 *   3. golden images — small reference PNGs in tests/golden, compared with
 *      a tolerance. These are per machine: after a GPU/driver change, or to
 *      accept an intended visual change, re-run with ROUNDTABLE_GOLDEN_UPDATE=1
 *      and review the changed PNGs before committing them.
 * Composited cases also save → load the project and must render identically.
 *
 * Failures write actual/expected/diff PNGs to ROUNDTABLE_GOLDEN_FAILURE_DIR.
 * Needs a Vulkan device; skips without one.
 */

#include <gtest/gtest.h>

#include "CompositeService.h"
#include "Constants.h"
#include "GpuContext.h"
#include "cache/FrameCache.h"
#include "decode/VideoDecoder.h"
#include "effects/Effect.h"
#include "panels/export/ExportRenderSession.h"
#include "playback/EngineContracts.h"
#include "playback/MediaPool.h"
#include "playback/SnapshotPublisher.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/ImageClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/SequenceClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include <QApplication>
#include <QColor>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

using namespace rt;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kW = 256, kH = 144;          // sequence == source == output
constexpr double   kSeqFps = 30.0;
constexpr int64_t  kFrameTicks = kTicksPerSecond / 30;
constexpr int      kBits = 8, kBandH = 48, kGop = 45, kVideoFrames = 240;

int64_t seconds(double s) { return static_cast<int64_t>(std::llround(s * kTicksPerSecond)); }
int64_t frames(int64_t n) { return n * kFrameTicks; }

// ─── Fixtures ────────────────────────────────────────────────────────────────

/// 256×144 MPEG-4 (GOP 45, 2 B-frames): frame i carries i as 8 stripes in
/// the top band, with a gradient below that moves with i.
bool writeIndexedVideo(const std::string& path, int fps)
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
    enc->time_base = AVRational{1, fps};
    enc->framerate = AVRational{fps, 1};
    enc->gop_size = kGop;
    enc->max_b_frames = 2;
    enc->bit_rate = 4'000'000;
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
            if (pkt->duration <= 0) pkt->duration = 1;   // else the fps reads wrong
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (av_interleaved_write_frame(fmt, pkt) < 0) return false;
        }
        return true;
    };
    for (int i = 0; ok && i < kVideoFrames; ++i) {
        ok = av_frame_make_writable(frame) >= 0;
        for (int y = 0; y < static_cast<int>(kH); ++y)
            for (int x = 0; x < static_cast<int>(kW); ++x) {
                uint8_t luma;
                if (y < kBandH) {
                    const int bit = x / (kW / kBits);
                    luma = ((i >> bit) & 1) ? 235 : 16;
                } else {
                    luma = static_cast<uint8_t>(32 + ((x + 2 * i) % kW) * 180 / kW);
                }
                frame->data[0][y * frame->linesize[0] + x] = luma;
            }
        for (int y = 0; y < static_cast<int>(kH) / 2; ++y)
            for (int x = 0; x < static_cast<int>(kW) / 2; ++x) {
                const bool band = y < kBandH / 2;
                frame->data[1][y * frame->linesize[1] + x] =
                    band ? 128 : static_cast<uint8_t>(64 + y * 2);
                frame->data[2][y * frame->linesize[2] + x] =
                    band ? 128 : static_cast<uint8_t>(192 - x);
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

QImage colorBars()
{
    QImage image(kW, kH, QImage::Format_ARGB32);
    const QColor bars[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green,
                           Qt::magenta, Qt::red, Qt::blue, Qt::black};
    QPainter p(&image);
    for (int i = 0; i < 8; ++i)
        p.fillRect(i * kW / 8, 0, kW / 8, kH * 2 / 3, bars[i]);
    for (int x = 0; x < static_cast<int>(kW); ++x)
        p.fillRect(x, kH * 2 / 3, 1, kH - kH * 2 / 3, QColor(x, x, x));
    return image;
}

QImage checker()
{
    QImage image(kW, kH, QImage::Format_ARGB32);
    for (int y = 0; y < static_cast<int>(kH); ++y)
        for (int x = 0; x < static_cast<int>(kW); ++x)
            image.setPixel(x, y, ((x / 16 + y / 16) & 1) ? qRgb(200, 200, 210)
                                                        : qRgb(40, 60, 110));
    return image;
}

// ─── Frame helpers ───────────────────────────────────────────────────────────

const uint8_t* pixelAt(const CachedFrame& f, uint32_t x, uint32_t y)
{
    const uint32_t stride = f.stride ? f.stride : f.width * 4;
    return f.pixels.data() + static_cast<size_t>(y) * stride + x * 4;
}

/// Source frame index from the barcode (output == source geometry).
int frameIndexOf(const CachedFrame& f)
{
    if (f.width < kW || f.height < kH || f.pixels.empty()) return -1;
    const uint32_t stripe = kW / kBits;
    int index = 0;
    for (int bit = 0; bit < kBits; ++bit)
        if (pixelAt(f, bit * stripe + stripe / 2, kBandH / 2)[1] > 128)
            index |= 1 << bit;
    return index;
}

double meanLuma(const CachedFrame& f)
{
    double sum = 0.0;
    for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
            const uint8_t* p = pixelAt(f, x, y);
            sum += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
        }
    return sum / std::max<double>(1.0, double(f.width) * f.height);
}

bool samePixels(const CachedFrame& a, const CachedFrame& b)
{
    if (a.width != b.width || a.height != b.height) return false;
    for (uint32_t y = 0; y < a.height; ++y)
        if (std::memcmp(pixelAt(a, 0, y), pixelAt(b, 0, y), a.width * 4) != 0)
            return false;
    return true;
}

QImage toImage(const CachedFrame& f)
{
    const uint32_t stride = f.stride ? f.stride : f.width * 4;
    return QImage(f.pixels.data(), int(f.width), int(f.height), int(stride),
                  QImage::Format_ARGB32).copy();
}

// ─── Render harness (GPU, pool, both compositors) ────────────────────────────

struct Harness
{
    fs::path dir;
    std::map<int, std::string> videos;           // fps → indexed video
    std::string bars, checker;
    std::shared_ptr<FrameCache> cache;
    std::unique_ptr<MediaPool> pool;
    std::unique_ptr<CompositeService> preview;
    std::unique_ptr<ExportRenderSession> exporter;
    const void* previewScene{nullptr};
};

Harness* g_harness = nullptr;
bool g_gpu = false;

class GoldenEnvironment : public ::testing::Environment
{
public:
    void SetUp() override
    {
        static int argc = 1;
        static const char* argv[] = {"test_golden_frames"};
        if (!QApplication::instance())
            new QApplication(argc, const_cast<char**>(argv));

        g_gpu = GpuContext::get().init();
        if (!g_gpu) return;
        auto* h = new Harness;
        // Per process: ctest runs each test as its own process, in parallel.
        h->dir = fs::temp_directory_path() /
                 ("roundtable_golden_fixtures_" +
                  std::to_string(QCoreApplication::applicationPid()));
        std::error_code ec;
        fs::remove_all(h->dir, ec);
        fs::create_directories(h->dir);
        for (int fps : {24, 30, 60}) {
            const auto path = (h->dir / ("indexed_" + std::to_string(fps) + ".mp4")).string();
            if (writeIndexedVideo(path, fps)) h->videos[fps] = path;
        }
        h->bars = (h->dir / "bars.png").string();
        h->checker = (h->dir / "checker.png").string();
        colorBars().save(QString::fromStdString(h->bars));
        checker().save(QString::fromStdString(h->checker));

        h->cache = std::make_shared<FrameCache>(512ull * 1024 * 1024);
        h->pool = std::make_unique<MediaPool>(h->cache);
        h->pool->onGpuContextReady();
        h->pool->setProjectFps(kSeqFps);
        h->preview = std::make_unique<CompositeService>();
        h->preview->setMediaPool(h->pool.get());
        h->preview->setPlaybackTier(ResolutionTier::Full);
        ExportRenderSession::Dependencies deps;
        deps.mediaPool = h->pool.get();
        h->exporter = std::make_unique<ExportRenderSession>(deps);
        g_harness = h;
    }

    void TearDown() override
    {
        if (g_harness) {
            g_harness->exporter.reset();
            g_harness->preview->shutdown();
            g_harness->preview.reset();
            g_harness->pool.reset();
            g_harness->cache.reset();
            std::error_code ec;
            fs::remove_all(g_harness->dir, ec);
            delete g_harness;
            g_harness = nullptr;
        }
        if (g_gpu) GpuContext::get().shutdown();
    }
};

[[maybe_unused]] auto* const g_env =
    ::testing::AddGlobalTestEnvironment(new GoldenEnvironment);

#define REQUIRE_GPU()                                                         \
    do {                                                                      \
        if (!g_harness) GTEST_SKIP() << "No Vulkan device";                   \
    } while (0)

/// A project under test plus the publisher that snapshots it, as the app has.
struct Scene
{
    std::unique_ptr<Project> project;
    SnapshotPublisher publisher;
    std::shared_ptr<RenderSnapshot> exportSnapshot;

    Scene() : project(std::make_unique<Project>())
    {
        for (size_t i = 0; i < project->sequenceCount(); ++i) setup(*project->sequence(i));
    }
    explicit Scene(std::unique_ptr<Project> loaded) : project(std::move(loaded)) {}

    static void setup(Timeline& seq)
    {
        seq.settings().setResolution(kW, kH);
        seq.settings().setFrameRate(kSeqFps);
    }
    Timeline& seq(size_t i = 0) { return *project->sequence(i); }
    /// `count` video tracks, returned bottom-up ([0] = V1). Track lists run
    /// top to bottom and addVideoTrack appends below, so create top-first.
    std::vector<Track*> videoTracks(int count, Timeline* in = nullptr)
    {
        Timeline& target = in ? *in : seq();
        std::vector<Track*> tracks(static_cast<size_t>(count));
        for (int i = count; i >= 1; --i)
            tracks[static_cast<size_t>(i - 1)] = target.addVideoTrack("V" + std::to_string(i));
        return tracks;
    }
    Timeline& addSequence(const std::string& name)
    {
        Timeline* s = project->addSequence(name);
        setup(*s);
        return *s;
    }

    /// Snapshot after the scene is built (or changed).
    void publish()
    {
        publisher.bind(project.get(), &seq());
        publisher.publish();
        exportSnapshot = std::make_shared<RenderSnapshot>(*publisher.latest());
        exportSnapshot->rangeStartTick = 0;
        exportSnapshot->rangeEndTick = seconds(30);
    }
};

std::shared_ptr<CachedFrame> renderPreview(Scene& scene, int64_t tick, std::string* why)
{
    auto& h = *g_harness;
    if (h.previewScene != &scene) {
        h.preview->setProject(scene.project.get());
        h.preview->setTimeline(&scene.seq());
        h.preview->setSnapshotPublisher(&scene.publisher);
        h.preview->invalidateCacheDirect();
        h.previewScene = &scene;
    }
    // The paused Program Monitor's request (TimelineWorkspace::renderFrame),
    // read back to the CPU like its Export Frame: a GPU-display frame has no
    // readback, and the composite work is the same either way.
    RenderRequest request;
    request.type = RenderRequestType::Still;
    request.quality = RenderQuality::Full;
    request.exactness = RenderExactness::ExactRequired;
    request.timelineTick = tick;
    request.outputWidth = kW;
    request.outputHeight = kH;
    request.stillFrame = true;
    request.preferGpuOutput = false;
    request.caller = "test_golden_frames";
    // Still requests never block on decode: a miss shows a nearby cached
    // frame (marked provisional) while the exact one decodes, and the monitor
    // keeps re-rendering the tick — so does this.
    RenderResult result;
    for (int attempt = 0; attempt < 400; ++attempt) {
        result = h.preview->renderFrame(request);
        if (result.status != RenderResultStatus::Pending &&
            !result.diagnostics.provisionalSource)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (result.diagnostics.provisionalSource) {
        *why = "preview still provisional after 4 s";
        return nullptr;
    }
    if (!result.frame || !result.frame->ensurePixels()) {
        *why = "preview " + std::string(toString(result.status)) +
               ": " + result.diagnostics.warning;
        return nullptr;
    }
    return result.frame;
}

std::shared_ptr<CachedFrame> renderExport(Scene& scene, int64_t tick, std::string* why)
{
    auto& h = *g_harness;
    if (!scene.exportSnapshot) scene.publish();
    for (int attempt = 0; attempt < 400; ++attempt) {
        const auto preflight = h.exporter->preflight(scene.exportSnapshot);
        if (preflight.status == RenderResultStatus::Ready) break;
        if (preflight.status != RenderResultStatus::Pending) {
            *why = "export preflight: " + preflight.warning;
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto result = h.exporter->renderFrame(scene.exportSnapshot, tick, kW, kH,
                                                /*scrubMode=*/false, /*preserveAlpha=*/false);
    if (!result.frame || !result.frame->ensurePixels()) {
        *why = "export " + std::string(toString(result.status)) +
               ": " + result.diagnostics.warning;
        return nullptr;
    }
    return result.frame;
}

/// Render through both paths; they must agree byte for byte. Returns the
/// export frame (null on failure, with a recorded test failure).
std::shared_ptr<CachedFrame> renderBoth(Scene& scene, int64_t tick)
{
    if (!scene.exportSnapshot) scene.publish();
    std::string why;
    auto exported = renderExport(scene, tick, &why);
    EXPECT_NE(exported, nullptr) << "tick " << tick << ": " << why;
    auto previewed = renderPreview(scene, tick, &why);
    EXPECT_NE(previewed, nullptr) << "tick " << tick << ": " << why;
    if (!exported || !previewed) return nullptr;
    if (!samePixels(*exported, *previewed)) {
        int maxDiff = 0;
        size_t differing = 0;
        for (uint32_t y = 0; y < exported->height; ++y)
            for (uint32_t x = 0; x < exported->width; ++x) {
                const uint8_t* a = pixelAt(*exported, x, y);
                const uint8_t* b = pixelAt(*previewed, x, y);
                int m = 0;
                for (int c = 0; c < 4; ++c) m = std::max(m, std::abs(a[c] - b[c]));
                if (m) ++differing;
                maxDiff = std::max(maxDiff, m);
            }
        ADD_FAILURE() << "preview and export differ at frame " << tick / kFrameTicks
                      << ": " << differing << " pixels, max channel diff " << maxDiff
                      << ", barcode export=" << frameIndexOf(*exported)
                      << " preview=" << frameIndexOf(*previewed);
    }
    return exported;
}

/// Save → load → render: must match the original export render exactly.
/// Export the same tick as NV12 (an 8-bit 4:2:0 encoder's format): the GPU
/// conversion of the composite must equal the CPU encoder's conversion of the
/// BGRA export frame within one code value (test_rgb_to_nv12 pins the
/// converter; this pins the export plumbing, nested composites included).
void expectNv12MatchesBgra(Scene& scene, int64_t tick, const CachedFrame& bgra)
{
    auto& h = *g_harness;
    if (!scene.exportSnapshot) scene.publish();
    auto nv12Snapshot = std::make_shared<RenderSnapshot>(*scene.exportSnapshot);
    nv12Snapshot->outputFormat = RenderOutputFormat::Nv12;
    const auto result = h.exporter->renderFrame(nv12Snapshot, tick, kW, kH,
                                                /*scrubMode=*/false, /*preserveAlpha=*/false);
    ASSERT_TRUE(result.frame) << toString(result.status);
    const CachedFrame& frame = *result.frame;
    ASSERT_TRUE(frame.hasNv12()) << "export did not come back as NV12";
    EXPECT_TRUE(frame.pixels.empty());
    ASSERT_GE(frame.nv12Stride, kW);

    // The CPU path: swscale BGRA -> NV12, BT.709, as FfmpegEncoderBase sets it up.
    std::vector<uint8_t> want(static_cast<size_t>(kW) * kH * 3 / 2);
    SwsContext* sws = sws_getContext(kW, kH, AV_PIX_FMT_BGRA, kW, kH, AV_PIX_FMT_NV12,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    ASSERT_NE(sws, nullptr);
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(sws, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    const uint8_t* src[] = {bgra.pixels.data()};
    const int srcStride[] = {static_cast<int>(bgra.stride ? bgra.stride : kW * 4)};
    uint8_t* dst[] = {want.data(), want.data() + kW * kH};
    const int dstStride[] = {static_cast<int>(kW), static_cast<int>(kW)};
    sws_scale(sws, src, srcStride, 0, kH, dst, dstStride);
    sws_freeContext(sws);

    int worst = 0;
    for (uint32_t row = 0; row < kH * 3 / 2; ++row)
        for (uint32_t x = 0; x < kW; ++x)
            worst = std::max(worst, std::abs(frame.nv12.get()[row * frame.nv12Stride + x] -
                                             want[row * kW + x]));
    EXPECT_LE(worst, 1) << "NV12 export differs from the BGRA export's conversion";
}

void expectRoundTrip(Scene& scene, int64_t tick, const CachedFrame& original)
{
    ProjectSerializer serializer;
    const auto bytes = serializer.serialize(*scene.project);
    auto loaded = serializer.deserialize(bytes);
    ASSERT_NE(loaded, nullptr) << "deserialize failed";
    Scene copy(std::move(loaded));
    copy.publish();
    std::string why;
    auto frame = renderExport(copy, tick, &why);
    ASSERT_NE(frame, nullptr) << why;
    EXPECT_TRUE(samePixels(*frame, original))
        << "the saved and reloaded project renders differently at tick " << tick;
}

// ─── Golden images ───────────────────────────────────────────────────────────

bool updateGoldens()
{
    const char* v = std::getenv("ROUNDTABLE_GOLDEN_UPDATE");
    return v && *v && std::string(v) != "0";
}

void expectGolden(const CachedFrame& frame, const std::string& name)
{
    const fs::path golden = fs::path(ROUNDTABLE_GOLDEN_DIR) / (name + ".png");
    const QImage actual = toImage(frame);
    if (updateGoldens()) {
        fs::create_directories(golden.parent_path());
        EXPECT_TRUE(actual.save(QString::fromStdWString(golden.wstring())))
            << "could not write " << golden.string();
        return;
    }
    QImage expected(QString::fromStdWString(golden.wstring()));
    if (expected.isNull()) {
        ADD_FAILURE() << "no golden image " << golden.string()
                      << " — run with ROUNDTABLE_GOLDEN_UPDATE=1 and review it";
        return;
    }
    expected = expected.convertToFormat(QImage::Format_ARGB32);

    bool sizeMatches = expected.size() == actual.size();
    int maxDiff = 0;
    size_t over1 = 0;
    QImage diff(actual.size(), QImage::Format_ARGB32);
    if (sizeMatches) {
        for (int y = 0; y < actual.height(); ++y) {
            const auto* a = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
            const auto* e = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
            auto* d = reinterpret_cast<QRgb*>(diff.scanLine(y));
            for (int x = 0; x < actual.width(); ++x) {
                const int dr = std::abs(qRed(a[x]) - qRed(e[x]));
                const int dg = std::abs(qGreen(a[x]) - qGreen(e[x]));
                const int db = std::abs(qBlue(a[x]) - qBlue(e[x]));
                const int m = std::max({dr, dg, db});
                maxDiff = std::max(maxDiff, m);
                if (m > 1) ++over1;
                d[x] = qRgb(std::min(255, dr * 8), std::min(255, dg * 8), std::min(255, db * 8));
            }
        }
    }
    const size_t pixels = size_t(actual.width()) * actual.height();
    const bool ok = sizeMatches && maxDiff <= 3 && over1 * 1000 <= pixels;
    if (ok) return;

    const fs::path out = fs::path(ROUNDTABLE_GOLDEN_FAILURE_DIR);
    fs::create_directories(out);
    actual.save(QString::fromStdWString((out / (name + "_actual.png")).wstring()));
    expected.save(QString::fromStdWString((out / (name + "_expected.png")).wstring()));
    if (sizeMatches)
        diff.save(QString::fromStdWString((out / (name + "_diff.png")).wstring()));
    ADD_FAILURE() << "golden mismatch: " << name << " (max channel diff " << maxDiff
                  << ", " << over1 << " of " << pixels << " pixels off by >1"
                  << (sizeMatches ? "" : ", size differs") << ")\n  see "
                  << (out / name).string() << "_{actual,expected,diff}.png";
}

// ─── Scene builders ──────────────────────────────────────────────────────────

VideoClip* addIndexedVideo(Track& track, int fps, int64_t timelineIn,
                           int64_t duration, int64_t sourceInFrames = 0,
                           double speed = 1.0)
{
    auto clip = std::make_unique<VideoClip>(g_harness->videos.at(fps));
    clip->setSourceFps(fps);
    clip->setSourceDuration(kVideoFrames * kTicksPerSecond / fps);
    clip->setSourceMetadata(kW, kH, 0);
    clip->setHasAudio(false);
    clip->setTimelineIn(timelineIn);
    clip->setDuration(duration);
    clip->setSourceIn(sourceInFrames * kTicksPerSecond / fps);
    clip->setSpeed(speed);
    return static_cast<VideoClip*>(track.addClip(std::move(clip)));
}

ImageClip* addImage(Track& track, const std::string& path, int64_t timelineIn,
                    int64_t duration)
{
    auto clip = std::make_unique<ImageClip>(path);
    clip->setSourceResolution(kW, kH);
    clip->setTimelineIn(timelineIn);
    clip->setDuration(duration);
    return static_cast<ImageClip*>(track.addClip(std::move(clip)));
}

/// Source frame a clip should show at `tick`, computed independently of the
/// app's mapping: rounded source position, clamped to the file.
int expectedFrame(const VideoClip& clip, int64_t tick)
{
    const double srcTicks = double(clip.sourceIn()) +
                            double(tick - clip.timelineIn()) * clip.speed();
    const auto frame = std::llround(srcTicks / kTicksPerSecond * clip.sourceFps());
    return int(std::clamp<long long>(frame, 0, kVideoFrames - 1));
}

/// Render `ticks` in order (as an export walks the timeline) and check every
/// frame shows the source frame `clipAt(tick)` should show.
void expectFrames(Scene& scene, const std::vector<int64_t>& ticks,
                  const std::function<const VideoClip*(int64_t)>& clipAt)
{
    for (int64_t tick : ticks) {
        auto frame = renderBoth(scene, tick);
        if (!frame) return;
        const VideoClip* clip = clipAt(tick);
        if (!clip) {
            EXPECT_LT(meanLuma(*frame), 2.0) << "gap at frame " << tick / kFrameTicks
                                             << " is not black";
            continue;
        }
        EXPECT_EQ(frameIndexOf(*frame), expectedFrame(*clip, tick))
            << "sequence frame " << tick / kFrameTicks;
    }
}

std::vector<int64_t> frameRange(int64_t first, int64_t last)
{
    std::vector<int64_t> ticks;
    for (int64_t f = first; f <= last; ++f) ticks.push_back(frames(f));
    return ticks;
}

std::string sanitize(std::string s)
{
    for (char& c : s)
        c = std::isalnum(static_cast<unsigned char>(c))
            ? char(std::tolower(static_cast<unsigned char>(c))) : '_';
    return s;
}

/// Move every parameter well off its default so the effect visibly acts.
void driveParams(Effect& effect)
{
    for (size_t i = 0; i < effect.paramCount(); ++i) {
        auto& p = effect.param(i);
        p.track.writeValue(0, p.minVal + 0.7f * (p.maxVal - p.minVal));
    }
}

} // namespace

// ─── A. Frame identity ───────────────────────────────────────────────────────

TEST(GoldenFrames, PlainClipShowsEverySourceFrameInOrder)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* clip = addIndexedVideo(*v1, 30, 0, frames(kVideoFrames));
    // Sequential through the first GOP boundary (45), then jumps.
    auto ticks = frameRange(0, 60);
    for (int64_t f : {120, 44, 45, 46, 239}) ticks.push_back(frames(f));
    expectFrames(scene, ticks, [&](int64_t) { return clip; });
}

TEST(GoldenFrames, TrimmedClipStartsExactlyOnItsInPoint)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    // In point 37 (mid-GOP), placed at 1 s, after a gap.
    auto* clip = addIndexedVideo(*v1, 30, seconds(1), frames(60), 37);
    expectFrames(scene, frameRange(25, 50), [&](int64_t tick) {
        return tick >= clip->timelineIn() ? clip : nullptr;
    });
}

TEST(GoldenFrames, SpeedChangesMapSourceFrames)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* slow = addIndexedVideo(*v1, 30, 0, frames(40), 10, 0.5);
    auto* fast = addIndexedVideo(*v1, 30, frames(40), frames(40), 20, 2.0);
    expectFrames(scene, frameRange(0, 79), [&](int64_t tick) {
        return tick < fast->timelineIn() ? slow : fast;
    });
}

TEST(GoldenFrames, ForeignFrameRatesFollowCadence)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* film = addIndexedVideo(*v1, 24, 0, frames(45));         // 24p in 30p
    auto* high = addIndexedVideo(*v1, 60, frames(45), frames(45)); // 60p in 30p
    expectFrames(scene, frameRange(0, 89), [&](int64_t tick) {
        return tick < high->timelineIn() ? film : high;
    });
}

TEST(GoldenFrames, CutsBetweenUsesOfOneSourceLandExactly)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    // Late section, then a backward jump to an early one, then forward past
    // where the first use was.
    auto* first = addIndexedVideo(*v1, 30, 0, frames(20), 150);
    auto* second = addIndexedVideo(*v1, 30, frames(20), frames(20), 12);
    auto* third = addIndexedVideo(*v1, 30, frames(40), frames(20), 200);
    expectFrames(scene, frameRange(0, 59), [&](int64_t tick) -> const VideoClip* {
        if (tick < second->timelineIn()) return first;
        if (tick < third->timelineIn()) return second;
        return third;
    });
}

// The paused monitor's decode pattern (seek, short forward jump, single
// steps) must land on the same pictures on the hardware decoder as on
// software.
TEST(GoldenFrames, HardwareDecoderStepsLikeSoftware)
{
    REQUIRE_GPU();
    auto barcode = [](VideoDecoder& d, const DecodedFrame& f) {
        DecodedFrame cpu;
        const DecodedFrame* use = &f;
        if (f.isHardware) {
            if (!d.transferHardwareFrame(f, cpu)) return -1;
            use = &cpu;
        }
        int index = 0;
        const uint8_t* row = use->data[0] + (kBandH / 2) * use->linesize[0];
        for (int bit = 0; bit < kBits; ++bit)
            if (row[bit * (kW / kBits) + kW / kBits / 2] > 128) index |= 1 << bit;
        return index;
    };
    for (bool software : {true, false}) {
        SCOPED_TRACE(software ? "software" : "hardware");
        VideoDecoder d;
        ASSERT_TRUE(d.open(g_harness->videos.at(30), software));
        DecodedFrame f;
        ASSERT_TRUE(d.seek(13 / 30.0, SeekMode::Precise));
        ASSERT_TRUE(d.decodeNext(f));
        EXPECT_EQ(barcode(d, f), 13) << "after a seek";
        ASSERT_TRUE(d.advanceTo(30 / 30.0));
        ASSERT_TRUE(d.decodeNext(f));
        EXPECT_EQ(barcode(d, f), 30) << "after a forward jump";
        for (int i = 31; i < 36; ++i) {
            ASSERT_TRUE(d.decodeNext(f));
            EXPECT_EQ(barcode(d, f), i) << "single step";
        }
    }
}

// Still requests never wait for a decode: a miss is answered with a nearby
// cached frame. Every such answer must say it is provisional, or the monitor
// (and this harness) would take it as the exact frame. The fast path that
// hands back a lone full-frame source used to drop the mark.
TEST(GoldenFrames, StandInPreviewFramesAreMarkedProvisional)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* clip = addIndexedVideo(*v1, 30, 0, frames(kVideoFrames));
    scene.publish();
    auto& h = *g_harness;
    h.preview->setProject(scene.project.get());
    h.preview->setTimeline(&scene.seq());
    h.preview->setSnapshotPublisher(&scene.publisher);
    h.preview->invalidateCacheDirect();
    h.previewScene = &scene;

    size_t standIns = 0;
    for (int64_t f = 0; f < 90; ++f) {
        RenderRequest request;
        request.type = RenderRequestType::Still;
        request.quality = RenderQuality::Full;
        request.exactness = RenderExactness::ExactRequired;
        request.timelineTick = frames(f);
        request.outputWidth = kW;
        request.outputHeight = kH;
        request.stillFrame = true;
        const auto result = h.preview->renderFrame(request);   // one pass only
        if (!result.frame || !result.frame->ensurePixels()) continue;
        if (frameIndexOf(*result.frame) == expectedFrame(*clip, frames(f))) continue;
        ++standIns;
        EXPECT_TRUE(result.diagnostics.provisionalSource)
            << "frame " << f << " shows source frame " << frameIndexOf(*result.frame)
            << " but is not marked provisional";
    }
    std::cout << "[golden] stand-in answers in a 90-frame step: " << standIns << std::endl;
}

// ─── B. Transitions ──────────────────────────────────────────────────────────

namespace {

struct TransitionScene
{
    Scene scene;
    Clip* left{nullptr};
    Clip* right{nullptr};
};

/// Bars [0, 2 s) then checker [2 s, 4 s), optionally with a transition at
/// the edit point.
void buildCut(TransitionScene& s, const TransitionType* type)
{
    auto* v1 = s.scene.seq().addVideoTrack("V1");
    s.left = addImage(*v1, g_harness->bars, 0, seconds(2));
    s.right = addImage(*v1, g_harness->checker, seconds(2), seconds(2));
    if (type) {
        Transition t;
        t.type = *type;
        t.duration = seconds(1);
        t.leftClipId = s.left->id();
        t.rightClipId = s.right->id();
        t.editPointTick = seconds(2);
        v1->addTransition(t);
    }
}

} // namespace

TEST(GoldenFrames, TransitionsAtStartMiddleAndEnd)
{
    REQUIRE_GPU();
    TransitionScene plain;
    buildCut(plain, nullptr);
    const int64_t start = seconds(1.5), end = seconds(2.5);
    auto beforeCut = renderBoth(plain.scene, start - kFrameTicks);
    auto afterCut = renderBoth(plain.scene, end);
    ASSERT_TRUE(beforeCut && afterCut);

    const std::pair<TransitionType, const char*> types[] = {
        {TransitionType::CrossDissolve, "cross_dissolve"},
        {TransitionType::DipToBlack, "dip_to_black"},
        {TransitionType::WipeLeft, "wipe_left"},
        {TransitionType::PushLeft, "push_left"},
    };
    for (const auto& [type, name] : types) {
        SCOPED_TRACE(name);
        TransitionScene s;
        buildCut(s, &type);
        // Outside the transition the cut renders exactly as without it.
        auto before = renderBoth(s.scene, start - kFrameTicks);
        auto after = renderBoth(s.scene, end);
        ASSERT_TRUE(before && after);
        EXPECT_TRUE(samePixels(*before, *beforeCut)) << "frame before the transition changed";
        EXPECT_TRUE(samePixels(*after, *afterCut)) << "frame after the transition changed";
        const std::pair<int64_t, const char*> samples[] = {
            {start, "start"}, {seconds(2), "middle"}, {end - kFrameTicks, "last"}};
        for (const auto& [tick, when] : samples) {
            auto frame = renderBoth(s.scene, tick);
            ASSERT_TRUE(frame);
            expectGolden(*frame, std::string("transition_") + name + "_" + when);
        }
        auto mid = renderBoth(s.scene, seconds(2));
        ASSERT_TRUE(mid);
        expectRoundTrip(s.scene, seconds(2), *mid);
    }
}

TEST(GoldenFrames, FadeInStartsFromBlackOnTheClipsFirstFrame)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* clip = addImage(*v1, g_harness->bars, seconds(1), seconds(2));
    Transition fade;
    fade.type = TransitionType::FadeFromBlack;
    fade.duration = seconds(0.5);
    fade.rightClipId = clip->id();
    fade.editPointTick = clip->timelineIn();
    v1->addTransition(fade);

    // The opacity-flash bug showed the clip at full strength for one frame.
    auto head = renderBoth(scene, clip->timelineIn());
    ASSERT_TRUE(head);
    EXPECT_LT(meanLuma(*head), 8.0) << "the first frame of a fade-in is not black";
    auto mid = renderBoth(scene, clip->timelineIn() + seconds(0.25));
    ASSERT_TRUE(mid);
    expectGolden(*mid, "fade_from_black_middle");
    auto done = renderBoth(scene, clip->timelineIn() + seconds(0.5));
    ASSERT_TRUE(done);
    EXPECT_GT(meanLuma(*done), meanLuma(*mid));
}

// ─── C. Every video effect ───────────────────────────────────────────────────

TEST(GoldenFrames, EveryVideoEffectAtDefaultAndDrivenParameters)
{
    REQUIRE_GPU();
    Scene bare;
    addImage(*bare.seq().addVideoTrack("V1"), g_harness->bars, 0, seconds(4));
    auto plain = renderBoth(bare, seconds(1));
    ASSERT_TRUE(plain);
    expectGolden(*plain, "source_bars");

    std::vector<std::string> unchanged;
    for (int t = 0; t < static_cast<int>(EffectType::Count); ++t) {
        const auto type = static_cast<EffectType>(t);
        if (isAudioEffect(type)) continue;
        const std::string name = std::to_string(t) + "_" + sanitize(effectTypeName(type));
        SCOPED_TRACE(name);
        for (bool driven : {false, true}) {
            Scene scene;
            auto* clip = addImage(*scene.seq().addVideoTrack("V1"),
                                  g_harness->bars, 0, seconds(4));
            auto effect = createEffect(type);
            ASSERT_NE(effect, nullptr);
            if (driven) driveParams(*effect);
            clip->effects().addEffect(std::move(effect));
            // Clip-local time 1 s: the glitch/beat clocks are deterministic.
            auto frame = renderBoth(scene, seconds(1));
            ASSERT_TRUE(frame);
            expectGolden(*frame, "effect_" + name + (driven ? "_driven" : "_default"));
            if (driven) {
                if (samePixels(*frame, *plain)) unchanged.push_back(name);
                expectRoundTrip(scene, seconds(1), *frame);
            }
        }
    }
    // Informational: effects that leave the image untouched even when driven
    // (a file-less LUT, effects needing beats). A new entry is worth a look.
    std::string list;
    for (const auto& n : unchanged) list += " " + n;
    std::cout << "[golden] effects with no visible change when driven:" << list << std::endl;
}

// ─── D. Compositing ──────────────────────────────────────────────────────────

TEST(GoldenFrames, TransformAndOpacityKeyframes)
{
    REQUIRE_GPU();
    Scene scene;
    const auto tracks = scene.videoTracks(2);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
    top->positionX().writeValue(0, 300.0f);
    top->positionY().writeValue(0, -150.0f);
    top->scaleX().writeValue(0, 0.5f);
    top->scaleY().writeValue(0, 0.5f);
    top->rotation().writeValue(0, 30.0f);
    top->opacity().addKeyframe(0, 0.2f);
    top->opacity().addKeyframe(seconds(1), 1.0f);
    auto frame = renderBoth(scene, seconds(0.5));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "transform_opacity_keyframes");
    expectRoundTrip(scene, seconds(0.5), *frame);
}

TEST(GoldenFrames, EveryBlendMode)
{
    REQUIRE_GPU();
    for (int mode = 0; mode <= 21; ++mode) {
        SCOPED_TRACE(mode);
        Scene scene;
        const auto tracks = scene.videoTracks(2);
        addImage(*tracks[0], g_harness->checker, 0, seconds(4));
        auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
        top->setBlendMode(mode);
        auto frame = renderBoth(scene, seconds(1));
        ASSERT_TRUE(frame);
        expectGolden(*frame, "blend_" + std::to_string(mode));
        if (mode == 7) expectRoundTrip(scene, seconds(1), *frame);
    }
}

TEST(GoldenFrames, AdjustmentClipGradesEverythingBelow)
{
    REQUIRE_GPU();
    Scene scene;
    const auto tracks = scene.videoTracks(3);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto* bars = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
    bars->scaleX().writeValue(0, 0.6f);
    bars->scaleY().writeValue(0, 0.6f);
    auto adjustment = std::make_unique<AdjustmentClip>();
    adjustment->setDuration(seconds(4));
    auto tint = createEffect(EffectType::Tint);
    driveParams(*tint);
    adjustment->effects().addEffect(std::move(tint));
    tracks[2]->addClip(std::move(adjustment));
    auto frame = renderBoth(scene, seconds(1));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "adjustment_tint");
    expectRoundTrip(scene, seconds(1), *frame);
    expectNv12MatchesBgra(scene, seconds(1), *frame);
}

TEST(GoldenFrames, ClipAndEffectMasks)
{
    REQUIRE_GPU();
    Scene scene;
    const auto tracks = scene.videoTracks(2);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
    OpacityMask ellipse;
    ellipse.shape = MaskShape::Ellipse;
    ellipse.base.centerX = 0.4f;
    ellipse.base.width = 0.6f;
    ellipse.base.height = 0.7f;
    ellipse.feather.setDefaultValue(12.0f);
    top->addMask(ellipse);
    auto blur = createEffect(EffectType::Blur);
    driveParams(*blur);
    OpacityMask rect;
    rect.shape = MaskShape::Rectangle;
    rect.base.centerX = 0.7f;
    rect.base.width = 0.3f;
    rect.base.height = 0.4f;
    rect.inverted = true;
    blur->addMask(rect);
    top->effects().addEffect(std::move(blur));
    auto frame = renderBoth(scene, seconds(1));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "masks_clip_ellipse_effect_rect");
    expectRoundTrip(scene, seconds(1), *frame);
    expectNv12MatchesBgra(scene, seconds(1), *frame);
}

TEST(GoldenFrames, NestedSequenceAndCrop)
{
    REQUIRE_GPU();
    Scene scene;
    Timeline& inner = scene.addSequence("Inner");
    auto* innerBars = addImage(*inner.addVideoTrack("Inner V1"), g_harness->bars, 0, seconds(4));
    auto blur = createEffect(EffectType::Blur);
    driveParams(*blur);
    innerBars->effects().addEffect(std::move(blur));

    const auto tracks = scene.videoTracks(3);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto nested = std::make_unique<SequenceClip>();
    nested->setSequenceIndex(1);
    nested->setDuration(seconds(4));
    nested->scaleX().writeValue(0, 0.5f);
    nested->scaleY().writeValue(0, 0.5f);
    nested->positionX().writeValue(0, -400.0f);
    nested->rotation().writeValue(0, 10.0f);
    tracks[1]->addClip(std::move(nested));
    auto* cropped = addImage(*tracks[2], g_harness->bars, 0, seconds(4));
    cropped->setCrop(55.0f, 10.0f, 5.0f, 15.0f);   // percent

    auto frame = renderBoth(scene, seconds(1));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "nested_sequence_and_crop");
    expectRoundTrip(scene, seconds(1), *frame);
    expectNv12MatchesBgra(scene, seconds(1), *frame);
}

// ─── E. Text ─────────────────────────────────────────────────────────────────

TEST(GoldenFrames, TextLayer)
{
    REQUIRE_GPU();
    if (!QFontDatabase::hasFamily(QStringLiteral("Arial")))
        GTEST_SKIP() << "Arial is not installed";
    Scene scene;
    const auto tracks = scene.videoTracks(2);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto graphic = std::make_unique<GraphicClip>();
    graphic->setDuration(seconds(4));
    auto* text = graphic->addTextLayer("Golden 123");
    text->setFontFamily("Arial");
    text->setFontSize(32.0f);   // sequence pixels (256 wide)
    text->setFillForAll(true, 0xFFFFD040);
    tracks[1]->addClip(std::move(graphic));
    auto frame = renderBoth(scene, seconds(1));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "text_arial");
    expectRoundTrip(scene, seconds(1), *frame);
}

// ─── F. Paths the render core must keep ──────────────────────────────────────
// Pins for compositor paths the sections above don't reach: partial and
// stacked adjustment layers, several effected layers in one frame, temporal
// interpolation, one-sided dissolves, legacy-space masks and static reuse.

TEST(GoldenFrames, AdjustmentClipAtPartialStrengthAndStacked)
{
    REQUIRE_GPU();
    auto build = [](Scene& scene, bool stacked) {
        const auto tracks = scene.videoTracks(stacked ? 4 : 3);
        addImage(*tracks[0], g_harness->checker, 0, seconds(4));
        auto* bars = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
        bars->scaleX().writeValue(0, 0.6f);
        bars->scaleY().writeValue(0, 0.6f);
        auto tint = std::make_unique<AdjustmentClip>();
        tint->setDuration(seconds(4));
        auto effect = createEffect(EffectType::Tint);
        driveParams(*effect);
        tint->effects().addEffect(std::move(effect));
        if (!stacked) tint->opacity().writeValue(0, 0.5f);
        tracks[2]->addClip(std::move(tint));
        if (stacked) {
            // Directly on top of the first: its graded frame feeds this one.
            auto blur = std::make_unique<AdjustmentClip>();
            blur->setDuration(seconds(4));
            auto effect2 = createEffect(EffectType::Blur);
            driveParams(*effect2);
            blur->effects().addEffect(std::move(effect2));
            tracks[3]->addClip(std::move(blur));
        }
    };
    {
        SCOPED_TRACE("half strength");
        Scene scene;
        build(scene, false);
        auto frame = renderBoth(scene, seconds(1));
        ASSERT_TRUE(frame);
        expectGolden(*frame, "adjustment_tint_half_strength");
        expectRoundTrip(scene, seconds(1), *frame);
        expectNv12MatchesBgra(scene, seconds(1), *frame);
    }
    {
        SCOPED_TRACE("stacked");
        Scene scene;
        build(scene, true);
        auto frame = renderBoth(scene, seconds(1));
        ASSERT_TRUE(frame);
        expectGolden(*frame, "adjustment_tint_then_blur");
        expectRoundTrip(scene, seconds(1), *frame);
    }
}

// EffectProcessor has one pair of ping-pong images for every layer, so each
// effected layer's result must be copied out before the next one runs.
TEST(GoldenFrames, TwoEffectedClipsAndADissolveBetweenThem)
{
    REQUIRE_GPU();
    {
        SCOPED_TRACE("stacked layers");
        Scene scene;
        const auto tracks = scene.videoTracks(2);
        auto* bottom = addImage(*tracks[0], g_harness->checker, 0, seconds(4));
        auto blur = createEffect(EffectType::Blur);
        driveParams(*blur);
        bottom->effects().addEffect(std::move(blur));
        auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
        top->scaleX().writeValue(0, 0.5f);
        top->scaleY().writeValue(0, 0.5f);
        auto tint = createEffect(EffectType::Tint);
        driveParams(*tint);
        top->effects().addEffect(std::move(tint));
        auto frame = renderBoth(scene, seconds(1));
        ASSERT_TRUE(frame);
        expectGolden(*frame, "two_effected_layers");
        expectRoundTrip(scene, seconds(1), *frame);
    }
    {
        SCOPED_TRACE("dissolve");
        TransitionScene s;
        const TransitionType type = TransitionType::CrossDissolve;
        buildCut(s, &type);
        auto tint = createEffect(EffectType::Tint);
        driveParams(*tint);
        s.left->effects().addEffect(std::move(tint));
        auto blur = createEffect(EffectType::Blur);
        driveParams(*blur);
        s.right->effects().addEffect(std::move(blur));
        auto frame = renderBoth(s.scene, seconds(2));
        ASSERT_TRUE(frame);
        expectGolden(*frame, "dissolve_between_effected_clips");
        expectNv12MatchesBgra(s.scene, seconds(2), *frame);
    }
}

TEST(GoldenFrames, FrameBlendingOnASlowedClip)
{
    REQUIRE_GPU();
    const std::pair<TimeInterpolation, const char*> modes[] = {
        {TimeInterpolation::FrameBlending, "frame_blending"},
        {TimeInterpolation::OpticalFlow, "optical_flow"},
    };
    for (const auto& [mode, name] : modes) {
        SCOPED_TRACE(name);
        Scene scene;
        auto* v1 = scene.seq().addVideoTrack("V1");
        auto* clip = addIndexedVideo(*v1, 30, 0, frames(80), 10, 0.5);
        clip->setTimeInterpolation(mode);
        // Sequence frame 3 lands halfway between source frames 11 and 12.
        auto frame = renderBoth(scene, frames(3));
        ASSERT_TRUE(frame);
        expectGolden(*frame, std::string(name) + "_half_speed");
        expectRoundTrip(scene, frames(3), *frame);
    }
}

TEST(GoldenFrames, OneSidedCrossDissolvesAtHeadAndTail)
{
    REQUIRE_GPU();
    Scene scene;
    auto* v1 = scene.seq().addVideoTrack("V1");
    auto* clip = addImage(*v1, g_harness->bars, seconds(1), seconds(2));
    Transition in;
    in.type = TransitionType::CrossDissolve;
    in.duration = seconds(0.5);
    in.rightClipId = clip->id();
    in.editPointTick = clip->timelineIn();
    v1->addTransition(in);
    Transition out;
    out.type = TransitionType::CrossDissolve;
    out.duration = seconds(0.5);
    out.leftClipId = clip->id();
    out.editPointTick = clip->timelineOut();
    v1->addTransition(out);

    auto head = renderBoth(scene, clip->timelineIn());
    ASSERT_TRUE(head);
    EXPECT_LT(meanLuma(*head), 8.0) << "the first frame of a dissolve-in is not black";
    auto in25 = renderBoth(scene, clip->timelineIn() + seconds(0.25));
    ASSERT_TRUE(in25);
    expectGolden(*in25, "cross_dissolve_in_middle");
    auto out25 = renderBoth(scene, clip->timelineOut() - seconds(0.25));
    ASSERT_TRUE(out25);
    expectGolden(*out25, "cross_dissolve_out_middle");
    expectRoundTrip(scene, clip->timelineIn() + seconds(0.25), *in25);
}

// Masks from projects older than format v30 are kept in sequence-frame space
// and placed through the clip transform at render time.
TEST(GoldenFrames, LegacySequenceSpaceMask)
{
    REQUIRE_GPU();
    Scene scene;
    const auto tracks = scene.videoTracks(2);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
    top->scaleX().writeValue(0, 0.7f);
    top->scaleY().writeValue(0, 0.7f);
    top->positionX().writeValue(0, 40.0f);
    OpacityMask legacy;
    legacy.shape = MaskShape::Ellipse;
    legacy.coordinateSpace = MaskCoordinateSpace::LegacySequenceFrame;
    legacy.base.centerX = 0.45f;
    legacy.base.width = 0.5f;
    legacy.base.height = 0.6f;
    top->addMask(legacy);
    // A source-space mask on the same clip is drawn with it.
    OpacityMask local;
    local.shape = MaskShape::Rectangle;
    local.base.centerX = 0.75f;
    local.base.width = 0.2f;
    local.base.height = 0.3f;
    top->addMask(local);
    // No save and load here: loading upgrades legacy masks to source space.
    auto frame = renderBoth(scene, seconds(1));
    ASSERT_TRUE(frame);
    expectGolden(*frame, "mask_legacy_and_source_space");
}

// An unchanged stack of still sources is composited once and reused until
// the layer state changes or the cache is invalidated.
TEST(GoldenFrames, StaticFramesAreReusedUntilAnEdit)
{
    REQUIRE_GPU();
    Scene scene;
    const auto tracks = scene.videoTracks(2);
    addImage(*tracks[0], g_harness->checker, 0, seconds(4));
    auto* top = addImage(*tracks[1], g_harness->bars, 0, seconds(4));
    top->scaleX().writeValue(0, 0.5f);
    top->scaleY().writeValue(0, 0.5f);
    scene.publish();

    std::string why;
    auto first = renderPreview(scene, seconds(1), &why);
    ASSERT_TRUE(first) << why;
    auto again = renderPreview(scene, seconds(1), &why);
    ASSERT_TRUE(again) << why;
    EXPECT_EQ(again.get(), first.get()) << "an unchanged still frame was composited again";
    auto later = renderPreview(scene, seconds(2), &why);
    ASSERT_TRUE(later) << why;
    EXPECT_EQ(later.get(), first.get())
        << "the same still picture at another tick was composited again";

    top->opacity().writeValue(0, 0.5f);
    scene.publish();
    auto edited = renderPreview(scene, seconds(1), &why);
    ASSERT_TRUE(edited) << why;
    EXPECT_NE(edited.get(), first.get()) << "an edit reused the old composite";
    EXPECT_FALSE(samePixels(*edited, *first));

    g_harness->preview->requestCacheInvalidation();
    auto refreshed = renderPreview(scene, seconds(1), &why);
    ASSERT_TRUE(refreshed) << why;
    EXPECT_NE(refreshed.get(), edited.get()) << "an invalidation kept the old composite";
    EXPECT_TRUE(samePixels(*refreshed, *edited));
}
