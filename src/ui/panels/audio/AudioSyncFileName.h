#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "PathUtils.h"

namespace rt {

inline std::string extractCharacterName(const std::string& filePath)
{
    std::filesystem::path path(filePath);
    std::string name = pathToUtf8(path.stem());
    if (name.empty()) return "Unknown";

    static const std::vector<std::string> prefixes = {
        "rvc", "voice", "vo_", "audio_", "rec_", "recording_"
    };

    std::string lower = name;
    for (auto& ch : lower)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (const auto& prefix : prefixes) {
        if (lower.substr(0, prefix.size()) == prefix) {
            name = name.substr(prefix.size());
            break;
        }
    }

    auto isSeparator = [](char ch) {
        return ch == ' ' || ch == '_' || ch == '-';
    };

    // Prefix removal can leave a delimiter (for example "voice ONLY ONE").
    while (!name.empty() && isSeparator(name.front()))
        name.erase(name.begin());

    static const std::vector<std::string> suffixes = {
        "fix", "final", "new", "old", "alt", "v2", "v3", "redo",
        "done", "fixed", "clean", "raw", "edit", "edited", "master",
        "draft", "wip", "temp", "test", "copy", "backup"
    };

    // Strip recording metadata from the RIGHT edge one token at a time.
    // The previous implementation inspected everything after the first
    // separator and treated any <=2-character token as metadata.  That made
    // "ONLY ONE FIX" collapse to "ONLY" because "ONE" followed the first
    // space.  Right-to-left stripping removes only "FIX" and preserves the
    // complete multi-word character name.
    while (!name.empty()) {
        while (!name.empty() && isSeparator(name.back()))
            name.pop_back();
        if (name.empty()) break;

        size_t tokenStart = name.size();
        while (tokenStart > 0 && !isSeparator(name[tokenStart - 1]))
            --tokenStart;

        std::string token = name.substr(tokenStart);
        for (auto& ch : token)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        const bool isNumber = !token.empty()
            && std::all_of(token.begin(), token.end(), [](unsigned char ch) {
                   return std::isdigit(ch) != 0;
               });
        const bool isSuffix =
            std::find(suffixes.begin(), suffixes.end(), token) != suffixes.end();
        const bool isTake = token.rfind("take", 0) == 0;
        if (!isNumber && !isSuffix && !isTake)
            break;

        name.erase(tokenStart);
    }

    while (!name.empty()) {
        char ch = name.back();
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == ' ')
            name.pop_back();
        else
            break;
    }

    if (name.size() >= 4) {
        std::string tail = name.substr(name.size() - 4);
        for (auto& ch : tail)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (tail == "take")
            name = name.substr(0, name.size() - 4);
    }

    while (!name.empty()) {
        char ch = name.back();
        if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == ' ')
            name.pop_back();
        else
            break;
    }

    if (name.size() < 2) {
        std::string stem = pathToUtf8(path.stem());
        for (size_t i = 0; i < stem.size(); ++i) {
            if (std::isupper(static_cast<unsigned char>(stem[i]))) {
                size_t j = i + 1;
                while (j < stem.size() && std::islower(static_cast<unsigned char>(stem[j])))
                    ++j;
                if (j - i >= 2) {
                    name = stem.substr(i, j - i);
                    break;
                }
            }
        }
    }

    if (name.empty()) name = pathToUtf8(path.stem());
    if (!name.empty()) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        for (size_t i = 1; i < name.size(); ++i)
            name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    return name;
}

} // namespace rt
