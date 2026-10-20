/*
 * test_audio_sync_workflow.cpp — characterization of the AudioSync workflow:
 * auto-sync, clip edits, export to the timeline, script sessions and the
 * project blob. The expected strings were recorded from the code as it was
 * before the Stage 14 split; a later change that moves a clip boundary by a
 * millisecond, renumbers a match or reorders an export shows up here.
 *
 * Audio is synthetic: each character's file holds speech-like bursts at known
 * times, and the transcription results (whisper's job) are written by hand
 * with whisper's usual loose segment edges.
 */

#include <gtest/gtest.h>
#include "AudioSyncFixture.h"
#include "../support/StallGuard.h"

#include "ai/Transcriber.h"
#include "command/CommandStack.h"
#include "panels/audio/AudioSync.h"
#include "timeline/Clip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"

#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace rt {

/// Private steps the workflow tests drive (friend of AudioSync).
struct AudioSyncTestAccess
{
    static std::vector<SyncClip>& clips(AudioSync& a) { return a.session().clips; }
    static const std::vector<TranscriptionResult>& transcriptions(AudioSync& a)
    {
        return a.session().allTranscriptionResults;
    }
    static void confirmAll(AudioSync& a) { a.m_confirmAllActionBtn->click(); }
    static void closeGaps(AudioSync& a) { a.closeInterClipGaps(); }
    static void split(AudioSync& a, size_t clip) { a.splitClip(clip); }
    static void merge(AudioSync& a, size_t clip) { a.mergeClipWithNext(clip); }
    static void removeFile(AudioSync& a, int file) { a.removeFileFromSync(file); }
    static void switchTo(AudioSync& a, const std::string& key) { a.switchToScript(key); }
    static QListWidget* sessionList(AudioSync& a) { return a.m_scriptSessionList; }
    static std::vector<SyncClip>& clipsOf(AudioSync& a, const std::string& key)
    {
        return a.m_scriptSessions.at(key).clips;
    }
    static bool transcribing(AudioSync& a) { return a.m_transcriptionRunner->isActive(); }
    static void cancelTranscription(AudioSync& a) { a.onCancelTranscriptionClicked(); }
    static std::string transcribeStatus(AudioSync& a)
    {
        return a.m_transcribeStatus->text().toStdString();
    }
    static bool filterCharacter(AudioSync& a, const QString& name)
    {
        QListWidget* list = a.m_charFilterList;
        if (!list) return false;
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->text().compare(name, Qt::CaseInsensitive) == 0) {
                list->setCurrentRow(row);
                return true;
            }
        }
        return false;
    }
};

} // namespace rt

