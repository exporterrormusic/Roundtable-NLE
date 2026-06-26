/*
 * ShotPreset.cpp — shot composition data model + persistence.
 */

#include "spine/ShotPreset.h"

#include "PathUtils.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

// ─── Minimal JSON helpers ───────────────────────────────────────────────────
// We use a lightweight approach: manual JSON writing + minimal parsing.
// This avoids pulling in a JSON library dependency.  For reading, we use
// a simple key-value scanner that handles the flat / nested structure
// our presets need.

namespace {

// Map an "Ark Ranger" character name to the canonical spine name where the
// colour comes FIRST ("Black Ark Ranger", "Red Ark Ranger", "Blue Ark Ranger").
// Scripts sometimes write the colour on the other side ("Ark Ranger Black"),
// which otherwise fails to resolve to any shot. Detection is case-insensitive;
// any name containing "ark ranger" plus one of the three colours (in any
// position) is rewritten to the canonical form. Other names pass through
// unchanged.
std::string normalizeArkRangerName(const std::string& name)
{
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        return s;
    };
    const std::string low = lower(name);
    if (low.find("ark ranger") == std::string::npos)
        return name;

    static const std::pair<const char*, const char*> kColors[] = {
        {"black", "Black Ark Ranger"},
        {"red",   "Red Ark Ranger"},
        {"blue",  "Blue Ark Ranger"},
    };
    for (const auto& [needle, canonical] : kColors) {
        if (low.find(needle) != std::string::npos)
            return canonical;
    }
    return name;
}

// Escape a string for JSON output
std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

// ─── Tiny JSON tokenizer / parser (read-only) ──────────────────────────────
// Supports: objects, arrays, strings, numbers, booleans, null

enum class JTok { LBrace, RBrace, LBracket, RBracket, Colon, Comma,
                  String, Number, True, False, Null, End, Error };

struct JLexer {
    const char* p;
    const char* end;
    std::string sval;
    double      nval = 0;

    explicit JLexer(const std::string& src) : p(src.data()), end(p + src.size()) {}

    void skipWS() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    JTok next() {
        skipWS();
        if (p >= end) return JTok::End;
        char c = *p++;
        switch (c) {
        case '{': return JTok::LBrace;
        case '}': return JTok::RBrace;
        case '[': return JTok::LBracket;
        case ']': return JTok::RBracket;
        case ':': return JTok::Colon;
        case ',': return JTok::Comma;
        case '"': {
            sval.clear();
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    switch (*p) {
                    case '"':  sval += '"';  break;
                    case '\\': sval += '\\'; break;
                    case 'n':  sval += '\n'; break;
                    case 'r':  sval += '\r'; break;
                    case 't':  sval += '\t'; break;
                    default:   sval += *p;   break;
                    }
                } else {
                    sval += *p;
                }
                ++p;
            }
            if (p < end) ++p; // skip closing quote
            return JTok::String;
        }
        case 't':
            if (p + 2 < end && p[0] == 'r' && p[1] == 'u' && p[2] == 'e') { p += 3; return JTok::True; }
            return JTok::Error;
        case 'f':
            if (p + 3 < end && p[0] == 'a' && p[1] == 'l' && p[2] == 's' && p[3] == 'e') { p += 4; return JTok::False; }
            return JTok::Error;
        case 'n':
            if (p + 2 < end && p[0] == 'u' && p[1] == 'l' && p[2] == 'l') { p += 3; return JTok::Null; }
            return JTok::Error;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                const char* start = p - 1;
                while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
                    ++p;
                nval = std::strtod(start, nullptr);
                return JTok::Number;
            }
            return JTok::Error;
        }
    }

    // Peek at next token type without consuming
    JTok peek() {
        const char* saved = p;
        auto t = next();
        p = saved;
        // Restore sval/nval is not perfect for peek, but works for structure tokens
        return t;
    }
};

// Simple skip: skip one value (object, array, string, number, bool, null)
void skipValue(JLexer& lex) {
    auto t = lex.next();
    if (t == JTok::LBrace) {
        int depth = 1;
        while (depth > 0) {
            auto t2 = lex.next();
            if (t2 == JTok::LBrace) ++depth;
            else if (t2 == JTok::RBrace) --depth;
            else if (t2 == JTok::End || t2 == JTok::Error) break;
        }
    } else if (t == JTok::LBracket) {
        int depth = 1;
        while (depth > 0) {
            auto t2 = lex.next();
            if (t2 == JTok::LBracket) ++depth;
            else if (t2 == JTok::RBracket) --depth;
            else if (t2 == JTok::End || t2 == JTok::Error) break;
        }
    }
    // else: already consumed the single token
}

} // anon

namespace rt {

// ─── ShotPreset ──────────────────────────────────────────────────────────────

ShotPreset::ShotPreset(const std::string& name)
    : m_name(name)
{
}

// ── Shows ─────────────────────────────────────────────────────────────────

bool ShotPreset::hasShow(const std::string& show) const
{
    if (m_show.size() != show.size()) return false;
    for (size_t i = 0; i < m_show.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(m_show[i])) !=
            std::tolower(static_cast<unsigned char>(show[i])))
            return false;
    }
    return true;
}

// ── Backgrounds ─────────────────────────────────────────────────────────────

int ShotPreset::addBackground(const BackgroundState& bg)
{
    int idx = static_cast<int>(m_backgrounds.size());
    m_backgrounds.push_back(bg);
    m_layerOrder.push_back({LayerType::Background, idx});
    return idx;
}

bool ShotPreset::removeBackground(int index)
{
    if (index < 0 || index >= static_cast<int>(m_backgrounds.size()))
        return false;

    m_backgrounds.erase(m_backgrounds.begin() + index);

    // Remove from layer order and fix indices
    m_layerOrder.erase(
        std::remove_if(m_layerOrder.begin(), m_layerOrder.end(),
            [index](const LayerRef& r) {
                return r.type == LayerType::Background && r.index == index;
            }),
        m_layerOrder.end());

    for (auto& lr : m_layerOrder) {
        if (lr.type == LayerType::Background && lr.index > index)
            --lr.index;
    }
    return true;
}

