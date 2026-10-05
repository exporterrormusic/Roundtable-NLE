/*
 * test_comfy_panel.cpp — COMFYUI page: assembling chosen renders onto a
 * timeline (order, tail-pad trim, key, fill-width placement, backgrounds,
 * linked audio, segment markers, one-step undo).
 */
#include <gtest/gtest.h>

#include "AudioMixdown.h"
#include "Constants.h"
#include "ai/ScriptMatcher.h"
#include "command/CommandStack.h"
#include "effects/ChromaKey.h"
#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"
#include "panels/comfy/ComfyPanel.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QApplication>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QKeyEvent>
#include <QPushButton>
#include <QTest>
#include <QTreeWidget>
#include <QWindow>

#include <cmath>
#include <memory>

using namespace rt;

namespace {

int g_argc = 1;
char g_arg0[] = "test_comfy_panel";
char* g_argv[] = {g_arg0, nullptr};

class ComfyPanelTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(g_argc, g_argv);
    }

    QString makeTone(const QString& name, double seconds)
    {
        constexpr uint32_t sampleRate = 44100;
        MixdownResult r;
        r.sampleRate = sampleRate;
        r.channels = 1;
        r.totalFrames = static_cast<int64_t>(seconds * sampleRate);
        r.duration = seconds;
        r.samples.assign(static_cast<size_t>(r.totalFrames), 0.1f);
        const QString path = dir.filePath(name);
        EXPECT_TRUE(AudioMixdown::writeWav(r, std::filesystem::path(path.toStdWString())));
        return path;
    }

    /// A 2 s 1280x704 16 fps "render" with audio, made by the bundled ffmpeg.
    QString makeRender(const QString& name)
    {
        const QString ffmpeg = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
        if (!QFileInfo::exists(ffmpeg)) return {};
        const QString out = dir.filePath(name);
        const int rc = QProcess::execute(ffmpeg, {
            "-y", "-loglevel", "error",
            "-f", "lavfi", "-i", "color=c=0x00FF00:s=1280x704:r=16:d=2",
            "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
            "-shortest", "-c:v", "mpeg4", "-c:a", "aac", out});
        return rc == 0 && QFileInfo::exists(out) ? out : QString();
    }

    QTemporaryDir dir;
    static std::unique_ptr<QApplication> app;
};

std::unique_ptr<QApplication> ComfyPanelTest::app;

QString row(const char* shot, const char* action, const char* text)
{
    return QString("<tr><td>%1</td><td>%2</td><td>%3</td></tr>").arg(shot, action, text);
}

} // namespace

