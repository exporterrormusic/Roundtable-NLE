/*
 * Ots.cpp — "Over the Shoulder" broadcast-graphic effect.
 *
 * Defaults load/save from assets/presets/effects/OTS_LEFT.json or
 * OTS_RIGHT.json using a tiny manual JSON reader/writer (the core
 * library does not link against Qt).
 */

#include "effects/Ots.h"
#include "AppPaths.h"
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
    const fs::path presets = AppPaths::assets() / "presets" / "effects";
    if (fs::is_directory(presets))
        return presets / leaf;
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
    // Parameters and their built-in defaults: EffectRegistry.cpp. Each
    // side then takes its saved defaults, if any.
    loadDefaultsFromDisk();
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
