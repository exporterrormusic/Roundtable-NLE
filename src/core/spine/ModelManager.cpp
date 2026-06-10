/*
 * ModelManager.cpp — Directory scanner + character_metadata.json loader.
 */

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/ModelManager.h"

#include "PathUtils.h"
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

// ─── Minimal JSON parsing (for character_metadata.json) ─────────────────────
// We use a simple state-machine approach rather than pulling in nlohmann/json
// as a dependency.  character_metadata.json is a flat structure that's easy to
// parse with basic string ops.
//
// Actually, let's keep it really simple: we parse the JSON manually via
// very basic token extraction.  The format is well-defined and small.

namespace {

std::string toLower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

/// Extract a JSON string value after a key like "key": "value"
std::string extractString(const std::string& json, const std::string& key, size_t startPos = 0)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search, startPos);
    if (pos == std::string::npos) return {};

    // Skip past key and colon
    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return {};

    // Find opening quote
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};

    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};

    return json.substr(pos + 1, end - pos - 1);
}

/// Extract a boolean value after a key like "key": true
bool extractBool(const std::string& json, const std::string& key, size_t startPos = 0)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search, startPos);
    if (pos == std::string::npos) return false;

    pos = json.find(':', pos + search.length());
    if (pos == std::string::npos) return false;

    // Skip whitespace
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;

    return json.substr(pos, 4) == "true";
}

/// Sanitize a name for use as a Windows path component (same logic as CharacterBrowser)
std::string sanitizeForPath(const std::string& s)
{
    std::string r = s;
    for (auto& c : r) {
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            c = '_';
    }
    return r;
}

} // anonymous namespace

