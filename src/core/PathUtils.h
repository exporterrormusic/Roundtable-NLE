/*
 * PathUtils.h — Safe filesystem path ↔ UTF-8 string conversion helpers.
 *
 * std::filesystem::path::string() uses the system ANSI codepage (CP_ACP)
 * on Windows and throws std::system_error when a character cannot be
 * represented.  These helpers use UTF-8 which is lossless and never throws.
 */

#pragma once

#include <filesystem>
#include <string>

namespace rt {

inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

inline std::filesystem::path utf8ToPath(const std::string& u8str) {
    std::u8string u8;
    u8.reserve(u8str.size());
    for (char c : u8str)
        u8.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(std::move(u8));
}

} // namespace rt
