/*
 * test_auto_sync.cpp — the AudioSync algorithms (core/audiosync) on in-memory
 * audio: clips from transcriptions, auto-sync (word-aligned and heuristic),
 * a one-character sync, shot-table takes and gap closing. Exact millisecond
 * results through the panel are pinned in tests/ui/test_audio_sync_workflow.
 */

#include <gtest/gtest.h>

#include "ai/ScriptMatcher.h"
#include "ai/Transcriber.h"
#include "audiosync/AutoSync.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace rt;

namespace {

constexpr uint32_t kRate = 16000;
constexpr double kSeconds = 9.5;

struct Said
{
    const char* text;
    double start;
    double end;
};

const char* const kScript =
    "ALICE: Welcome back to the morning show everyone.\n"
    "BOB: Thanks Alice it is great to be here today.\n"
    "ALICE: Let us talk about the weather this weekend.\n"
    "BOB: It will be sunny with a light breeze on Saturday.\n";

const std::vector<Said> kAlice = {
    {"Welcome back to the morning show everyone.", 0.40, 2.40},
    {"Let us talk about the weather this weekend.", 3.20, 5.30},
};
const std::vector<Said> kBob = {
    {"Thanks Alice it is great to be here today.", 0.30, 2.50},
    {"It will be sunny with a light breeze on Saturday.", 3.40, 5.60},
};

/// Voiced bursts with a syllable-rate envelope over a faint noise floor.
AudioSampleData speech(const std::vector<Said>& said)
{
    constexpr double pi = 3.141592653589793;
    std::vector<float> samples(static_cast<size_t>(kSeconds * kRate));
    uint32_t noise = 12345u;
    for (size_t i = 0; i < samples.size(); ++i) {
        noise = noise * 1664525u + 1013904223u;
        const double t = double(i) / kRate;
        double v = 0.002 * (double(noise >> 8) / double(1u << 24) - 0.5);
        for (const auto& s : said) {
            if (t < s.start || t >= s.end) continue;
            const double edge = std::min({1.0, (t - s.start) / 0.03, (s.end - t) / 0.03});
            const double syllable = 0.6 + 0.4 * std::sin(2.0 * pi * 4.0 * (t - s.start));
            v += 0.3 * edge * syllable * (std::sin(2.0 * pi * 180.0 * t) + 0.5 * std::sin(2.0 * pi * 360.0 * t));
        }
        samples[i] = static_cast<float>(v);
    }
    AudioSampleData data;
    data.samples = SampleBuffer(std::move(samples));
    data.sampleRate = kRate;
    return data;
}

/// Whisper-like: one segment per burst, starting early and ending late.
TranscriptionResult transcription(const std::vector<Said>& said, bool withWords)
{
    TranscriptionResult result;
    result.status = TranscriptionStatus::Success;
    result.duration = kSeconds;
    int id = 0;
    for (const auto& s : said) {
        TranscriptionSegment segment;
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
            for (size_t i = 0; i < words.size(); ++i)
                segment.words.push_back({" " + words[i], from + step * double(i),
                                         from + step * double(i + 1) - 0.02, 1.0f});
        }
        result.segments.push_back(std::move(segment));
    }
    return result;
}

/// Two characters' recordings, transcribed, as one script session.
struct Session
{
    explicit Session(bool withWords, const char* scriptText = kScript)
        : script(Script::fromText(scriptText)),
          paths{"C:/takes/ALICE.wav", "C:/takes/BOB.wav"},
          transcriptions{transcription(kAlice, withWords), transcription(kBob, withWords)}
    {
        samples.emplace(paths[0], speech(kAlice));
        samples.emplace(paths[1], speech(kBob));
        clips = audiosync::clipsFromTranscriptions(source());
    }

    audiosync::SyncSource source() const { return {&script, paths, transcriptions, samples}; }

    Script script;
    std::vector<std::string> paths;
    std::vector<TranscriptionResult> transcriptions;
    std::unordered_map<std::string, AudioSampleData> samples;
    std::vector<SyncClip> clips;
};

/// The script line each spoken text belongs to.
int lineOf(const Script& script, const std::string& text)
{
    for (const auto& line : script.lines)
        if (line.dialogue == text) return line.lineNumber;
    return -1;
}

const Said* saidFor(const SyncClip& clip)
{
    for (const auto* list : {&kAlice, &kBob})
        for (const auto& s : *list)
            if (clip.transcript == s.text) return &s;
    return nullptr;
}

} // namespace