namespace {

using rt::AudioSyncTestAccess;

/// One spoken script line inside a character's recording.
struct Said
{
    const char* text;
    double start;   ///< burst start (s)
    double end;     ///< burst end (s)
};

const char* const kScript =
    "ALICE: Welcome back to the morning show everyone.\n"
    "BOB: Thanks Alice it is great to be here today.\n"
    "ALICE: Let us talk about the weather this weekend.\n"
    "BOB: It will be sunny with a light breeze on Saturday.\n"
    "ALICE: That sounds perfect for a picnic in the park.\n"
    "BOB: Do not forget to bring an umbrella just in case.\n";

const std::vector<Said> kAlice = {
    {"Welcome back to the morning show everyone.", 0.40, 2.40},
    {"Let us talk about the weather this weekend.", 3.20, 5.30},
    {"That sounds perfect for a picnic in the park.", 6.10, 8.00},
};
const std::vector<Said> kBob = {
    {"Thanks Alice it is great to be here today.", 0.30, 2.50},
    {"It will be sunny with a light breeze on Saturday.", 3.40, 5.60},
    {"Do not forget to bring an umbrella just in case.", 6.30, 8.40},
};
constexpr double kFileSeconds = 9.5;

std::vector<std::pair<double, double>> burstsOf(const std::vector<Said>& said)
{
    std::vector<std::pair<double, double>> bursts;
    for (const auto& s : said) bursts.emplace_back(s.start, s.end);
    return bursts;
}

/// What whisper returns for a file: one segment per burst with loose edges
/// (starts early, ends late) and, optionally, word times spread over it.
rt::TranscriptionResult transcriptionOf(const std::vector<Said>& said, bool withWords)
{
    rt::TranscriptionResult result;
    result.status = rt::TranscriptionStatus::Success;
    result.language = "en";
    result.duration = kFileSeconds;
    int id = 0;
    for (const auto& s : said) {
        rt::TranscriptionSegment segment;
        segment.id = id++;
        segment.text = s.text;
        segment.start = std::max(0.0, s.start - 0.25);
        segment.end = s.end + 0.35;
        if (withWords) {
            std::vector<std::string> words;
            std::istringstream in(s.text);
            for (std::string w; in >> w;) words.push_back(w);
            const double from = s.start + 0.05;
            const double step = (s.end - 0.05 - from) / double(words.size());
            for (size_t i = 0; i < words.size(); ++i) {
                rt::WordSegment word;
                word.word = " " + words[i];
                word.start = from + step * double(i);
                word.end = from + step * double(i + 1) - 0.02;
                segment.words.push_back(word);
            }
        }
        result.segments.push_back(std::move(segment));
    }
    return result;
}

long long ms(double seconds) { return std::llround(seconds * 1000.0); }

std::string describe(const std::vector<rt::SyncClip>& clips)
{
    std::ostringstream out;
    for (const auto& c : clips) {
        const auto slash = c.sourceFile.find_last_of("/\\");
        out << c.id << ' ' << c.sourceFile.substr(slash == std::string::npos ? 0 : slash + 1)
            << ' ' << c.character << " line=" << c.scriptLineNumber << " state=" << c.matchState
            << ' ' << ms(c.start) << '-' << ms(c.end) << '\n';
    }
    return out.str();
}

std::string describe(const rt::Timeline& timeline)
{
    std::ostringstream out;
    for (size_t t = 0; t < timeline.trackCount(); ++t) {
        const rt::Track* track = timeline.track(t);
        out << (track->type() == rt::TrackType::Audio ? "A " : "V ") << track->name()
            << (track->isCaptionTrack() ? " [captions]" : "") << '\n';
        for (size_t c = 0; c < track->clipCount(); ++c) {
            const rt::Clip* clip = track->clip(c);
            out << "  " << clip->label() << " in=" << clip->timelineIn()
                << " dur=" << clip->duration() << " src=" << clip->sourceIn()
                << " line=" << clip->syncLine() << '\n';
        }
    }
    return out.str();
}

/// Prints the actual value so a re-recorded expectation can be pasted in.
void expectPinned(const std::string& name, const std::string& actual, const std::string& expected)
{
    if (actual != expected)
        std::printf("PIN %s BEGIN\n%sPIN %s END\n", name.c_str(), actual.c_str(), name.c_str());
    EXPECT_EQ(actual, expected) << name;
}

class AudioSyncWorkflowTest : public AudioSyncFixture
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory.isValid());
        alicePath = makeSpeech(directory.path(), QStringLiteral("ALICE.wav"), kFileSeconds,
                               burstsOf(kAlice)).toStdString();
        bobPath = makeSpeech(directory.path(), QStringLiteral("BOB.wav"), kFileSeconds,
                             burstsOf(kBob)).toStdString();
    }

    /// Script loaded, both files imported and decoded, transcriptions in.
    void prepare(rt::AudioSync& audioSync, bool withWords,
                 const std::string& key = "memory://morning-show")
    {
        ASSERT_TRUE(audioSync.loadScript(kScript, key));
        ASSERT_TRUE(audioSync.importAudio(alicePath));
        ASSERT_TRUE(audioSync.importAudio(bobPath));
        ASSERT_TRUE(waitFor([&] { return !audioSync.audioSamplesPending(); }));
        audioSync.setTranscriptionsForTesting(
            {transcriptionOf(kAlice, withWords), transcriptionOf(kBob, withWords)});
    }

    QTemporaryDir directory;
    std::string alicePath;
    std::string bobPath;
};

