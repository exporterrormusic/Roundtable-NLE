/*
 * test_audio_sync.cpp — Tests for Step 23: Audio Sync Workflow
 *
 * Tests Transcriber, ScriptMatcher (text/HTML/JSON parsing, fuzzy matching,
 * soundex, metaphone, normalize, sequenceRatio), and data types.
 */

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ai/Transcriber.h"
#include "ai/ScriptMatcher.h"
#include "ai/ShotPipeline.h"
#include "Constants.h"
#include "project/BinaryIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using namespace rt;

// ═══════════════════════════════════════════════════════════════════════════
//  Transcriber data types
// ═══════════════════════════════════════════════════════════════════════════

TEST(TranscriberTest, ModelNames)
{
    EXPECT_STREQ(whisperModelName(WhisperModelSize::Tiny), "tiny");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::Base), "base");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::Small), "small");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::Medium), "medium");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::LargeV3Turbo), "large-v3-turbo");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::LargeV2), "large-v2");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::LargeV3), "large-v3");
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    EXPECT_STREQ(whisperModelName(WhisperModelSize::CrisperWhisper2Large),
                 "crisperwhisper-2-large-personal");
#endif
}

TEST(TranscriberTest, ModelFromName)
{
    EXPECT_EQ(whisperModelFromName("tiny"), WhisperModelSize::Tiny);
    EXPECT_EQ(whisperModelFromName("base"), WhisperModelSize::Base);
    EXPECT_EQ(whisperModelFromName("small"), WhisperModelSize::Small);
    EXPECT_EQ(whisperModelFromName("medium"), WhisperModelSize::Medium);
    EXPECT_EQ(whisperModelFromName("large-v3-turbo"), WhisperModelSize::LargeV3Turbo);
    EXPECT_EQ(whisperModelFromName("large-v2"), WhisperModelSize::LargeV2);
    EXPECT_EQ(whisperModelFromName("large-v3"), WhisperModelSize::LargeV3);
#ifdef ROUNDTABLE_HAS_CRISPERWHISPER
    EXPECT_EQ(whisperModelFromName("crisperwhisper-2-large-personal"),
              WhisperModelSize::CrisperWhisper2Large);
#endif
    EXPECT_EQ(whisperModelFromName("invalid"), kDefaultWhisperModel);
}

TEST(TranscriberTest, Defaults)
{
    Transcriber t;
    EXPECT_EQ(t.currentModel(), kDefaultWhisperModel);
    EXPECT_TRUE(t.isVadEnabled());
    t.setVadEnabled(false);
    EXPECT_FALSE(t.isVadEnabled());
#ifdef ROUNDTABLE_HAS_WHISPER_CUDA
    EXPECT_TRUE(t.isCudaAvailable());
#else
    EXPECT_FALSE(t.isCudaAvailable());
#endif
}

TEST(TranscriberTest, TranscriptionResultFullText)
{
    TranscriptionResult result;
    result.segments.push_back({0, "Hello world", 0.0, 1.0, {}, ""});
    result.segments.push_back({1, "How are you", 1.0, 2.0, {}, ""});

    EXPECT_EQ(result.fullText(), "Hello world How are you");
}

TEST(TranscriberTest, TranscriptionResultEmpty)
{
    TranscriptionResult result;
    EXPECT_TRUE(result.fullText().empty());
}