TEST(AutoSync, TranscriptionsBecomeOneClipPerSegment)
{
    Session s(true);
    ASSERT_EQ(s.clips.size(), 4u);
    for (size_t i = 0; i < s.clips.size(); ++i) {
        const auto& clip = s.clips[i];
        EXPECT_EQ(clip.id, static_cast<int>(i));
        EXPECT_EQ(clip.matchState, 0);
        EXPECT_EQ(clip.scriptLineNumber, -1);
        EXPECT_EQ(clip.transcript, clip.editedText);
        EXPECT_EQ(clip.character, i < 2 ? "Alice" : "Bob") << "from the file name";
    }
    EXPECT_EQ(s.clips[0].sourceFile, s.paths[0]);
    EXPECT_DOUBLE_EQ(s.clips[0].start, 0.15);
    EXPECT_DOUBLE_EQ(s.clips[0].end, 2.75);
}

TEST(AutoSync, AppendingKeepsClipsOfFilesThatHaveThem)
{
    Session s(true);
    std::vector<SyncClip> clips = {s.clips[0]};
    clips[0].matchState = 2;
    clips[0].scriptLineNumber = 1;
    audiosync::appendClipsFromTranscriptions(s.source(), clips);
    ASSERT_EQ(clips.size(), 3u) << "ALICE already has a clip; BOB's two are added";
    EXPECT_EQ(clips[0].matchState, 2);
    EXPECT_EQ(clips[1].sourceFile, s.paths[1]);
    EXPECT_EQ(clips[1].id, clips[0].id + 1);
}

TEST(AutoSync, WordTimesAlignEveryClipToItsLine)
{
    Session s(true);
    const auto result = audiosync::autoSyncClips(s.source(), s.clips, {});
    EXPECT_EQ(result.path, audiosync::AutoSyncResult::Path::WordAligned);
    ASSERT_EQ(s.clips.size(), 4u);
    for (const auto& clip : s.clips) {
        const Said* said = saidFor(clip);
        ASSERT_NE(said, nullptr) << clip.transcript;
        EXPECT_EQ(clip.scriptLineNumber, lineOf(s.script, said->text));
        EXPECT_EQ(clip.matchState, 1) << "auto-sync only proposes";
        EXPECT_NEAR(clip.start, said->start, 0.12) << clip.transcript;
        EXPECT_NEAR(clip.end, said->end, 0.30) << clip.transcript;
    }
}

TEST(AutoSync, WithoutWordTimesTheHeuristicPathMatchesAndTrims)
{
    Session s(false);
    std::vector<std::pair<int, std::string>> progress;
    audiosync::AutoSyncOptions options;
    options.progress = [&](int percent, const std::string& text) { progress.emplace_back(percent, text); };
    const auto result = audiosync::autoSyncClips(s.source(), s.clips, options);
    EXPECT_EQ(result.path, audiosync::AutoSyncResult::Path::Heuristic);
    EXPECT_EQ(result.matchCount, 4);
    EXPECT_EQ(result.scopeTotal, 4);
    for (const auto& clip : s.clips) {
        const Said* said = saidFor(clip);
        ASSERT_NE(said, nullptr) << clip.transcript;
        EXPECT_EQ(clip.scriptLineNumber, lineOf(s.script, said->text));
        EXPECT_EQ(clip.matchState, 1);
        EXPECT_NEAR(clip.start, said->start, 0.10) << "trimmed to the speech";
        EXPECT_NEAR(clip.end, said->end + options.endPadSec, 0.12);
    }
    ASSERT_FALSE(progress.empty());
    EXPECT_TRUE(std::is_sorted(progress.begin(), progress.end(),
                               [](const auto& a, const auto& b) { return a.first < b.first; }));
}

TEST(AutoSync, ACharacterFilterLeavesEveryoneElseUntouched)
{
    Session s(true);
    const std::vector<SyncClip> before = s.clips;
    audiosync::AutoSyncOptions options;
    options.characterFilter = "BOB";
    const auto result = audiosync::autoSyncClips(s.source(), s.clips, options);
    EXPECT_EQ(result.path, audiosync::AutoSyncResult::Path::Heuristic)
        << "a scoped sync never re-cuts everyone with word alignment";
    EXPECT_EQ(result.scopeTotal, 2);
    for (size_t i = 0; i < s.clips.size(); ++i) {
        if (s.clips[i].character == "Alice") {
            EXPECT_EQ(s.clips[i], before[i]);
        } else {
            EXPECT_EQ(s.clips[i].scriptLineNumber, lineOf(s.script, s.clips[i].transcript));
        }
    }
}