namespace rt {

// ─── Construction ───────────────────────────────────────────────────────────
ModelManager::ModelManager() = default;
ModelManager::~ModelManager() = default;

// ─── Metadata loading ───────────────────────────────────────────────────────
void ModelManager::loadMetadata(const std::string& metadataPath)
{
    std::ifstream file(metadataPath);
    if (!file.is_open()) {
        spdlog::debug("ModelManager: no metadata file at {}", metadataPath);
        return;
    }

    // Read entire file
    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

    // Parse each character block
    // Format: "c010": { "name": "Rapi", "category": "nikke", "has_mouth_animation": true, ... }
    size_t pos = 0;
    while (pos < json.size()) {
        // Find character ID key (e.g., "c010")
        auto keyStart = json.find("\"c", pos);
        if (keyStart == std::string::npos) break;

        auto keyEnd = json.find('"', keyStart + 1);
        if (keyEnd == std::string::npos) break;

        std::string charId = json.substr(keyStart + 1, keyEnd - keyStart - 1);

        // Validate it looks like a character ID (c followed by digits)
        if (charId.size() < 2 || charId[0] != 'c' ||
            !std::isdigit(static_cast<unsigned char>(charId[1]))) {
            pos = keyEnd + 1;
            continue;
        }

        // Find the opening brace of the character object
        auto braceStart = json.find('{', keyEnd);
        if (braceStart == std::string::npos) break;

        // Find matching closing brace (handles nested objects)
        int depth = 1;
        size_t braceEnd = braceStart + 1;
        while (braceEnd < json.size() && depth > 0) {
            if (json[braceEnd] == '{') depth++;
            else if (json[braceEnd] == '}') depth--;
            braceEnd++;
        }

        std::string block = json.substr(braceStart, braceEnd - braceStart);

        CharMetadata meta;
        meta.id = charId;
        meta.displayName = extractString(block, "name");
        meta.category = extractString(block, "category");
        meta.hasMouthAnimation = extractBool(block, "has_mouth_animation");

        // Parse outfits
        auto outfitsPos = block.find("\"outfits\"");
        if (outfitsPos != std::string::npos) {
            auto outfitBrace = block.find('{', outfitsPos + 9);
            if (outfitBrace != std::string::npos) {
                // Find each outfit key
                size_t oPos = outfitBrace + 1;
                while (oPos < block.size()) {
                    auto oKeyStart = block.find('"', oPos);
                    if (oKeyStart == std::string::npos || oKeyStart >= braceEnd) break;

                    auto oKeyEnd = block.find('"', oKeyStart + 1);
                    if (oKeyEnd == std::string::npos) break;

                    std::string outfitKey = block.substr(oKeyStart + 1, oKeyEnd - oKeyStart - 1);

                    // Skip if this is a known JSON key that isn't an outfit
                    if (outfitKey == "display_name" || outfitKey == "variants") {
                        oPos = oKeyEnd + 1;
                        continue;
                    }

                    // Try to find display_name within this outfit block
                    auto oBrace = block.find('{', oKeyEnd);
                    if (oBrace != std::string::npos) {
                        auto oBraceEnd = block.find('}', oBrace);
                        if (oBraceEnd != std::string::npos) {
                            std::string oBlock = block.substr(oBrace, oBraceEnd - oBrace + 1);
                            std::string displayName = extractString(oBlock, "display_name");
                            if (!displayName.empty()) {
                                meta.outfitDisplayNames[outfitKey] = displayName;
                            }
                            oPos = oBraceEnd + 1;
                        } else {
                            oPos = oKeyEnd + 1;
                        }
                    } else {
                        oPos = oKeyEnd + 1;
                    }
                }
            }
        }

        if (!meta.displayName.empty()) {
            m_metadata[toLower(meta.displayName)] = meta;

            // Also build sanitized folder name → display name mapping
            std::string sanitized = sanitizeForPath(meta.displayName);
            if (sanitized != meta.displayName)
                m_folderToDisplay[toLower(sanitized)] = meta.displayName;
        }
        m_metadataById[charId] = meta;

        pos = braceEnd;
    }

    spdlog::info("ModelManager: loaded metadata for {} characters ({} by id)",
                 m_metadata.size(), m_metadataById.size());
}

// ─── Stance scanning ────────────────────────────────────────────────────────
void ModelManager::scanStance(const std::string& dir, CharacterStance stance,
                               ModelOutfit& outfit)
{
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;

    ModelVariant variant;
    variant.stance = stance;

    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;

        auto ext = pathToUtf8(entry.path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".skel" && variant.skelPath.empty()) {
            variant.skelPath = pathToUtf8(entry.path());
        } else if (ext == ".atlas" && variant.atlasPath.empty()) {
            variant.atlasPath = pathToUtf8(entry.path());
        } else if (ext == ".png" && variant.texturePath.empty()) {
            variant.texturePath = pathToUtf8(entry.path());
        }
    }

    if (!variant.skelPath.empty() && !variant.atlasPath.empty()) {
        spdlog::info("ModelManager: scanStance({}) — OK skel='{}' atlas='{}'",
                     dir, variant.skelPath, variant.atlasPath);
        outfit.variants.push_back(std::move(variant));
    } else {
        spdlog::warn("ModelManager: scanStance({}) — skipped (skel={}, atlas={})",
                      dir, !variant.skelPath.empty(), !variant.atlasPath.empty());
    }
}

// ─── Outfit scanning ────────────────────────────────────────────────────────
void ModelManager::scanOutfitDirectory(const std::string& outfitDir,
                                        const std::string& outfitName,
                                        ModelEntry& entry)
{
    ModelOutfit outfit;
    outfit.name = outfitName;
    outfit.displayName = outfitName;

    // Check metadata for display name
    auto* metaPtr = findMetadata(entry.name);
    if (metaPtr) {
        auto dnIt = metaPtr->outfitDisplayNames.find(outfitName);
        if (dnIt != metaPtr->outfitDisplayNames.end()) {
            outfit.displayName = dnIt->second;
        }
    }

    // Scan default stance (files in the outfit directory itself)
    scanStance(outfitDir, CharacterStance::Default, outfit);

    // Scan aim stance
    auto aimDir = pathToUtf8(utf8ToPath(outfitDir) / "aim");
    scanStance(aimDir, CharacterStance::Aim, outfit);

    // Scan cover stance
    auto coverDir = pathToUtf8(utf8ToPath(outfitDir) / "cover");
    scanStance(coverDir, CharacterStance::Cover, outfit);

    if (!outfit.variants.empty()) {
        entry.outfits.push_back(std::move(outfit));
    }
}

