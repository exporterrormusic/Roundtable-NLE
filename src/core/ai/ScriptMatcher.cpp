/*
 * ScriptMatcher.cpp — Script parsing and fuzzy matching.
 */

#include "ai/ScriptMatcher.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
//  Script — queries
// ═══════════════════════════════════════════════════════════════════════════

std::vector<const ScriptLine*> Script::linesBySegment(const std::string& segmentName) const
{
    std::vector<const ScriptLine*> result;
    for (const auto& line : lines) {
        if (line.segment == segmentName)
            result.push_back(&line);
    }
    return result;
}

std::string Script::segmentForLine(int lineNumber) const
{
    for (const auto& line : lines) {
        if (line.lineNumber == lineNumber)
            return line.segment;
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════
//  Script — parsers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

/// Title-case a string: first letter uppercase, rest lowercase.
std::string titleCase(const std::string& s)
{
    if (s.empty()) return s;
    std::string result;
    result.reserve(s.size());
    bool nextUpper = true;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            result += c;
            nextUpper = true;
        } else if (nextUpper) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            nextUpper = false;
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

/// Trim whitespace from both ends.
std::string trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

/// Strip HTML tags from a string, inserting newlines for block elements.
std::string stripHtmlTags(const std::string& html)
{
    // Strip <style> blocks entirely (including their content) so CSS
    // definitions don't leak into the plain text output.
    static const std::regex styleRe("<style[^>]*>.*?</style>", std::regex::icase | std::regex::nosubs);
    std::string result = std::regex_replace(html, styleRe, "\n");
    // Strip <script> blocks entirely for the same reason.
    static const std::regex scriptRe("<script[^>]*>.*?</script>", std::regex::icase | std::regex::nosubs);
    result = std::regex_replace(result, scriptRe, "\n");
    // Insert newlines before block-level closing tags
    static const std::regex blockRe("</(?:p|div|li|tr|h[1-6]|br)[^>]*>", std::regex::icase);
    result = std::regex_replace(result, blockRe, "\n");
    // Also replace <br> and <br/> with newlines
    static const std::regex brRe("<br\\s*/?>" , std::regex::icase);
    result = std::regex_replace(result, brRe, "\n");
    // Strip remaining tags
    static const std::regex tagRe("<[^>]*>");
    return std::regex_replace(result, tagRe, "");
}

/// Parse timecode from "[MM:SS]" or "[MM:SS.ms]" at start of dialogue.
/// Returns (timecode_seconds, remainder_text) or (nullopt, original_text).
std::pair<std::optional<double>, std::string> parseTimecode(const std::string& dialogue)
{
    static const std::regex tcRe(R"(^\[(\d+):(\d+(?:\.\d+)?)\]\s*(.+)$)");
    std::smatch m;
    if (std::regex_match(dialogue, m, tcRe)) {
        double mins = std::stod(m[1].str());
        double secs = std::stod(m[2].str());
        return {mins * 60.0 + secs, m[3].str()};
    }
    return {std::nullopt, dialogue};
}

/// Encode a Unicode codepoint as UTF-8 bytes appended to `out`.
void encodeUtf8(int codepoint, std::string& out)
{
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

/// Try to match dialogue patterns. Returns (character, dialogue) or empty strings.
std::pair<std::string, std::string> matchDialogue(const std::string& line)
{
    // Pattern 1: CHARACTER: dialogue
    static const std::regex p1(R"(^([A-Z][A-Z0-9_\s]+)\s*:\s*(.+)$)");
    // Pattern 2: [Character] dialogue
    static const std::regex p2(R"(^\[([A-Za-z0-9_\s]+)\]\s*(.+)$)");

    std::smatch m;
    if (std::regex_match(line, m, p1)) {
        return {trim(m[1].str()), trim(m[2].str())};
    }
    if (std::regex_match(line, m, p2)) {
        return {trim(m[1].str()), trim(m[2].str())};
    }
    return {"", ""};
}

void finalizeScript(Script& script)
{
    // Add UNTITLED to segments if used
    bool hasUntitled = false;
    for (const auto& line : script.lines) {
        if (line.segment == "UNTITLED") {
            hasUntitled = true;
            break;
        }
    }
    if (hasUntitled) {
        auto it = std::find(script.segments.begin(), script.segments.end(), "UNTITLED");
        if (it == script.segments.end())
            script.segments.insert(script.segments.begin(), "UNTITLED");
    }

    // Build sorted unique character list
    std::set<std::string> chars;
    for (const auto& line : script.lines)
        chars.insert(line.character);
    script.characters.assign(chars.begin(), chars.end());
}

} // namespace

/// Strip UTF-8 BOM if present.
std::string stripBom(std::string s)
{
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
        spdlog::debug("ScriptMatcher: Stripped UTF-8 BOM from script content");
    }
    return s;
}

Script Script::fromText(const std::string& text)
{
    Script script;
    std::string currentSegment = "UNTITLED";
    int lineNum = 0;
    std::unordered_set<std::string> seenSegments;

    // Segment marker pattern: **SEGMENT NAME**
    static const std::regex segmentRe(R"(^\*\*([^*]+)\*\*\s*$)");

    std::istringstream stream(stripBom(text));
    std::string line;

    while (std::getline(stream, line)) {
        ++lineNum;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        // Check for segment marker
        std::smatch sm;
        if (std::regex_match(line, sm, segmentRe)) {
            currentSegment = trim(sm[1].str());
            if (seenSegments.insert(currentSegment).second) {
                script.segments.push_back(currentSegment);
            }
            spdlog::trace("ScriptMatcher: Found segment marker: '{}'", currentSegment);
            continue;
        }

        // Try dialogue patterns
        auto [charName, dialogue] = matchDialogue(line);
        if (!charName.empty() && !dialogue.empty()) {
            charName = titleCase(charName);
            auto [timecode, cleanDialogue] = parseTimecode(dialogue);

            ScriptLine sl;
            sl.character   = charName;
            sl.dialogue    = cleanDialogue;
            sl.lineNumber  = lineNum;
            sl.segment     = currentSegment;
            sl.timecode    = timecode;
            script.lines.push_back(std::move(sl));
        }
    }

    finalizeScript(script);
    spdlog::debug("ScriptMatcher: Parsed {} lines, {} segments from text",
                  script.lines.size(), script.segments.size());
    return script;
}

namespace {

/// Decode the HTML character references Google Docs exports emit.
std::string decodeHtmlEntities(std::string plainText)
{
    // Decode numeric HTML character references: &#NNNN;
    {
        static const std::regex numericRef(R"(&#(\d+);)");
        std::string out;
        std::sregex_iterator it(plainText.begin(), plainText.end(), numericRef);
        std::sregex_iterator end;
        size_t lastPos = 0;
        for (; it != end; ++it) {
            out.append(plainText, lastPos, static_cast<size_t>(it->position()) - lastPos);
            encodeUtf8(std::stoi((*it)[1].str()), out);
            lastPos = static_cast<size_t>(it->position() + it->length());
        }
        out.append(plainText, lastPos, plainText.size() - lastPos);
        plainText = std::move(out);
    }

    // Decode hex character references: &#xHHHH;
    {
        static const std::regex hexRef(R"(&#x([0-9a-fA-F]+);)");
        std::string out;
        std::sregex_iterator it(plainText.begin(), plainText.end(), hexRef);
        std::sregex_iterator end;
        size_t lastPos = 0;
        for (; it != end; ++it) {
            out.append(plainText, lastPos, static_cast<size_t>(it->position()) - lastPos);
            encodeUtf8(std::stoi((*it)[1].str(), nullptr, 16), out);
            lastPos = static_cast<size_t>(it->position() + it->length());
        }
        out.append(plainText, lastPos, plainText.size() - lastPos);
        plainText = std::move(out);
    }

    // Decode named HTML entities using literal string replacement (no regex overhead).
    // Order matters: decode & last since other entities contain '&'
    auto replaceAll = [](std::string& s, const std::string& what, const std::string& with) {
        size_t pos = 0;
        while ((pos = s.find(what, pos)) != std::string::npos) {
            s.replace(pos, what.size(), with);
            pos += with.size();
        }
    };

    replaceAll(plainText, "&nbsp;",  " ");
    replaceAll(plainText, "&rsquo;", "\xe2\x80\x99");
    replaceAll(plainText, "&lsquo;", "\xe2\x80\x98");
    replaceAll(plainText, "&rdquo;", "\xe2\x80\x9d");
    replaceAll(plainText, "&ldquo;", "\xe2\x80\x9c");
    replaceAll(plainText, "&mdash;", "\xe2\x80\x94");
    replaceAll(plainText, "&ndash;", "\xe2\x80\x93");
    replaceAll(plainText, "&hellip;","\xe2\x80\xa6");
    replaceAll(plainText, "&bull;",  "\xe2\x80\xa2");
    replaceAll(plainText, "&trade;", "\xe2\x84\xa2");
    replaceAll(plainText, "&copy;",  "\xc2\xa9");
    replaceAll(plainText, "&reg;",   "\xc2\xae");
    replaceAll(plainText, "&deg;",   "\xc2\xb0");
    replaceAll(plainText, "&times;", "\xc3\x97");
    replaceAll(plainText, "&divide;","\xc3\xb7");
    {
      std::string amp(1, '&');
      replaceAll(plainText, amp + "apos;", "'");
      replaceAll(plainText, amp + "quot;",  "\"");
      replaceAll(plainText, amp + "lt;",    "<");
      replaceAll(plainText, amp + "gt;",    ">");
      replaceAll(plainText, amp + "amp;",   "&");  // Must be last!
    }

    return plainText;
}

/// ASCII-only lowercase; UTF-8 bytes are untouched so offsets still line up
/// with the original string.
std::string asciiLower(std::string s)
{
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string asciiUpper(std::string s)
{
    for (auto& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

/// Find the next "<name" tag opener in [from, to) of a lowercased document,
/// requiring a tag boundary so "<th" does not match "<thead".
size_t findTagOpen(const std::string& lower, const std::string& name, size_t from, size_t to)
{
    const std::string needle = "<" + name;
    for (size_t pos = lower.find(needle, from); pos != std::string::npos && pos < to;
         pos = lower.find(needle, pos + 1)) {
        const size_t after = pos + needle.size();
        if (after >= lower.size()) return std::string::npos;
        const char c = lower[after];
        if (c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c)))
            return pos;
    }
    return std::string::npos;
}

/// Plain text of one table cell: paragraphs joined by a space, whitespace
/// runs (including non-breaking spaces) collapsed.
std::string tableCellText(const std::string& cellHtml)
{
    std::string text = decodeHtmlEntities(stripHtmlTags(cellHtml));
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const bool nbsp = (c == 0xC2 && i + 1 < text.size() &&
                           static_cast<unsigned char>(text[i + 1]) == 0xA0);
        if (std::isspace(c) || nbsp) {
            if (nbsp) ++i;
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out += ' ';
            pendingSpace = false;
        }
        out += static_cast<char>(c);
    }
    return out;
}

/// Cell texts of every row in the table spanning [begin, end) of `html`.
std::vector<std::vector<std::string>> extractTableRows(const std::string& html,
                                                       const std::string& lower,
                                                       size_t begin, size_t end)
{
    std::vector<std::vector<std::string>> rows;
    size_t pos = begin;
    while (true) {
        const size_t tr = findTagOpen(lower, "tr", pos, end);
        if (tr == std::string::npos) break;
        size_t trEnd = lower.find("</tr", tr);
        if (trEnd == std::string::npos || trEnd > end) trEnd = end;

        std::vector<std::string> cells;
        size_t cellPos = tr + 3;
        while (true) {
            size_t td = findTagOpen(lower, "td", cellPos, trEnd);
            const size_t th = findTagOpen(lower, "th", cellPos, trEnd);
            if (td == std::string::npos || (th != std::string::npos && th < td)) td = th;
            if (td == std::string::npos) break;
            const size_t open = lower.find('>', td);
            if (open == std::string::npos || open >= trEnd) break;
            size_t close = lower.find(lower[td + 2] == 'd' ? "</td" : "</th", open);
            if (close == std::string::npos || close > trEnd) close = trEnd;
            cells.push_back(tableCellText(html.substr(open + 1, close - open - 1)));
            cellPos = close + 1;
        }
        rows.push_back(std::move(cells));
        pos = trEnd + 1;
    }
    return rows;
}

/// Canonical shot label: WIDE / CLOSE-UP for the usual spellings, otherwise
/// the doc's own label uppercased.
std::string normalizeShotType(const std::string& raw)
{
    const std::string upper = asciiUpper(trim(raw));
    std::string key;
    for (char c : upper) {
        if (c != ' ' && c != '-' && c != '_' && c != '.') key += c;
    }
    if (key == "WIDE" || key == "WS" || key == "WIDESHOT") return "WIDE";
    if (key == "CLOSEUP" || key == "CU" || key == "CLOSE") return "CLOSE-UP";
    return upper;
}

} // namespace

Script Script::fromHtml(const std::string& html)
{
    Script shotScript = fromShotTable(html);
    if (!shotScript.isEmpty())
        return shotScript;

    // Simple HTML parser: strip tags, then parse as text.
    // Google Docs bold detection is complex — for now we strip and use text parser.
    // A more sophisticated parser could detect <b>/<strong>/CSS font-weight:700.
    return fromText(decodeHtmlEntities(stripHtmlTags(html)));
}

Script Script::fromShotTable(const std::string& html)
{
    Script script;
    const std::string lower = asciiLower(html);

    for (size_t tablePos = findTagOpen(lower, "table", 0, lower.size());
         tablePos != std::string::npos && script.lines.empty();
         tablePos = findTagOpen(lower, "table", tablePos + 1, lower.size())) {
        size_t tableEnd = lower.find("</table", tablePos);
        if (tableEnd == std::string::npos) tableEnd = lower.size();
        const auto rows = extractTableRows(html, lower, tablePos, tableEnd);
        if (rows.empty()) continue;

        int shotCol = -1, actionCol = -1, scriptCol = -1;
        for (size_t c = 0; c < rows.front().size(); ++c) {
            const std::string header = asciiUpper(rows.front()[c]);
            if (header == "SHOT")                                shotCol   = static_cast<int>(c);
            else if (header == "ACTION")                         actionCol = static_cast<int>(c);
            else if (header == "SCRIPT" || header == "DIALOGUE") scriptCol = static_cast<int>(c);
        }
        if (shotCol < 0 || scriptCol < 0) continue;

        auto cell = [](const std::vector<std::string>& row, int col) {
            return (col >= 0 && static_cast<size_t>(col) < row.size())
                ? row[static_cast<size_t>(col)] : std::string{};
        };

        std::string currentSegment = "INTRO";
        std::unordered_set<std::string> seenSegments;
        for (size_t r = 1; r < rows.size(); ++r) {
            const std::string shot   = cell(rows[r], shotCol);
            const std::string action = cell(rows[r], actionCol);
            const std::string text   = cell(rows[r], scriptCol);

            // "- | STORY | -" starts a new segment, as does an all-caps
            // ACTION-only row ("  | MINI-GAME |  ").
            const bool capsActionOnly = shot.empty() && text.empty() && !action.empty() &&
                std::none_of(action.begin(), action.end(),
                             [](char c) { return c >= 'a' && c <= 'z'; });
            if (shot == "-" || text == "-" || capsActionOnly) {
                if (!action.empty() && action != "-")
                    currentSegment = action;
                continue;
            }
            if (text.empty()) continue;

            ScriptLine sl;
            sl.character  = kShotScriptCharacter;
            sl.dialogue   = text;
            sl.lineNumber = static_cast<int>(r);
            sl.segment    = currentSegment;
            sl.action     = action;
            if (!shot.empty())
                sl.shot = normalizeShotType(shot);  // blank stays blank: user must pick
            if (seenSegments.insert(sl.segment).second)
                script.segments.push_back(sl.segment);
            script.lines.push_back(std::move(sl));
        }
    }

    if (!script.lines.empty()) {
        script.shotTable = true;
        finalizeScript(script);
        spdlog::debug("ScriptMatcher: Parsed {} shot lines, {} segments from shot table",
                      script.lines.size(), script.segments.size());
    }
    return script;
}

Script Script::fromJson(const std::string& jsonStr)
{
    // Simple JSON parser for our script format.
    // Expected: {"lines": [{"character": "X", "dialogue": "Y", ...}], "segments": [...]}
    // For robustness we use a simple state-machine approach.
    Script script;

    // Find "lines" array and parse entries
    // This is intentionally simplified — production code would use nlohmann/json
    auto extractString = [](const std::string& s, const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = s.find(search);
        if (pos == std::string::npos) return "";
        pos = s.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        pos = s.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = s.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return s.substr(pos + 1, end - pos - 1);
    };

    // Find each {...} object within the "lines" array
    auto linesStart = jsonStr.find("\"lines\"");
    if (linesStart == std::string::npos) {
        spdlog::warn("ScriptMatcher: No 'lines' key found in JSON");
        return script;
    }

    auto arrStart = jsonStr.find('[', linesStart);
    if (arrStart == std::string::npos) return script;

    // Find matching ]
    int depth = 0;
    size_t arrEnd = arrStart;
    for (size_t i = arrStart; i < jsonStr.size(); ++i) {
        if (jsonStr[i] == '[') ++depth;
        if (jsonStr[i] == ']') { --depth; if (depth == 0) { arrEnd = i; break; } }
    }

    // Parse each object
    int lineNum = 0;
    size_t pos = arrStart;
    std::unordered_set<std::string> seenSegments;
    while (pos < arrEnd) {
        auto objStart = jsonStr.find('{', pos);
        if (objStart == std::string::npos || objStart > arrEnd) break;
        auto objEnd = jsonStr.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string obj = jsonStr.substr(objStart, objEnd - objStart + 1);
        ++lineNum;

        std::string character = titleCase(trim(extractString(obj, "character")));
        std::string dialogue  = trim(extractString(obj, "dialogue"));
        std::string segment   = extractString(obj, "segment");
        if (segment.empty()) segment = "UNTITLED";

        if (!character.empty() && !dialogue.empty()) {
            ScriptLine sl;
            sl.character  = character;
            sl.dialogue   = dialogue;
            sl.lineNumber = lineNum;
            sl.segment    = segment;

            // Optional timecode
            std::string tcStr = extractString(obj, "timecode");
            if (!tcStr.empty()) {
                try { sl.timecode = std::stod(tcStr); } catch (...) {}
            }

            script.lines.push_back(std::move(sl));

            if (seenSegments.insert(segment).second) {
                script.segments.push_back(segment);
            }
        }

        pos = objEnd + 1;
    }

    finalizeScript(script);
    spdlog::debug("ScriptMatcher: Parsed {} lines from JSON", script.lines.size());
    return script;
}

Script Script::load(const std::string& pathOrContent)
{
    // Auto-detect format
    std::string content = pathOrContent;

    // If it's a file path, read it
    if (content.size() < 500 && (content.find('\n') == std::string::npos)) {
        // Might be a path
        std::ifstream f(content);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            content = ss.str();

            // JSON detection
            if (pathOrContent.size() > 5 &&
                pathOrContent.substr(pathOrContent.size() - 5) == ".json")
            {
                return fromJson(content);
            }
        }
    }

    // Content-based detection
    std::string trimmed = trim(content);
    if (trimmed.size() > 0 && (trimmed[0] == '{' || trimmed[0] == '[')) {
        return fromJson(content);
    }
    if (content.find("<html") != std::string::npos ||
        content.find("<body") != std::string::npos ||
        content.find("<HTML") != std::string::npos)
    {
        return fromHtml(content);
    }
    return fromText(content);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — text utilities
// ═══════════════════════════════════════════════════════════════════════════

std::string ScriptMatcher::normalize(const std::string& text)
{
    std::string result;
    result.reserve(text.size());

    bool lastWasSpace = true;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || std::isspace(uc)) {
            if (std::isspace(uc)) {
                if (!lastWasSpace) {
                    result += ' ';
                    lastWasSpace = true;
                }
            } else {
                result += static_cast<char>(std::tolower(uc));
                lastWasSpace = false;
            }
        }
        // Punctuation is skipped
    }

    // Trim trailing space
    while (!result.empty() && result.back() == ' ')
        result.pop_back();

    return result;
}