TEST_F(AudioSyncWorkflowTest, TranscriptionsBecomeClipsPerFileSegment)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true);
    EXPECT_TRUE(audioSync.isTranscriptionComplete());
    expectPinned("clips after transcription",
                 describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=-1 state=0 150-2750\n"
        "1 ALICE.wav Alice line=-1 state=0 2950-5650\n"
        "2 ALICE.wav Alice line=-1 state=0 5850-8350\n"
        "3 BOB.wav Bob line=-1 state=0 50-2850\n"
        "4 BOB.wav Bob line=-1 state=0 3150-5950\n"
        "5 BOB.wav Bob line=-1 state=0 6050-8750\n");
}

TEST_F(AudioSyncWorkflowTest, AutoSyncWithWordTimesMatchesEveryLineAndTrims)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true);
    audioSync.runAutoSync();
    EXPECT_TRUE(audioSync.isSyncComplete());
    expectPinned("word-aligned sync", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=1 state=1 410-2500\n"
        "1 ALICE.wav Alice line=3 state=1 3210-5430\n"
        "2 ALICE.wav Alice line=5 state=1 6110-8130\n"
        "3 BOB.wav Bob line=2 state=1 310-2630\n"
        "4 BOB.wav Bob line=4 state=1 3410-5730\n"
        "5 BOB.wav Bob line=6 state=1 6310-8530\n");
}

TEST_F(AudioSyncWorkflowTest, AutoSyncWithoutWordTimesUsesTheHeuristicPath)
{
    rt::AudioSync audioSync;
    prepare(audioSync, false);
    audioSync.runAutoSync();
    expectPinned("heuristic sync", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=1 state=1 400-2600\n"
        "1 ALICE.wav Alice line=3 state=1 3200-5500\n"
        "2 ALICE.wav Alice line=5 state=1 6100-8200\n"
        "3 BOB.wav Bob line=2 state=1 300-2690\n"
        "4 BOB.wav Bob line=4 state=1 3400-5790\n"
        "5 BOB.wav Bob line=6 state=1 6300-8600\n");
}

TEST_F(AudioSyncWorkflowTest, ScopedSyncLeavesTheOtherCharacterAlone)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true);
    ASSERT_TRUE(AudioSyncTestAccess::filterCharacter(audioSync, QStringLiteral("BOB")));
    audioSync.runAutoSync();
    expectPinned("BOB-only sync", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=-1 state=0 150-2750\n"
        "1 ALICE.wav Alice line=-1 state=0 2950-5650\n"
        "2 ALICE.wav Alice line=-1 state=0 5850-8350\n"
        "3 BOB.wav Bob line=2 state=1 300-2630\n"
        "4 BOB.wav Bob line=4 state=1 3400-5730\n"
        "5 BOB.wav Bob line=6 state=1 6300-8530\n");
}

TEST_F(AudioSyncWorkflowTest, AutoSyncUndoAndRedoRestoreTheClips)
{
    rt::CommandStack commands;
    rt::AudioSync audioSync;
    audioSync.setCommandStack(&commands);
    prepare(audioSync, true);
    const std::string before = describe(AudioSyncTestAccess::clips(audioSync));
    audioSync.runAutoSync();
    const std::string after = describe(AudioSyncTestAccess::clips(audioSync));
    ASSERT_NE(before, after);
    ASSERT_TRUE(commands.undo());
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), before);
    ASSERT_TRUE(commands.redo());
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), after);
}

