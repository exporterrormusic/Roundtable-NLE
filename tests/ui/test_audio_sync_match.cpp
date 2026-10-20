#include <gtest/gtest.h>
#include "AudioSyncFixture.h"
#include "audio/AudioFile.h"
#include "AudioMixdown.h"
#include "command/CommandStack.h"
#include "panels/audio/AudioSync.h"
#include "widgets/VideoFrameView.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {

class AudioSyncMatchTest : public AudioSyncFixture
{
protected:
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

    // Streaming 300 cards takes ~11 s on an idle machine and well past the
    // default 20 s under a parallel ctest run; this checks order, not speed.
    ASSERT_TRUE(waitFor([&] { return !audioSync.cardBuildPending(); }, 90000));
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

    // The samples are the file's, served from a memory-mapped cache with
    // their waveform peaks already built (off the UI thread).
    rt::AudioFile direct;
    ASSERT_TRUE(direct.open(tone.toStdString()));
    const std::vector<float> expected = direct.readAll();
    const rt::AudioSampleData* loaded = audioSync.voiceAudioSamples(tone);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->sampleRate, 44100u);
    EXPECT_TRUE(loaded->samples.isMapped());
    EXPECT_TRUE(loaded->samples.hasBlockPeaks());
    ASSERT_EQ(loaded->samples.size(), expected.size());
    EXPECT_TRUE(std::equal(loaded->samples.begin(), loaded->samples.end(), expected.begin()));

    // A second panel loads the same file from the cache, mapped as well.
    rt::AudioSync again;
    ASSERT_TRUE(again.importAudio(tone.toStdString()));
    ASSERT_TRUE(waitFor([&] { return !again.audioSamplesPending(); }));
    const rt::AudioSampleData* cached = again.voiceAudioSamples(tone);
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(cached->samples.isMapped());
    ASSERT_EQ(cached->samples.size(), expected.size());
    EXPECT_TRUE(std::equal(cached->samples.begin(), cached->samples.end(), expected.begin()));
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

TEST_F(AudioSyncMatchTest, ConfirmKeepsTheVideoPreviewAndControls)
{
    for (bool shotScript : {false, true}) {
        SCOPED_TRACE(shotScript ? "shot script" : "dialogue script");
        rt::AudioSync audioSync;
        audioSync.resize(1400, 900);
        audioSync.show();
        const std::string script = shotScript
            ? "<html><body><table><tr><td>SHOT</td><td>ACTION</td><td>SCRIPT</td></tr>"
              "<tr><td>WIDE</td><td></td><td>A test video line.</td></tr></table></body></html>"
            : "ALICE: A test video line.\n";
        ASSERT_TRUE(audioSync.loadScript(script, "memory://preview-test"));
        auto clips = clipsFor(1, "C:/missing/preview.mp4");
        clips.front().matchState = 1;
        audioSync.setClipsForTesting(std::move(clips));
        audioSync.finishCardBuild();
        QPointer<rt::VideoFrameView> preview = audioSync.findChild<rt::VideoFrameView*>();
        ASSERT_TRUE(preview);
        QImage poster(192, 108, QImage::Format_RGB32);
        poster.fill(Qt::green);
        preview->setPoster(poster);
        QImage live(192, 108, QImage::Format_RGB32);
        live.fill(Qt::red);
        preview->setFrame(live);
        QPointer<QPushButton> confirm;
        for (auto* button : audioSync.findChildren<QPushButton*>())
            if (button->toolTip() == "Confirm match") confirm = button;
        ASSERT_TRUE(confirm);
        const auto buttonsBefore = audioSync.findChildren<QPushButton*>().size();
        confirm->click();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ASSERT_TRUE(preview) << "Confirm must not discard the displayed video frame";
        EXPECT_EQ(preview->grab().toImage().pixelColor(96, 54), QColor(Qt::red));
        EXPECT_EQ(audioSync.findChildren<QPushButton*>().size(), buttonsBefore);
        ASSERT_TRUE(confirm);
        EXPECT_EQ(confirm->toolTip(), "Click to unconfirm match");
        confirm->click();
        QCoreApplication::processEvents();
        ASSERT_TRUE(preview);
        EXPECT_EQ(preview->grab().toImage().pixelColor(96, 54), QColor(Qt::red));
        EXPECT_EQ(confirm->toolTip(), "Confirm match");
    }
}

