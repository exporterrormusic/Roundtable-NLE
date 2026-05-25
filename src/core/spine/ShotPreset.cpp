/*
 * ShotPreset.cpp — shot composition data model + persistence.
 */

#include "spine/ShotPreset.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>

// ─── Minimal JSON helpers ───────────────────────────────────────────────────
// We use a lightweight approach: manual JSON writing + minimal parsing.
// This avoids pulling in a JSON library dependency.  For reading, we use
// a simple key-value scanner that handles the flat / nested structure
// our presets need.

namespace {

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

int ShotPresetManager::scan(const std::filesystem::path& presetsDir)
{
    m_directory = presetsDir;
    m_presets.clear();
    m_aliases.clear();

    if (!std::filesystem::exists(presetsDir)) {
        spdlog::info("ShotPresetManager: presets directory does not exist: {}",
                     presetsDir.string());
        return 0;
    }

    loadAliases();

    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(presetsDir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        // Case-insensitive .json check
        if (ext != ".json" && ext != ".JSON") continue;

        std::ifstream ifs(entry.path(), std::ios::binary);
        if (!ifs) continue;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        auto preset = ShotPreset::fromJson(content);
        if (preset) {
            // Use filename stem as name if preset has no name
            if (preset->name().empty())
                preset->setName(entry.path().stem().string());
            m_presets.emplace_back(preset->name(), std::move(*preset));
            ++count;
        } else {
            spdlog::warn("ShotPresetManager: failed to parse preset: {}",
                         entry.path().string());
        }
    }

    spdlog::info("ShotPresetManager: loaded {} presets from {}", count, presetsDir.string());
    return count;
}

bool ShotPresetManager::save(const ShotPreset& preset)
{
    if (preset.name().empty())
        return false;

    // Ensure directory exists
    if (!m_directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
    }

    auto path = pathForPreset(preset.name());
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs)
        return false;

    ofs << preset.toJson();

    // Update in-memory cache
    for (auto& [n, p] : m_presets) {
        if (n == preset.name()) {
            p = preset;
            return true;
        }
    }
    m_presets.emplace_back(preset.name(), preset);
    return true;
}

std::optional<ShotPreset> ShotPresetManager::load(const std::string& name) const
{
    for (const auto& [n, p] : m_presets) {
        if (n == name) return p;
    }
    return std::nullopt;
}

bool ShotPresetManager::remove(const std::string& name)
{
    // Always delete files from disk first, regardless of cache state —
    // this covers orphaned thumbnails that may linger after earlier operations.
    {
        std::error_code ec;
        auto path = pathForPreset(name);
        std::filesystem::remove(path, ec);

        auto thumbDir = m_directory / "thumbnails";
        std::string sanitized;
        sanitized.reserve(name.size());
        for (char c : name) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                sanitized += '_';
            else
                sanitized += c;
        }
        std::filesystem::remove(thumbDir / (sanitized + ".png"), ec);
    }

    // Remove from in-memory cache if present
    auto it = std::find_if(m_presets.begin(), m_presets.end(),
                           [&](const auto& pair) { return pair.first == name; });
    if (it == m_presets.end())
        return false;

    m_presets.erase(it);
    return true;
}

std::vector<std::string> ShotPresetManager::presetNames() const
{
    std::vector<std::string> names;
    names.reserve(m_presets.size());
    for (const auto& [n, _] : m_presets)
        names.emplace_back(n);
    return names;
}

bool ShotPresetManager::hasPreset(const std::string& name) const
{
    return std::any_of(m_presets.begin(), m_presets.end(),
                       [&](const auto& pair) { return pair.first == name; });
}

std::optional<ShotPreset> ShotPresetManager::resolveDefaultShot(
    const std::string& characterName) const
{
    if (m_directory.empty())
        return std::nullopt;

    // ── Step 0: Resolve any alias display name → real character name ────
    // realNameFor returns the input unchanged if no alias matches.
    std::string realName = realNameFor(characterName);

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

std::filesystem::path ShotPresetManager::pathForPreset(const std::string& name) const
{
    // Sanitize the name for use as a filename
    std::string sanitized;
    sanitized.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            sanitized += '_';
        else
            sanitized += c;
    }
    return m_directory / (sanitized + ".json");
}

} // namespace rt

