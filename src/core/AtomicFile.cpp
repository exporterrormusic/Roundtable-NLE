/*
 * AtomicFile.cpp — see AtomicFile.h.
 */

#include "AtomicFile.h"
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace rt {

bool writeFileAtomically(const std::filesystem::path& path, std::string_view data)
{
    // Staged in the same directory, so the final rename never crosses volumes.
    std::filesystem::path staged = path;
    staged += ".tmp";
    std::error_code ignored;

    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            out.flush();
        }
        if (!out.is_open() || !out.good()) {
            out.close();
            std::filesystem::remove(staged, ignored);
            spdlog::warn("writeFileAtomically: could not write '{}'", pathToUtf8(staged));
            return false;
        }
    }

#ifdef _WIN32
    if (!::MoveFileExW(staged.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long error = ::GetLastError();
        std::filesystem::remove(staged, ignored);
        spdlog::warn("writeFileAtomically: could not replace '{}' (error {})",
                     pathToUtf8(path), error);
        return false;
    }
#else
    std::error_code renameError;
    std::filesystem::rename(staged, path, renameError);
    if (renameError) {
        std::filesystem::remove(staged, ignored);
        spdlog::warn("writeFileAtomically: could not replace '{}': {}",
                     pathToUtf8(path), renameError.message());
        return false;
    }
#endif
    return true;
}

} // namespace rt