// ─── Character scanning ────────────────────────────────────────────────────
void ModelManager::scanCharacterDirectory(const std::string& charDir,
                                           const std::string& charName)
{
    ModelEntry entry;
    entry.name = charName;
    entry.displayName = charName;  // default: same as folder name

    // Try to find metadata (handles disambiguated names like "E.H. (c940)")
    auto* meta = findMetadata(charName);
    if (meta) {
        entry.id = meta->id;
        entry.category = meta->category;
        entry.hasMouthAnim = meta->hasMouthAnimation;
        if (!meta->displayName.empty())
            entry.displayName = meta->displayName;
    }

    // Scan subdirectories as outfits
    for (auto& dirEntry : fs::directory_iterator(charDir)) {
        if (!dirEntry.is_directory()) continue;

        auto outfitName = pathToUtf8(dirEntry.path().filename());

        // Skip obvious non-outfit directories
        if (outfitName.empty() || outfitName[0] == '.') continue;

        scanOutfitDirectory(pathToUtf8(dirEntry.path()), outfitName, entry);
    }

    spdlog::debug("ModelManager: {} \u2014 {} outfits found", charName, entry.outfits.size());
    for (const auto& o : entry.outfits)
        spdlog::debug("  outfit '{}' \u2014 {} variants", o.name, o.variants.size());

    if (!entry.outfits.empty()) {
        m_entries.push_back(std::move(entry));
    }
}

// ─── Main scan ──────────────────────────────────────────────────────────────
int ModelManager::scan(const std::string& assetsDir)
{
    m_entries.clear();
    m_metadata.clear();
    m_metadataById.clear();
    m_folderToDisplay.clear();
    m_assetsDir = assetsDir;
    m_scanned = false;

    auto charsDir = utf8ToPath(assetsDir) / "characters";
    if (!fs::exists(charsDir) || !fs::is_directory(charsDir)) {
        spdlog::warn("ModelManager: characters directory not found: {}", pathToUtf8(charsDir));
        return 0;
    }

    // Load metadata if available
    auto metadataPath = utf8ToPath(assetsDir) / "character_metadata.json";
    if (fs::exists(metadataPath)) {
        loadMetadata(pathToUtf8(metadataPath));
    }

    // Scan each character directory
    for (auto& dirEntry : fs::directory_iterator(charsDir)) {
        if (!dirEntry.is_directory()) continue;

        auto charName = pathToUtf8(dirEntry.path().filename());
        if (charName.empty() || charName[0] == '.') continue;

        scanCharacterDirectory(pathToUtf8(dirEntry.path()), charName);
    }

    // Sort entries by name
    std::sort(m_entries.begin(), m_entries.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });

    m_scanned = true;

    spdlog::info("ModelManager: scanned {} characters from {}",
                 m_entries.size(), pathToUtf8(charsDir));

    return static_cast<int>(m_entries.size());
}

int ModelManager::scanAdditional(const std::string& assetsDir)
{
    auto charsDir = utf8ToPath(assetsDir) / "characters";
    if (!fs::exists(charsDir) || !fs::is_directory(charsDir)) {
        spdlog::debug("ModelManager: additional characters directory not found: {}", pathToUtf8(charsDir));
        return 0;
    }

    // Load metadata if available
    auto metadataPath = utf8ToPath(assetsDir) / "character_metadata.json";
    if (fs::exists(metadataPath)) {
        loadMetadata(pathToUtf8(metadataPath));
    }

    int before = static_cast<int>(m_entries.size());

    // Scan each character directory, skipping already-known names
    for (auto& dirEntry : fs::directory_iterator(charsDir)) {
        if (!dirEntry.is_directory()) continue;

        auto charName = pathToUtf8(dirEntry.path().filename());
        if (charName.empty() || charName[0] == '.') continue;

        // Skip if this character is already registered
        if (findByName(charName)) continue;

        scanCharacterDirectory(pathToUtf8(dirEntry.path()), charName);
    }

    // Re-sort to keep list consistent
    std::sort(m_entries.begin(), m_entries.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.name < b.name; });

    int added = static_cast<int>(m_entries.size()) - before;

    spdlog::info("ModelManager: scanned {} additional characters from {}",
                 added, pathToUtf8(charsDir));

    return added;
}