BackgroundState* ShotPreset::background(int index)
{
    if (index < 0 || index >= static_cast<int>(m_backgrounds.size()))
        return nullptr;
    return &m_backgrounds[static_cast<size_t>(index)];
}

const BackgroundState* ShotPreset::background(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_backgrounds.size()))
        return nullptr;
    return &m_backgrounds[static_cast<size_t>(index)];
}

// ── Characters ──────────────────────────────────────────────────────────────

int ShotPreset::addCharacter(const CharacterState& ch)
{
    int idx = static_cast<int>(m_characters.size());
    m_characters.push_back(ch);
    m_layerOrder.push_back({LayerType::Character, idx});
    return idx;
}

bool ShotPreset::removeCharacter(int index)
{
    if (index < 0 || index >= static_cast<int>(m_characters.size()))
        return false;

    m_characters.erase(m_characters.begin() + index);

    // Remove from layer order and fix indices
    m_layerOrder.erase(
        std::remove_if(m_layerOrder.begin(), m_layerOrder.end(),
            [index](const LayerRef& r) {
                return r.type == LayerType::Character && r.index == index;
            }),
        m_layerOrder.end());

    for (auto& lr : m_layerOrder) {
        if (lr.type == LayerType::Character && lr.index > index)
            --lr.index;
    }
    return true;
}

CharacterState* ShotPreset::character(int index)
{
    if (index < 0 || index >= static_cast<int>(m_characters.size()))
        return nullptr;
    return &m_characters[static_cast<size_t>(index)];
}

const CharacterState* ShotPreset::character(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_characters.size()))
        return nullptr;
    return &m_characters[static_cast<size_t>(index)];
}

// ── Layer ordering ──────────────────────────────────────────────────────────

bool ShotPreset::swapLayers(int indexA, int indexB)
{
    if (indexA < 0 || indexA >= layerCount() ||
        indexB < 0 || indexB >= layerCount() ||
        indexA == indexB)
        return false;

    std::swap(m_layerOrder[static_cast<size_t>(indexA)],
              m_layerOrder[static_cast<size_t>(indexB)]);
    return true;
}

bool ShotPreset::moveLayerUp(int index)
{
    if (index <= 0 || index >= layerCount())
        return false;
    return swapLayers(index, index - 1);
}

bool ShotPreset::moveLayerDown(int index)
{
    if (index < 0 || index >= layerCount() - 1)
        return false;
    return swapLayers(index, index + 1);
}

bool ShotPreset::moveLayerToFront(int index)
{
    if (index <= 0 || index >= layerCount())
        return false;
    auto ref = m_layerOrder[static_cast<size_t>(index)];
    m_layerOrder.erase(m_layerOrder.begin() + index);
    m_layerOrder.insert(m_layerOrder.begin(), ref);
    return true;
}

bool ShotPreset::moveLayerToBack(int index)
{
    if (index < 0 || index >= layerCount() - 1)
        return false;
    auto ref = m_layerOrder[static_cast<size_t>(index)];
    m_layerOrder.erase(m_layerOrder.begin() + index);
    m_layerOrder.push_back(ref);
    return true;
}

bool ShotPreset::moveLayerTo(int from, int to)
{
    if (from < 0 || from >= layerCount()) return false;
    to = std::clamp(to, 0, layerCount() - 1);
    if (from == to) return true;
    auto ref = m_layerOrder[static_cast<size_t>(from)];
    m_layerOrder.erase(m_layerOrder.begin() + from);
    m_layerOrder.insert(m_layerOrder.begin() + to, ref);
    return true;
}

int ShotPreset::findLayerIndex(LayerRef ref) const
{
    for (int i = 0; i < layerCount(); ++i) {
        if (m_layerOrder[static_cast<size_t>(i)] == ref)
            return i;
    }
    return -1;
}

void ShotPreset::ensureLayerOrder()
{
    // Add any missing layers (backgrounds first, then characters)
    for (int i = 0; i < backgroundCount(); ++i) {
        LayerRef ref{LayerType::Background, i};
        if (findLayerIndex(ref) < 0)
            m_layerOrder.push_back(ref);
    }
    for (int i = 0; i < characterCount(); ++i) {
        LayerRef ref{LayerType::Character, i};
        if (findLayerIndex(ref) < 0)
            m_layerOrder.push_back(ref);
    }

    // Remove stale references
    m_layerOrder.erase(
        std::remove_if(m_layerOrder.begin(), m_layerOrder.end(),
            [this](const LayerRef& r) {
                if (r.type == LayerType::Background)
                    return r.index < 0 || r.index >= backgroundCount();
                else
                    return r.index < 0 || r.index >= characterCount();
            }),
        m_layerOrder.end());
}

// ── Serialization ───────────────────────────────────────────────────────────

static const char* stanceToString(CharacterStance s)
{
    switch (s) {
    case CharacterStance::Aim:   return "aim";
    case CharacterStance::Cover: return "cover";
    default:                     return "default";
    }
}

static CharacterStance stanceFromString(const std::string& s)
{
    if (s == "aim")   return CharacterStance::Aim;
    if (s == "cover") return CharacterStance::Cover;
    return CharacterStance::Default;
}

