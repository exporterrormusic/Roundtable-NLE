#include <gtest/gtest.h>
#include "AudioMixdown.h"
#include "panels/audio/AudioSync.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_argc = 1;
char g_arg0[] = "test_audio_sync_match";
char* g_argv[] = {g_arg0, nullptr};

class AudioSyncMatchTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(g_argc, g_argv);
        settingsDirectory = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(settingsDirectory->isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           settingsDirectory->path());
    }

    static void TearDownTestSuite() { settingsDirectory.reset(); }

    static QString makeTone(const QString& directory, const QString& name, double seconds)
    {
        constexpr uint32_t sampleRate = 44100;
        rt::MixdownResult result;
        result.sampleRate = sampleRate;
        result.channels = 1;
        result.totalFrames = static_cast<int64_t>(seconds * sampleRate);
        result.duration = seconds;
        result.samples.resize(static_cast<size_t>(result.totalFrames));
        for (int64_t i = 0; i < result.totalFrames; ++i)
            result.samples[static_cast<size_t>(i)] =
                0.15f * std::sin(2.0 * 3.141592653589793 * 220.0 * i / sampleRate);
        const QString path = directory + QStringLiteral("/") + name;
        EXPECT_TRUE(rt::AudioMixdown::writeWav(
            result, std::filesystem::path(path.toStdWString())));
        return path;
    }

    static std::string scriptOf(int lines)
    {
        std::string script;
        for (int i = 1; i <= lines; ++i) {
            script += (i % 2) ? "ALICE: " : "BOB: ";
            script += "This is script line number " + std::to_string(i) +
                      " with enough words to wrap a little in the card.\n";
        }
        return script;
    }

    /// One matched clip on every other line (alternating states).
    static std::vector<rt::SyncClip> clipsFor(int lines, const std::string& source)
    {
        std::vector<rt::SyncClip> clips;
        for (int line = 1; line <= lines; line += 2) {
            rt::SyncClip clip;
            clip.id = static_cast<int>(clips.size());
            clip.sourceFile = source;
            clip.character = "Alice";
            clip.start = line * 2.0;
            clip.end = line * 2.0 + 1.5;
            clip.transcript = "line";
            clip.matchState = (line % 4 == 1) ? 2 : 1;
            clip.confidence = 0.9f;
            clip.scriptLineNumber = line;
            clips.push_back(std::move(clip));
        }
        return clips;
    }

    /// Spin the event loop until `done` or the deadline.
    static bool waitFor(const std::function<bool()>& done, int timeoutMs = 20000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!done()) {
            if (timer.elapsed() > timeoutMs) return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        return true;
    }

    static inline std::unique_ptr<QApplication> app;
    static inline std::unique_ptr<QTemporaryDir> settingsDirectory;
};

TEST_F(AudioSyncMatchTest, CardsStreamInIndexAlignedWithTheLeftList)
{
    constexpr int kLines = 300;
    rt::AudioSync audioSync;
    audioSync.resize(1400, 900);
    audioSync.show();
    ASSERT_TRUE(audioSync.loadScript(scriptOf(kLines), "memory://stream-test"));
    ASSERT_TRUE(waitFor([&] { return !audioSync.cardBuildPending(); }));

    QElapsedTimer timer;
    timer.start();
    audioSync.setClipsForTesting(clipsFor(kLines, "C:/missing/take.wav"));
    const qint64 firstScreenMs = timer.elapsed();

    // Every row exists up front (index-aligned with the left list); only a
    // screenful was built before returning.
    EXPECT_EQ(audioSync.cardRowCount(), kLines);
    ASSERT_NE(audioSync.scriptListWidget(), nullptr);
    EXPECT_EQ(audioSync.scriptListWidget()->count(), kLines);
    EXPECT_TRUE(audioSync.cardBuildPending());
    EXPECT_GT(audioSync.builtCardCount(), 0);
    EXPECT_LT(audioSync.builtCardCount(), kLines);
    QListWidget* left = audioSync.scriptListWidget();
    EXPECT_NE(left->itemWidget(left->item(0)), nullptr);            // first screen
    EXPECT_EQ(left->itemWidget(left->item(kLines - 1)), nullptr);   // still to come

    ASSERT_TRUE(waitFor([&] { return !audioSync.cardBuildPending(); }));
    const qint64 allMs = timer.elapsed();
    EXPECT_EQ(audioSync.builtCardCount(), kLines);
    for (int row = 0; row < kLines; ++row)
        ASSERT_NE(left->itemWidget(left->item(row)), nullptr) << "left row " << row;

    std::printf("[ perf ] %d matched-script cards: first screen %lld ms, all %lld ms\n",
                kLines, static_cast<long long>(firstScreenMs),
                static_cast<long long>(allMs));
}