TEST(TranscriberTest, WordSegmentDefaults)
{
    WordSegment ws;
    EXPECT_TRUE(ws.word.empty());
    EXPECT_DOUBLE_EQ(ws.start, 0.0);
    EXPECT_DOUBLE_EQ(ws.end, 0.0);
    EXPECT_FLOAT_EQ(ws.probability, 1.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Transcriber instance (stub mode)
// ═══════════════════════════════════════════════════════════════════════════

TEST(TranscriberTest, Construction)
{
    Transcriber t;
    EXPECT_FALSE(t.isModelLoaded());
    EXPECT_FALSE(t.isLoading());
    EXPECT_FALSE(t.isTranscribing());
}

TEST(TranscriberTest, LoadModelStub)
{
    Transcriber t;
    bool ok = t.loadModel(WhisperModelSize::Tiny);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(t.isModelLoaded());
    EXPECT_EQ(t.currentModel(), WhisperModelSize::Tiny);
}

TEST(TranscriberTest, UnloadModel)
{
    Transcriber t;
    t.loadModel(WhisperModelSize::Base);
    EXPECT_TRUE(t.isModelLoaded());
    t.unloadModel();
    EXPECT_FALSE(t.isModelLoaded());
}

TEST(TranscriberTest, TranscribeNonExistentFile)
{
    Transcriber t;
    t.loadModel(WhisperModelSize::Tiny);
    auto result = t.transcribe("nonexistent_audio.wav");
    EXPECT_TRUE(result.segments.empty());
}

TEST(TranscriberTest, TranscribeStubReturnsEmpty)
{
    Transcriber t;
    t.loadModel(WhisperModelSize::Base);
    // Stub mode always returns empty segments (but no error for existing files)
    // We test with a non-existent file to verify error handling
    auto result = t.transcribe("test_file_that_does_not_exist.wav");
    EXPECT_TRUE(result.segments.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script parsing — Text format
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptTest, ParseBasicText)
{
    std::string text =
        "CROWN: Welcome everyone!\n"
        "CREW: Thanks for having us.\n"
        "CROWN: Let's get started.\n";

    auto script = Script::fromText(text);
    EXPECT_EQ(script.lineCount(), 3u);
    EXPECT_EQ(script.lines[0].character, "Crown");
    EXPECT_EQ(script.lines[0].dialogue, "Welcome everyone!");
    EXPECT_EQ(script.lines[1].character, "Crew");
    EXPECT_EQ(script.lines[2].character, "Crown");
}

TEST(ScriptTest, ParseWithSegments)
{
    std::string text =
        "**INTRO**\n"
        "CROWN: Hello!\n"
        "**MAIN SEGMENT**\n"
        "CREW: Let's talk.\n"
        "CROWN: Agreed.\n";

    auto script = Script::fromText(text);
    EXPECT_EQ(script.lineCount(), 3u);
    EXPECT_EQ(script.segments.size(), 2u);
    EXPECT_EQ(script.segments[0], "INTRO");
    EXPECT_EQ(script.segments[1], "MAIN SEGMENT");
    EXPECT_EQ(script.lines[0].segment, "INTRO");
    EXPECT_EQ(script.lines[1].segment, "MAIN SEGMENT");
    EXPECT_EQ(script.lines[2].segment, "MAIN SEGMENT");
}

TEST(ScriptTest, ParseWithTimecode)
{
    std::string text =
        "CROWN: [1:30] This starts at 90 seconds.\n";

    auto script = Script::fromText(text);
    ASSERT_EQ(script.lineCount(), 1u);
    EXPECT_EQ(script.lines[0].dialogue, "This starts at 90 seconds.");
    ASSERT_TRUE(script.lines[0].timecode.has_value());
    EXPECT_DOUBLE_EQ(script.lines[0].timecode.value(), 90.0);
}

TEST(ScriptTest, ParseBracketFormat)
{
    std::string text =
        "[Crown] Hello there.\n"
        "[Crew] Hi!\n";

    auto script = Script::fromText(text);
    EXPECT_EQ(script.lineCount(), 2u);
    EXPECT_EQ(script.lines[0].character, "Crown");
    EXPECT_EQ(script.lines[1].character, "Crew");
}

TEST(ScriptTest, SkipCommentsAndEmpty)
{
    std::string text =
        "# This is a comment\n"
        "\n"
        "CROWN: Hello!\n"
        "   \n"
        "# Another comment\n"
        "CREW: Hi!\n";

    auto script = Script::fromText(text);
    EXPECT_EQ(script.lineCount(), 2u);
}

TEST(ScriptTest, CharacterListSorted)
{
    std::string text =
        "MODERNIA: Hi!\n"
        "CROWN: Hello!\n"
        "ALICE: Hey!\n";

    auto script = Script::fromText(text);
    ASSERT_EQ(script.characters.size(), 3u);
    EXPECT_EQ(script.characters[0], "Alice");
    EXPECT_EQ(script.characters[1], "Crown");
    EXPECT_EQ(script.characters[2], "Modernia");
}

TEST(ScriptTest, EmptyScript)
{
    auto script = Script::fromText("");
    EXPECT_TRUE(script.isEmpty());
    EXPECT_EQ(script.lineCount(), 0u);
}

TEST(ScriptTest, LinesBySegment)
{
    std::string text =
        "**SEG1**\n"
        "ALICE: Line1\n"
        "BOB: Line2\n"
        "**SEG2**\n"
        "CHARLIE: Line3\n";

    auto script = Script::fromText(text);
    auto seg1 = script.linesBySegment("SEG1");
    auto seg2 = script.linesBySegment("SEG2");
    EXPECT_EQ(seg1.size(), 2u);
    EXPECT_EQ(seg2.size(), 1u);
}

TEST(ScriptTest, SegmentForLine)
{
    std::string text =
        "**SEG1**\n"
        "ALICE: Line1\n";

    auto script = Script::fromText(text);
    EXPECT_EQ(script.segmentForLine(2), "SEG1"); // line 2 (after segment marker)
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script parsing — HTML format
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptTest, ParseHtml)
{
    std::string html =
        "<html><body>"
        "<p>CROWN: Hello from HTML!</p>"
        "<p>CREW: HTML is great.</p>"
        "</body></html>";

    auto script = Script::fromHtml(html);
    EXPECT_EQ(script.lineCount(), 2u);
    EXPECT_EQ(script.lines[0].character, "Crown");
}

TEST(ScriptTest, ParseHtmlEntities)
{
    std::string html =
        "<html><body>"
        "<p>CROWN: Hello &amp; welcome!</p>"
        "</body></html>";

    auto script = Script::fromHtml(html);
    ASSERT_EQ(script.lineCount(), 1u);
    EXPECT_TRUE(script.lines[0].dialogue.find("&") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script parsing — SHOT | ACTION | SCRIPT table (Google Docs export)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

std::string shotRow(const std::string& shot, const std::string& action, const std::string& text)
{
    auto cell = [](const std::string& s) {
        return "<td class=\"c0\" colspan=\"1\" rowspan=\"1\"><p class=\"c3\"><span class=\"c6\">"
               + s + "</span></p></td>";
    };
    return "<tr class=\"c8\">" + cell(shot) + cell(action) + cell(text) + "</tr>";
}

// Mirrors the structure of a real Google Docs export of the show script.
std::string shotTableHtml()
{
    return "<html><head><style>.c0{border:1px}</style></head><body>"
           "<p class=\"title\"><span>GACHA IS SUFFERING: COIN RUSH SHOWDOWN</span></p>"
           "<table class=\"c16\"><tbody>"
           + shotRow("SHOT", "ACTION", "SCRIPT")
           + shotRow("WIDE", "", "Welcome to GACHA IS SUFFERING. ")
           + shotRow("CLOSE-UP", "a smug grin", "Let&rsquo;s talk about it.")
           + shotRow("-", "STORY", "-")
           + shotRow("WIDE", "", "Let&rsquo;s start with the story.")
           + shotRow("CLOSE-UP", "", "They had me in the first half.")
           + shotRow("", "", "But the Hard Mode ending did save the event.")
           + shotRow("", "", "")
           + shotRow("-", "BANNER", "-")
           + shotRow("Close up", "", "The banner units were much better.")
           + shotRow("", "MINI-GAME", "")
           + shotRow("WIDE SHOT", "", "The mini-game was simple.")
           + shotRow("", "lean in", "")
           + "</tbody></table></body></html>";
}

} // namespace

TEST(ScriptTest, ShotTableParsesShotsActionsAndSegments)
{
    auto script = Script::fromHtml(shotTableHtml());
    ASSERT_TRUE(script.isShotScript());
    ASSERT_EQ(script.lineCount(), 7u);

    EXPECT_EQ(script.lines[0].shot, "WIDE");
    EXPECT_EQ(script.lines[0].dialogue, "Welcome to GACHA IS SUFFERING.");
    EXPECT_EQ(script.lines[0].segment, "INTRO");
    EXPECT_EQ(script.lines[0].character, kShotScriptCharacter);

    EXPECT_EQ(script.lines[1].shot, "CLOSE-UP");
    EXPECT_EQ(script.lines[1].action, "a smug grin");
    EXPECT_EQ(script.lines[1].dialogue, "Let\xe2\x80\x99s talk about it.");

    EXPECT_EQ(script.lines[2].segment, "STORY");
    EXPECT_EQ(script.lines[5].segment, "BANNER");
    EXPECT_EQ(script.lines[5].shot, "CLOSE-UP");  // "Close up" normalized

    // Dash-less all-caps ACTION-only row is a segment; a lowercase one is not.
    EXPECT_EQ(script.lines[6].segment, "MINI-GAME");
    EXPECT_EQ(script.lines[6].shot, "WIDE");  // "WIDE SHOT" normalized

    const std::vector<std::string> expectedSegments{"INTRO", "STORY", "BANNER", "MINI-GAME"};
    EXPECT_EQ(script.segments, expectedSegments);
    EXPECT_EQ(script.characters, std::vector<std::string>{kShotScriptCharacter});
}

TEST(ScriptTest, ShotTableBlankShotStaysBlank)
{
    // A blank SHOT cell is a script error the user fixes; never guess it.
    auto script = Script::fromHtml(shotTableHtml());
    ASSERT_EQ(script.lineCount(), 7u);
    EXPECT_EQ(script.lines[3].shot, "CLOSE-UP");
    EXPECT_TRUE(script.lines[4].shot.empty());
    EXPECT_TRUE(script.isShotScript());
}

TEST(ScriptTest, ShotTableLineNumbersAreUniqueAndPositive)
{
    auto script = Script::fromHtml(shotTableHtml());
    std::vector<int> numbers;
    for (const auto& line : script.lines) {
        EXPECT_GT(line.lineNumber, 0);
        numbers.push_back(line.lineNumber);
    }
    EXPECT_TRUE(std::is_sorted(numbers.begin(), numbers.end()));
    EXPECT_EQ(std::adjacent_find(numbers.begin(), numbers.end()), numbers.end());
}

TEST(ScriptTest, ShotTableAutoDetectedByLoad)
{
    auto script = Script::load(shotTableHtml());
    EXPECT_TRUE(script.isShotScript());
    EXPECT_EQ(script.lineCount(), 7u);
}

TEST(ScriptTest, NonShotTableFallsBackToDialogueParser)
{
    // A table without SHOT/SCRIPT headers must not hijack a dialogue script.
    std::string html =
        "<html><body>"
        "<table><tr><td>NAME</td><td>ROLE</td></tr><tr><td>Crown</td><td>Lead</td></tr></table>"
        "<p>CROWN: Hello from HTML!</p>"
        "</body></html>";
    auto script = Script::fromHtml(html);
    EXPECT_FALSE(script.isShotScript());
    ASSERT_EQ(script.lineCount(), 1u);
    EXPECT_EQ(script.lines[0].character, "Crown");
    EXPECT_TRUE(script.lines[0].shot.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  findLineTakes — every take of every line in one recording
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// Append a spoken phrase, one word per 0.3 s, then a 1 s pause.
void speak(std::vector<TimedWord>& words, double& t, const std::string& phrase)
{
    std::istringstream ss(phrase);
    std::string w;
    while (ss >> w) {
        words.push_back({w, t, t + 0.25});
        t += 0.3;
    }
    t += 1.0;
}

std::vector<ScriptLine> takeLines()
{
    std::vector<ScriptLine> lines(3);
    lines[0].lineNumber = 1; lines[0].dialogue = "Welcome to Gacha is Suffering.";
    lines[1].lineNumber = 2; lines[1].dialogue = "Where the eternal debate over maids vs bunny girls never ends!";
    lines[2].lineNumber = 3; lines[2].dialogue = "Let's talk about it.";
    return lines;
}

} // namespace

TEST(FindLineTakesTest, FindsEveryRepeatedTake)
{
    std::vector<TimedWord> words;
    double t = 0.0;
    speak(words, t, "welcome to gacha is suffering");
    speak(words, t, "welcome to gacha is suffering");   // second take
    speak(words, t, "where the eternal debate over maids vs bunny girls never ends");
    speak(words, t, "lets talk about it");
    speak(words, t, "welcome to gacha is suffering");   // third take, out of order

    const auto takes = findLineTakes(words, takeLines());
    ASSERT_EQ(takes.size(), 5u);
    EXPECT_EQ(takes[0].lineNumber, 1);
    EXPECT_EQ(takes[1].lineNumber, 1);
    EXPECT_EQ(takes[2].lineNumber, 2);
    EXPECT_EQ(takes[3].lineNumber, 3);
    EXPECT_EQ(takes[4].lineNumber, 1);
    for (size_t i = 1; i < takes.size(); ++i)
        EXPECT_GT(takes[i].start, takes[i - 1].end);  // distinct, time-ordered
    EXPECT_FLOAT_EQ(takes[0].coverage, 1.0f);
    EXPECT_DOUBLE_EQ(takes[0].start, words[0].start);
    EXPECT_DOUBLE_EQ(takes[0].end, words[4].end);
}

TEST(FindLineTakesTest, FlubbedRestartKeepsOnlyTheFullRead)
{
    std::vector<TimedWord> words;
    double t = 0.0;
    // Stumble, restart immediately without a clean break.
    speak(words, t, "where the eternal uh where the eternal debate over maids vs bunny girls never ends");

    const auto takes = findLineTakes(words, takeLines());
    ASSERT_EQ(takes.size(), 1u);
    EXPECT_EQ(takes[0].lineNumber, 2);
    EXPECT_FLOAT_EQ(takes[0].coverage, 1.0f);
    EXPECT_EQ(words[static_cast<size_t>(takes[0].firstWord)].text, "where");
    EXPECT_GE(takes[0].firstWord, 4);  // the full read, not the stumble
}

TEST(FindLineTakesTest, IgnoresPartialReadsAndChatter)
{
    std::vector<TimedWord> words;
    double t = 0.0;
    speak(words, t, "okay rolling");
    speak(words, t, "where the eternal");                  // abandoned, < 60% of the line
    speak(words, t, "lets talk about it");

    const auto takes = findLineTakes(words, takeLines());
    ASSERT_EQ(takes.size(), 1u);
    EXPECT_EQ(takes[0].lineNumber, 3);
}

TEST(FindLineTakesTest, ShortLineInsideLongerLineIsNotATake)
{
    std::vector<ScriptLine> lines(2);
    lines[0].lineNumber = 1; lines[0].dialogue = "Let's talk about it.";
    lines[1].lineNumber = 2; lines[1].dialogue = "Before we start, let's talk about it for a minute.";

    std::vector<TimedWord> words;
    double t = 0.0;
    speak(words, t, "before we start lets talk about it for a minute");

    const auto takes = findLineTakes(words, lines);
    ASSERT_EQ(takes.size(), 1u);
    EXPECT_EQ(takes[0].lineNumber, 2);
}

TEST(FindLineTakesTest, TranscriptPunctuationAndCaseIgnored)
{
    std::vector<TimedWord> words;
    double t = 0.0;
    speak(words, t, "Let's Talk, about it!");

    const auto takes = findLineTakes(words, takeLines());
    ASSERT_EQ(takes.size(), 1u);
    EXPECT_EQ(takes[0].lineNumber, 3);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script parsing — JSON format
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptTest, ParseJson)
{
    std::string json = R"({
        "segments": ["INTRO"],
        "lines": [
            {"character": "CROWN", "dialogue": "Hello!", "segment": "INTRO"},
            {"character": "CREW", "dialogue": "Hi!", "segment": "INTRO"}
        ]
    })";

    auto script = Script::fromJson(json);
    EXPECT_EQ(script.lineCount(), 2u);
    EXPECT_EQ(script.lines[0].character, "Crown");
    EXPECT_EQ(script.lines[0].dialogue, "Hello!");
    EXPECT_EQ(script.lines[0].segment, "INTRO");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script auto-detection
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptTest, LoadAutoDetectText)
{
    std::string text = "CROWN: Hello!\nCREW: Hi!\n";
    auto script = Script::load(text);
    EXPECT_EQ(script.lineCount(), 2u);
}

TEST(ScriptTest, LoadAutoDetectJson)
{
    std::string json = R"({"lines": [{"character": "CROWN", "dialogue": "Hello!"}]})";
    auto script = Script::load(json);
    EXPECT_EQ(script.lineCount(), 1u);
}

TEST(ScriptTest, LoadAutoDetectHtml)
{
    std::string html = "<html><body><p>CROWN: Hello!</p></body></html>";
    auto script = Script::load(html);
    EXPECT_EQ(script.lineCount(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — text utilities
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptMatcherTest, Normalize)
{
    EXPECT_EQ(ScriptMatcher::normalize("Hello, World!"), "hello world");
    EXPECT_EQ(ScriptMatcher::normalize("  Multiple   Spaces  "), "multiple spaces");
    EXPECT_EQ(ScriptMatcher::normalize("It's a test."), "its a test");
    EXPECT_EQ(ScriptMatcher::normalize(""), "");
}

TEST(ScriptMatcherTest, SequenceRatioIdentical)
{
    float ratio = ScriptMatcher::sequenceRatio("hello world", "hello world");
    EXPECT_FLOAT_EQ(ratio, 1.0f);
}

TEST(ScriptMatcherTest, SequenceRatioEmpty)
{
    float ratio = ScriptMatcher::sequenceRatio("", "");
    EXPECT_FLOAT_EQ(ratio, 1.0f);
}

TEST(ScriptMatcherTest, SequenceRatioOneEmpty)
{
    EXPECT_FLOAT_EQ(ScriptMatcher::sequenceRatio("hello", ""), 0.0f);
    EXPECT_FLOAT_EQ(ScriptMatcher::sequenceRatio("", "hello"), 0.0f);
}

TEST(ScriptMatcherTest, SequenceRatioSimilar)
{
    float ratio = ScriptMatcher::sequenceRatio("hello world", "hello wurld");
    // Very similar — should be high but not 1.0
    EXPECT_GT(ratio, 0.7f);
    EXPECT_LT(ratio, 1.0f);
}

TEST(ScriptMatcherTest, SequenceRatioCompleteDiff)
{
    float ratio = ScriptMatcher::sequenceRatio("abc", "xyz");
    EXPECT_FLOAT_EQ(ratio, 0.0f);
}

TEST(ScriptMatcherTest, Soundex)
{
    // Robert and Rupert should have the same Soundex code
    EXPECT_EQ(ScriptMatcher::soundex("Robert"), ScriptMatcher::soundex("Rupert"));
    // Smith and Smyth
    EXPECT_EQ(ScriptMatcher::soundex("Smith"), ScriptMatcher::soundex("Smyth"));
    // Different names
    EXPECT_NE(ScriptMatcher::soundex("Robert"), ScriptMatcher::soundex("Smith"));
}

TEST(ScriptMatcherTest, SoundexEmpty)
{
    EXPECT_EQ(ScriptMatcher::soundex(""), "0000");
}

TEST(ScriptMatcherTest, SoundexPadding)
{
    // Short words should be padded to 4 characters
    std::string result = ScriptMatcher::soundex("A");
    EXPECT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], 'A');
}

TEST(ScriptMatcherTest, Metaphone)
{
    // Known values — metaphone produces consistent phonetic codes
    std::string m1 = ScriptMatcher::metaphone("Knight");
    std::string m2 = ScriptMatcher::metaphone("Night");
    // Both should start with N (silent K)
    EXPECT_EQ(m1[0], 'N');
    EXPECT_EQ(m2[0], 'N');
}

TEST(ScriptMatcherTest, MetaphoneEmpty)
{
    EXPECT_TRUE(ScriptMatcher::metaphone("").empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — matching
// ═══════════════════════════════════════════════════════════════════════════

TEST(ScriptMatcherTest, MatchSingleSegment)
{
    std::string text =
        "CROWN: Welcome everyone to Roundtable Talk!\n"
        "CREW: Thanks for having us.\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    auto [match, confidence] = matcher.matchSegment("Welcome everyone to Roundtable Talk");
    ASSERT_NE(match, nullptr);
    EXPECT_EQ(match->character, "Crown");
    EXPECT_GT(confidence, 0.8f);
}

TEST(ScriptMatcherTest, MatchNoResult)
{
    std::string text = "CROWN: Hello!\n";
    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    auto [match, confidence] = matcher.matchSegment("completely unrelated text about nothing");
    EXPECT_EQ(match, nullptr);
    EXPECT_FLOAT_EQ(confidence, 0.0f);
}

TEST(ScriptMatcherTest, MatchAllSegments)
{
    std::string text =
        "CROWN: Welcome everyone!\n"
        "CREW: Thanks for having us.\n"
        "CROWN: Let's get started.\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    std::vector<ScriptMatcher::SegmentInput> segments = {
        {"Welcome everyone", 0.0, 2.0},
        {"Thanks for having us", 2.0, 4.0},
        {"Let's get started", 4.0, 6.0},
    };

    auto results = matcher.matchAllSegments(segments);
    ASSERT_EQ(results.size(), 3u);

    EXPECT_EQ(results[0].character, "Crown");
    EXPECT_EQ(results[1].character, "Crew");
    EXPECT_EQ(results[2].character, "Crown");

    // All should be tentative
    for (const auto& r : results) {
        EXPECT_EQ(r.state, MatchState::Tentative);
        EXPECT_GT(r.confidence, 0.0f);
    }
}

TEST(ScriptMatcherTest, MatchWithSequentialBias)
{
    std::string text =
        "ALICE: First line here\n"
        "BOB: Second line now\n"
        "ALICE: Third line there\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    // First segment matches "First line", so second should get bias toward "Second line"
    std::vector<ScriptMatcher::SegmentInput> segments = {
        {"First line here", 0.0, 2.0},
        {"Second line now", 2.0, 4.0},
    };

    auto results = matcher.matchAllSegments(segments);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].character, "Alice");
    EXPECT_EQ(results[1].character, "Bob");
}

TEST(ScriptMatcherTest, MatchWithRetakes)
{
    std::string text =
        "CROWN: Hello there!\n"
        "CREW: Hi!\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    // Two takes of the same line (retakes mode)
    std::vector<ScriptMatcher::SegmentInput> segments = {
        {"Hello there", 0.0, 2.0},
        {"Hello there take two", 2.0, 4.0},
    };

    auto results = matcher.matchAllSegments(segments, /*allowRetakes=*/true);
    ASSERT_EQ(results.size(), 2u);
    // Both should match Crown's line
    EXPECT_EQ(results[0].character, "Crown");
}

TEST(ScriptMatcherTest, MatchWithTimecode)
{
    std::string text =
        "CROWN: [0:05] Hello!\n"
        "CREW: [0:15] Goodbye!\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    std::vector<ScriptMatcher::SegmentInput> segments = {
        {"Hello", 5.0, 7.0},  // Matches timecode 5.0
        {"Goodbye", 15.0, 17.0},  // Matches timecode 15.0
    };

    auto results = matcher.matchAllSegments(segments);
    EXPECT_EQ(results[0].character, "Crown");
    EXPECT_EQ(results[1].character, "Crew");
}

TEST(ScriptMatcherTest, Threshold)
{
    std::string text = "CROWN: Hello!\n";
    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    EXPECT_FLOAT_EQ(matcher.threshold(), 0.4f);
    matcher.setThreshold(0.9f);
    EXPECT_FLOAT_EQ(matcher.threshold(), 0.9f);

    // With very high threshold, weak matches should fail
    auto [match, confidence] = matcher.matchSegment("Hell");
    // This might or might not match depending on ratio
    // But with threshold 0.9, marginal matches are filtered
}

TEST(ScriptMatcherTest, Characters)
{
    std::string text =
        "CROWN: Hello!\n"
        "CREW: Hi!\n"
        "ALICE: Hey!\n";

    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    ASSERT_EQ(matcher.characters().size(), 3u);
    EXPECT_EQ(matcher.characters()[0], "Alice");
    EXPECT_EQ(matcher.characters()[1], "Crew");
    EXPECT_EQ(matcher.characters()[2], "Crown");
}

TEST(ScriptMatcherTest, ScriptAccessor)
{
    std::string text = "CROWN: Hello!\n";
    auto script = Script::fromText(text);
    ScriptMatcher matcher(script);

    EXPECT_EQ(matcher.script().lineCount(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Semantic splitting (via Transcriber)
// ═══════════════════════════════════════════════════════════════════════════

TEST(TranscriberTest, SemanticSplitNoWords)
{
    // With no word-level data, segment should pass through
    Transcriber t;
    t.loadModel(WhisperModelSize::Tiny);
    // Can't easily test private semanticSplit, but we verify
    // the transcriber handles empty segments gracefully
    auto result = t.transcribe("nonexistent.wav");
    EXPECT_TRUE(result.segments.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
//  Match state enum
// ═══════════════════════════════════════════════════════════════════════════

TEST(MatchStateTest, Values)
{
    EXPECT_EQ(static_cast<int>(MatchState::Unmatched), 0);
    EXPECT_EQ(static_cast<int>(MatchState::Tentative), 1);
    EXPECT_EQ(static_cast<int>(MatchState::Confirmed), 2);
}

// ═══════════════════════════════════════════════════════════════════════════
//  SyncClip data
// ═══════════════════════════════════════════════════════════════════════════

TEST(SyncClipTest, Defaults)
{
    // SyncClip is in AudioSync.h which requires Qt — so test manually
    // Just verify MatchResult defaults
    MatchResult mr;
    EXPECT_EQ(mr.segmentIndex, -1);
    EXPECT_TRUE(mr.character.empty());
    EXPECT_FLOAT_EQ(mr.confidence, 0.0f);
    EXPECT_EQ(mr.state, MatchState::Unmatched);
}


// ═══════════════════════════════════════════════════════════════════════════
//  ShotPipeline — framing math, export command, persistence
// ═══════════════════════════════════════════════════════════════════════════

TEST(ShotPipelineTest, HalfScaleOf4kExactlyFills1080p)
{
    const auto p = computeCropPlacement(3840, 2160, ShotFraming{0.5, 0.5, 0.5});
    EXPECT_EQ(p.scaledW, 1920);
    EXPECT_EQ(p.scaledH, 1080);
    EXPECT_EQ(p.cropX, 1920);   // exactly over the scaled source (pad offset)
    EXPECT_EQ(p.cropY, 1080);
}

TEST(ShotPipelineTest, CloseUpCropsAroundCentre)
{
    const auto p = computeCropPlacement(3840, 2160, ShotFraming{0.9, 0.5, 0.4});
    EXPECT_EQ(p.scaledW, 3456);
    EXPECT_EQ(p.scaledH, 1944);
    EXPECT_EQ(p.cropX, 1920 + 1728 - 960);
    EXPECT_EQ(p.cropY, 1080 + static_cast<int>(std::lround(0.4 * 1944)) - 540);
}

TEST(ShotPipelineTest, CropStaysInsidePaddedCanvas)
{
    const auto p = computeCropPlacement(3840, 2160, ShotFraming{0.9, 5.0, -5.0});
    EXPECT_EQ(p.cropX, p.scaledW + 1920);
    EXPECT_EQ(p.cropY, 0);
}

TEST(ShotPipelineTest, ExportArgsTrimFrameAndPad)
{
    ShotExportJob job;
    job.sourceFile = "C:/rec/take 1.mp4";
    job.start = 12.5;
    job.end = 15.0;
    job.tailPadSec = 0.25;
    job.framing = ShotFraming{0.5, 0.5, 0.5};
    job.outputFile = "C:/out/001_INTRO_WIDE_L1.mp4";
    const auto args = buildShotExportArgs(job);

    auto after = [&args](const std::string& flag) {
        auto it = std::find(args.begin(), args.end(), flag);
        return (it != args.end() && it + 1 != args.end()) ? *(it + 1) : std::string{};
    };
    EXPECT_EQ(after("-ss"), "12.500");
    EXPECT_EQ(after("-i"), "C:/rec/take 1.mp4");
    EXPECT_EQ(after("-t"), "2.750");   // take + tail pad
    EXPECT_EQ(after("-c:v"), "h264_nvenc");
    const std::string vf = after("-vf");
    EXPECT_NE(vf.find("scale=1920:1080"), std::string::npos);
    EXPECT_NE(vf.find("color=0x00FF00"), std::string::npos);   // outside source = keyable green
    EXPECT_NE(vf.find("crop=1920:1080:1920:1080"), std::string::npos);
    EXPECT_EQ(args.back(), "C:/out/001_INTRO_WIDE_L1.mp4");
}

TEST(ShotPipelineTest, FallbacksAndOverrides)
{
    ShowPipelineState state;
    state.defaultPadFrames = 3;
    state.recordings["a.mp4"].closeUp = ShotFraming{0.8, 0.45, 0.35};

    EXPECT_EQ(state.framingFor("line", "a.mp4", true), (ShotFraming{0.8, 0.45, 0.35}));
    EXPECT_EQ(state.framingFor("line", "a.mp4", false), RecordingFraming{}.wide);
    EXPECT_EQ(state.framingFor("line", "unknown.mp4", true), RecordingFraming{}.closeUp);
    EXPECT_EQ(state.padFramesFor("line"), 3);
    EXPECT_EQ(state.fpsFor("line"), kScailDefaultFps);

    auto& shot = state.shots["line"];
    shot.hasFraming = true;
    shot.framing = ShotFraming{1.0, 0.5, 0.3};
    shot.padFrames = 0;   // explicit "none" beats the default
    shot.fps = kScailReducedFps;
    EXPECT_EQ(state.framingFor("line", "a.mp4", true), (ShotFraming{1.0, 0.5, 0.3}));
    EXPECT_EQ(state.padFramesFor("line"), 0);
    EXPECT_EQ(state.fpsFor("line"), kScailReducedFps);
}

TEST(ShotPipelineTest, StateRoundTrips)
{
    ShowPipelineState state;
    state.recordings["C:/rec/\xe2\x80\x99take.mp4"].wide = ShotFraming{0.45, 0.52, 0.5};
    auto& shot = state.shots["welcome to gacha is suffering"];
    shot.hasFraming = true;
    shot.framing = ShotFraming{0.95, 0.5, 0.38};
    shot.padFrames = 4;
    shot.expression = "a smug grin";
    shot.fps = 12;
    shot.exportedFile = "C:/out/001.mp4";
    shot.exportedTailPadSec = 0.25;
    state.defaultPadFrames = 2;
    state.defaultExpression = "a calm, attentive expression";
    state.frameCapThreshold = 161;
    state.outputDir = "C:/out";
    state.workflowFile = "F:/ComfyUI/user/default/workflows/SCAIL 2 FINAL V10.json";
    state.wideReferenceImage = "C:/refs/wide.png";
    state.closeUpReferenceImage = "C:/refs/cu.png";
    state.seed = 153;
    state.background = "C:/bg/bkg loop.mp4";
    state.closeUpBackgroundZoom = 1.65;
    state.segmentGapSec = 4.5;
    shot.seed = 987654321012LL;
    shot.renders.push_back({"C:/renders/001_v1.mp4", 153, "a smug grin", 16, 0.25, 1790000000});
    shot.renders.push_back({"C:/renders/001_v2.mp4", 777, "a big grin", 12, 0.33, 1790000100});
    shot.chosenRender = 0;

    BinaryWriter w;
    state.write(w);
    BinaryReader r(w.data().data(), w.data().size());
    ShowPipelineState back;
    back.read(r);
    EXPECT_EQ(back, state);
}

TEST(ShotPipelineTest, SeedsFramesAndActiveRender)
{
    ShowPipelineState state;
    EXPECT_EQ(state.seedFor("a"), -1);            // workflow's own seed
    state.seed = 153;
    EXPECT_EQ(state.seedFor("a"), 153);
    state.shots["a"].seed = 42;                   // re-rolled shot
    EXPECT_EQ(state.seedFor("a"), 42);

    state.defaultPadFrames = 3;
    EXPECT_EQ(state.renderFramesFor("b", 5.0), 80 + 3);   // 5 s @16 + pad
    state.shots["b"].fps = 12;
    EXPECT_EQ(state.renderFramesFor("b", 5.0), 60 + 3);

    ShotSettings s;
    EXPECT_EQ(s.activeRender(), nullptr);
    s.renders.push_back({"v1.mp4"});
    s.renders.push_back({"v2.mp4"});
    EXPECT_EQ(s.activeRender()->file, "v2.mp4");  // newest by default
    s.chosenRender = 0;
    EXPECT_EQ(s.activeRender()->file, "v1.mp4");
}

TEST(ShotPipelineTest, RenderFillsWidthOnTheBottomEdge)
{
    // SCAIL renders 1280x704: cover-fit would be 1.534x; width-fit is 1.5x.
    const auto p = fillWidthBottom(1280, 704);
    EXPECT_NEAR(p.scale * (1080.0 / 704.0), 1.5, 1e-6);
    EXPECT_NEAR(p.posYPx, (1080.0 - 704.0 * 1.5) / 2.0, 1e-4);   // 12 px down
    const auto same = fillWidthBottom(1920, 1080);
    EXPECT_FLOAT_EQ(same.scale, 1.0f);
    EXPECT_FLOAT_EQ(same.posYPx, 0.0f);
}

TEST(ShotPipelineTest, AssemblyLaysOutShotsGapsMarkersAndBackgrounds)
{
    const int64_t T = kTicksPerSecond;
    std::vector<AssemblyShot> shots{
        {"INTRO", false, 2.25, 0.25},   // 2 s
        {"INTRO", true,  3.0,  0.0},    // 3 s
        {"INTRO", true,  1.0,  0.0},    // 1 s  (same CU run)
        {"STORY", false, 4.0,  0.0},    // 4 s, after a 2 s gap
    };
    const auto plan = planAssembly(shots, 10 * T, 2.0, /*background loop*/ 3.5);

    ASSERT_EQ(plan.shots.size(), 4u);
    EXPECT_EQ(plan.shots[0].timelineIn, 10 * T);
    EXPECT_EQ(plan.shots[0].duration, 2 * T);        // tail pad trimmed
    EXPECT_EQ(plan.shots[1].timelineIn, 12 * T);
    EXPECT_EQ(plan.shots[2].timelineIn, 15 * T);
    EXPECT_EQ(plan.shots[3].timelineIn, 18 * T);     // 16 + 2 s stinger gap
    EXPECT_EQ(plan.end, 22 * T);

    ASSERT_EQ(plan.markers.size(), 2u);
    EXPECT_EQ(plan.markers[0].time, 10 * T);
    EXPECT_EQ(plan.markers[0].label, "INTRO");
    EXPECT_EQ(plan.markers[1].time, 18 * T);
    EXPECT_EQ(plan.markers[1].label, "STORY");

    // One 3.5 s loop running continuously from 10 s, split per shot-type run
    // and at every wrap:
    //   WIDE 10-12 (src 0-2) | CU 12-13.5 (src 2-3.5) | CU 13.5-16 (src 0-2.5)
    //   | stinger gap | WIDE 18-20.5 (src 1-3.5) | WIDE 20.5-22 (src 0-1.5)
    const int64_t H = T / 2;
    ASSERT_EQ(plan.backgrounds.size(), 5u);
    EXPECT_FALSE(plan.backgrounds[0].closeUp);
    EXPECT_EQ(plan.backgrounds[0].timelineIn, 10 * T);
    EXPECT_EQ(plan.backgrounds[0].duration, 2 * T);
    EXPECT_EQ(plan.backgrounds[0].sourceIn, 0);
    EXPECT_TRUE(plan.backgrounds[1].closeUp);
    EXPECT_EQ(plan.backgrounds[1].firstShot, 1u);
    EXPECT_EQ(plan.backgrounds[1].timelineIn, 12 * T);
    EXPECT_EQ(plan.backgrounds[1].sourceIn, 2 * T);    // no jump at the cut
    EXPECT_EQ(plan.backgrounds[1].duration, 3 * H);
    EXPECT_EQ(plan.backgrounds[2].timelineIn, 12 * T + 3 * H);
    EXPECT_EQ(plan.backgrounds[2].sourceIn, 0);        // wrapped
    EXPECT_EQ(plan.backgrounds[2].duration, 5 * H);
    EXPECT_FALSE(plan.backgrounds[3].closeUp);
    EXPECT_EQ(plan.backgrounds[3].timelineIn, 18 * T);
    EXPECT_EQ(plan.backgrounds[3].sourceIn, 1 * T);    // (18 - 10) mod 3.5
    EXPECT_EQ(plan.backgrounds[3].duration, 5 * H);
    EXPECT_EQ(plan.backgrounds[4].sourceIn, 0);
    EXPECT_EQ(plan.backgrounds[4].duration, 3 * H);

    // A still image never wraps: one clip per run.
    const auto still = planAssembly(shots, 0, 2.0, 0.0);
    EXPECT_EQ(still.backgrounds.size(), 3u);
}

TEST(ShotPipelineTest, FileNamesSortAndAreSafe)
{
    EXPECT_EQ(shotFileName(7, "STORY", "CLOSE-UP", 12), "007_STORY_CU_L12.mp4");
    EXPECT_EQ(shotFileName(42, "MINI-GAME", "WIDE", 3), "042_MINI-GAME_WIDE_L3.mp4");
    EXPECT_EQ(shotFileName(1, "a/b:c", "WIDE", 1), "001_ABC_WIDE_L1.mp4");
}

TEST(ShotPipelineTest, ExportSignatureChangesOnlyWhenTheShotDoes)
{
    ShotExportJob job;
    job.sourceFile = "C:/rec/take.mp4";
    job.start = 12.5;
    job.end = 15.25;
    job.tailPadSec = 0.25;
    job.outputFile = "C:/shots/001_STORY_WIDE_L1.mp4";
    const std::string base = shotExportSignature(job, 1000, 42);
    EXPECT_EQ(shotExportSignature(job, 1000, 42), base);

    ShotExportJob other = job;
    other.videoEncoder = "libopenh264";            // encoder fallback: same pixels
    EXPECT_EQ(shotExportSignature(other, 1000, 42), base);

    auto differs = [&](auto mutate) {
        ShotExportJob j = job;
        mutate(j);
        return shotExportSignature(j, 1000, 42) != base;
    };
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.end += 1e-6; }));          // trimmed out point
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.start = 12.0; }));
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.tailPadSec = 0.5; }));
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.framing.scale = 0.9; }));   // re-framed
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.framing.centerY = 0.4; }));
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.sourceFile = "C:/rec/take2.mp4"; }));
    EXPECT_TRUE(differs([](ShotExportJob& j) { j.outputFile = "C:/shots/002.mp4"; }));
    EXPECT_NE(shotExportSignature(job, 1001, 42), base);   // source file replaced
    EXPECT_NE(shotExportSignature(job, 1000, 43), base);
}