std::string ShotPreset::toJson() const
{
    std::ostringstream o;
    o << std::fixed;
    o << "{\n";
    o << "  \"name\": \"" << jsonEscape(m_name) << "\",\n";

    // Show (namespace)
    o << "  \"show\": \"" << jsonEscape(m_show) << "\",\n";

    // Camera
    o << "  \"cameraZoom\": " << m_cameraZoom << ",\n";
    o << "  \"cameraX\": " << m_cameraX << ",\n";
    o << "  \"cameraY\": " << m_cameraY << ",\n";

    // Backgrounds
    o << "  \"backgrounds\": [\n";
    for (size_t i = 0; i < m_backgrounds.size(); ++i) {
        const auto& bg = m_backgrounds[i];
        o << "    {\n";
        o << "      \"path\": \"" << jsonEscape(bg.path) << "\",\n";
        o << "      \"posX\": " << bg.posX << ",\n";
        o << "      \"posY\": " << bg.posY << ",\n";
        o << "      \"scale\": " << bg.scale << ",\n";
        o << "      \"opacity\": " << bg.opacity << ",\n";
        o << "      \"nativeWidth\": " << bg.nativeWidth << ",\n";
        o << "      \"nativeHeight\": " << bg.nativeHeight << ",\n";
        o << "      \"visible\": " << (bg.visible ? "true" : "false") << ",\n";
        o << "      \"layerType\": \"" << jsonEscape(bg.layerType) << "\",\n";
        o << "      \"inPoint\": " << bg.inPoint << ",\n";
        o << "      \"outPoint\": " << bg.outPoint << ",\n";
        o << "      \"cropLeft\": " << bg.cropLeft << ",\n";
        o << "      \"cropRight\": " << bg.cropRight << ",\n";
        o << "      \"cropTop\": " << bg.cropTop << ",\n";
        o << "      \"cropBottom\": " << bg.cropBottom << ",\n";
        o << "      \"blur\": " << bg.blur << "\n";
        o << "    }" << (i + 1 < m_backgrounds.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    // Characters
    o << "  \"characters\": [\n";
    for (size_t i = 0; i < m_characters.size(); ++i) {
        const auto& ch = m_characters[i];
        o << "    {\n";
        o << "      \"characterName\": \"" << jsonEscape(ch.characterName) << "\",\n";
        o << "      \"outfit\": \"" << jsonEscape(ch.outfit) << "\",\n";
        o << "      \"stance\": \"" << stanceToString(ch.stance) << "\",\n";
        o << "      \"animation\": \"" << jsonEscape(ch.animation) << "\",\n";
        o << "      \"isTalking\": " << (ch.isTalking ? "true" : "false") << ",\n";
        if (!ch.videoMutePath.empty())
            o << "      \"videoMutePath\": \"" << jsonEscape(ch.videoMutePath) << "\",\n";
        if (!ch.videoTalkPath.empty())
            o << "      \"videoTalkPath\": \"" << jsonEscape(ch.videoTalkPath) << "\",\n";
        if (!ch.puppetFolder.empty()) {
            o << "      \"puppetFolder\": \"" << jsonEscape(ch.puppetFolder) << "\",\n";
            o << "      \"puppetVariant\": \"" << jsonEscape(ch.puppetVariant) << "\",\n";
        }
        o << "      \"posX\": " << ch.posX << ",\n";
        o << "      \"posY\": " << ch.posY << ",\n";
        o << "      \"scale\": " << ch.scale << ",\n";
        o << "      \"rotation\": " << ch.rotation << ",\n";
        o << "      \"flipX\": " << (ch.flipX ? "true" : "false") << ",\n";
        o << "      \"flipY\": " << (ch.flipY ? "true" : "false") << ",\n";
        o << "      \"opacity\": " << ch.opacity << ",\n";
        o << "      \"cropLeft\": " << ch.cropLeft << ",\n";
        o << "      \"cropRight\": " << ch.cropRight << ",\n";
        o << "      \"cropTop\": " << ch.cropTop << ",\n";
        o << "      \"cropBottom\": " << ch.cropBottom << ",\n";
        o << "      \"blur\": " << ch.blur << ",\n";
        o << "      \"visible\": " << (ch.visible ? "true" : "false") << "\n";
        o << "    }" << (i + 1 < m_characters.size() ? "," : "") << "\n";
    }
    o << "  ],\n";

    // Layer order
    o << "  \"layerOrder\": [\n";
    for (size_t i = 0; i < m_layerOrder.size(); ++i) {
        const auto& lr = m_layerOrder[i];
        o << "    {\"type\": \"" << (lr.type == LayerType::Background ? "bg" : "ch")
          << "\", \"index\": " << lr.index << "}"
          << (i + 1 < m_layerOrder.size() ? "," : "") << "\n";
    }
    o << "  ]\n";
    o << "}\n";

    return o.str();
}

std::optional<ShotPreset> ShotPreset::fromJson(const std::string& json)
{
    JLexer lex(json);

    if (lex.next() != JTok::LBrace)
        return std::nullopt;

    ShotPreset preset;

    while (true) {
        auto t = lex.next();
        if (t == JTok::RBrace) break;
        if (t == JTok::Comma) continue;
        if (t != JTok::String) return std::nullopt;

        std::string key = lex.sval;

        if (lex.next() != JTok::Colon) return std::nullopt;

        if (key == "name") {
            if (lex.next() != JTok::String) return std::nullopt;
            preset.m_name = lex.sval;
        }
        else if (key == "show") {
            if (lex.next() != JTok::String) return std::nullopt;
            preset.m_show = lex.sval;
        }
        else if (key == "shows") {
            // Legacy multi-show tag array → migrate to the single show
            // namespace (take the first tag).
            if (lex.next() != JTok::LBracket) return std::nullopt;
            bool took = false;
            while (true) {
                auto st = lex.next();
                if (st == JTok::RBracket || st == JTok::End) break;
                if (st == JTok::Comma) continue;
                if (st == JTok::String && !took && !lex.sval.empty()) {
                    if (preset.m_show.empty()) preset.m_show = lex.sval;
                    took = true;
                }
            }
        }
        else if (key == "cameraZoom") {
            if (lex.next() != JTok::Number) return std::nullopt;
            preset.m_cameraZoom = static_cast<float>(lex.nval);
        }
        else if (key == "cameraX") {
            if (lex.next() != JTok::Number) return std::nullopt;
            preset.m_cameraX = static_cast<float>(lex.nval);
        }
        else if (key == "cameraY") {
            if (lex.next() != JTok::Number) return std::nullopt;
            preset.m_cameraY = static_cast<float>(lex.nval);
        }
        else if (key == "backgrounds") {
            if (lex.next() != JTok::LBracket) return std::nullopt;
            while (true) {
                auto bt = lex.next();
                if (bt == JTok::RBracket) break;
                if (bt == JTok::Comma) continue;
                if (bt != JTok::LBrace) return std::nullopt;

                BackgroundState bg;
                while (true) {
                    auto ft = lex.next();
                    if (ft == JTok::RBrace) break;
                    if (ft == JTok::Comma) continue;
                    if (ft != JTok::String) return std::nullopt;
                    std::string fkey = lex.sval;
                    if (lex.next() != JTok::Colon) return std::nullopt;

                    if (fkey == "path")          { lex.next(); bg.path = lex.sval; }
                    else if (fkey == "posX")     { lex.next(); bg.posX = static_cast<float>(lex.nval); }
                    else if (fkey == "posY")     { lex.next(); bg.posY = static_cast<float>(lex.nval); }
                    else if (fkey == "scale")    { lex.next(); bg.scale = static_cast<float>(lex.nval); }
                    else if (fkey == "opacity")  { lex.next(); bg.opacity = static_cast<float>(lex.nval); }
                    else if (fkey == "nativeWidth")  { lex.next(); bg.nativeWidth = static_cast<int>(lex.nval); }
                    else if (fkey == "nativeHeight") { lex.next(); bg.nativeHeight = static_cast<int>(lex.nval); }
                    else if (fkey == "visible")  { auto vt = lex.next(); bg.visible = (vt == JTok::True); }
                    else if (fkey == "layerType") { lex.next(); bg.layerType = lex.sval; }
                    else if (fkey == "inPoint")   { lex.next(); bg.inPoint = static_cast<float>(lex.nval); }
                    else if (fkey == "outPoint")  { lex.next(); bg.outPoint = static_cast<float>(lex.nval); }
                    else if (fkey == "cropLeft")   { lex.next(); bg.cropLeft = static_cast<float>(lex.nval); }
                    else if (fkey == "cropRight")  { lex.next(); bg.cropRight = static_cast<float>(lex.nval); }
                    else if (fkey == "cropTop")    { lex.next(); bg.cropTop = static_cast<float>(lex.nval); }
                    else if (fkey == "cropBottom") { lex.next(); bg.cropBottom = static_cast<float>(lex.nval); }
                    else if (fkey == "blur")       { lex.next(); bg.blur = static_cast<float>(lex.nval); }
                    else skipValue(lex);
                }
                preset.m_backgrounds.push_back(bg);
            }
        }
        else if (key == "characters") {
            if (lex.next() != JTok::LBracket) return std::nullopt;
            while (true) {
                auto ct = lex.next();
                if (ct == JTok::RBracket) break;
                if (ct == JTok::Comma) continue;
                if (ct != JTok::LBrace) return std::nullopt;

                CharacterState ch;
                while (true) {
                    auto ft = lex.next();
                    if (ft == JTok::RBrace) break;
                    if (ft == JTok::Comma) continue;
                    if (ft != JTok::String) return std::nullopt;
                    std::string fkey = lex.sval;
                    if (lex.next() != JTok::Colon) return std::nullopt;

                    if (fkey == "characterName")     { lex.next(); ch.characterName = lex.sval; }
                    else if (fkey == "outfit")        { lex.next(); ch.outfit = lex.sval; }
                    else if (fkey == "stance")        { lex.next(); ch.stance = stanceFromString(lex.sval); }
                    else if (fkey == "animation")     { lex.next(); ch.animation = lex.sval; }
                    else if (fkey == "isTalking")     { auto vt = lex.next(); ch.isTalking = (vt == JTok::True); }
                    else if (fkey == "videoMutePath") { lex.next(); ch.videoMutePath = lex.sval; }
                    else if (fkey == "videoTalkPath") { lex.next(); ch.videoTalkPath = lex.sval; }
                    else if (fkey == "puppetFolder")  { lex.next(); ch.puppetFolder = lex.sval; }
                    else if (fkey == "puppetVariant") { lex.next(); ch.puppetVariant = lex.sval; }
                    else if (fkey == "posX")          { lex.next(); ch.posX = static_cast<float>(lex.nval); }
                    else if (fkey == "posY")          { lex.next(); ch.posY = static_cast<float>(lex.nval); }
                    else if (fkey == "scale")         { lex.next(); ch.scale = static_cast<float>(lex.nval); }
                    else if (fkey == "rotation")      { lex.next(); ch.rotation = static_cast<float>(lex.nval); }
                    else if (fkey == "flipX")         { auto vt = lex.next(); ch.flipX = (vt == JTok::True); }
                    else if (fkey == "flipY")         { auto vt = lex.next(); ch.flipY = (vt == JTok::True); }
                    else if (fkey == "opacity")       { lex.next(); ch.opacity = static_cast<float>(lex.nval); }
                    else if (fkey == "cropLeft")      { lex.next(); ch.cropLeft = static_cast<float>(lex.nval); }
                    else if (fkey == "cropRight")     { lex.next(); ch.cropRight = static_cast<float>(lex.nval); }
                    else if (fkey == "cropTop")       { lex.next(); ch.cropTop = static_cast<float>(lex.nval); }
                    else if (fkey == "cropBottom")    { lex.next(); ch.cropBottom = static_cast<float>(lex.nval); }
                    else if (fkey == "blur")          { lex.next(); ch.blur = static_cast<float>(lex.nval); }
                    else if (fkey == "visible")       { auto vt = lex.next(); ch.visible = (vt == JTok::True); }
                    else skipValue(lex);
                }
                preset.m_characters.push_back(ch);
            }
        }
        else if (key == "layerOrder") {
            if (lex.next() != JTok::LBracket) return std::nullopt;
            while (true) {
                auto lt = lex.next();
                if (lt == JTok::RBracket) break;
                if (lt == JTok::Comma) continue;
                if (lt != JTok::LBrace) return std::nullopt;

                LayerRef lr;
                while (true) {
                    auto ft = lex.next();
                    if (ft == JTok::RBrace) break;
                    if (ft == JTok::Comma) continue;
                    if (ft != JTok::String) return std::nullopt;
                    std::string fkey = lex.sval;
                    if (lex.next() != JTok::Colon) return std::nullopt;

                    if (fkey == "type") {
                        lex.next();
                        lr.type = (lex.sval == "bg") ? LayerType::Background : LayerType::Character;
                    }
                    else if (fkey == "index") {
                        lex.next();
                        lr.index = static_cast<int>(lex.nval);
                    }
                    else skipValue(lex);
                }
                preset.m_layerOrder.push_back(lr);
            }
        }
        else {
            skipValue(lex);
        }
    }

    // Always sanitize layer order — remove stale refs and add missing ones
    preset.ensureLayerOrder();

    return preset;
}

ShotPreset ShotPreset::createDefault(const std::string& characterName)
{
    ShotPreset preset(characterName + " - Default");

    CharacterState ch;
    ch.characterName = characterName;
    ch.outfit   = "default";
    ch.posX     = 0.5f;
    ch.posY     = 0.75f;
    ch.scale    = 1.0f;
    ch.animation = "idle";
    ch.isTalking = false;

    preset.addCharacter(ch);
    return preset;
}

// ─── ShotPresetManager ───────────────────────────────────────────────────────

std::string ShotPresetManager::makeKey(const std::string& show,
                                        const std::string& name)
{
    return show.empty() ? name : (show + "/" + name);
}

void ShotPresetManager::splitKey(const std::string& key, std::string& show,
                                 std::string& name)
{
    auto pos = key.find('/');
    if (pos == std::string::npos) { show.clear(); name = key; }
    else { show = key.substr(0, pos); name = key.substr(pos + 1); }
}

int ShotPresetManager::scan(const std::filesystem::path& presetsDir)
{
    m_directory = presetsDir;
    m_presets.clear();
    m_aliases.clear();
    m_knownShows.clear();
    m_showThumbnails.clear();
    m_showDefaults.clear();

    if (!std::filesystem::exists(presetsDir)) {
        spdlog::info("ShotPresetManager: presets directory does not exist: {}",
                     pathToUtf8(presetsDir));
        return 0;
    }

    loadAliases();
    loadShows();
    loadShowThumbnails();
    loadShowDefaults();

    int count = 0;
    std::set<std::string> seenKeys;   // dedupe guard (exact keys)

    // ── Snapshot the directory listing BEFORE any migration writes ──────
    // Migration creates show subdirectories + files; if we iterated live we
    // would descend into a directory we just created and load each migrated
    // shot a second time (the "doubled shot count" bug). Collect first.
    std::vector<std::filesystem::path> rootFiles;                       // No-Show
    std::vector<std::pair<std::string, std::filesystem::path>> subFiles; // (show, file)
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(presetsDir, ec)) {
            if (entry.is_regular_file()) {
                auto ext = pathToUtf8(entry.path().extension());
                if (ext != ".json" && ext != ".JSON") continue;
                auto stem = pathToUtf8(entry.path().filename());
                if (!stem.empty() && stem.front() == '_') continue; // config files
                rootFiles.push_back(entry.path());
            } else if (entry.is_directory()) {
                const std::string dirName = pathToUtf8(entry.path().filename());
                if (dirName == "thumbnails" ||
                    (!dirName.empty() && dirName.front() == '_'))
                    continue;
                std::error_code ec2;
                for (const auto& sub :
                         std::filesystem::directory_iterator(entry.path(), ec2)) {
                    if (!sub.is_regular_file()) continue;
                    auto ext = pathToUtf8(sub.path().extension());
                    if (ext != ".json" && ext != ".JSON") continue;
                    subFiles.emplace_back(dirName, sub.path());
                }
            }
        }
    }

    auto parse = [](const std::filesystem::path& path) -> std::optional<ShotPreset> {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) return std::nullopt;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        auto preset = ShotPreset::fromJson(content);
        if (preset && preset->name().empty())
            preset->setName(pathToUtf8(path.stem()));
        return preset;
    };

    // ── 1) Show shots (subdirectories) — the location is authoritative ──
    for (const auto& [showDir, file] : subFiles) {
        auto preset = parse(file);
        if (!preset) {
            spdlog::warn("ShotPresetManager: failed to parse preset: {}", pathToUtf8(file));
            continue;
        }
        preset->setShow(showDir);
        const std::string key = makeKey(showDir, preset->name());
        if (!seenKeys.insert(key).second) continue; // already loaded
        m_presets.emplace_back(key, std::move(*preset));
        ++count;
    }

    // ── 2) Root files: No-Show shots, or legacy show-tagged → migrate ───
    for (const auto& file : rootFiles) {
        auto preset = parse(file);
        if (!preset) {
            spdlog::warn("ShotPresetManager: failed to parse preset: {}", pathToUtf8(file));
            continue;
        }
        const std::string show = preset->show();
        const std::string name = preset->name();
        const std::string key  = makeKey(show, name);

        if (!show.empty()) {
            // Legacy tagged shot. If the show namespace already has this shot
            // (from step 1), just delete the stale root copy; otherwise move it.
            std::error_code ec;
            if (seenKeys.count(key)) {
                std::filesystem::remove(file, ec);
                continue;
            }
            auto dst = pathForPreset(show, name);
            std::filesystem::create_directories(dst.parent_path(), ec);
            std::ofstream ofs(dst, std::ios::binary);
            if (ofs) {
                ofs << preset->toJson();
                ofs.close();
                std::filesystem::remove(file, ec);
                spdlog::info("ShotPresetManager: migrated '{}' into show '{}'", name, show);
            }
        } else {
            if (seenKeys.count(key)) continue; // duplicate No-Show name
        }
        seenKeys.insert(key);
        m_presets.emplace_back(key, std::move(*preset));
        ++count;
    }

    // ── Auto-register shows discovered on disk ──────────────────────────
    // A show folder (or a show-tagged shot) is authoritative even when it's
    // missing from _shows.json — otherwise the show is invisible in the UI
    // (e.g. a "ROUNDTABLE TALK" folder that isn't listed). Heal the registry
    // so every show with presets shows up, and persist once if anything was
    // added.
    {
        auto ciEqual = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return true;
        };
        bool showsChanged = false;
        auto ensureShow = [&](const std::string& s) {
            if (s.empty()) return;
            for (const auto& k : m_knownShows)
                if (ciEqual(k, s)) return;
            m_knownShows.push_back(s);
            showsChanged = true;
        };
        for (const auto& [showDir, file] : subFiles) ensureShow(showDir);
        for (const auto& [key, preset] : m_presets) ensureShow(preset.show());
        if (showsChanged)
            saveShows();
    }

    spdlog::info("ShotPresetManager: loaded {} presets from {}", count, pathToUtf8(presetsDir));
    return count;
}