TEST_F(AudioSyncWorkflowTest, CloseGapsSplitMergeAndRemoveAFile)
{
    rt::AudioSync audioSync;
    prepare(audioSync, false);
    AudioSyncTestAccess::closeGaps(audioSync);
    expectPinned("after close gaps", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=-1 state=0 340-2440\n"
        "1 ALICE.wav Alice line=-1 state=0 3140-5340\n"
        "2 ALICE.wav Alice line=-1 state=0 6040-8030\n"
        "3 BOB.wav Bob line=-1 state=0 240-2530\n"
        "4 BOB.wav Bob line=-1 state=0 3340-5630\n"
        "5 BOB.wav Bob line=-1 state=0 6240-8440\n");

    AudioSyncTestAccess::split(audioSync, 0);
    expectPinned("after split", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=-1 state=0 340-1390\n"
        "6 ALICE.wav Alice line=-1 state=0 1390-2440\n"
        "1 ALICE.wav Alice line=-1 state=0 3140-5340\n"
        "2 ALICE.wav Alice line=-1 state=0 6040-8030\n"
        "3 BOB.wav Bob line=-1 state=0 240-2530\n"
        "4 BOB.wav Bob line=-1 state=0 3340-5630\n"
        "5 BOB.wav Bob line=-1 state=0 6240-8440\n");
    AudioSyncTestAccess::merge(audioSync, 0);
    expectPinned("after merge", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=-1 state=0 340-2440\n"
        "1 ALICE.wav Alice line=-1 state=0 3140-5340\n"
        "2 ALICE.wav Alice line=-1 state=0 6040-8030\n"
        "3 BOB.wav Bob line=-1 state=0 240-2530\n"
        "4 BOB.wav Bob line=-1 state=0 3340-5630\n"
        "5 BOB.wav Bob line=-1 state=0 6240-8440\n");

    AudioSyncTestAccess::removeFile(audioSync, 0);
    ASSERT_EQ(audioSync.audioPaths().size(), 1u);
    EXPECT_EQ(audioSync.audioPaths()[0], bobPath);
    EXPECT_EQ(AudioSyncTestAccess::transcriptions(audioSync).size(), 1u);
    expectPinned("after removing ALICE", describe(AudioSyncTestAccess::clips(audioSync)),
        "3 BOB.wav Bob line=-1 state=0 240-2530\n"
        "4 BOB.wav Bob line=-1 state=0 3340-5630\n"
        "5 BOB.wav Bob line=-1 state=0 6240-8440\n");
}

TEST_F(AudioSyncWorkflowTest, ExportPlacesConfirmedClipsAndReexportMovesOnlyTheChange)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true);
    audioSync.runAutoSync();
    AudioSyncTestAccess::confirmAll(audioSync);

    rt::Timeline timeline;
    const int exported = audioSync.exportToTimeline(&timeline);
    EXPECT_GT(exported, 0) << audioSync.lastExportError().toStdString();
    expectPinned("export", describe(timeline),
        "A Alice\n"
        "  ALICE.wav in=0 dur=100320 src=19680 line=1\n"
        "  ALICE.wav in=211681 dur=106559 src=154080 line=3\n"
        "  ALICE.wav in=429600 dur=96961 src=293278 line=5\n"
        "A Bob\n"
        "  BOB.wav in=100320 dur=111361 src=14878 line=2\n"
        "  BOB.wav in=318240 dur=111360 src=163680 line=4\n"
        "  BOB.wav in=526561 dur=106559 src=302880 line=6\n");

    // Lengthen BOB's second line by half a second and export again.
    auto& clips = AudioSyncTestAccess::clips(audioSync);
    for (auto& clip : clips)
        if (clip.scriptLineNumber == 4) clip.end += 0.5;
    EXPECT_GT(audioSync.exportToTimeline(&timeline), 0) << audioSync.lastExportError().toStdString();
    expectPinned("re-export", describe(timeline),
        "A Alice\n"
        "  ALICE.wav in=0 dur=100320 src=19680 line=1\n"
        "  ALICE.wav in=211681 dur=106559 src=154080 line=3\n"
        "  ALICE.wav in=453600 dur=96961 src=293278 line=5\n"
        "A Bob\n"
        "  BOB.wav in=100320 dur=111361 src=14878 line=2\n"
        "  BOB.wav in=318240 dur=135360 src=163680 line=4\n"
        "  BOB.wav in=550561 dur=106559 src=302880 line=6\n");
}

TEST_F(AudioSyncWorkflowTest, BlobRoundTripIsByteIdenticalWithTwoScripts)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://first");
    audioSync.runAutoSync();
    ASSERT_TRUE(audioSync.loadScript(
        "CAROL: A second script with one line only.\n", "memory://second"));
    const std::vector<uint8_t> blob = audioSync.serializeToBlob();
    ASSERT_FALSE(blob.empty());

    rt::AudioSync restored;
    restored.deserializeFromBlob(blob);
    EXPECT_EQ(restored.serializeToBlob(), blob);

    AudioSyncTestAccess::switchTo(restored, "memory://first");
    AudioSyncTestAccess::switchTo(audioSync, "memory://first");
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(restored)),
              describe(AudioSyncTestAccess::clips(audioSync)));
}

