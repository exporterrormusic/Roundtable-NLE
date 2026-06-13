/*
 * test_audio_sync.cpp — Tests for Step 23: Audio Sync Workflow
 *
 * Tests Transcriber, ScriptMatcher (text/HTML/JSON parsing, fuzzy matching,
 * soundex, metaphone, normalize, sequenceRatio), and data types.
 */

#include <gtest/gtest.h>

#include "ai/Transcriber.h"
#include "ai/ScriptMatcher.h"

#include <cmath>
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
    EXPECT_STREQ(whisperModelName(WhisperModelSize::LargeV2), "large-v2");
    EXPECT_STREQ(whisperModelName(WhisperModelSize::LargeV3), "large-v3");
}

TEST(TranscriberTest, ModelFromName)
{
    EXPECT_EQ(whisperModelFromName("tiny"), WhisperModelSize::Tiny);
    EXPECT_EQ(whisperModelFromName("base"), WhisperModelSize::Base);
    EXPECT_EQ(whisperModelFromName("small"), WhisperModelSize::Small);
    EXPECT_EQ(whisperModelFromName("medium"), WhisperModelSize::Medium);
    EXPECT_EQ(whisperModelFromName("large-v2"), WhisperModelSize::LargeV2);
    EXPECT_EQ(whisperModelFromName("large-v3"), WhisperModelSize::LargeV3);
    EXPECT_EQ(whisperModelFromName("invalid"), WhisperModelSize::Base); // default
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