TEST_F(ComfyPanelTest, AssemblesChosenRendersOntoTheTimeline)
{
    const QString render = makeRender("render.mp4");
    if (render.isEmpty()) GTEST_SKIP() << "bundled ffmpeg not available";

    const QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT")
        + row("WIDE", "", "Welcome to the show.")
        + row("CLOSE-UP", "", "Let us talk about it.")
        + row("-", "STORY", "-")
        + row("WIDE", "", "Here is the story.")
        + "</table></body></html>";

    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "comfy-assemble-test"));
    const char* lines[] = {"Welcome to the show.", "Let us talk about it.", "Here is the story."};
    const int lineNumbers[] = {1, 2, 4};
    const char* segments[] = {"INTRO", "INTRO", "STORY"};
    for (int i = 0; i < 3; ++i) {
        const QString take = makeTone(QString("take%1.wav").arg(i), 2.0);
        sync.attachGeneratedAudio(take, QString::fromLatin1(kShotScriptCharacter), lines[i],
                                  lineNumbers[i], segments[i], 2.0);
    }
    ASSERT_EQ(sync.showShots().size(), 3u);
    for (const auto& s : sync.showShots()) ASSERT_TRUE(s.hasTake) << s.dialogue;

    auto& state = sync.pipeline();
    for (const char* line : lines) {
        RenderVersion rv;
        rv.file = render.toStdString();
        rv.tailPadSec = 0.25;   // SCAIL end-of-clip padding to trim
        state.shots[ScriptMatcher::normalize(line)].renders.push_back(rv);
    }
    QImage bg(64, 36, QImage::Format_RGB32);
    bg.fill(Qt::blue);
    const QString bgPath = dir.filePath("wide_bg.png");
    ASSERT_TRUE(bg.save(bgPath));
    state.background = bgPath.toStdString();
    state.closeUpBackgroundZoom = 0.0;   // auto: CU framing ÷ WIDE framing (defaults 0.9 / 0.5)
    state.segmentGapSec = 2.0;

    Timeline timeline;
    CommandStack stack;
    ComfyPanel panel(&sync);
    QString note;
    ASSERT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &note), 3) << note.toStdString();

    // Tracks: renders on the layer above the backgrounds, audio on A1.
    std::vector<const Track*> video, audio;
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        const Track* t = timeline.track(i);
        if (t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack()) video.push_back(t);
        if (t->type() == TrackType::Audio && !t->isDivider()) audio.push_back(t);
    }
    ASSERT_GE(video.size(), 2u);
    const Track* bgTrack = video[video.size() - 1];
    const Track* shotTrack = video[video.size() - 2];

    const int64_t T = kTicksPerSecond;
    ASSERT_EQ(shotTrack->clipCount(), 3u);
    const auto* first = static_cast<const VideoClip*>(shotTrack->clip(0));
    EXPECT_EQ(first->timelineIn(), 0);
    EXPECT_NEAR(static_cast<double>(first->duration()) / T, 1.75, 0.07);   // 2 s − 0.25 s pad
    ASSERT_EQ(first->effects().effectCount(), 1u);
    EXPECT_NE(dynamic_cast<const ChromaKey*>(&first->effects().effect(0)), nullptr);
    const auto fit = fillWidthBottom(1280, 704);
    auto* firstMutable = const_cast<VideoClip*>(first);   // transform accessors are non-const
    EXPECT_FLOAT_EQ(firstMutable->scaleX().defaultValue(), fit.scale);
    EXPECT_FLOAT_EQ(firstMutable->positionY().defaultValue(), fit.posYPx);
    const auto* third = shotTrack->clip(2);
    EXPECT_EQ(third->timelineIn(), shotTrack->clip(1)->timelineIn() + shotTrack->clip(1)->duration() + 2 * T);

    // Linked audio from the render itself.
    ASSERT_FALSE(audio.empty());
    ASSERT_EQ(audio.front()->clipCount(), 3u);
    EXPECT_EQ(audio.front()->clip(0)->linkId(), first->linkId());
    EXPECT_NE(first->linkId(), 0u);

    // One background clip per run (WIDE | CLOSE-UP | WIDE), the same file,
    // full size under WIDE and zoomed under the CLOSE-UP.
    ASSERT_EQ(bgTrack->clipCount(), 3u);
    auto* bg0 = const_cast<Clip*>(bgTrack->clip(0));
    auto* bg1 = const_cast<Clip*>(bgTrack->clip(1));
    EXPECT_EQ(bg0->timelineIn(), 0);
    EXPECT_EQ(bg0->duration(), first->duration());
    EXPECT_FLOAT_EQ(bg0->scaleX().defaultValue(), 1.0f);
    EXPECT_EQ(bg1->timelineIn(), shotTrack->clip(1)->timelineIn());
    // Auto zoom = the recording's default CLOSE-UP scale ÷ WIDE scale.
    EXPECT_NEAR(bg1->scaleX().defaultValue(), RecordingFraming{}.closeUp.scale / RecordingFraming{}.wide.scale, 1e-5);
    EXPECT_EQ(bgTrack->clip(2)->timelineIn(), third->timelineIn());

    ASSERT_EQ(timeline.markers().size(), 2u);
    EXPECT_EQ(timeline.markers()[0].label, "INTRO");
    EXPECT_EQ(timeline.markers()[1].label, "STORY");
    EXPECT_EQ(timeline.markers()[1].time, third->timelineIn());

    // One undo step removes everything.
    ASSERT_TRUE(stack.undo());
    size_t clips = 0;
    for (size_t i = 0; i < timeline.trackCount(); ++i) clips += timeline.track(i)->clipCount();
    EXPECT_EQ(clips, 0u);
    EXPECT_TRUE(timeline.markers().empty());
}

TEST_F(ComfyPanelTest, AssembleWithoutRendersExplainsWhy)
{
    const QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT")
        + row("WIDE", "", "Only line.") + "</table></body></html>";
    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "comfy-assemble-empty"));
    Timeline timeline;
    ComfyPanel panel(&sync);
    QString note;
    EXPECT_EQ(panel.assembleIntoTimeline(&timeline, nullptr, &note), 0);
    EXPECT_TRUE(note.contains("RENDER ALL"));
}