TEST_F(AudioSyncMatchTest, LeavingPreviewWhilePosterIsPendingKeepsLastFrame)
{
    rt::VideoFrameView preview;
    preview.resize(192, 108);
    QImage live(192, 108, QImage::Format_RGB32);
    live.fill(Qt::red);
    preview.setFrame(live);
    preview.showPoster();
    EXPECT_EQ(preview.grab().toImage().pixelColor(96, 54), QColor(Qt::red));

    QImage poster(192, 108, QImage::Format_RGB32);
    poster.fill(Qt::green);
    preview.setPoster(poster);
    EXPECT_EQ(preview.grab().toImage().pixelColor(96, 54), QColor(Qt::green));

    // A late poster must not interrupt playback, and a failed decode must
    // not erase the last good frame. Switching sources clears explicitly.
    preview.setFrame(live);
    preview.setPoster(poster);
    preview.setFrame(QImage());
    EXPECT_EQ(preview.grab().toImage().pixelColor(96, 54), QColor(Qt::red));
    preview.clear();
    EXPECT_EQ(preview.grab().toImage().pixelColor(96, 54), QColor(Qt::black));
}

TEST_F(AudioSyncMatchTest, PickingAnotherTakeAndUndoKeepPreviewAndUpdateChips)
{
    rt::CommandStack commands;
    rt::AudioSync audioSync;
    audioSync.setCommandStack(&commands);
    ASSERT_TRUE(audioSync.loadScript(
        "<html><body><table><tr><td>SHOT</td><td>ACTION</td><td>SCRIPT</td></tr>"
        "<tr><td>WIDE</td><td></td><td>A test video line.</td></tr></table></body></html>",
        "memory://take-confirm-test"));
    auto clips = clipsFor(1, "C:/missing/takes.mp4");
    auto second = clips.front();
    second.id = 1;
    second.start += 3.0;
    second.end += 3.0;
    second.matchState = 1;
    clips.push_back(second);
    audioSync.setClipsForTesting(std::move(clips));
    audioSync.finishCardBuild();
    auto chips = audioSync.findChildren<QPushButton*>("takeChip");
    ASSERT_EQ(chips.size(), 2);
    chips[1]->click();
    QCoreApplication::processEvents();
    chips = audioSync.findChildren<QPushButton*>("takeChip");
    ASSERT_EQ(chips.size(), 2);
    QPointer<rt::VideoFrameView> preview = audioSync.findChild<rt::VideoFrameView*>();
    ASSERT_TRUE(preview);
    QImage image(192, 108, QImage::Format_RGB32);
    image.fill(Qt::red);
    preview->setFrame(image);
    auto* confirm = audioSync.findChild<QPushButton*>("confirmMatchButton");
    ASSERT_NE(confirm, nullptr);
    confirm->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(preview);
    EXPECT_EQ(audioSync.clip(0).matchState, 1);
    EXPECT_EQ(audioSync.clip(1).matchState, 2);
    EXPECT_FALSE(chips[0]->text().startsWith(QChar(0x2713)));
    EXPECT_TRUE(chips[1]->text().startsWith(QChar(0x2713)));
    commands.undo();
    QCoreApplication::processEvents();
    ASSERT_TRUE(preview);
    EXPECT_EQ(audioSync.clip(0).matchState, 2);
    EXPECT_EQ(audioSync.clip(1).matchState, 1);
    EXPECT_TRUE(chips[0]->text().startsWith(QChar(0x2713)));
    EXPECT_FALSE(chips[1]->text().startsWith(QChar(0x2713)));
    commands.redo();
    QCoreApplication::processEvents();
    ASSERT_TRUE(preview);
    EXPECT_EQ(audioSync.clip(0).matchState, 1);
    EXPECT_EQ(audioSync.clip(1).matchState, 2);
    EXPECT_FALSE(chips[0]->text().startsWith(QChar(0x2713)));
    EXPECT_TRUE(chips[1]->text().startsWith(QChar(0x2713)));
    EXPECT_EQ(preview->grab().toImage().pixelColor(96, 54), QColor(Qt::red));
}

} // namespace