TEST_F(AudioSyncWorkflowTest, SwitchingScriptsKeepsEachScriptsData)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://first");
    audioSync.runAutoSync();
    const std::string first = describe(AudioSyncTestAccess::clips(audioSync));
    const auto firstPaths = audioSync.audioPaths();

    ASSERT_TRUE(audioSync.loadScript(kScript, "memory://second"));
    expectPinned("second script on load", describe(AudioSyncTestAccess::clips(audioSync)), "");
    ASSERT_TRUE(audioSync.importAudio(bobPath));
    ASSERT_TRUE(waitFor([&] { return !audioSync.audioSamplesPending(); }));
    audioSync.setTranscriptionsForTesting({transcriptionOf(kBob, false)});
    const std::string second = describe(AudioSyncTestAccess::clips(audioSync));

    AudioSyncTestAccess::switchTo(audioSync, "memory://first");
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), first);
    EXPECT_EQ(audioSync.audioPaths(), firstPaths);
    EXPECT_EQ(AudioSyncTestAccess::transcriptions(audioSync).size(), 2u);
    EXPECT_TRUE(audioSync.isSyncComplete());

    AudioSyncTestAccess::switchTo(audioSync, "memory://second");
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), second);
    ASSERT_EQ(audioSync.audioPaths().size(), 1u);
    EXPECT_EQ(audioSync.audioPaths()[0], bobPath);
}

TEST_F(AudioSyncWorkflowTest, AudioImportedBeforeAnyScript)
{
    rt::AudioSync audioSync;
    ASSERT_TRUE(audioSync.importAudio(alicePath));
    ASSERT_TRUE(waitFor([&] { return !audioSync.audioSamplesPending(); }));
    ASSERT_TRUE(audioSync.loadScript(kScript, "memory://after-import"));
    std::ostringstream state;
    state << "paths=" << audioSync.audioPaths().size()
          << " imported=" << audioSync.isAudioImported()
          << " samples=" << audioSync.hasAudioSamples(alicePath) << '\n';
    expectPinned("import before script", state.str(),
        "paths=1 imported=1 samples=1\n");
}

// Re-loading the same source (a Google Doc edited and fetched again) keeps
// the clips, remapped onto the new lines, and asks for a new sync.
TEST_F(AudioSyncWorkflowTest, ReloadingTheOpenScriptKeepsItsClips)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://doc");
    audioSync.runAutoSync();
    std::string edited = kScript;
    edited.insert(0, "ALICE: A brand new opening line.\n");
    ASSERT_TRUE(audioSync.loadScript(edited, "memory://doc"));
    EXPECT_EQ(audioSync.scriptLineCount(), 7);
    EXPECT_FALSE(audioSync.isSyncComplete());
    expectPinned("open script reloaded", describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=2 state=1 410-2500\n"
        "1 ALICE.wav Alice line=4 state=1 3210-5430\n"
        "2 ALICE.wav Alice line=6 state=1 6110-8130\n"
        "3 BOB.wav Bob line=3 state=1 310-2630\n"
        "4 BOB.wav Bob line=5 state=1 3410-5730\n"
        "5 BOB.wav Bob line=7 state=1 6310-8530\n");
}

TEST_F(AudioSyncWorkflowTest, ReloadingAScriptInTheBackgroundSwitchesToIt)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://doc");
    audioSync.runAutoSync();
    ASSERT_TRUE(audioSync.loadScript(
        "CAROL: A second script with one line only.\n", "memory://other"));
    std::string edited = kScript;
    edited.insert(0, "ALICE: A brand new opening line.\n");
    ASSERT_TRUE(audioSync.loadScript(edited, "memory://doc"));
    EXPECT_EQ(audioSync.scriptLineCount(), 7);
    expectPinned("background script reloaded",
                 describe(AudioSyncTestAccess::clips(audioSync)),
        "0 ALICE.wav Alice line=2 state=1 410-2500\n"
        "1 ALICE.wav Alice line=4 state=1 3210-5430\n"
        "2 ALICE.wav Alice line=6 state=1 6110-8130\n"
        "3 BOB.wav Bob line=3 state=1 310-2630\n"
        "4 BOB.wav Bob line=5 state=1 3410-5730\n"
        "5 BOB.wav Bob line=7 state=1 6310-8530\n");
    std::ostringstream state;
    state << "paths=" << audioSync.audioPaths().size()
          << " transcriptions=" << AudioSyncTestAccess::transcriptions(audioSync).size()
          << " synced=" << audioSync.isSyncComplete() << '\n';
    expectPinned("background script reloaded state", state.str(),
        "paths=2 transcriptions=2 synced=0\n");
}

// A script that fails to load (no lines) leaves the open script as it was.
// (It used to empty it and replace its saved text with the failed text.)
TEST_F(AudioSyncWorkflowTest, AFailedScriptLoadKeepsTheOpenScript)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://doc");
    audioSync.runAutoSync();
    const std::string clipsBefore = describe(AudioSyncTestAccess::clips(audioSync));
    const std::vector<uint8_t> blobBefore = audioSync.serializeToBlob();

    EXPECT_FALSE(audioSync.loadScript("no dialogue here at all", "memory://broken"));
    EXPECT_EQ(audioSync.scriptLineCount(), 6);
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), clipsBefore);
    EXPECT_EQ(audioSync.serializeToBlob(), blobBefore);
}

// The script list's row for the open script shows its clips and progress.
// (It used to show "0 clips" and the progress from when it was opened.)
TEST_F(AudioSyncWorkflowTest, ActiveScriptRowShowsItsClipsAndState)
{
    rt::AudioSync audioSync;
    prepare(audioSync, true, "memory://first");
    ASSERT_TRUE(audioSync.loadScript(
        "CAROL: A second script with one line only.\n", "memory://second"));
    AudioSyncTestAccess::switchTo(audioSync, "memory://first");
    // Synced while open: the row must follow.
    audioSync.runAutoSync();
    ASSERT_TRUE(audioSync.isSyncComplete());

    QListWidget* list = AudioSyncTestAccess::sessionList(audioSync);
    ASSERT_NE(list, nullptr);
    bool found = false;
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        if (!item->data(Qt::UserRole + 2).toBool()) continue;
        found = true;
        const QString meta = item->data(Qt::UserRole + 1).toString();
        EXPECT_TRUE(meta.contains(QStringLiteral("%1 clips").arg(audioSync.clipCount())))
            << meta.toStdString();
        EXPECT_EQ(item->data(Qt::UserRole + 3).toInt(), 3) << "synced";
    }
    EXPECT_TRUE(found) << "no active row";
}

// ── Transcription runs (TranscriptionRunner with a fake transcribe) ────────

/// Holds a fake transcribe call until released or stopped.
struct TranscribeGate
{
    std::mutex mutex;
    std::condition_variable_any cv;
    bool open{false};
    std::atomic<bool> entered{false};

    bool wait(std::stop_token stop)
    {
        entered = true;
        std::unique_lock lock(mutex);
        return cv.wait(lock, stop, [this] { return open; });
    }
    void release()
    {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

class AudioSyncTranscribeTest : public AudioSyncWorkflowTest
{
protected:
    /// Script loaded and both files imported and decoded; no transcriptions.
    void importBoth(rt::AudioSync& audioSync, const std::string& key = "memory://morning-show")
    {
        ASSERT_TRUE(audioSync.loadScript(kScript, key));
        ASSERT_TRUE(audioSync.importAudio(alicePath));
        ASSERT_TRUE(audioSync.importAudio(bobPath));
        ASSERT_TRUE(waitFor([&] { return !audioSync.audioSamplesPending(); }));
    }

    /// Whisper's results for the two files; `bob` decides Bob's.
    rt::TranscriptionRunner::TranscribeFn fake(
        std::function<rt::TranscriptionResult(std::stop_token)> bob = {}) const
    {
        const std::string alice = alicePath;
        return [alice, bob](const std::string& path, std::stop_token stop,
                            const rt::TranscriptionRunner::ProgressFn& progress) {
            progress(50.0f, "Transcribing...");
            if (path == alice) return transcriptionOf(kAlice, true);
            return bob ? bob(stop) : transcriptionOf(kBob, true);
        };
    }

    static rt::TranscriptionResult stopped()
    {
        rt::TranscriptionResult result;
        result.status = rt::TranscriptionStatus::Cancelled;
        return result;
    }

    const char* const kAllClips =
        "0 ALICE.wav Alice line=-1 state=0 150-2750\n"
        "1 ALICE.wav Alice line=-1 state=0 2950-5650\n"
        "2 ALICE.wav Alice line=-1 state=0 5850-8350\n"
        "3 BOB.wav Bob line=-1 state=0 50-2850\n"
        "4 BOB.wav Bob line=-1 state=0 3150-5950\n"
        "5 BOB.wav Bob line=-1 state=0 6050-8750\n";
    const char* const kAliceClips =
        "0 ALICE.wav Alice line=-1 state=0 150-2750\n"
        "1 ALICE.wav Alice line=-1 state=0 2950-5650\n"
        "2 ALICE.wav Alice line=-1 state=0 5850-8350\n";
};

// A run builds the same clips as installing the results directly.
TEST_F(AudioSyncTranscribeTest, ARunBuildsTheClipsOfEveryFile)
{
    rt::AudioSync audioSync;
    importBoth(audioSync);
    audioSync.setTranscribeFunctionForTesting(fake());
    audioSync.startTranscription();
    EXPECT_TRUE(AudioSyncTestAccess::transcribing(audioSync));
    ASSERT_TRUE(waitFor([&] { return !AudioSyncTestAccess::transcribing(audioSync); }));

    EXPECT_TRUE(audioSync.isTranscriptionComplete());
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), kAllClips);
    EXPECT_EQ(AudioSyncTestAccess::transcriptions(audioSync).size(), 2u);
    EXPECT_EQ(AudioSyncTestAccess::transcribeStatus(audioSync),
              "Transcribed 6 segments from 2 file(s)");

    // Everything is transcribed: a second click has nothing to do.
    audioSync.startTranscription();
    EXPECT_FALSE(AudioSyncTestAccess::transcribing(audioSync));
    EXPECT_EQ(AudioSyncTestAccess::transcribeStatus(audioSync), "All files already transcribed");
}

TEST_F(AudioSyncTranscribeTest, AFailedFileLeavesTheOthersTranscribed)
{
    rt::AudioSync audioSync;
    importBoth(audioSync);
    audioSync.setTranscribeFunctionForTesting(fake([](std::stop_token) {
        rt::TranscriptionResult failed;
        failed.status = rt::TranscriptionStatus::Failed;
        failed.error.message = "could not decode BOB.wav";
        return failed;
    }));
    audioSync.startTranscription();
    ASSERT_TRUE(waitFor([&] { return !AudioSyncTestAccess::transcribing(audioSync); }));

    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), kAliceClips);
    EXPECT_EQ(AudioSyncTestAccess::transcribeStatus(audioSync),
              "Transcription completed with errors (2/2): could not decode BOB.wav");
}

// Cancel keeps the files already done and stops the one in progress.
TEST_F(AudioSyncTranscribeTest, CancelKeepsTheFilesAlreadyDone)
{
    TranscribeGate gate;   // outlives the panel and its run
    rt::AudioSync audioSync;
    importBoth(audioSync);
    audioSync.setTranscribeFunctionForTesting(fake([&](std::stop_token stop) {
        return gate.wait(stop) ? transcriptionOf(kBob, true) : stopped();
    }));
    audioSync.startTranscription();
    ASSERT_TRUE(waitFor([&] {
        return gate.entered.load()
            && !AudioSyncTestAccess::transcriptions(audioSync)[0].segments.empty();
    }));

    AudioSyncTestAccess::cancelTranscription(audioSync);
    EXPECT_EQ(AudioSyncTestAccess::transcribeStatus(audioSync), "Cancelling transcription...");
    ASSERT_TRUE(waitFor([&] { return !AudioSyncTestAccess::transcribing(audioSync); }));

    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), kAliceClips);
    EXPECT_TRUE(AudioSyncTestAccess::transcriptions(audioSync)[1].segments.empty());
    EXPECT_EQ(AudioSyncTestAccess::transcribeStatus(audioSync),
              "Transcription cancelled (1 of 2 file(s) completed)");
}