TEST_F(ComfyPanelTest, UsableRenderSkipsDeletedFilesAndFallsBackToNewestOnDisk)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto touch = [&](const QString& name) {
        QFile f(dir.filePath(name));
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
        return dir.filePath(name).toStdString();
    };
    ShotSettings s;
    RenderVersion a, b, c;
    a.file = touch("v1.mp4");
    b.file = touch("v2.mp4");
    c.file = dir.filePath("v3_deleted.mp4").toStdString();   // never written = deleted
    s.renders = {a, b, c};

    s.chosenRender = 0;
    ASSERT_NE(ComfyPanel::usableRender(s), nullptr);
    EXPECT_EQ(ComfyPanel::usableRender(s)->file, a.file);     // chosen, on disk

    s.chosenRender = -1;                                      // newest = v3, deleted
    ASSERT_NE(ComfyPanel::usableRender(s), nullptr);
    EXPECT_EQ(ComfyPanel::usableRender(s)->file, b.file);     // newest still on disk

    QFile::remove(QString::fromStdString(a.file));
    QFile::remove(QString::fromStdString(b.file));
    EXPECT_EQ(ComfyPanel::usableRender(s), nullptr);
}

namespace {
/// Stands in for MainWindow's app-wide filter, which sends Space/J/K/L to the
/// TIMELINE page and consumes them.
class TransportThief : public QObject
{
public:
    int stolen = 0;
protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress) return false;
        const int k = static_cast<QKeyEvent*>(e)->key();
        if (k == Qt::Key_Space || k == Qt::Key_J || k == Qt::Key_K || k == Qt::Key_L) { ++stolen; return true; }
        return false;
    }
};
} // namespace

TEST_F(ComfyPanelTest, SpaceAndJklDriveTheShotPlayerNotTheTimeline)
{
    const QString take = makeRender("take.mp4");   // a video take (the transport ignores audio-only takes)
    if (take.isEmpty()) GTEST_SKIP() << "bundled ffmpeg not available";
    const QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT")
        + row("WIDE", "", "Only line.") + "</table></body></html>";
    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "comfy-keys"));
    sync.attachGeneratedAudio(take, QString::fromLatin1(kShotScriptCharacter), "Only line.", 1, "INTRO", 2.0);
    ASSERT_EQ(sync.showShots().size(), 1u);

    TransportThief thief;
    qApp->installEventFilter(&thief);   // installed first, like MainWindow's
    ComfyPanel panel(&sync);
    panel.resize(1500, 950);
    panel.show();
    QApplication::setActiveWindow(&panel);
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&panel));
    panel.refresh();

    QPushButton* play = nullptr;
    for (auto* b : panel.findChildren<QPushButton*>())
        if (b->text() == QStringLiteral("▶")) play = b;
    ASSERT_NE(play, nullptr);
    ASSERT_TRUE(play->isEnabled()) << "transport disabled: shot not selected / not a video take";

    // Real key presses arrive at the WINDOW first (then Qt forwards them to
    // the focus widget) — an app filter that consumes them there wins unless
    // the panel claims them at that stage too.
    panel.shotTree()->setFocus();
    QWindow* win = panel.windowHandle();
    ASSERT_NE(win, nullptr);
    QTest::keyClick(win, Qt::Key_Space);
    EXPECT_EQ(play->text(), QStringLiteral("⏸")) << "Space did not start playback";
    QTest::keyClick(win, Qt::Key_K);
    EXPECT_EQ(play->text(), QStringLiteral("▶")) << "K did not pause";
    QTest::keyClick(win, Qt::Key_L);
    EXPECT_EQ(play->text(), QStringLiteral("⏸")) << "L did not play";
    QTest::keyClick(win, Qt::Key_Space);
    EXPECT_EQ(thief.stolen, 0) << "an earlier app filter got the transport keys";
    qApp->removeEventFilter(&thief);
}

// KJNodes' animated live preview arrives as a short MP4 (2 fps): the panel
// decodes every frame of it to loop in LIVE PREVIEW.
TEST_F(ComfyPanelTest, LivePreviewClipDecodesEveryFrame)
{
    const QString ffmpeg = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
    if (!QFileInfo::exists(ffmpeg)) GTEST_SKIP() << "bundled ffmpeg not available";
    const QString clip = dir.filePath("preview.mp4");
    ASSERT_EQ(QProcess::execute(ffmpeg, {"-y", "-loglevel", "error", "-f", "lavfi",
                                         "-i", "testsrc=s=640x352:r=2:d=3", "-c:v", "mpeg4", clip}), 0);
    const std::vector<QImage> frames = VideoFrameGrabber::decodeAllFrames(clip.toStdString(), QSize(320, 320));
    ASSERT_EQ(frames.size(), 6u);
    EXPECT_EQ(frames.front().size(), QSize(320, 176));
    EXPECT_NE(frames.front(), frames.back());   // distinct frames, not one repeated
}
