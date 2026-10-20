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
#include "effects/ColorGrading.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "panels/comfy/ComfyTimelineTemplate.h"
#include "timeline/AudioClip.h"
#include "timeline/AdjustmentClip.h"
#include "timeline/ImageClip.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/audio/AudioSync.h"
#include "panels/audio/VideoFrameGrabber.h"
#include "panels/comfy/ComfyPanel.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHeaderView>
#include <QProcess>
#include <QStandardPaths>
#include <QSettings>
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
        // Column resize/sort tests must never write the user's layout settings.
        static QTemporaryDir settingsDir;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());
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

    QString makeTimelineTemplate(const QString& render, const QString& music)
    {
        auto project = Project::createNew("Template");
        auto* timeline = project->timeline();
        timeline->setName("Template");
        timeline->settings().setFrameRate(60);
        auto* video = timeline->addVideoTrack("Templates");
        auto* audio = timeline->addAudioTrack("Music templates");
        auto addVideo = [&](const char* label, int64_t at, int64_t duration, int64_t sourceIn = 0) {
            auto clip = std::make_unique<VideoClip>(render.toStdString());
            clip->setLabel(label); clip->setTimelineIn(at); clip->setDuration(duration); clip->setSourceIn(sourceIn);
            clip->setSourceDuration(2 * kTicksPerSecond); clip->setSourceFps(16); clip->setSourceResolution(1280, 704);
            return static_cast<VideoClip*>(video->addClip(std::move(clip)));
        };
        auto* wide = addVideo("Character wide", 30 * kTicksPerSecond, kTicksPerSecond);
        wide->positionX().setDefaultValue(-548); wide->positionY().setDefaultValue(12);
        wide->scaleX().setDefaultValue(fillWidthBottom(1280, 704).scale);
        wide->scaleY() = wide->scaleX();
        auto key = std::make_unique<ChromaKey>(); key->applyFlatGreenPreset(); wide->effects().addEffect(std::move(key));
        auto center = wide->clone(); center->positionX().setDefaultValue(0);
        auto* centered = video->addClip(std::move(center));
        auto close = wide->clone(); close->positionX().setDefaultValue(0);
        auto* closeUp = video->addClip(std::move(close));
        auto* stinger = addVideo("Stinger", 5 * kTicksPerSecond, kTicksPerSecond);
        auto* ending = addVideo("Ending", 20 * kTicksPerSecond, kTicksPerSecond / 2);
        const QString image = dir.filePath("white.png");
        QImage white(64, 36, QImage::Format_RGB32); white.fill(Qt::white); EXPECT_TRUE(white.save(image));
        auto addImage = [&](const char* label, int64_t at, int64_t duration) {
            auto clip = std::make_unique<ImageClip>(image.toStdString());
            clip->setLabel(label); clip->setTimelineIn(at); clip->setDuration(duration);
            return video->addClip(std::move(clip));
        };
        auto* bar = addImage("BAR", 0, 25 * kTicksPerSecond);
        auto flash = [&](int64_t cut) {
            auto* clip = addImage("WHITE", cut - kTicksPerSecond / 5, 2 * kTicksPerSecond / 5);
            Transition fade; fade.duration = kTicksPerSecond / 5;
            fade.rightClipId = clip->id(); fade.editPointTick = clip->timelineIn(); video->addTransition(fade);
            fade.leftClipId = clip->id(); fade.rightClipId = 0; fade.editPointTick = clip->timelineOut(); video->addTransition(fade);
            return clip;
        };
        auto* leadFlash = flash(5 * kTicksPerSecond);
        auto* tailFlash = flash(6 * kTicksPerSecond);
        auto* endingFlash = flash(20 * kTicksPerSecond);
        auto* endingTailFlash = flash(20 * kTicksPerSecond + kTicksPerSecond / 2);
        auto adjustment = std::make_unique<AdjustmentClip>(); adjustment->setLabel("Grade");
        adjustment->setDuration(25 * kTicksPerSecond);
        auto grade = std::make_unique<ColorGrading>();
        grade->param(ColorGrading::Vibrance).track.setDefaultValue(40);
        grade->param(ColorGrading::CreativeSat).track.setDefaultValue(115);
        adjustment->effects().addEffect(std::move(grade));
        auto* adjustmentClip = video->addClip(std::move(adjustment));
        auto addMusic = [&](int64_t at, int64_t in, int64_t duration, float level) {
            auto clip = std::make_unique<AudioClip>(music.toStdString()); clip->setLabel("Music");
            clip->setTimelineIn(at); clip->setSourceIn(in); clip->setDuration(duration);
            clip->setSourceDuration(10 * kTicksPerSecond); clip->volume().setDefaultValue(level);
            return audio->addClip(std::move(clip));
        };
        auto* lead = addMusic(5 * kTicksPerSecond, 0, kTicksPerSecond, 0.5f);
        auto* bed = addMusic(6 * kTicksPerSecond, kTicksPerSecond, 2 * kTicksPerSecond, 0.1f);
        Transition cross; cross.duration = kTicksPerSecond / 2; cross.leftClipId = lead->id();
        cross.rightClipId = bed->id(); cross.editPointTick = bed->timelineIn(); audio->addTransition(cross);
        auto id = [](const Clip* clip) { return double(clip->id()); };
        QJsonObject section{{"video", id(stinger)}, {"leadMusic", id(lead)}, {"bedMusic", id(bed)},
                            {"leadFlash", id(leadFlash)}, {"tailFlash", id(tailFlash)}, {"musicLoopTicks", 2 * kTicksPerSecond}};
        QJsonObject intro = section; intro.remove("leadFlash");
        QJsonObject mini = section; mini.remove("musicLoopTicks");
        mini["musicFile"] = music; mini["musicSourceTimecode"] = "00:00:01:12";
        mini["videoSourceTimecode"] = "00:00:00:08"; // 8/16 = 0.5s, never 8/60
        QJsonObject recipe{{"version", 1}, {"project", "template.rtp"}, {"sequence", "Template"},
            {"wideCharacter", id(wide)}, {"centeredCharacter", id(centered)}, {"closeUpCharacter", id(closeUp)},
            {"bar", id(bar)}, {"adjustment", id(adjustmentClip)}, {"endingVideo", id(ending)}, {"endingFlash", id(endingFlash)},
            {"endingTailFlash", id(endingTailFlash)},
            {"sections", QJsonObject{{"INTRO", intro}, {"STORY", section}, {"CONCLUSION", section}, {"MINIGAME", mini}}}};
        EXPECT_TRUE(ProjectSerializer{}.save(*project, std::filesystem::path(dir.filePath("template.rtp").toStdWString())));
        const auto path = dir.filePath("template.json"); QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(recipe).toJson());
        return path;
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
    const auto& key = first->effects().effect(0);
    EXPECT_FLOAT_EQ(key.evalParam(ChromaKey::Transparency, 0), 0.0f);
    EXPECT_FLOAT_EQ(key.evalParam(ChromaKey::Choke, 0), 20.0f);
    EXPECT_FLOAT_EQ(key.evalParam(ChromaKey::Spill, 0), 100.0f);
    EXPECT_FLOAT_EQ(key.evalParam(ChromaKey::Luma, 0), 0.0f);
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

    // One background clip per shot (WIDE | CLOSE-UP | WIDE), the same file,
    // every WIDE centred at 100%, CLOSE-UP auto-zoomed.
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
    auto* bg2 = const_cast<Clip*>(bgTrack->clip(2));
    EXPECT_FLOAT_EQ(bg2->scaleX().defaultValue(), 1.0f);
    EXPECT_FLOAT_EQ(bg2->scaleY().defaultValue(), 1.0f);
    EXPECT_FLOAT_EQ(bg2->positionX().defaultValue() + 960.0f, 960.0f);
    EXPECT_FLOAT_EQ(bg2->positionY().defaultValue() + 540.0f, 540.0f);

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

    // A video restarts for each shot while keeping its requested framing.
    state.background = makeRender("background.mp4").toStdString();
    ASSERT_FALSE(state.background.empty());
    state.closeUpBackgroundZoom = 2.0;
    auto* x = panel.findChild<QDoubleSpinBox*>("comfyBackgroundPositionX");
    auto* y = panel.findChild<QDoubleSpinBox*>("comfyBackgroundPositionY");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    x->setValue(1905.8);
    y->setValue(1069.7);
    EXPECT_NEAR(state.closeUpBackgroundOffsetX, 945.8, 1e-8);
    EXPECT_NEAR(state.closeUpBackgroundOffsetY, 529.7, 1e-8);
    ASSERT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &note), 3);
    const Track* loopTrack = nullptr;
    for (size_t i = 0; i < timeline.trackCount(); ++i) {
        const Track* t = timeline.track(i);
        if (t->type() == TrackType::Video && !t->isDivider() && !t->isCaptionTrack()) loopTrack = t;
    }
    ASSERT_NE(loopTrack, nullptr);
    ASSERT_EQ(loopTrack->clipCount(), 3u);
    const bool closeUp[] = {false, true, false};
    for (size_t i = 0; i < loopTrack->clipCount(); ++i) {
        auto* clip = const_cast<Clip*>(loopTrack->clip(i));
        EXPECT_FLOAT_EQ(clip->scaleX().defaultValue(), closeUp[i] ? 2.0f : 1.0f);
        EXPECT_FLOAT_EQ(clip->scaleY().defaultValue(), closeUp[i] ? 2.0f : 1.0f);
        EXPECT_FLOAT_EQ(clip->positionX().defaultValue(), closeUp[i] ? 945.8f : 0.0f);
        EXPECT_FLOAT_EQ(clip->positionY().defaultValue(), closeUp[i] ? 529.7f : 0.0f);
        EXPECT_EQ(clip->sourceIn(), 0);
        EXPECT_NEAR(static_cast<double>(clip->duration()) / T, 1.75, 0.07);
        EXPECT_EQ(static_cast<const VideoClip*>(clip)->mediaPath(), state.background);
    }
    // Missing opening render: subsequent wides still use default framing.
    ASSERT_TRUE(stack.undo());
    state.shots[ScriptMatcher::normalize(lines[0])].renders.clear();
    ASSERT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &note), 2);
    for (size_t t = 0; t < timeline.trackCount(); ++t) {
        auto* track = timeline.track(t);
        for (size_t c = 0; c < track->clipCount(); ++c) {
            auto* clip = dynamic_cast<VideoClip*>(track->clip(c));
            if (!clip || clip->mediaPath() != state.background) continue;
            const bool isWide = clip->timelineIn() >= 3 * T;
            EXPECT_FLOAT_EQ(clip->scaleX().defaultValue(), isWide ? 1.0f : 2.0f);
            EXPECT_FLOAT_EQ(clip->positionX().defaultValue(), isWide ? 0.0f : 945.8f);
            EXPECT_FLOAT_EQ(clip->positionY().defaultValue(), isWide ? 0.0f : 529.7f);
        }
    }
    // A missing configured background must not silently assemble bare shots.
    ASSERT_TRUE(stack.undo());
    state.background = dir.filePath("missing-background.mp4").toStdString();
    EXPECT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &note), 0);
    EXPECT_TRUE(note.contains("Cannot read the assembly background"));
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

