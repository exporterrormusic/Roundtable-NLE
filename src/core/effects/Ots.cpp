/*
 * Ots.cpp — "Over the Shoulder" broadcast-graphic effect.
 *
 * Defaults load/save from assets/presets/effects/OTS_LEFT.json or
 * OTS_RIGHT.json using a tiny manual JSON reader/writer (the core
 * library does not link against Qt).
 */

#include "effects/Ots.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

namespace {

std::filesystem::path defaultsPathFor(EffectType type)
{
    namespace fs = std::filesystem;
    const char* leaf = (type == EffectType::OtsLeft)  ? "OTS_LEFT.json"
                     : (type == EffectType::OtsIntro) ? "OTS_INTRO.json"
                                                      : "OTS_RIGHT.json";
    // Walk up from the current working dir until we find "assets/".
    fs::path d = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        if (fs::is_directory(d / "assets" / "presets" / "effects"))
            return d / "assets" / "presets" / "effects" / leaf;
        if (!d.has_parent_path()) break;
        d = d.parent_path();
    }
    // Fall back to user data directory (always writable)
#ifdef _WIN32
    {
        wchar_t appData[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            fs::path dataDir(appData);
            dataDir /= "ROUNDTABLE/presets/effects";
            return dataDir / leaf;
        }
    }
#endif
    return fs::path("assets/presets/effects") / leaf;
}

// Extract "key": <number> pairs from a trivial flat JSON object.
bool extractNumber(const std::string& src, const std::string& key, double& out)
{
    const std::string needle = "\"" + key + "\"";
    size_t p = src.find(needle);
    if (p == std::string::npos) return false;
    p = src.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
    size_t start = p;
    while (p < src.size() &&
           (std::isdigit(static_cast<unsigned char>(src[p])) ||
            src[p] == '-' || src[p] == '+' || src[p] == '.' ||
            src[p] == 'e' || src[p] == 'E'))
    {
        ++p;
    }
    if (p == start) return false;
    try {
        out = std::stod(src.substr(start, p - start));
        return true;
    } catch (...) {
        return false;
    }
}

} // anon

Ots::Ots(EffectType type)
    : Effect(type)
{
    const float side = (type == EffectType::OtsRight) ? 1.0f : 0.0f;

    addParam("Side",          side,  0.0f, 1.0f);
    addParam("PosX",          0.05f, 0.0f, 1.0f);
    addParam("PosY",          0.55f, 0.0f, 1.0f);
    addParam("Scale",         0.35f, 0.0f, 1.0f);
    addParam("StrokeWidth",   3.0f,  0.0f, 32.0f);
    addParam("StrokeR",       1.0f,  0.0f, 1.0f);
    addParam("StrokeG",       1.0f,  0.0f, 1.0f);
    addParam("StrokeB",       1.0f,  0.0f, 1.0f);
    addParam("ShadowOffsetX", 4.0f, -64.0f, 64.0f);
    addParam("ShadowOffsetY", 4.0f, -64.0f, 64.0f);
    addParam("ShadowBlur",   12.0f,  0.0f, 128.0f);
    addParam("ShadowOpacity", 0.6f,  0.0f, 1.0f);
    addParam("CornerRadius",  0.0f,  0.0f, 64.0f);
    addParam("AspectMode",    0.0f,  0.0f, 1.0f);
    addParam("CropFocusX",    0.5f,  0.0f, 1.0f);
    addParam("CropFocusY",    0.5f,  0.0f, 1.0f);

    loadDefaultsFromDisk();
}

std::unique_ptr<Effect> Ots::clone() const
{
    auto copy = std::make_unique<Ots>(effectType());
    copy->m_enabled = m_enabled;
    for (size_t i = 0; i < m_params.size() && i < copy->m_params.size(); ++i)
        copy->m_params[i].track = m_params[i].track;
    return copy;
}

void Ots::loadDefaultsFromDisk()
{
    namespace fs = std::filesystem;
    const fs::path path = defaultsPathFor(effectType());
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();

    for (auto& p : m_params) {
        double v = 0.0;
        if (extractNumber(src, p.name, v)) {
            p.track = KeyframeTrack<float>{};
            p.track.setDefaultValue(static_cast<float>(v));
        }
    }
}

bool Ots::saveAsDefault() const
{
    namespace fs = std::filesystem;
    const fs::path path = defaultsPathFor(effectType());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        spdlog::warn("Ots: cannot write defaults to {}", pathToUtf8(path));
        return false;
    }

    f << "{\n";
    for (size_t i = 0; i < m_params.size(); ++i) {
        const float v = m_params[i].track.evaluate(0);
        f << "  \"" << m_params[i].name << "\": " << v;
        if (i + 1 < m_params.size()) f << ",";
        f << "\n";
    }
    f << "}\n";
    return true;
}

} // namespace rt