TEST_F(AudioSyncMatchTest, FullCardBuildCost)
{
    constexpr int kLines = 200;
    rt::AudioSync audioSync;
    audioSync.resize(1400, 900);
    audioSync.show();
    ASSERT_TRUE(audioSync.loadScript(scriptOf(kLines), "memory://cost-test"));
    ASSERT_TRUE(waitFor([&] { return !audioSync.cardBuildPending(); }));

    QElapsedTimer timer;
    timer.start();
    audioSync.setClipsForTesting(clipsFor(kLines, "C:/missing/take.wav"));
    audioSync.finishCardBuild();
    const qint64 ms = timer.elapsed();
    EXPECT_EQ(audioSync.builtCardCount(), kLines);
    std::printf("[ perf ] full synchronous build of %d cards: %lld ms (%.2f ms/card)\n",
                kLines, static_cast<long long>(ms), static_cast<double>(ms) / kLines);
}

TEST_F(AudioSyncMatchTest, RebuildWhileStreamingReplacesTheStaleBuild)
{
    rt::AudioSync audioSync;
    audioSync.resize(1400, 900);
    audioSync.show();
    ASSERT_TRUE(audioSync.loadScript(scriptOf(240), "memory://first-script"));
    audioSync.setClipsForTesting(clipsFor(240, "C:/missing/a.wav"));
    ASSERT_TRUE(audioSync.cardBuildPending());

    // A new script while the first build is still streaming.
    ASSERT_TRUE(audioSync.loadScript(scriptOf(90), "memory://second-script"));
    ASSERT_TRUE(waitFor([&] { return !audioSync.cardBuildPending(); }));
    EXPECT_EQ(audioSync.cardRowCount(), 90);
    EXPECT_EQ(audioSync.builtCardCount(), 90);
    EXPECT_EQ(audioSync.scriptListWidget()->count(), 90);
}

TEST_F(AudioSyncMatchTest, ImportDecodesSamplesInTheBackground)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString tone = makeTone(temporary.path(), QStringLiteral("ALICE-take.wav"), 1.5);

    rt::AudioSync audioSync;
    ASSERT_TRUE(audioSync.importAudio(tone.toStdString()));
    // The import returns before the decode lands (it used to block the UI
    // thread for the whole file).
    EXPECT_TRUE(audioSync.audioSamplesPending());
    EXPECT_FALSE(audioSync.hasAudioSamples(tone.toStdString()));

    ASSERT_TRUE(waitFor([&] { return !audioSync.audioSamplesPending(); }));
    EXPECT_TRUE(audioSync.hasAudioSamples(tone.toStdString()));
}

TEST_F(AudioSyncMatchTest, PanelClosedMidDecodeIsSafe)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString tone = makeTone(temporary.path(), QStringLiteral("BOB-take.wav"), 2.0);

    auto audioSync = std::make_unique<rt::AudioSync>();
    ASSERT_TRUE(audioSync->importAudio(tone.toStdString()));
    ASSERT_TRUE(audioSync->audioSamplesPending());
    audioSync.reset();

    // Any completion still queued must find the panel gone and do nothing.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    SUCCEED();
}

} // namespace