float ScriptMatcher::sequenceRatio(const std::string& a, const std::string& b)
{
    // Ratcliff/Obershelp algorithm — equivalent to Python's
    // difflib.SequenceMatcher.ratio():  2.0 * M / T
    // where M = total matched characters from longest common contiguous
    // substrings found recursively, T = total length of both strings.
    if (a.empty() && b.empty()) return 1.0f;
    if (a.empty() || b.empty()) return 0.0f;

    // Recursive helper: find longest common contiguous substring,
    // then recurse on the left and right remainders.
    struct Helper {
        static size_t matchingChars(const std::string& s1, size_t lo1, size_t hi1,
                                    const std::string& s2, size_t lo2, size_t hi2)
        {
            if (lo1 >= hi1 || lo2 >= hi2) return 0;

            // Find longest common contiguous substring in s1[lo1..hi1) and s2[lo2..hi2)
            size_t bestLen = 0, bestI = lo1, bestJ = lo2;
            for (size_t i = lo1; i < hi1; ++i) {
                for (size_t j = lo2; j < hi2; ++j) {
                    size_t k = 0;
                    while (i + k < hi1 && j + k < hi2 && s1[i + k] == s2[j + k])
                        ++k;
                    if (k > bestLen) {
                        bestLen = k;
                        bestI = i;
                        bestJ = j;
                    }
                }
            }
            if (bestLen == 0) return 0;

            // Recurse on left and right of the match
            size_t leftMatch  = matchingChars(s1, lo1, bestI, s2, lo2, bestJ);
            size_t rightMatch = matchingChars(s1, bestI + bestLen, hi1,
                                              s2, bestJ + bestLen, hi2);
            return leftMatch + bestLen + rightMatch;
        }
    };

    size_t matched = Helper::matchingChars(a, 0, a.size(), b, 0, b.size());
    return static_cast<float>(2.0 * matched) / static_cast<float>(a.size() + b.size());
}