TEST_F(ComfyPanelTest, NativeAlphaSkipsKeyIncludingSavedTemplateEffects)
{
    const QString render = dir.filePath("alpha.mov");
    const QString ffmpeg = QCoreApplication::applicationDirPath() + "/ffmpeg.exe";
    ASSERT_EQ(QProcess::execute(ffmpeg, {"-y", "-v", "error", "-f", "lavfi", "-i",
        "color=c=red@0.5:s=1280x704:r=16:d=2,format=rgba", "-c:v", "png", render}), 0);
    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(("<html><body><table>" + row("SHOT", "ACTION", "SCRIPT") +
        row("WIDE", "", "Alpha character.") + "</table></body></html>").toStdString(), "alpha-template"));
    sync.attachGeneratedAudio(makeTone("voice.wav", 2), QString::fromLatin1(kShotScriptCharacter),
                              "Alpha character.", 1, "INTRO", 2.0);
    RenderVersion version; version.file = render.toStdString();
    sync.pipeline().shots[ScriptMatcher::normalize("Alpha character.")].renders.push_back(version);
    ComfyPanel panel(&sync);
    const QString opaque = makeRender("opaque.mp4");
    const QString recipe = makeTimelineTemplate(opaque, makeTone("music.wav", 10));
    for (bool useTemplate : {false, true}) {
        sync.pipeline().timelineTemplate = useTemplate ? recipe.toStdString() : "";
        Timeline timeline;
        QString error;
        ASSERT_EQ(panel.assembleIntoTimeline(&timeline, nullptr, &error), 1) << error.toStdString();
        int characters = 0;
        for (size_t t = 0; t < timeline.trackCount(); ++t)
            for (size_t c = 0; c < timeline.track(t)->clipCount(); ++c)
                if (auto* video = dynamic_cast<VideoClip*>(timeline.track(t)->clip(c));
                    video && video->mediaPath() == render.toStdString()) {
                    ++characters;
                    for (size_t e = 0; e < video->effects().effectCount(); ++e)
                        EXPECT_NE(video->effects().effect(e).effectType(), EffectType::ChromaKey);
                }
        EXPECT_EQ(characters, 1);
    }
}

TEST_F(ComfyPanelTest, ProductionTemplateBuildsSectionsMusicFlashesAndCharacterExceptions)
{
    const QString render = makeRender("render.mp4");
    if (render.isEmpty()) GTEST_SKIP() << "bundled ffmpeg not available";
    const QString music = makeTone("music.wav", 10);
    const QString recipe = makeTimelineTemplate(render, music);
    const char* segments[] = {"INTRO", "INTRO", "INTRO", "STORY", "STORY", "MINI-GAME", "CONCLUSION", "CONCLUSION"};
    const char* types[] = {"WIDE", "WIDE", "WIDE", "WIDE", "CLOSE-UP", "WIDE", "WIDE", "WIDE"};
    QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT");
    int numbers[8]{};
    int number = 0;
    for (int i = 0; i < 8; ++i) {
        if (i && std::string(segments[i]) != segments[i - 1]) { html += row("-", segments[i], "-"); ++number; }
        numbers[i] = ++number;
        const auto line = QString("Line number %1.").arg(i).toStdString();
        html += row(types[i], "", line.c_str());
    }
    html += "</table></body></html>";
    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "production-template"));
    for (int i = 0; i < 8; ++i) {
        const QString line = QString("Line number %1.").arg(i);
        sync.attachGeneratedAudio(makeTone(QString("voice%1.wav").arg(i), 2), QString::fromLatin1(kShotScriptCharacter),
                                  line, numbers[i], segments[i], 2.0);
        RenderVersion rv; rv.file = render.toStdString(); rv.tailPadSec = 0.25;
        sync.pipeline().shots[ScriptMatcher::normalize(line.toStdString())].renders.push_back(rv);
    }
    sync.pipeline().background = render.toStdString();
    sync.pipeline().closeUpBackgroundZoom = 2;
    sync.pipeline().closeUpBackgroundOffsetX = 945.8;
    sync.pipeline().closeUpBackgroundOffsetY = 529.7;
    sync.pipeline().timelineTemplate = recipe.toStdString();
    Timeline timeline;
    CommandStack stack;
    ComfyPanel panel(&sync);
    QString error;
    ASSERT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &error), 8) << error.toStdString();
    EXPECT_EQ(timeline.settings().frameRate(), 60);
    std::vector<Track*> video, audio;
    for (size_t t = 0; t < timeline.trackCount(); ++t) {
        auto* track = timeline.track(t);
        if (track->isDivider()) continue;
        (track->type() == TrackType::Video ? video : audio).push_back(track);
    }
    ASSERT_EQ(video.size(), 6u); ASSERT_EQ(audio.size(), 2u);
    // Eight shots, then the ending stinger on the same track (the episode's
    // own footage goes under it, on V1).
    ASSERT_EQ(video[4]->clipCount(), 9u);
    const Clip* ending = video[4]->clip(8);
    EXPECT_EQ(ending->label(), "Ending");
    EXPECT_GE(ending->timelineIn(), video[4]->clip(7)->timelineOut());
    const bool center[] = {true, false, true, false, true, false, false, true};
    for (size_t i = 0; i < 8; ++i) {
        auto* character = video[4]->clip(i);
        EXPECT_FLOAT_EQ(character->positionX().defaultValue(), center[i] ? 0.0f : -548.0f);
        EXPECT_FLOAT_EQ(character->positionY().defaultValue(), 12.0f);
        ASSERT_EQ(character->effects().effectCount(), 1u);
        EXPECT_EQ(character->effects().effect(0).effectType(), EffectType::ChromaKey);
        EXPECT_EQ(audio[0]->clip(i)->timelineIn(), character->timelineIn());
        EXPECT_EQ(audio[0]->clip(i)->linkId(), character->linkId());
        auto active = video[5]->clipsAtTime(character->timelineIn());
        ASSERT_EQ(active.size(), 1u);
        EXPECT_EQ(active.front()->sourceIn(), 0);
        EXPECT_FLOAT_EQ(active.front()->scaleX().defaultValue(), i == 4 ? 2.0f : 1.0f);
        EXPECT_FLOAT_EQ(active.front()->positionX().defaultValue(), i == 4 ? 945.8f : 0.0f);
    }
    EXPECT_EQ(video[4]->clip(0)->timelineIn(), kTicksPerSecond);
    ASSERT_EQ(video[0]->clipCount(), 1u); ASSERT_EQ(video[2]->clipCount(), 1u);
    // Bar and grade run to the end of the ending stinger; only its tail flash
    // reaches past it, onto where the episode's own footage goes.
    EXPECT_EQ(video[0]->clip(0)->timelineOut(), ending->timelineOut());
    EXPECT_EQ(video[2]->clip(0)->timelineOut(), ending->timelineOut());
    const auto& grade = video[2]->clip(0)->effects().effect(0);
    EXPECT_FLOAT_EQ(grade.evalParam(ColorGrading::Vibrance, 0), 40);
    EXPECT_FLOAT_EQ(grade.evalParam(ColorGrading::CreativeSat, 0), 115);
    // Section flashes plus a white flash before and after the ending stinger.
    EXPECT_EQ(video[1]->clipCount(), 9u); EXPECT_EQ(video[1]->transitionCount(), 18u);
    const auto endingFlashes = video[1]->clipsAtTime(ending->timelineIn());
    ASSERT_EQ(endingFlashes.size(), 1u);
    EXPECT_EQ(endingFlashes.front()->timelineIn(), ending->timelineIn() - kTicksPerSecond / 5);
    const auto tailFlashes = video[1]->clipsAtTime(ending->timelineOut() - 1);
    ASSERT_EQ(tailFlashes.size(), 1u);
    EXPECT_EQ(tailFlashes.front()->timelineIn(), ending->timelineOut() - kTicksPerSecond / 5);
    EXPECT_EQ(timeline.duration(), tailFlashes.front()->timelineOut());
    EXPECT_EQ(video[3]->clipCount(), 0u);
    ASSERT_EQ(timeline.markers().size(), 4u);
    EXPECT_EQ(timeline.markers()[2].label, "MINI-GAME");
    const auto miniStart = timeline.markers()[2].time - 3 * kTicksPerSecond / 2;
    auto miniVideo = video[5]->clipsAtTime(miniStart);
    ASSERT_EQ(miniVideo.size(), 1u);
    EXPECT_EQ(miniVideo.front()->sourceIn(), kTicksPerSecond / 2); // 8 frames at 16 fps
    EXPECT_EQ(miniVideo.front()->duration(), 3 * kTicksPerSecond / 2);
    auto miniMusic = audio[1]->clipsAtTime(miniStart);
    ASSERT_EQ(miniMusic.size(), 1u);
    EXPECT_EQ(miniMusic.front()->sourceIn(), 3 * kTicksPerSecond / 2); // WAV Source Monitor: 1:12 at 24 fps
    EXPECT_FLOAT_EQ(static_cast<AudioClip*>(miniMusic.front())->volume().defaultValue(), 0.5f);
    EXPECT_EQ(audio[1]->transitionCount(), 4u);
    for (const auto& transition : audio[1]->transitions()) {
        EXPECT_LT(audio[1]->findClipIndexById(transition.leftClipId), audio[1]->clipCount());
        EXPECT_LT(audio[1]->findClipIndexById(transition.rightClipId), audio[1]->clipCount());
    }
    ASSERT_TRUE(stack.undo()); EXPECT_EQ(timeline.duration(), 0);
    ASSERT_TRUE(stack.redo()); ASSERT_EQ(timeline.markers().size(), 4u);
    EXPECT_EQ(timeline.markers()[2].label, "MINI-GAME");

    // Missing media or an unknown section aborts the detached build, leaving
    // the existing timeline and undo stack intact.
    const auto duration = timeline.duration();
    QFile::remove(music);
    EXPECT_EQ(panel.assembleIntoTimeline(&timeline, &stack, &error), 0);
    EXPECT_TRUE(error.contains("Missing template media"));
    EXPECT_EQ(timeline.duration(), duration);
}

TEST_F(ComfyPanelTest, RealProductionTemplateSmoke)
{
    const auto path = qEnvironmentVariable("ROUNDTABLE_TEST_TIMELINE_TEMPLATE");
    if (path.isEmpty()) GTEST_SKIP() << "Set template path to validate the local production assets";
    ComfyTimelineTemplate production;
    QString error;
    ASSERT_TRUE(production.load(path, &error)) << error.toStdString();
    Timeline timeline;
    AssemblyPlan plan;
    std::vector<AssemblyShot> shots{{"INTRO", false, 4}, {"INTRO", true, 3}, {"STORY", false, 75},
        {"BANNER", false, 4}, {"SKINS", false, 4}, {"MINI-GAME", false, 6}, {"CONCLUSION", false, 4}};
    ASSERT_TRUE(production.build(shots, 0, 15.069, timeline, plan, &error)) << error.toStdString();
    ASSERT_EQ(plan.shots.size(), shots.size()); ASSERT_EQ(plan.markers.size(), 6u);
    ASSERT_FALSE(plan.backgrounds.empty());
    for (auto& bg : plan.backgrounds) EXPECT_EQ(bg.sourceIn, 0);
    EXPECT_GT(plan.end, plan.shots.back().timelineIn + plan.shots.back().duration);
    // Ordinary Belorta-style export has no mini-game stinger or music.
    shots.erase(shots.begin() + 5);
    Timeline withoutMini;
    ASSERT_TRUE(production.build(shots, 0, 15.069, withoutMini, plan, &error)) << error.toStdString();
    ASSERT_EQ(plan.markers.size(), 5u);
    for (size_t t = 0; t < withoutMini.trackCount(); ++t)
        for (size_t c = 0; c < withoutMini.track(t)->clipCount(); ++c)
            if (auto* audio = dynamic_cast<AudioClip*>(withoutMini.track(t)->clip(c)))
                EXPECT_EQ(audio->mediaPath().find("MINIGAME"), std::string::npos);
}

TEST_F(ComfyPanelTest, InspectSourceMonitorAudioTimecode)
{
    const auto path = qEnvironmentVariable("ROUNDTABLE_TEST_SOURCE_AUDIO");
    if (path.isEmpty()) GTEST_SKIP() << "Set source audio path for the source-monitor diagnostic";
    MediaPool pool;
    auto handle = pool.open(std::filesystem::path(path.toStdWString()));
    ASSERT_NE(handle, 0u);
    SourceMonitor monitor;
    monitor.loadClip(handle, &pool);
    EXPECT_DOUBLE_EQ(monitor.frameRate(), 24.0);
    const auto tick = timecodeToTick(Timecode{0, 3, 5, 52}, monitor.frameRate());
    EXPECT_EQ(tick, 8984000);
    EXPECT_EQ(tickToTimecode(tick, monitor.frameRate()).toString(), "00:03:07:04");
    monitor.clearClip();
}

TEST_F(ComfyPanelTest, CapturedProjectProducesReviewExport)
{
    const auto recipePath = qEnvironmentVariable("ROUNDTABLE_TEST_TIMELINE_TEMPLATE");
    const auto outputPath = qEnvironmentVariable("ROUNDTABLE_TEST_ASSEMBLY_OUTPUT");
    if (recipePath.isEmpty() || outputPath.isEmpty()) GTEST_SKIP() << "Set template and review-output paths";
    QFile recipeFile(recipePath); ASSERT_TRUE(recipeFile.open(QIODevice::ReadOnly));
    const auto recipe = QJsonDocument::fromJson(recipeFile.readAll()).object();
    const auto sourcePath = QFileInfo(recipePath).absoluteDir().absoluteFilePath(recipe.value("project").toString());
    auto source = ProjectSerializer{}.load(std::filesystem::path(sourcePath.toStdWString()));
    ASSERT_TRUE(source);
    AudioSync sync;
    sync.deserializeFromBlob(source->audioSyncBlob());
    sync.pipeline().timelineTemplate = recipePath.toStdString();
    ASSERT_EQ(sync.showShots().size(), 48u);
    auto review = Project::createNew("ComfyUI production template review");
    ComfyPanel panel(&sync);
    QString error;
    ASSERT_EQ(panel.assembleIntoTimeline(review->timeline(), review->commandStack(), &error), 48) << error.toStdString();
    review->timeline()->setName("GACHA IS SUFFERING: LITTLE WITCH TRICK — export review");
    ASSERT_EQ(review->timeline()->markers().size(), 5u);
    ASSERT_TRUE(ProjectSerializer{}.save(*review, std::filesystem::path(outputPath.toStdWString())));
    auto reloaded = ProjectSerializer{}.load(std::filesystem::path(outputPath.toStdWString()));
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(ProjectSerializer{}.serialize(*review), ProjectSerializer{}.serialize(*reloaded));
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

TEST_F(ComfyPanelTest, ReviewCheckboxesAreExclusiveAndRenderAllCountsMissingAndRetakes)
{
    const QString video = makeRender("review.mp4");
    if (video.isEmpty()) GTEST_SKIP() << "bundled ffmpeg not available";
    AudioSync sync;
    const QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT")
        + row("WIDE", "", "First line.") + row("CLOSE-UP", "", "Second line.") + "</table></body></html>";
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "comfy-review-test"));
    const QString secondVideo = dir.filePath("review-second.mp4");
    ASSERT_TRUE(QFile::copy(video, secondVideo));
    sync.attachGeneratedAudio(video, QString::fromLatin1(kShotScriptCharacter), "First line.", 1, "INTRO", 2.0);
    sync.attachGeneratedAudio(secondVideo, QString::fromLatin1(kShotScriptCharacter), "Second line.", 2, "INTRO", 2.0);
    ASSERT_EQ(sync.showShots().size(), 2u);
    for (const auto& shot : sync.showShots()) ASSERT_TRUE(shot.hasTake);
    auto& settings = sync.pipeline().shots[ScriptMatcher::normalize("First line.")];
    settings.renders.push_back({video.toStdString()});
    ComfyPanel panel(&sync);
    panel.refresh();
    auto* tree = panel.shotTree();
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    ASSERT_EQ(tree->topLevelItem(0)->childCount(), 2);
    auto* first = tree->topLevelItem(0)->child(0);
    auto* second = tree->topLevelItem(0)->child(1);
    int approve = -1, retake = -1;
    for (int c = 0; c < tree->columnCount(); ++c) {
        if (tree->headerItem()->text(c) == "APPROVE") approve = c;
        if (tree->headerItem()->text(c) == "RETAKE") retake = c;
    }
    ASSERT_GE(approve, 0);
    ASSERT_GE(retake, 0);
    EXPECT_EQ(panel.renderAllButton()->text(), "RENDER ALL (1)");
    EXPECT_FALSE(ComfyPanel::needsRender(settings)); // existing, unreviewed
    second->setCheckState(approve, Qt::Checked);    // no render: cannot approve
    EXPECT_EQ(second->checkState(approve), Qt::Unchecked);
    first->setCheckState(approve, Qt::Checked);
    EXPECT_EQ(settings.review, ShotReview::Approved);
    EXPECT_EQ(settings.reviewedRender, video.toStdString());
    EXPECT_FALSE(ComfyPanel::needsRender(settings));
    first->setCheckState(retake, Qt::Checked);
    EXPECT_EQ(first->checkState(approve), Qt::Unchecked);
    EXPECT_EQ(settings.review, ShotReview::Retake);
    EXPECT_TRUE(ComfyPanel::needsRender(settings));
    EXPECT_EQ(panel.renderAllButton()->text(), "RENDER ALL (2)");
    panel.refresh(); // refreshing rows must not clear the review decision
    first = tree->topLevelItem(0)->child(0);
    EXPECT_EQ(first->checkState(retake), Qt::Checked);
    first->setCheckState(approve, Qt::Checked);
    EXPECT_EQ(first->checkState(retake), Qt::Unchecked);
    EXPECT_EQ(panel.renderAllButton()->text(), "RENDER ALL (1)");
    first->setCheckState(approve, Qt::Unchecked);
    EXPECT_EQ(settings.review, ShotReview::Unreviewed);
    EXPECT_FALSE(ComfyPanel::needsRender(settings));
    settings.renders[0].file = dir.filePath("deleted.mp4").toStdString();
    settings.review = ShotReview::Approved;
    settings.reviewedRender = settings.renders[0].file;
    EXPECT_TRUE(ComfyPanel::needsRender(settings)); // stale records must not suppress a render
    panel.refresh();
    EXPECT_EQ(panel.renderAllButton()->text(), "RENDER ALL (2)");
    EXPECT_EQ(tree->topLevelItem(0)->child(0)->checkState(approve), Qt::Unchecked);
}

TEST_F(ComfyPanelTest, EveryShotHeaderSortsBothDirectionsWithoutMixingSegmentsOrShotIdentity)
{
    const QString firstVideo = makeRender("take10.mp4");
    if (firstVideo.isEmpty()) GTEST_SKIP() << "bundled ffmpeg not available";
    const QString secondVideo = dir.filePath("take2.mp4");
    ASSERT_TRUE(QFile::copy(firstVideo, secondVideo));
    QString html = "<html><body><table>" + row("SHOT", "ACTION", "SCRIPT")
        + row("WIDE", "", "Zebra line.") + row("CLOSE-UP", "", "Apple line.");
    for (int i = 3; i <= 10; ++i) html += row("WIDE", "", qPrintable(QString("Extra line %1.").arg(i)));
    html += row("-", "STORY", "-") + row("WIDE", "", "Story line.") + "</table></body></html>";
    AudioSync sync;
    ASSERT_TRUE(sync.loadScript(html.toStdString(), "comfy-sort-test"));
    sync.attachGeneratedAudio(firstVideo, QString::fromLatin1(kShotScriptCharacter), "Zebra line.", 1, "INTRO", 2.0);
    sync.attachGeneratedAudio(secondVideo, QString::fromLatin1(kShotScriptCharacter), "Apple line.", 2, "INTRO", 10.0);
    auto& first = sync.pipeline().shots[ScriptMatcher::normalize("Zebra line.")];
    auto& second = sync.pipeline().shots[ScriptMatcher::normalize("Apple line.")];
    first.padFrames = 10;
    first.renders.push_back({firstVideo.toStdString()});
    first.review = ShotReview::Approved;
    first.reviewedRender = firstVideo.toStdString();
    second.padFrames = 2;
    second.renders.push_back({secondVideo.toStdString()});
    second.review = ShotReview::Retake;
    ComfyPanel panel(&sync);
    panel.resize(2400, 900);
    panel.show();
    panel.refresh();
    auto* tree = panel.shotTree();
    auto* header = tree->header();
    for (int c = 0; c < tree->columnCount(); ++c) {
        header->showSection(c);
        header->resizeSection(c, 60);
    }
    QApplication::processEvents();
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    auto position = [&](int number) {
        auto* group = tree->topLevelItem(0);
        for (int i = 0; i < group->childCount(); ++i)
            if (group->child(i)->text(0).toInt() == number) return i;
        return -1;
    };
    // Expected relative order of shots 1 and 2 for every ascending column.
    const bool firstBeforeSecond[] = {true, false, false, false, true, false, true, false, true};
    ASSERT_EQ(tree->columnCount(), 9);
    for (int c = 0; c < tree->columnCount(); ++c) {
        SCOPED_TRACE(header->model()->headerData(c, Qt::Horizontal).toString().toStdString());
        tree->sortItems(c, Qt::DescendingOrder);
        const QPoint point(header->sectionViewportPosition(c) + header->sectionSize(c) / 2, header->height() / 2);
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        EXPECT_EQ(header->sortIndicatorOrder(), Qt::AscendingOrder);
        EXPECT_EQ(position(1) < position(2), firstBeforeSecond[c]);
        if (c == 0) EXPECT_LT(position(2), position(10));
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        EXPECT_EQ(header->sortIndicatorOrder(), Qt::DescendingOrder);
        EXPECT_EQ(position(1) < position(2), !firstBeforeSecond[c]);
        EXPECT_EQ(tree->topLevelItem(0)->text(0), "INTRO");
        EXPECT_EQ(tree->topLevelItem(1)->text(0), "STORY");
    }
    tree->sortItems(1, Qt::AscendingOrder); // CLOSE-UP first; still belongs to shot 2
    auto* closeUp = tree->topLevelItem(0)->child(position(2));
    tree->setCurrentItem(closeUp);
    closeUp->setCheckState(7, Qt::Checked);
    EXPECT_EQ(second.review, ShotReview::Approved);
    EXPECT_EQ(first.review, ShotReview::Approved);
    panel.refresh();
    EXPECT_EQ(tree->sortColumn(), 1);
    EXPECT_EQ(tree->currentItem()->text(0), "2");
    EXPECT_LT(position(2), position(1));
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