TEST(ShotPipelineTest, RenderTrimInOutDrivesAssemblyLength)
{
    // 4 s render with a 0.5 s tail pad: automatic = 3.5 s.
    AssemblyShot autoTrim{"STORY", false, 4.0, 0.5};
    // Same render trimmed by hand to 0.25 .. 3.0 = 2.75 s (tail number ignored).
    AssemblyShot manual{"STORY", false, 4.0, 0.5, 0.25, 3.0};
    const AssemblyPlan plan = planAssembly({autoTrim, manual}, 0, 3.0, 0.0);
    ASSERT_EQ(plan.shots.size(), 2u);
    EXPECT_EQ(plan.shots[0].duration, static_cast<int64_t>(std::llround(3.5 * kTicksPerSecond)));
    EXPECT_EQ(plan.shots[1].duration, static_cast<int64_t>(std::llround(2.75 * kTicksPerSecond)));

    RenderVersion r;
    r.tailPadSec = 0.5;
    EXPECT_DOUBLE_EQ(r.effectiveOut(4.0), 3.5);   // automatic
    r.trimOut = 3.2;
    EXPECT_DOUBLE_EQ(r.effectiveOut(4.0), 3.2);   // manual wins
}

TEST(ShotPipelineTest, RenderTrimSurvivesSaveAndLoad)
{
    ShowPipelineState state;
    RenderVersion r;
    r.file = "C:/renders/001.mp4";
    r.tailPadSec = 0.5;
    r.trimIn = 0.125;
    r.trimOut = 3.375;
    state.shots["line"].renders.push_back(r);

    BinaryWriter w;
    state.write(w);
    BinaryReader reader(w.data().data(), w.data().size());
    ShowPipelineState loaded;
    loaded.read(reader);
    ASSERT_EQ(loaded.shots["line"].renders.size(), 1u);
    EXPECT_DOUBLE_EQ(loaded.shots["line"].renders[0].trimIn, 0.125);
    EXPECT_DOUBLE_EQ(loaded.shots["line"].renders[0].trimOut, 3.375);
    EXPECT_EQ(loaded, state);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Transcriber end to end (needs local models; skipped without them)
// ═════════════════════════════════════════════════════════════════════════════

namespace {

/// The models folder the build copies next to the binaries, if it holds
/// what `files` names. Tests never download models.
std::filesystem::path localModelsDir(std::initializer_list<const char*> files)
{
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0)
        candidates.push_back(std::filesystem::path(exe).parent_path() / "models");
#endif
    candidates.push_back(std::filesystem::current_path() / "models");
    for (const auto& dir : candidates) {
        bool all = true;
        for (const char* file : files)
            all = all && std::filesystem::exists(dir / file);
        if (all) return dir;
    }
    return {};
}

} // namespace

