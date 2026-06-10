/*
 * AssetDatabase.cpp — In-memory asset catalog implementation.
 * Step 5: Project Serialization
 *
 * This is the in-memory implementation. When SQLite is available as a
 * dependency, we'll add a persistent backing store behind the same API.
 */

#include "project/AssetDatabase.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include "PathUtils.h"

#include <spdlog/spdlog.h>

namespace rt {

AssetDatabase::AssetDatabase()  = default;
AssetDatabase::~AssetDatabase() = default;

// ── Scanning ────────────────────────────────────────────────────────────────

void AssetDatabase::scanDirectory(const std::filesystem::path& dir)
{
    if (!std::filesystem::exists(dir))
    {
        spdlog::warn("AssetDatabase: directory does not exist: {}", pathToUtf8(dir));
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;

        auto ext = pathToUtf8(entry.path().extension());
        // Lowercase extension for comparison
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });

        AssetType type;
        if (ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg" ||
            ext == ".aiff" || ext == ".aif" || ext == ".m4a" || ext == ".wma" ||
            ext == ".ac3" || ext == ".eac3" || ext == ".opus")
            type = AssetType::Audio;
        else if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".webm" ||
                 ext == ".mxf" || ext == ".ts" || ext == ".m2ts" || ext == ".vob" ||
                 ext == ".wmv" || ext == ".flv" || ext == ".m4v" || ext == ".mpg" ||
                 ext == ".mpeg" || ext == ".3gp" || ext == ".gif")
            type = AssetType::Video;
        else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".webp" ||
                 ext == ".tiff" || ext == ".tif" || ext == ".exr" || ext == ".dpx" ||
                 ext == ".tga" || ext == ".hdr" || ext == ".psd")
            type = AssetType::Image;
        else if (ext == ".ttf" || ext == ".otf")
            type = AssetType::Font;
        else
            continue; // Skip unknown file types

        AssetEntry asset;
        asset.type         = type;
        asset.name         = pathToUtf8(entry.path().stem());
        asset.path         = std::filesystem::relative(entry.path(), dir);
        asset.absolutePath = std::filesystem::absolute(entry.path());

        try {
            asset.fileSize = entry.file_size();
        } catch (...) {
            asset.fileSize = 0;
        }

        addAsset(std::move(asset));
    }

    spdlog::info("AssetDatabase: scanned {} — {} assets found", pathToUtf8(dir), m_assets.size());
}

void AssetDatabase::scanImageSequences(const std::filesystem::path& dir)
{
    if (!std::filesystem::exists(dir)) return;

    // Image extensions eligible for sequence detection
    static const std::set<std::string> seqExts = {
        ".png", ".jpg", ".jpeg", ".exr", ".dpx", ".tga", ".tiff", ".tif", ".bmp"
    };

    // Regex to match numbered filenames: prefix + digits + extension
    // e.g. "frame_0001.png" → prefix="frame_", digits="0001", ext=".png"
    static const std::regex numPattern(R"(^(.*?)(\d+)(\.[a-zA-Z]+)$)");

    // Group files by (directory, prefix, extension) → sorted frame numbers
    struct SeqKey {
        std::filesystem::path dir;
        std::string prefix;
        std::string ext;
        bool operator<(const SeqKey& o) const {
            if (dir != o.dir) return dir < o.dir;
            if (prefix != o.prefix) return prefix < o.prefix;
            return ext < o.ext;
        }
    };
    std::map<SeqKey, std::vector<int64_t>> sequences;

    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;

        auto ext = pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (seqExts.find(ext) == seqExts.end()) continue;

        std::string stem = pathToUtf8(entry.path().stem());
        std::smatch m;
        if (std::regex_match(stem, m, numPattern) && m.size() == 4) {
            SeqKey key{entry.path().parent_path(), m[1].str(), ext};
            int64_t num = std::stoll(m[2].str());
            sequences[key].push_back(num);
        }
    }

    // Create ImageSequence assets for groups with ≥2 consecutive frames
    for (auto& [key, frames] : sequences) {
        if (frames.size() < 2) continue;

        std::sort(frames.begin(), frames.end());

        // Determine digit padding from the first frame number
        std::string firstFile = key.prefix + std::to_string(frames.front()) + key.ext;
        // Scan for actual file to get exact digit width
        int digitWidth = 1;
        for (const auto& entry : std::filesystem::directory_iterator(key.dir)) {
            std::string fn = pathToUtf8(entry.path().stem());
            std::smatch m;
            if (std::regex_match(fn, m, numPattern) && m[1].str() == key.prefix) {
                digitWidth = static_cast<int>(m[2].str().length());
                break;
            }
        }

        // Build FFmpeg-style pattern: prefix%04d.ext
        std::string pattern = key.prefix + "%" +
            (digitWidth > 1 ? "0" + std::to_string(digitWidth) : "") + "d" + key.ext;

        AssetEntry asset;
        asset.type = AssetType::ImageSequence;
        asset.name = key.prefix.empty() ? "sequence" : key.prefix;
        // Trim trailing separator from name
        if (!asset.name.empty() && (asset.name.back() == '_' || asset.name.back() == '-' || asset.name.back() == '.'))
            asset.name.pop_back();
        asset.path = std::filesystem::relative(key.dir, dir) / pattern;
        asset.absolutePath = key.dir / pattern;
        asset.sequencePattern = pattern;
        asset.sequenceStart = frames.front();
        asset.sequenceCount = static_cast<int64_t>(frames.size());
        addAsset(std::move(asset));

        spdlog::info("AssetDatabase: image sequence detected — {} ({} frames, start={})",
                     pattern, frames.size(), frames.front());
    }
}