bool ShotPresetManager::save(const ShotPreset& preset)
{
    if (preset.name().empty())
        return false;

    auto path = pathForPreset(preset.show(), preset.name());
    if (!m_directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;

    ofs << preset.toJson();

    const std::string key = makeKey(preset.show(), preset.name());
    for (auto& [n, p] : m_presets) {
        if (n == key) {
            p = preset;
            return true;
        }
    }
    m_presets.emplace_back(key, preset);
    return true;
}

std::optional<ShotPreset> ShotPresetManager::load(const std::string& show,
                                                  const std::string& name) const
{
    const std::string key = makeKey(show, name);
    for (const auto& [k, p] : m_presets)
        if (k == key) return p;
    return std::nullopt;
}

std::optional<ShotPreset> ShotPresetManager::load(const std::string& key) const
{
    // Exact key match first.
    for (const auto& [k, p] : m_presets)
        if (k == key) return p;

    // Back-compat: a bare name (no show) may reference a shot that now lives in
    // a show subdirectory (e.g. an old project's clip reference). Fall back to
    // the first preset whose bare name matches.
    if (key.find('/') == std::string::npos) {
        for (const auto& [k, p] : m_presets)
            if (p.name() == key) return p;
    }
    return std::nullopt;
}

bool ShotPresetManager::remove(const std::string& show, const std::string& name)
{
    return remove(makeKey(show, name));
}

bool ShotPresetManager::remove(const std::string& key)
{
    // Resolve the actual stored key (handle bare-name back-compat).
    std::string actualKey = key;
    if (!std::any_of(m_presets.begin(), m_presets.end(),
                     [&](const auto& pr) { return pr.first == key; }) &&
        key.find('/') == std::string::npos) {
        for (const auto& [k, p] : m_presets)
            if (p.name() == key) { actualKey = k; break; }
    }

    std::string show, name;
    splitKey(actualKey, show, name);

    {
        std::error_code ec;
        std::filesystem::remove(pathForPreset(show, name), ec);

        // Thumbnail keyed by the sanitized full key.
        auto thumbDir = m_directory / "thumbnails";
        std::string sanitized;
        sanitized.reserve(actualKey.size());
        for (char c : actualKey) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                sanitized += '_';
            else
                sanitized += c;
        }
        std::filesystem::remove(thumbDir / (sanitized + ".png"), ec);
    }

    auto it = std::find_if(m_presets.begin(), m_presets.end(),
                           [&](const auto& pair) { return pair.first == actualKey; });
    if (it == m_presets.end())
        return false;
    m_presets.erase(it);
    return true;
}