// ─── Lookup ─────────────────────────────────────────────────────────────────
const ModelEntry* ModelManager::findByName(const std::string& name) const
{
    auto lower = toLower(name);
    for (auto& e : m_entries) {
        if (toLower(e.name) == lower) return &e;
    }
    return nullptr;
}

const ModelEntry* ModelManager::findById(const std::string& id) const
{
    for (auto& e : m_entries) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<std::string> ModelManager::characterNames() const
{
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (auto& e : m_entries) {
        names.push_back(e.name);
    }
    return names;
}

std::vector<std::string> ModelManager::characterDisplayNames() const
{
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (auto& e : m_entries) {
        names.push_back(e.displayName);
    }
    return names;
}

std::string ModelManager::getDisplayName(const std::string& folderName) const
{
    auto lower = toLower(folderName);
    for (auto& e : m_entries) {
        if (toLower(e.name) == lower) return e.displayName;
    }
    return folderName;  // fallback: return as-is
}

std::string ModelManager::getFolderName(const std::string& displayName) const
{
    auto lower = toLower(displayName);
    for (auto& e : m_entries) {
        if (toLower(e.displayName) == lower) return e.name;
    }
    return displayName;  // fallback: return as-is
}

const ModelVariant* ModelManager::findVariant(
    const std::string& characterName,
    const std::string& outfit,
    CharacterStance stance) const
{
    auto* entry = findByName(characterName);
    if (!entry) return nullptr;

    for (auto& o : entry->outfits) {
        if (o.name == outfit) {
            for (auto& v : o.variants) {
                if (v.stance == stance) return &v;
            }
            break;
        }
    }

    return nullptr;
}

std::vector<ModelManager::MetadataOutfit> ModelManager::getMetadataOutfits(const std::string& charName) const
{
    std::vector<MetadataOutfit> result;

    auto* meta = findMetadata(charName);
    if (!meta) return result;

    // outfitDisplayNames maps outfit key → display name
    // Always include "default" first
    if (meta->outfitDisplayNames.count("default")) {
        result.push_back({"default", meta->outfitDisplayNames.at("default")});
    } else {
        result.push_back({"default", meta->displayName});
    }

    // Add remaining outfits sorted by key
    std::vector<std::pair<std::string, std::string>> sorted;
    for (const auto& [key, displayName] : meta->outfitDisplayNames) {
        if (key != "default") {
            sorted.push_back({key, displayName});
        }
    }
    std::sort(sorted.begin(), sorted.end());
    for (auto& [key, displayName] : sorted) {
        result.push_back({key, displayName});
    }

    return result;
}

std::string ModelManager::getCharacterId(const std::string& charName) const
{
    auto* meta = findMetadata(charName);
    if (!meta) return {};
    return meta->id;
}

const ModelManager::CharMetadata* ModelManager::findMetadata(const std::string& charName) const
{
    // Direct lookup by display name
    auto it = m_metadata.find(toLower(charName));
    if (it != m_metadata.end()) return &it->second;

    // Try sanitized folder name → display name mapping
    auto folderIt = m_folderToDisplay.find(toLower(charName));
    if (folderIt != m_folderToDisplay.end()) {
        auto metaIt = m_metadata.find(toLower(folderIt->second));
        if (metaIt != m_metadata.end()) return &metaIt->second;
    }

    // Handle disambiguated names like "E.H. (c940)" or "E.H (enterheaven)" — extract charId from suffix
    auto parenPos = charName.rfind(" (");
    if (parenPos != std::string::npos && !charName.empty() && charName.back() == ')') {
        std::string extractedId = charName.substr(parenPos + 2, charName.size() - parenPos - 3);
        auto idIt = m_metadataById.find(extractedId);
        if (idIt != m_metadataById.end()) return &idIt->second;
    }

    return nullptr;
}

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE

