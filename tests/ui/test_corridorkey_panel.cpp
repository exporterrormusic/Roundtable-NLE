/*
 * test_corridorkey_panel.cpp — Ultra Key's CorridorKey row and the service.
 *
 * Hermetic tests: the row offers "Render with CorridorKey" on a video clip,
 * and on a clip that plays a CorridorKey render it offers "Back to Ultra Key",
 * which restores the source and the live key as one undo step.
 *
 * CorridorKeyLive.* runs the REAL setup (about 8 GB of downloads) and a real
 * render on the GPU. It only runs with ROUNDTABLE_CORRIDORKEY_LIVE=1 and
 * ROUNDTABLE_CORRIDORKEY_SOURCE=<a green-screen video>.
 */

#include <gtest/gtest.h>

#include "ai/CorridorKey.h"
#include "corridorkey/CorridorKeyService.h"
#include "panels/effects/EffectControlsPanel.h"

#include "Constants.h"
#include "PathUtils.h"
#include "command/CommandStack.h"
#include "decode/VideoDecoder.h"
#include "effects/ChromaKey.h"
#include "project/Project.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>

#include <filesystem>
#include <fstream>

namespace {

namespace ck = rt::corridorkey;
namespace fs = std::filesystem;

int   s_argc = 1;
char  s_arg0[] = "test_corridorkey_panel";
char* s_argv[] = {s_arg0, nullptr};

class CorridorKeyTestEnv : public ::testing::Environment
{
public:
    void SetUp() override
    {
        if (!QApplication::instance()) new QApplication(s_argc, s_argv);
        // Keep the Steadier preference out of the real app's settings.
        QCoreApplication::setOrganizationName(QStringLiteral("RoundtableTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CorridorKeyTests"));
    }
};
auto* g_env = ::testing::AddGlobalTestEnvironment(new CorridorKeyTestEnv);

QPushButton* buttonWithText(QWidget& root, const QString& text)
{
    for (auto* button : root.findChildren<QPushButton*>())
        if (button->text() == text) return button;
    return nullptr;
}

struct Fixture
{
    QWidget            window;
    rt::Project        project;
    rt::CommandStack   stack;
    rt::Timeline*      sequence{nullptr};
    rt::Track*         track{nullptr};
    rt::VideoClip*     clip{nullptr};
    uint64_t           ultraKey{0};
    std::unique_ptr<rt::CorridorKeyService> service;

    explicit Fixture(const std::string& media)
    {
        sequence = project.timeline();
        track = sequence->addVideoTrack("V1");
        auto owned = std::make_unique<rt::VideoClip>(media);
        owned->setDuration(2 * rt::kTicksPerSecond);
        owned->setSourceFps(30000.0 / 1001.0);
        auto key = std::make_unique<rt::ChromaKey>();
        ultraKey = key->id();
        owned->effects().addEffect(std::move(key));
        clip = owned.get();
        track->addClip(std::move(owned));
        service = std::make_unique<rt::CorridorKeyService>(&window);
        service->setEditContextProvider([this]() {
            return rt::CorridorKeyService::EditContext{&project, &stack};
        });
    }
};

TEST(CorridorKeyPanel, UltraKeyOffersRenderWithCorridorKeyOnVideoClips)
{
    Fixture f("C:/footage/green screen.mp4");
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&f.stack);
    panel.setTimeline(f.sequence);
    panel.setClip(f.clip, f.track);
    EXPECT_NE(buttonWithText(panel, "Render with CorridorKey"), nullptr);
    EXPECT_EQ(buttonWithText(panel, "Back to Ultra Key"), nullptr);
    QCheckBox* steadier = nullptr;
    for (auto* box : panel.findChildren<QCheckBox*>())
        if (box->text() == "Steadier") steadier = box;
    ASSERT_NE(steadier, nullptr);
    EXPECT_EQ(steadier->isChecked(), f.service->steadier());
}

TEST(CorridorKeyPanel, ARenderTooShortForTheClipOffersRenderAgain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const fs::path root = rt::utf8ToPath(dir.path().toStdString());
    const fs::path source = root / "C0001.MP4";
    const fs::path render = root / "C0001 CK 0-59 abcd1234.mov";
    std::ofstream(source) << "source";
    std::ofstream(render) << "render";
    ck::RenderRecord record;
    record.sourcePath = rt::pathToUtf8(source);
    record.fps = {30000, 1001};
    record.firstFrame = 0;
    record.lastFrame = 59;                     // 2 s of frames
    record.keyRect = {0, 0, 1920, 1080};
    record.key = "abcd1234";
    ASSERT_TRUE(ck::writeRecord(ck::recordPathFor(render), record));

    Fixture f(rt::pathToUtf8(render));
    f.clip->setDuration(5 * rt::kTicksPerSecond);   // trimmed longer than the render
    f.clip->effects().effectById(f.ultraKey)->setEnabled(false);
    rt::EffectControlsPanel panel;
    panel.setCommandStack(&f.stack);
    panel.setTimeline(f.sequence);
    panel.setClip(f.clip, f.track);
    EXPECT_NE(buttonWithText(panel, "Render Again"), nullptr);
    EXPECT_EQ(buttonWithText(panel, "Back to Ultra Key"), nullptr);
}

TEST(CorridorKeyPanel, BackToUltraKeyRestoresTheSourceAsOneUndoStep)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const fs::path root = rt::utf8ToPath(dir.path().toStdString());
    const fs::path source = root / rt::utf8ToPath("C0001 \xEF\xBD\x9C.MP4");
    const fs::path render = root / "C0001 CK 4770-4949 abcd1234.mov";
    std::ofstream(source) << "source";
    std::ofstream(render) << "render";
    ck::RenderRecord record;
    record.sourcePath = rt::pathToUtf8(source);
    record.sourceCodec = "h264";
    record.sourceDuration = 44'828'784;
    record.fps = {30000, 1001};
    record.firstFrame = 4770;
    record.lastFrame = 4949;
    record.tickShift = ck::framesToTicks(4770, record.fps);
    record.keyRect = {0, 0, 1920, 1080};
    record.key = "abcd1234";
    ASSERT_TRUE(ck::writeRecord(ck::recordPathFor(render), record));

    Fixture f(rt::pathToUtf8(render));
    f.clip->setSourceIn(48'000);
    f.clip->setSourceCodecName("prores");
    f.clip->effects().effectById(f.ultraKey)->setEnabled(false);

    rt::EffectControlsPanel panel;
    panel.setCommandStack(&f.stack);
    panel.setTimeline(f.sequence);
    panel.setClip(f.clip, f.track);
    ASSERT_EQ(buttonWithText(panel, "Render with CorridorKey"), nullptr);
    QPushButton* back = buttonWithText(panel, "Back to Ultra Key");
    ASSERT_NE(back, nullptr);
    back->click();
    QCoreApplication::processEvents();   // the panel rebuilds after the swap

    EXPECT_EQ(f.clip->mediaPath(), rt::pathToUtf8(source));
    EXPECT_EQ(f.clip->sourceIn(), 48'000 + record.tickShift);
    EXPECT_EQ(f.clip->sourceDuration(), 44'828'784);
    EXPECT_EQ(f.clip->sourceCodecName(), "h264");
    EXPECT_TRUE(f.clip->effects().effectById(f.ultraKey)->isEnabled());
    EXPECT_NE(buttonWithText(panel, "Render with CorridorKey"), nullptr);

    ASSERT_TRUE(f.stack.canUndo());
    f.stack.undo();
    EXPECT_EQ(f.clip->mediaPath(), rt::pathToUtf8(render));
    EXPECT_EQ(f.clip->sourceIn(), 48'000);
    EXPECT_FALSE(f.clip->effects().effectById(f.ultraKey)->isEnabled());
}

// ── Live: real setup + real render (opt-in) ─────────────────────────────────

bool waitFor(const std::function<bool()>& done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) return false;
        QTest::qWait(500);
    }
    return true;
}