TEST(TranscriberEndToEnd, VoiceActivityDetectionRunsWithoutAborting)
{
    // VAD weights used to be loaded on the GPU in CUDA builds while
    // whisper.cpp always computes VAD on the CPU: ggml aborted the whole
    // process on the first transcription with the VAD model present.
    const auto models = localModelsDir({"ggml-tiny.bin", "ggml-silero-v6.2.0.bin"});
    if (models.empty()) GTEST_SKIP() << "whisper tiny + silero VAD models not found locally";

    // 3 s of 44.1 kHz stereo tone: also exercises the 16 kHz mono load.
    const auto path = std::filesystem::temp_directory_path() / "rt_transcriber_vad_test.wav";
    {
        const uint32_t rate = 44100, frames = rate * 3;
        std::ofstream f(path, std::ios::binary);
        auto w32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4); w32(36 + frames * 4); f.write("WAVE", 4);
        f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(rate); w32(rate * 4); w16(4); w16(16);
        f.write("data", 4); w32(frames * 4);
        for (uint32_t i = 0; i < frames; ++i) {
            const auto v = static_cast<int16_t>(6000 * std::sin(2.0 * 3.14159265358979 * 220.0 * i / rate));
            w16(static_cast<uint16_t>(v));
            w16(static_cast<uint16_t>(v));
        }
    }

    Transcriber transcriber;
    transcriber.setModelsDirectory(models.string());
    ASSERT_TRUE(transcriber.loadModel(WhisperModelSize::Tiny));
    ASSERT_TRUE(transcriber.isVadEnabled());
    const TranscriptionResult result = transcriber.transcribe(path.string(), "en");
    EXPECT_TRUE(result.succeeded()) << static_cast<int>(result.status);
    EXPECT_NEAR(result.duration, 3.0, 0.01);
    std::filesystem::remove(path);
}