std::vector<std::string> ShotPresetManager::presetNames() const
{
    std::vector<std::string> keys;
    keys.reserve(m_presets.size());
    for (const auto& [k, _] : m_presets)
        keys.emplace_back(k);
    return keys;
}

std::vector<std::string> ShotPresetManager::namesForShow(const std::string& show) const
{
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    std::vector<std::string> names;
    for (const auto& [k, p] : m_presets) {
        std::string s, n;
        splitKey(k, s, n);
        if (ieq(s, show)) names.push_back(n);
    }
    return names;
}

bool ShotPresetManager::hasPreset(const std::string& show, const std::string& name) const
{
    const std::string key = makeKey(show, name);
    return std::any_of(m_presets.begin(), m_presets.end(),
                       [&](const auto& pair) { return pair.first == key; });
}

bool ShotPresetManager::hasPreset(const std::string& key) const
{
    if (std::any_of(m_presets.begin(), m_presets.end(),
                    [&](const auto& pair) { return pair.first == key; }))
        return true;
    if (key.find('/') == std::string::npos)
        return std::any_of(m_presets.begin(), m_presets.end(),
                           [&](const auto& pair) { return pair.second.name() == key; });
    return false;
}

std::optional<ShotPreset> ShotPresetManager::resolveDefaultShot(
    const std::string& characterName) const
{
    return resolveDefaultShot(characterName, std::string{});
}

std::optional<ShotPreset> ShotPresetManager::resolveDefaultShot(
    const std::string& characterName, const std::string& show) const
{
    if (m_directory.empty())
        return std::nullopt;

    // ── Step 0: Resolve any alias display name → real character name ────
    // realNameFor returns the input unchanged if no alias matches.
    std::string realName = realNameFor(characterName);

    // Normalize "Ark Ranger" colour variants ("Ark Ranger Black" → "Black Ark
    // Ranger") so the colour can sit on either side of the name in the script
    // and still resolve to the canonical spine character / shots.
    realName = normalizeArkRangerName(realName);

    // ── Step A: per-show default (checked first when a show is given) ────
    if (!show.empty()) {
        std::string lowerShow = show;
        std::transform(lowerShow.begin(), lowerShow.end(), lowerShow.begin(), ::tolower);
        auto sit = m_showDefaults.find(lowerShow);
        if (sit != m_showDefaults.end()) {
            auto cit = sit->second.find(realName);
            if (cit != sit->second.end()) {
                // The mapped shot lives in this show's namespace.
                auto preset = load(show, cit->second);
                if (!preset) preset = load(cit->second); // legacy/global fallback
                if (preset) return preset;
                // Mapped shot missing → fall through to global resolution.
            }
        }

        // Naming convention within the show: "<Character> (Default)".
        {
            auto preset = load(show, realName + " (Default)");
            if (preset) return preset;
        }
    }

    // ── Step 1: Check _defaults.json for an explicit mapping ────────────
    auto defaultsPath = m_directory / "_defaults.json";
    std::ifstream f(defaultsPath);
    if (f.is_open()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        JLexer lex(content);
        if (lex.next() == JTok::LBrace) {
            // Parse flat object: {"charName": "shotName", ...}
            while (true) {
                auto t = lex.next();
                if (t == JTok::RBrace) break;
                if (t == JTok::Comma) continue;
                if (t != JTok::String) break;

                std::string key = lex.sval;
                if (lex.next() != JTok::Colon) break;
                if (lex.next() != JTok::String) break;
                std::string val = lex.sval;

                if (key == realName) {
                    // Found an explicit mapping — look up the named preset
                    auto preset = load(val);
                    if (preset)
                        return preset;
                    // If the mapped shot doesn't exist, fall through
                    break;
                }
            }
        }
    }

    // ── Step 2: Fall back to naming convention ──────────────────────────
    std::string defaultName = realName + " (Default)";
    {
        auto preset = load(defaultName);
        if (preset) return preset;
    }

    // ── Step 3: Case-insensitive fallback ───────────────────────────────
    {
        std::string lowerChar = realName;
        std::transform(lowerChar.begin(), lowerChar.end(), lowerChar.begin(), ::tolower);
        std::string targetLower = lowerChar + " (default)";
        auto names = presetNames();
        for (const auto& pn : names) {
            std::string lowerPN = pn;
            std::transform(lowerPN.begin(), lowerPN.end(), lowerPN.begin(), ::tolower);
            if (lowerPN == targetLower)
                return load(pn);
        }
    }

    return std::nullopt;
}