void AssetDatabase::scanCharacters(const std::filesystem::path& charDir)
{
    if (!std::filesystem::exists(charDir))
    {
        spdlog::warn("AssetDatabase: character directory does not exist: {}", pathToUtf8(charDir));
        return;
    }

    for (const auto& charEntry : std::filesystem::directory_iterator(charDir))
    {
        if (!charEntry.is_directory()) continue;

        CharacterAsset character;
        character.name     = pathToUtf8(charEntry.path().filename());
        character.basePath = std::filesystem::relative(charEntry.path(), charDir);

        // Scan outfits (subdirectories of the character folder)
        for (const auto& outfitEntry : std::filesystem::directory_iterator(charEntry.path()))
        {
            if (!outfitEntry.is_directory()) continue;
            character.outfits.push_back(pathToUtf8(outfitEntry.path().filename()));
        }

        // Default stances for all characters
        character.stances = {"idle", "aim", "cover"};

        m_characters.push_back(std::move(character));
    }

    spdlog::info("AssetDatabase: found {} characters in {}", m_characters.size(), pathToUtf8(charDir));
}

// ── Lookup ──────────────────────────────────────────────────────────────────

const AssetEntry* AssetDatabase::findById(uint64_t id) const
{
    auto it = m_idIndex.find(id);
    if (it == m_idIndex.end()) return nullptr;
    return &m_assets[it->second];
}

AssetEntry* AssetDatabase::findById(uint64_t id)
{
    auto it = m_idIndex.find(id);
    if (it == m_idIndex.end()) return nullptr;
    return &m_assets[it->second];
}

AssetEntry* AssetDatabase::findByPath(const std::filesystem::path& p)
{
    for (auto& a : m_assets) {
        if (a.path == p || a.absolutePath == p)
            return &a;
    }
    return nullptr;
}

std::vector<const AssetEntry*> AssetDatabase::findByType(AssetType type) const
{
    std::vector<const AssetEntry*> result;
    for (const auto& a : m_assets)
    {
        if (a.type == type)
            result.push_back(&a);
    }
    return result;
}

const CharacterAsset* AssetDatabase::findCharacter(const std::string& name) const
{
    for (const auto& c : m_characters)
    {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const std::vector<CharacterAsset>& AssetDatabase::characters() const noexcept
{
    return m_characters;
}

// ── Mutation ────────────────────────────────────────────────────────────────

uint64_t AssetDatabase::addAsset(AssetEntry entry)
{
    entry.id = m_nextId++;
    uint64_t id = entry.id;
    size_t idx = m_assets.size();
    m_assets.push_back(std::move(entry));
    m_idIndex[id] = idx;
    return id;
}

void AssetDatabase::removeAsset(uint64_t id)
{
    auto it = m_idIndex.find(id);
    if (it == m_idIndex.end()) return;

    size_t idx = it->second;

    // Swap-remove: move last element into the deleted slot
    if (idx < m_assets.size() - 1)
    {
        m_assets[idx] = std::move(m_assets.back());
        m_idIndex[m_assets[idx].id] = idx;
    }
    m_assets.pop_back();
    m_idIndex.erase(it);
}

size_t AssetDatabase::assetCount() const noexcept
{
    return m_assets.size();
}

std::vector<uint64_t> AssetDatabase::findOfflineAssets() const
{
    std::vector<uint64_t> offline;
    for (const auto& asset : m_assets) {
        if (asset.absolutePath.empty()) continue;
        if (asset.type == AssetType::Character) continue; // Folders checked separately
        if (!std::filesystem::exists(asset.absolutePath)) {
            offline.push_back(asset.id);
            spdlog::warn("AssetDatabase: offline media — id={} name='{}' path='{}'",
                         asset.id, asset.name, pathToUtf8(asset.absolutePath));
        }
    }
    return offline;
}

bool AssetDatabase::relinkAsset(uint64_t id, const std::filesystem::path& newPath)
{
    auto it = m_idIndex.find(id);
    if (it == m_idIndex.end()) return false;

    if (!std::filesystem::exists(newPath)) return false;

    auto& asset = m_assets[it->second];
    asset.absolutePath = std::filesystem::absolute(newPath);
    spdlog::info("AssetDatabase: relinked asset id={} name='{}' -> '{}'",
                 id, asset.name, pathToUtf8(asset.absolutePath));
    return true;
}

} // namespace rt

