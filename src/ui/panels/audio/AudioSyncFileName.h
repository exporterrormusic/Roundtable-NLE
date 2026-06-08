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

    size_t separator = name.find_first_of(" _-");
    if (separator != std::string::npos && separator >= 2) {
        std::string rest = name.substr(separator + 1);
        std::string restLower = rest;
        for (auto& ch : restLower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        static const std::vector<std::string> suffixes = {
            "fix", "final", "new", "old", "alt", "v2", "v3", "redo",
            "done", "fixed", "clean", "raw", "edit", "edited", "master",
            "draft", "wip", "temp", "test", "copy", "backup"
        };

        bool allSuffix = true;
        size_t pos = 0;
        while (pos < restLower.size() && allSuffix) {
            size_t next = restLower.find_first_of(" _-", pos);
            if (next == std::string::npos) next = restLower.size();

            std::string word = restLower.substr(pos, next - pos);
            if (word.empty()) {
                pos = next + 1;
                continue;
            }

            bool isNum = true;
            for (char ch : word) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) {
                    isNum = false;
                    break;
                }
            }

            bool isSuffix = std::find(suffixes.begin(), suffixes.end(), word) != suffixes.end();
            bool isTake = word.substr(0, 4) == "take";
            bool isShort = word.size() <= 2;
            if (!isNum && !isSuffix && !isTake && !isShort)
                allSuffix = false;

            pos = next + 1;
        }

        if (allSuffix)
            name = name.substr(0, separator);
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