TEST(CorridorKeyLive, SetsUpAndRendersARealShot)
{
    if (qEnvironmentVariable("ROUNDTABLE_CORRIDORKEY_LIVE") != QStringLiteral("1"))
        GTEST_SKIP() << "set ROUNDTABLE_CORRIDORKEY_LIVE=1 and ROUNDTABLE_CORRIDORKEY_SOURCE to run";
    const std::string sourcePath = qEnvironmentVariable("ROUNDTABLE_CORRIDORKEY_SOURCE").toStdString();
    ASSERT_FALSE(sourcePath.empty());

    rt::VideoStreamInfo info;
    {
        rt::VideoDecoder probe;
        ASSERT_TRUE(probe.open(rt::utf8ToPath(sourcePath), true));
        info = probe.info();
    }

    Fixture f(sourcePath);
    QTemporaryDir projectDir;
    f.project.setFilePath(rt::utf8ToPath(projectDir.path().toStdString()) / "live.rtp");
    f.sequence->settings().setFrameRate(30000.0 / 1001.0);
    f.clip->setSourceFps(info.fps);
    f.clip->setSourceDuration(rt::secondsToTicks(info.duration));
    f.clip->setSourceCodecName(info.codecName);
    f.clip->setSourceIn(rt::kTicksPerSecond);          // 1 s in
    f.clip->setDuration(2 * rt::kTicksPerSecond);      // 2 s long

    if (!f.service->isInstalled()) {
        f.service->startSetup();
        ASSERT_TRUE(waitFor([&]() { return !f.service->isSetupRunning(); }, 3 * 3600 * 1000));
        ASSERT_TRUE(f.service->isInstalled()) << f.service->setupStage().toStdString();
    }

    const bool wasSteadier = f.service->steadier();
    f.service->setSteadier(qEnvironmentVariable("ROUNDTABLE_CORRIDORKEY_STEADIER") == QStringLiteral("1"));
    struct RestoreSteadier {
        rt::CorridorKeyService* s; bool on;
        ~RestoreSteadier() { s->setSteadier(on); }
    } restore{f.service.get(), wasSteadier};

    bool swapped = false;
    QObject::connect(f.service.get(), &rt::CorridorKeyService::clipSwapped,
                     [&](quint64 id) { swapped = id == f.clip->id(); });
    QString problem;
    ASSERT_TRUE(f.service->renderClip(f.sequence, f.clip, f.ultraKey, &problem)) << problem.toStdString();
    ASSERT_TRUE(waitFor([&]() { return swapped || (!f.service->jobStatus(f.clip->id())); },
                        60 * 60 * 1000));
    ASSERT_TRUE(swapped) << f.service->lastFailure(f.clip->id()).toStdString();

    // The clip plays a ProRes 4444 render the size of the source, with the
    // frames the record says, and Ultra Key is off.
    const auto record = ck::recordForMedia(f.clip->mediaPath());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->sourcePath, sourcePath);
    EXPECT_FALSE(f.clip->effects().effectById(f.ultraKey)->isEnabled());
    rt::VideoDecoder render;
    ASSERT_TRUE(render.open(rt::utf8ToPath(f.clip->mediaPath()), true));
    EXPECT_TRUE(render.info().hasAlpha);
    EXPECT_EQ(render.info().width, info.width);
    EXPECT_EQ(render.info().height, info.height);
    EXPECT_EQ(render.info().frameCount, record->lastFrame - record->firstFrame + 1);
    EXPECT_EQ(f.clip->sourceIn(), rt::kTicksPerSecond - record->tickShift);
    std::cout << "[live] render: " << f.clip->mediaPath() << " frames " << record->firstFrame << "-"
              << record->lastFrame << "\n";

    // Rendering the same clip again reuses the file instantly.
    QString back;
    ASSERT_TRUE(f.service->backToUltraKey(f.sequence, f.clip, f.ultraKey, &back));
    EXPECT_EQ(f.clip->mediaPath(), sourcePath);
    swapped = false;
    ASSERT_TRUE(f.service->renderClip(f.sequence, f.clip, f.ultraKey, &problem));
    EXPECT_TRUE(swapped);
    EXPECT_FALSE(f.service->jobStatus(f.clip->id()));
}

} // namespace