TEST(AutoSync, ConfirmedClipsAreNeverChanged)
{
    Session s(false);
    s.clips[1].matchState = 2;
    s.clips[1].scriptLineNumber = 3;
    s.clips[1].start = 3.0;
    s.clips[1].end = 5.5;
    const SyncClip confirmed = s.clips[1];
    (void)audiosync::autoSyncClips(s.source(), s.clips, {});
    const auto kept = std::find_if(s.clips.begin(), s.clips.end(), [&](const SyncClip& c) {
        return c.matchState == 2;
    });
    ASSERT_NE(kept, s.clips.end());
    EXPECT_EQ(*kept, confirmed);
}

TEST(AutoSync, NothingToSyncWithoutAScriptOrClips)
{
    Session s(true);
    std::vector<SyncClip> none;
    EXPECT_EQ(audiosync::autoSyncClips(s.source(), none, {}).path,
              audiosync::AutoSyncResult::Path::NothingToSync);
    const audiosync::SyncSource noScript{nullptr, s.paths, s.transcriptions, s.samples};
    EXPECT_EQ(audiosync::autoSyncClips(noScript, s.clips, {}).path,
              audiosync::AutoSyncResult::Path::NothingToSync);
}

TEST(AutoSync, ShotTableLinesReadTwiceGiveTwoTakes)
{
    Script script;
    script.shotTable = true;
    script.lines.push_back({kShotScriptCharacter, "The quick brown fox jumps over the lazy dog", 1});
    script.lines.push_back({kShotScriptCharacter, "Pack my box with five dozen liquor jugs today", 2});
    script.characters = {kShotScriptCharacter};
    // Line 1 twice, then line 2.
    const std::vector<Said> read = {
        {"The quick brown fox jumps over the lazy dog", 0.5, 3.0},
        {"The quick brown fox jumps over the lazy dog", 4.0, 6.5},
        {"Pack my box with five dozen liquor jugs today", 7.0, 9.3},
    };
    const std::vector<std::string> paths = {"C:/takes/announcer.mp4"};
    const std::vector<TranscriptionResult> transcriptions = {transcription(read, true)};
    std::unordered_map<std::string, AudioSampleData> samples;
    samples.emplace(paths[0], speech(read));
    const audiosync::SyncSource source{&script, paths, transcriptions, samples};

    std::vector<SyncClip> clips;
    ASSERT_TRUE(audiosync::alignShotTakes(source, clips, 0.0, 0.2));
    std::vector<int> lines;
    for (const auto& clip : clips) lines.push_back(clip.scriptLineNumber);
    EXPECT_EQ(lines, (std::vector<int>{1, 1, 2})) << "in recording order";
    for (size_t i = 0; i < clips.size(); ++i) {
        EXPECT_EQ(clips[i].id, static_cast<int>(i));
        EXPECT_EQ(clips[i].matchState, 1) << "the user picks the take";
    }

    // A confirmed take is kept as it is and not proposed again.
    clips[1].matchState = 2;
    const SyncClip picked = clips[1];
    ASSERT_TRUE(audiosync::alignShotTakes(source, clips, 0.0, 0.2));
    EXPECT_EQ(std::count_if(clips.begin(), clips.end(),
                            [](const SyncClip& c) { return c.scriptLineNumber == 1; }), 2);
    const auto kept = std::find_if(clips.begin(), clips.end(),
                                   [](const SyncClip& c) { return c.matchState == 2; });
    ASSERT_NE(kept, clips.end());
    EXPECT_EQ(kept->start, picked.start);
    EXPECT_EQ(kept->end, picked.end);
}

TEST(AutoSync, CloseGapsTrimsToSpeechAndJoinsTinyGaps)
{
    Session s(false);
    const auto result = audiosync::closeInterClipGaps(s.source(), s.clips);
    EXPECT_FALSE(result.tooFewToClose);
    EXPECT_EQ(result.trimmed, 4);
    for (const auto& clip : s.clips) {
        const Said* said = saidFor(clip);
        ASSERT_NE(said, nullptr);
        EXPECT_NEAR(clip.start, said->start, 0.10);
        EXPECT_NEAR(clip.end, said->end, 0.10);
    }

    // Two clips 30 ms apart in one file share one boundary afterwards.
    std::vector<SyncClip> pair(2);
    pair[0].sourceFile = pair[1].sourceFile = s.paths[0];
    pair[0].start = 0.40; pair[0].end = 2.40;
    pair[1].start = 2.43; pair[1].end = 2.90;
    const auto joined = audiosync::closeInterClipGaps(s.source(), pair);
    EXPECT_EQ(joined.gapsClosed, 1);
    EXPECT_DOUBLE_EQ(pair[0].end, pair[1].start);

    std::vector<SyncClip> single = {s.clips[0]};
    EXPECT_TRUE(audiosync::closeInterClipGaps(s.source(), single).tooFewToClose);
}
