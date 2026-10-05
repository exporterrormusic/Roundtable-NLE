/*
 * ScriptMatcher — Script parsing and fuzzy matching for audio sync.
 *
 * Parses scripts in multiple formats (plain text, HTML/Google Docs, JSON).
 * Matches transcribed segments to script lines using sequence matching
 * with sequential bias and timecode awareness.
 *
 * Ported from Python _OLD_/src/audio/script_matcher.py.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rt {

// ─── Script data types ──────────────────────────────────────────────────────

/// A single line of dialogue from the script.
struct ScriptLine
{
    std::string            character;
    std::string            dialogue;
    int                    lineNumber{0};
    std::string            segment{"UNTITLED"}; // Segment/section this line belongs to
    std::optional<double>  timecode;             // Optional manual timecode (seconds)

    // ── Shot-table scripts only (SHOT | ACTION | SCRIPT columns) ────────
    // Empty for character-dialogue scripts, so every existing consumer
    // behaves exactly as before. In a shot-table script an empty `shot`
    // means the doc's SHOT cell was blank: the user must pick one.
    std::string            shot;                 // "WIDE", "CLOSE-UP", or the doc's own label
    std::string            action;               // ACTION column text (facial-expression source)
};

/// Character assigned to every line of a shot-table script. These shows have
/// one on-camera announcer (the green-screen video), not character sprites.
inline constexpr const char* kShotScriptCharacter = "Announcer";

/// Parsed script containing dialogue lines and metadata.
struct Script
{
    std::vector<ScriptLine>  lines;
    std::vector<std::string> characters;  // Sorted unique character names
    std::vector<std::string> segments;    // Ordered segment names
    bool                     shotTable{false}; // Parsed from a SHOT | ACTION | SCRIPT table

    // ── Queries ─────────────────────────────────────────────────────────

    /// Get lines belonging to a specific segment.
    [[nodiscard]] std::vector<const ScriptLine*>
    linesBySegment(const std::string& segmentName) const;

    /// Get segment name for a given line number. Empty if not found.
    [[nodiscard]] std::string segmentForLine(int lineNumber) const;

    /// Number of lines.
    [[nodiscard]] size_t lineCount() const noexcept { return lines.size(); }

    /// True if script has no lines.
    [[nodiscard]] bool isEmpty() const noexcept { return lines.empty(); }

    /// True when parsed from a SHOT | ACTION | SCRIPT table.
    [[nodiscard]] bool isShotScript() const noexcept { return shotTable; }

    // ── Parsers ─────────────────────────────────────────────────────────

    /// Parse from plain text format.
    /// Format: "CHARACTER: Dialogue text" or "[Character] Dialogue text"
    /// Segment markers: "**SEGMENT NAME**"
    static Script fromText(const std::string& text);

    /// Parse from HTML (Google Docs export). Bold text = segment markers.
    /// A table whose header row has SHOT and SCRIPT columns is parsed as a
    /// shot-table script instead (see fromShotTable).
    static Script fromHtml(const std::string& html);

    /// Parse a SHOT | ACTION | SCRIPT table (Google Docs HTML export).
    /// Rows like "- | STORY | -" start a segment; lines before the first
    /// segment row belong to "INTRO"; a blank SHOT cell leaves `shot` empty.
    /// Text outside the table is ignored. Returns an empty script when no
    /// such table exists.
    static Script fromShotTable(const std::string& html);

    /// Parse from JSON format.
    static Script fromJson(const std::string& jsonStr);

    /// Auto-detect format and parse.
    static Script load(const std::string& pathOrContent);
};

// ─── Take finding (shot-table scripts) ──────────────────────────────────────

/// One transcribed word and its time span in the recording.
struct TimedWord
{
    std::string text;
    double      start{0.0};
    double      end{0.0};
};

/// One take of a script line found in a recording.
struct LineTake
{
    int    lineNumber{-1};
    int    firstWord{0};     ///< Index into the words vector (first matching word)
    int    lastWord{0};      ///< Index into the words vector (last matching word)
    int    hits{0};          ///< Words matching the script text
    float  coverage{0.0f};   ///< hits / script-line word count
    double start{0.0};       ///< words[firstWord].start
    double end{0.0};         ///< words[lastWord].end
};

/// Find EVERY take of every line in one continuous recording (someone reading
/// a script on camera, repeating lines until happy). Unlike a whole-file
/// alignment, repeated reads of the same line each come back as their own
/// take. Takes of different lines never overlap; a flubbed partial read that
/// overlaps a fuller read of the same line is dropped. Sorted by start time.
[[nodiscard]] std::vector<LineTake> findLineTakes(const std::vector<TimedWord>& words,
                                                  const std::vector<ScriptLine>& lines,
                                                  float minCoverage = 0.6f);

// ─── Match result ───────────────────────────────────────────────────────────

enum class MatchState : int
{
    Unmatched  = 0,
    Tentative  = 1,
    Confirmed  = 2
};

/// Result of matching one transcribed segment to a script line.
struct MatchResult
{
    int                    segmentIndex{-1};   // Index in transcription segments
    std::string            character;           // Matched character name (empty if unmatched)
    float                  confidence{0.0f};    // Match confidence 0-1
    int                    scriptLineNumber{-1};// Matched script line number
    std::string            scriptSegment;       // Script segment name
    MatchState             state{MatchState::Unmatched};
};

// ─── ScriptMatcher ──────────────────────────────────────────────────────────

class ScriptMatcher
{
public:
    explicit ScriptMatcher(const Script& script);
    ~ScriptMatcher() = default;

    // ── Single match ────────────────────────────────────────────────────

    /// Find the best matching script line for a segment of text.
    /// \return (matched line pointer, confidence). nullptr if no good match.
    [[nodiscard]] std::pair<const ScriptLine*, float>
    matchSegment(const std::string& segmentText) const;

    // ── Batch match ─────────────────────────────────────────────────────

    /// Input for batch matching: (text, start_time, end_time)
    struct SegmentInput
    {
        std::string text;
        double      start{0.0};
        double      end{0.0};
    };

    /// Match all transcribed segments to script lines.
    /// \param segments     List of (text, start, end) from transcription.
    /// \param allowRetakes If true, script lines can match multiple segments.
    [[nodiscard]] std::vector<MatchResult>
    matchAllSegments(const std::vector<SegmentInput>& segments,
                     bool allowRetakes = false) const;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set match threshold (0-1). Default: 0.4
    void setThreshold(float threshold) noexcept { m_threshold = threshold; }
    [[nodiscard]] float threshold() const noexcept { return m_threshold; }

    /// Get characters from the script.
    [[nodiscard]] const std::vector<std::string>& characters() const;

    /// Get the script.
    [[nodiscard]] const Script& script() const noexcept { return m_script; }

    // ── Text utilities (public for testing) ──────────────────────────────

    /// Normalize text for comparison (lowercase, strip punctuation, collapse whitespace).
    [[nodiscard]] static std::string normalize(const std::string& text);

    /// Calculate sequence similarity ratio (0-1) between two strings.
    [[nodiscard]] static float sequenceRatio(const std::string& a, const std::string& b);

    /// Soundex phonetic code.
    [[nodiscard]] static std::string soundex(const std::string& word);

    /// Double Metaphone phonetic code.
    [[nodiscard]] static std::string metaphone(const std::string& word);

private:
    Script m_script;
    float  m_threshold{0.4f};
};

} // namespace rt