// Opening another script mid-run: the results still go to the script the
// run started in. (They used to land in whichever script was open.)
TEST_F(AudioSyncTranscribeTest, ResultsGoToTheScriptTheRunStartedIn)
{
    TranscribeGate gate;   // outlives the panel and its run
    rt::AudioSync audioSync;
    importBoth(audioSync, "memory://first");
    audioSync.setTranscribeFunctionForTesting(fake([&](std::stop_token stop) {
        return gate.wait(stop) ? transcriptionOf(kBob, true) : stopped();
    }));
    audioSync.startTranscription();
    ASSERT_TRUE(waitFor([&] { return gate.entered.load(); }));

    ASSERT_TRUE(audioSync.loadScript(
        "CAROL: A second script with one line only.\n", "memory://second"));
    gate.release();
    ASSERT_TRUE(waitFor([&] { return !AudioSyncTestAccess::transcribing(audioSync); }));

    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), "");
    EXPECT_TRUE(audioSync.audioPaths().empty());
    EXPECT_EQ(describe(AudioSyncTestAccess::clipsOf(audioSync, "memory://first")), kAllClips);

    AudioSyncTestAccess::switchTo(audioSync, "memory://first");
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), kAllClips);
    EXPECT_EQ(AudioSyncTestAccess::transcriptions(audioSync).size(), 2u);
    EXPECT_TRUE(audioSync.isTranscriptionComplete());
}

// Opening a project mid-run cancels the run; its results are dropped, even
// when the project has a script and file of the same name.
TEST_F(AudioSyncTranscribeTest, OpeningAProjectMidRunDropsTheRun)
{
    std::vector<uint8_t> project;
    {
        rt::AudioSync other;
        ASSERT_TRUE(other.loadScript(kScript, "memory://morning-show"));
        ASSERT_TRUE(other.importAudio(bobPath));
        ASSERT_TRUE(waitFor([&] { return !other.audioSamplesPending(); }));
        project = other.serializeToBlob();
    }

    TranscribeGate gate;   // outlives the panel and its run
    rt::AudioSync audioSync;
    importBoth(audioSync);
    audioSync.setTranscribeFunctionForTesting(fake([&](std::stop_token stop) {
        return gate.wait(stop) ? transcriptionOf(kBob, true) : stopped();
    }));
    audioSync.startTranscription();
    ASSERT_TRUE(waitFor([&] { return gate.entered.load(); }));

    audioSync.deserializeFromBlob(project);
    ASSERT_TRUE(waitFor([&] { return !AudioSyncTestAccess::transcribing(audioSync); }));
    EXPECT_EQ(describe(AudioSyncTestAccess::clips(audioSync)), "");
    EXPECT_EQ(audioSync.audioPaths(), std::vector<std::string>{bobPath});
    EXPECT_EQ(audioSync.serializeToBlob(), project);
}

TEST_F(AudioSyncTranscribeTest, ClosingThePanelMidRunStopsTheRun)
{
    TranscribeGate gate;
    std::atomic<bool> sawStop{false};
    auto audioSync = std::make_unique<rt::AudioSync>();
    importBoth(*audioSync);
    audioSync->setTranscribeFunctionForTesting(fake([&](std::stop_token stop) {
        if (!gate.wait(stop)) sawStop = true;
        return stopped();
    }));
    audioSync->startTranscription();
    ASSERT_TRUE(waitFor([&] { return gate.entered.load(); }));

    {
        rt::test::StallGuard guard("AudioSync destroyed mid-transcription");
        audioSync.reset();
    }
    EXPECT_TRUE(sawStop.load());
    QCoreApplication::processEvents();   // nothing queued reaches the dead panel
}

} // namespace