std::string ScriptMatcher::soundex(const std::string& word)
{
    if (word.empty()) return "0000";

    std::string result;
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));

    // Soundex coding table
    auto code = [](char c) -> char {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == 'b' || c == 'f' || c == 'p' || c == 'v') return '1';
        if (c == 'c' || c == 'g' || c == 'j' || c == 'k' || c == 'q' ||
            c == 's' || c == 'x' || c == 'z') return '2';
        if (c == 'd' || c == 't') return '3';
        if (c == 'l') return '4';
        if (c == 'm' || c == 'n') return '5';
        if (c == 'r') return '6';
        return '0'; // a, e, i, o, u, h, w, y
    };

    char lastCode = code(word[0]);
    for (size_t i = 1; i < word.size() && result.size() < 4; ++i) {
        char c = code(word[i]);
        if (c != '0' && c != lastCode) {
            result += c;
        }
        lastCode = c;
    }

    while (result.size() < 4) result += '0';
    return result;
}

std::string ScriptMatcher::metaphone(const std::string& word)
{
    // Simplified Double Metaphone — primary code only.
    if (word.empty()) return "";

    std::string w;
    for (char c : word)
        w += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    std::string result;
    size_t len = w.size();

    // Skip initial silent consonants
    if (len >= 2) {
        std::string first2 = w.substr(0, 2);
        if (first2 == "GN" || first2 == "KN" || first2 == "PN" ||
            first2 == "AE" || first2 == "WR")
        {
            w = w.substr(1);
            len = w.size();
        }
    }

    for (size_t i = 0; i < len && result.size() < 6; ++i) {
        char c = w[i];
        // Skip duplicates (except CC)
        if (i > 0 && c == w[i - 1] && c != 'C') continue;

        switch (c) {
        case 'A': case 'E': case 'I': case 'O': case 'U':
            if (i == 0) result += c;
            break;
        case 'B':
            if (!(i == len - 1 && i > 0 && w[i - 1] == 'M'))
                result += 'B';
            break;
        case 'C':
            if (i + 1 < len && (w[i + 1] == 'E' || w[i + 1] == 'I' || w[i + 1] == 'Y'))
                result += 'S';
            else
                result += 'K';
            break;
        case 'D':
            if (i + 1 < len && w[i + 1] == 'G')
                result += 'J';
            else
                result += 'T';
            break;
        case 'F': result += 'F'; break;
        case 'G':
            if (i + 1 < len && (w[i + 1] == 'H' || w[i + 1] == 'N'))
                ; // silent
            else
                result += 'K';
            break;
        case 'H':
            if (i + 1 < len && std::string("AEIOU").find(w[i + 1]) != std::string::npos)
                result += 'H';
            break;
        case 'J': result += 'J'; break;
        case 'K':
            if (i == 0 || (i > 0 && w[i - 1] != 'C'))
                result += 'K';
            break;
        case 'L': result += 'L'; break;
        case 'M': result += 'M'; break;
        case 'N': result += 'N'; break;
        case 'P':
            if (i + 1 < len && w[i + 1] == 'H')
                result += 'F';
            else
                result += 'P';
            break;
        case 'Q': result += 'K'; break;
        case 'R': result += 'R'; break;
        case 'S':
            if (i + 1 < len && w[i + 1] == 'H')
                result += 'X';
            else
                result += 'S';
            break;
        case 'T':
            if (i + 1 < len && w[i + 1] == 'H')
                result += '0'; // theta
            else
                result += 'T';
            break;
        case 'V': result += 'F'; break;
        case 'W': case 'Y':
            if (i + 1 < len && std::string("AEIOU").find(w[i + 1]) != std::string::npos)
                result += c;
            break;
        case 'X': result += "KS"; break;
        case 'Z': result += 'S'; break;
        default: break;
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ScriptMatcher — matching
// ═══════════════════════════════════════════════════════════════════════════

ScriptMatcher::ScriptMatcher(const Script& script)
    : m_script(script)
{
}

std::pair<const ScriptLine*, float>
ScriptMatcher::matchSegment(const std::string& segmentText) const
{
    const ScriptLine* bestMatch = nullptr;
    float bestScore = 0.0f;

    std::string segClean = normalize(segmentText);

    for (const auto& line : m_script.lines) {
        std::string dialogueClean = normalize(line.dialogue);
        float score = sequenceRatio(segClean, dialogueClean);

        if (score > bestScore) {
            bestScore = score;
            bestMatch = &line;
        }
    }

    if (bestScore >= m_threshold)
        return {bestMatch, bestScore};
    return {nullptr, 0.0f};
}

std::vector<MatchResult>
ScriptMatcher::matchAllSegments(const std::vector<SegmentInput>& segments,
                                bool allowRetakes) const
{
    std::vector<MatchResult> results;
    results.reserve(segments.size());

    std::unordered_set<int> usedLines; // indices into m_script.lines
    int lastMatchedIdx = -1;

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& seg = segments[i];
        std::string segClean = normalize(seg.text);

        float bestScore = 0.0f;
        int bestLineIdx = -1;

        for (size_t j = 0; j < m_script.lines.size(); ++j) {
            // Skip used lines unless retakes allowed
            if (!allowRetakes && usedLines.count(static_cast<int>(j)))
                continue;

            std::string dialogueClean = normalize(m_script.lines[j].dialogue);
            float score = sequenceRatio(segClean, dialogueClean);

            // Sequential bias
            if (lastMatchedIdx != -1) {
                if (static_cast<int>(j) == lastMatchedIdx + 1)
                    score += 0.2f; // Strong bias for next line
                else if (static_cast<int>(j) > lastMatchedIdx)
                    score += 0.05f; // Slight bias for future lines
            }

            // Timecode bias
            if (m_script.lines[j].timecode.has_value()) {
                double timeDiff = std::abs(m_script.lines[j].timecode.value() - seg.start);
                if (timeDiff < 5.0) {
                    score += static_cast<float>(0.1 * (1.0 - timeDiff / 5.0));
                }
            }

            if (score > bestScore) {
                bestScore = score;
                bestLineIdx = static_cast<int>(j);
            }
        }

        float effectiveThreshold = allowRetakes ? 0.35f : m_threshold;

        MatchResult mr;
        mr.segmentIndex = static_cast<int>(i);

        if (bestScore >= effectiveThreshold && bestLineIdx >= 0) {
            const auto& line = m_script.lines[static_cast<size_t>(bestLineIdx)];
            mr.character        = line.character;
            mr.confidence       = bestScore;
            mr.scriptLineNumber = line.lineNumber;
            mr.scriptSegment    = line.segment;
            mr.state            = MatchState::Tentative;

            usedLines.insert(bestLineIdx);
            lastMatchedIdx = bestLineIdx;

            spdlog::trace("ScriptMatcher: Segment {} -> {} [{}] (Idx {}, {:.0f}%)",
                          i, line.character, line.segment, bestLineIdx, bestScore * 100);
        } else {
            mr.confidence = 0.0f;
            mr.state      = MatchState::Unmatched;
            spdlog::trace("ScriptMatcher: Segment {} -> No match", i);
        }

        results.push_back(std::move(mr));
    }

    return results;
}

const std::vector<std::string>& ScriptMatcher::characters() const
{
    return m_script.characters;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Take finding (shot-table scripts)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<LineTake> findLineTakes(const std::vector<TimedWord>& words,
                                    const std::vector<ScriptLine>& lines,
                                    float minCoverage)
{
    auto tokenize = [](const std::string& text) {
        std::vector<std::string> out;
        std::istringstream ss(ScriptMatcher::normalize(text));
        std::string token;
        while (ss >> token) out.push_back(std::move(token));
        return out;
    };

    // Recording token stream; each token remembers its source word.
    std::vector<std::string> W;
    std::vector<int> wordOf;
    for (size_t k = 0; k < words.size(); ++k) {
        for (auto& token : tokenize(words[k].text)) {
            W.push_back(std::move(token));
            wordOf.push_back(static_cast<int>(k));
        }
    }
    const int n = static_cast<int>(W.size());
    if (n == 0) return {};

    // Per line: fitting alignment of the WHOLE line against ANY stretch of the
    // recording (free start/end in W). Every local maximum of the final
    // column is one read of the line. Match +2, mismatch -1, gap -1.
    std::vector<LineTake> candidates;
    std::vector<int> score;
    std::vector<uint8_t> dir;   // 0 diag, 1 up (extra recording word), 2 left (missing script word)
    for (const auto& line : lines) {
        const auto S = tokenize(line.dialogue);
        const int m = static_cast<int>(S.size());
        if (m == 0) continue;
        const size_t cols = static_cast<size_t>(m) + 1;
        score.assign(static_cast<size_t>(n + 1) * cols, 0);
        dir.assign(score.size(), 0);
        auto at = [cols](int i, int j) { return static_cast<size_t>(i) * cols + static_cast<size_t>(j); };
        for (int j = 1; j <= m; ++j) { score[at(0, j)] = -j; dir[at(0, j)] = 2; }
        for (int i = 1; i <= n; ++i) {
            dir[at(i, 0)] = 1;   // score 0: a take may start anywhere
            for (int j = 1; j <= m; ++j) {
                int best = score[at(i - 1, j - 1)] + (W[i - 1] == S[j - 1] ? 2 : -1);
                uint8_t d = 0;
                if (score[at(i - 1, j)] - 1 > best) { best = score[at(i - 1, j)] - 1; d = 1; }
                if (score[at(i, j - 1)] - 1 > best) { best = score[at(i, j - 1)] - 1; d = 2; }
                score[at(i, j)] = best;
                dir[at(i, j)] = d;
            }
        }

        const int minHits = std::min(m, 2);
        for (int i = 1; i <= n; ++i) {
            const int s = score[at(i, m)];
            if (s <= 0) continue;
            if (i > 1 && score[at(i - 1, m)] >= s) continue;   // not a local max
            if (i < n && score[at(i + 1, m)] > s) continue;

            int hits = 0, first = -1, last = -1;
            for (int ti = i, tj = m; ti > 0 && tj > 0; ) {
                const uint8_t d = dir[at(ti, tj)];
                if (d == 0) {
                    if (W[ti - 1] == S[tj - 1]) {
                        ++hits;
                        first = ti - 1;
                        if (last < 0) last = ti - 1;
                    }
                    --ti; --tj;
                } else if (d == 1) {
                    --ti;
                } else {
                    --tj;
                }
            }
            const float coverage = static_cast<float>(hits) / static_cast<float>(m);
            if (hits < minHits || coverage < minCoverage || first < 0) continue;

            LineTake take;
            take.lineNumber = line.lineNumber;
            take.firstWord  = wordOf[static_cast<size_t>(first)];
            take.lastWord   = wordOf[static_cast<size_t>(last)];
            take.hits       = hits;
            take.coverage   = coverage;
            take.start      = words[static_cast<size_t>(take.firstWord)].start;
            take.end        = words[static_cast<size_t>(take.lastWord)].end;
            candidates.push_back(take);
        }
    }

    // Strongest reads win: a fuller read beats a flubbed partial of the same
    // line, and a long line's read beats a short line that merely occurs
    // inside it.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const LineTake& a, const LineTake& b) {
                         if (a.hits != b.hits) return a.hits > b.hits;
                         return a.coverage > b.coverage;
                     });
    std::vector<LineTake> accepted;
    for (const auto& c : candidates) {
        bool keep = true;
        for (const auto& a : accepted) {
            const int overlap = std::min(c.lastWord, a.lastWord)
                              - std::max(c.firstWord, a.firstWord) + 1;
            if (overlap <= 0) continue;
            const int shorter = std::min(c.lastWord - c.firstWord, a.lastWord - a.firstWord) + 1;
            if (c.lineNumber == a.lineNumber || overlap * 2 > shorter) { keep = false; break; }
        }
        if (keep) accepted.push_back(c);
    }
    std::sort(accepted.begin(), accepted.end(),
              [](const LineTake& a, const LineTake& b) { return a.start < b.start; });
    return accepted;
}

} // namespace rt