void ShotPresetManager::setAlias(const std::string& realName,
                                  const std::string& displayName)
{
    if (realName.empty()) return;
    if (displayName.empty() || displayName == realName)
        m_aliases.erase(realName);
    else
        m_aliases[realName] = displayName;
    saveAliases();
}

std::string ShotPresetManager::displayNameFor(const std::string& realName) const
{
    auto it = m_aliases.find(realName);
    return (it == m_aliases.end()) ? realName : it->second;
}

std::string ShotPresetManager::realNameFor(const std::string& displayName) const
{
    auto lower = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    };
    const std::string needle = lower(displayName);
    for (const auto& [real, disp] : m_aliases) {
        if (lower(disp) == needle) return real;
    }
    return displayName;
}

void ShotPresetManager::loadAliases()
{
    if (m_directory.empty()) return;
    auto path = m_directory / "_aliases.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    JLexer lex(content);
    if (lex.next() != JTok::LBrace) return;
    while (true) {
        auto t = lex.next();
        if (t == JTok::RBrace || t == JTok::End) break;
        if (t == JTok::Comma) continue;
        if (t != JTok::String) break;
        std::string key = lex.sval;
        if (lex.next() != JTok::Colon) break;
        if (lex.next() != JTok::String) break;
        std::string val = lex.sval;
        if (!key.empty() && !val.empty() && key != val)
            m_aliases[key] = val;
    }
}

void ShotPresetManager::saveAliases() const
{
    if (m_directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    auto path = m_directory / "_aliases.json";

    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& [real, disp] : m_aliases) {
        if (!first) os << ',';
        first = false;
        os << '"' << jsonEscape(real) << "\":\"" << jsonEscape(disp) << '"';
    }
    os << '}';

    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto s = os.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

// ── Show registry ────────────────────────────────────────────────────────

namespace {
bool iEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
} // anon

void ShotPresetManager::addShow(const std::string& show)
{
    if (show.empty()) return;
    for (const auto& s : m_knownShows)
        if (iEquals(s, show)) return; // already registered
    m_knownShows.push_back(show);
    saveShows();
}

void ShotPresetManager::removeShow(const std::string& show)
{
    auto before = m_knownShows.size();
    m_knownShows.erase(
        std::remove_if(m_knownShows.begin(), m_knownShows.end(),
            [&](const std::string& s) { return iEquals(s, show); }),
        m_knownShows.end());
    if (m_knownShows.size() != before)
        saveShows();

    // Drop any thumbnail registered for this show.
    std::string lower = show;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (m_showThumbnails.erase(lower) > 0)
        saveShowThumbnails();
}

void ShotPresetManager::renameShow(const std::string& oldName,
                                    const std::string& newName)
{
    if (newName.empty()) return;
    bool changed = false;
    for (auto& s : m_knownShows) {
        if (iEquals(s, oldName)) { s = newName; changed = true; }
    }
    if (!changed) {
        m_knownShows.push_back(newName);
        changed = true;
    }
    if (changed) saveShows();

    // Carry the thumbnail over to the new name.
    std::string oldLower = oldName, newLower = newName;
    std::transform(oldLower.begin(), oldLower.end(), oldLower.begin(), ::tolower);
    std::transform(newLower.begin(), newLower.end(), newLower.begin(), ::tolower);
    auto it = m_showThumbnails.find(oldLower);
    if (it != m_showThumbnails.end() && oldLower != newLower) {
        m_showThumbnails[newLower] = it->second;
        m_showThumbnails.erase(it);
        saveShowThumbnails();
    }
}

void ShotPresetManager::setShowThumbnail(const std::string& show,
                                          const std::string& path)
{
    if (show.empty()) return;
    std::string lower = show;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (path.empty())
        m_showThumbnails.erase(lower);
    else
        m_showThumbnails[lower] = path;
    saveShowThumbnails();
}

std::string ShotPresetManager::showThumbnail(const std::string& show) const
{
    std::string lower = show;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = m_showThumbnails.find(lower);
    return (it == m_showThumbnails.end()) ? std::string{} : it->second;
}

void ShotPresetManager::loadShowThumbnails()
{
    if (m_directory.empty()) return;
    auto path = m_directory / "_show_thumbnails.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    JLexer lex(content);
    if (lex.next() != JTok::LBrace) return;
    while (true) {
        auto t = lex.next();
        if (t == JTok::RBrace || t == JTok::End) break;
        if (t == JTok::Comma) continue;
        if (t != JTok::String) break;
        std::string key = lex.sval;
        if (lex.next() != JTok::Colon) break;
        if (lex.next() != JTok::String) break;
        std::string val = lex.sval;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (!key.empty() && !val.empty())
            m_showThumbnails[key] = val;
    }
}

void ShotPresetManager::saveShowThumbnails() const
{
    if (m_directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    auto path = m_directory / "_show_thumbnails.json";

    std::ostringstream os;
    os << '{';
    bool first = true;
    for (const auto& [show, img] : m_showThumbnails) {
        if (!first) os << ',';
        first = false;
        os << '"' << jsonEscape(show) << "\":\"" << jsonEscape(img) << '"';
    }
    os << '}';

    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto s = os.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

void ShotPresetManager::loadShows()
{
    if (m_directory.empty()) return;
    auto path = m_directory / "_shows.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    JLexer lex(content);
    if (lex.next() != JTok::LBracket) return;
    while (true) {
        auto t = lex.next();
        if (t == JTok::RBracket || t == JTok::End) break;
        if (t == JTok::Comma) continue;
        if (t == JTok::String && !lex.sval.empty()) {
            bool dup = false;
            for (const auto& s : m_knownShows)
                if (iEquals(s, lex.sval)) { dup = true; break; }
            if (!dup) m_knownShows.push_back(lex.sval);
        }
    }
}

void ShotPresetManager::saveShows() const
{
    if (m_directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    auto path = m_directory / "_shows.json";

    std::ostringstream os;
    os << '[';
    for (size_t i = 0; i < m_knownShows.size(); ++i) {
        if (i) os << ',';
        os << '"' << jsonEscape(m_knownShows[i]) << '"';
    }
    os << ']';

    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto s = os.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

// ── Per-show default shots ────────────────────────────────────────────────

void ShotPresetManager::setShowDefaultShot(const std::string& show,
                                            const std::string& characterName,
                                            const std::string& shotName)
{
    if (show.empty() || characterName.empty()) return;
    std::string lowerShow = show;
    std::transform(lowerShow.begin(), lowerShow.end(), lowerShow.begin(), ::tolower);

    if (shotName.empty()) {
        auto sit = m_showDefaults.find(lowerShow);
        if (sit != m_showDefaults.end()) {
            sit->second.erase(characterName);
            if (sit->second.empty()) m_showDefaults.erase(sit);
        }
    } else {
        m_showDefaults[lowerShow][characterName] = shotName;
    }
    saveShowDefaults();
}

void ShotPresetManager::loadShowDefaults()
{
    if (m_directory.empty()) return;
    auto path = m_directory / "_show_defaults.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    // Format: { "<show>": { "<character>": "<shot>", ... }, ... }
    JLexer lex(content);
    if (lex.next() != JTok::LBrace) return;
    while (true) {
        auto t = lex.next();
        if (t == JTok::RBrace || t == JTok::End) break;
        if (t == JTok::Comma) continue;
        if (t != JTok::String) break;
        std::string show = lex.sval;
        std::transform(show.begin(), show.end(), show.begin(), ::tolower);
        if (lex.next() != JTok::Colon) break;
        if (lex.next() != JTok::LBrace) break;
        std::map<std::string, std::string> charMap;
        while (true) {
            auto it = lex.next();
            if (it == JTok::RBrace || it == JTok::End) break;
            if (it == JTok::Comma) continue;
            if (it != JTok::String) break;
            std::string ch = lex.sval;
            if (lex.next() != JTok::Colon) break;
            if (lex.next() != JTok::String) break;
            if (!ch.empty() && !lex.sval.empty())
                charMap[ch] = lex.sval;
        }
        if (!show.empty() && !charMap.empty())
            m_showDefaults[show] = std::move(charMap);
    }
}

void ShotPresetManager::saveShowDefaults() const
{
    if (m_directory.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    auto path = m_directory / "_show_defaults.json";

    std::ostringstream os;
    os << '{';
    bool firstShow = true;
    for (const auto& [show, charMap] : m_showDefaults) {
        if (charMap.empty()) continue;
        if (!firstShow) os << ',';
        firstShow = false;
        os << '"' << jsonEscape(show) << "\":{";
        bool firstCh = true;
        for (const auto& [ch, shot] : charMap) {
            if (!firstCh) os << ',';
            firstCh = false;
            os << '"' << jsonEscape(ch) << "\":\"" << jsonEscape(shot) << '"';
        }
        os << '}';
    }
    os << '}';

    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto s = os.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

std::filesystem::path ShotPresetManager::pathForPreset(const std::string& show,
                                                       const std::string& name) const
{
    auto sanitize = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                out += '_';
            else
                out += c;
        }
        return out;
    };
    // No-Show shots live in the root; show shots in a per-show subdirectory.
    std::filesystem::path base =
        show.empty() ? m_directory : (m_directory / sanitize(show));
    return base / (sanitize(name) + ".json");
}

} // namespace rt

