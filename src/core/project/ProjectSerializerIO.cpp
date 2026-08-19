/*
 * ProjectSerializerIO.cpp — File I/O wrappers for ProjectSerializer.
 * Split from ProjectSerializer.cpp for maintainability.
 *
 * Contains: save(), load()
 * (readMetadata stays in ProjectSerializer.cpp because it uses the internal BinaryReader.)
 */

#include "project/ProjectSerializer.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include "PathUtils.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rt {

namespace {

std::filesystem::path withSuffix(std::filesystem::path path, const char* suffix)
{
    path += suffix;
    return path;
}

void removeTemporaryFile(const std::filesystem::path& path)
{
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
    if (cleanupError)
    {
        spdlog::warn("ProjectSerializer: failed to remove temporary file '{}': {}",
                     pathToUtf8(path), cleanupError.message());
    }
}

bool verifyFileContents(const std::filesystem::path& path,
                        const std::vector<uint8_t>& expected,
                        std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        error = "cannot reopen the finalized file";
        return false;
    }

    const auto end = file.tellg();
    if (end < 0 || static_cast<uint64_t>(end) != expected.size())
    {
        error = "finalized file size does not match the serialized data";
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::array<char, 64 * 1024> buffer{};
    size_t offset = 0;
    while (offset < expected.size())
    {
        const size_t count = std::min(buffer.size(), expected.size() - offset);
        file.read(buffer.data(), static_cast<std::streamsize>(count));
        if (file.gcount() != static_cast<std::streamsize>(count) ||
            std::memcmp(buffer.data(), expected.data() + offset, count) != 0)
        {
            error = "finalized file contents do not match the serialized data";
            return false;
        }
        offset += count;
    }

    return true;
}

#ifdef _WIN32

std::error_code lastWindowsError()
{
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

bool replaceOnWindows(const std::filesystem::path& stagedPath,
                      const std::filesystem::path& destinationPath,
                      const std::filesystem::path& backupPath,
                      bool destinationExists,
                      std::error_code& error)
{
    if (destinationExists)
    {
        // ReplaceFileW performs the destination swap and creation of the backup
        // as one filesystem operation. It never exposes a partially copied
        // project at destinationPath.
        if (::ReplaceFileW(destinationPath.c_str(), stagedPath.c_str(),
                           backupPath.c_str(), 0, nullptr, nullptr))
        {
            error.clear();
            return true;
        }

        error = lastWindowsError();
        return false;
    }

    // There is no file for ReplaceFileW to replace on a first save. The
    // staging file lives beside the destination, so this is a same-volume
    // rename. Do not allow a copy fallback: a copy could expose a partial
    // project if the process or machine stops during finalization.
    if (::MoveFileExW(stagedPath.c_str(), destinationPath.c_str(),
                      MOVEFILE_WRITE_THROUGH))
    {
        error.clear();
        return true;
    }

    error = lastWindowsError();

    // Handle a file created between the existence check and MoveFileExW. It
    // now has to be treated as an existing project and preserved as a backup.
    if (error.value() == ERROR_ALREADY_EXISTS || error.value() == ERROR_FILE_EXISTS)
    {
        if (::ReplaceFileW(destinationPath.c_str(), stagedPath.c_str(),
                           backupPath.c_str(), 0, nullptr, nullptr))
        {
            error.clear();
            return true;
        }
        error = lastWindowsError();
    }

    return false;
}

#else

bool replaceOnPosix(const std::filesystem::path& stagedPath,
                    const std::filesystem::path& destinationPath,
                    const std::filesystem::path& backupPath,
                    bool destinationExists,
                    std::error_code& error)
{
    if (destinationExists)
    {
        auto stagedBackup = withSuffix(backupPath, ".tmp");
        std::filesystem::copy_file(destinationPath, stagedBackup,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            removeTemporaryFile(stagedBackup);
            return false;
        }

        std::filesystem::rename(stagedBackup, backupPath, error);
        if (error)
        {
            removeTemporaryFile(stagedBackup);
            return false;
        }
    }

    // stagedPath is created in the destination directory, so rename is the
    // atomic same-filesystem operation on POSIX. Never fall back to copying.
    std::filesystem::rename(stagedPath, destinationPath, error);
    return !error;
}

#endif

bool replaceStagedFile(const std::filesystem::path& stagedPath,
                       const std::filesystem::path& destinationPath,
                       const std::filesystem::path& backupPath,
                       bool destinationExists,
                       std::error_code& error)
{
#ifdef _WIN32
    return replaceOnWindows(stagedPath, destinationPath, backupPath,
                            destinationExists, error);
#else
    return replaceOnPosix(stagedPath, destinationPath, backupPath,
                          destinationExists, error);
#endif
}

void restoreMissingDestination(const std::filesystem::path& destinationPath,
                               const std::filesystem::path& backupPath)
{
    std::error_code statusError;
    if (std::filesystem::exists(destinationPath, statusError) || statusError)
        return;

    statusError.clear();
    if (!std::filesystem::exists(backupPath, statusError) || statusError)
        return;

    // ReplaceFileW documents rare partial-failure modes in which the old file
    // has already acquired the backup name. Recover a destination copy while
    // retaining that backup. The save still reports failure.
    std::error_code restoreError;
    std::filesystem::copy_file(backupPath, destinationPath,
        std::filesystem::copy_options::none, restoreError);
    if (restoreError)
    {
        spdlog::critical("ProjectSerializer: destination '{}' is missing after failed "
                         "replacement; prior project remains at '{}': {}",
                         pathToUtf8(destinationPath), pathToUtf8(backupPath),
                         restoreError.message());
    }
    else
    {
        spdlog::warn("ProjectSerializer: restored destination '{}' from backup after "
                     "failed replacement", pathToUtf8(destinationPath));
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// File I/O wrappers
// ═══════════════════════════════════════════════════════════════════════════

bool ProjectSerializer::save(const Project& project, const std::filesystem::path& path) const
{
    auto data = serialize(project);

    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
    {
        spdlog::error("ProjectSerializer: serialized project is too large to write");
        return false;
    }

    // The staging file is deliberately in the destination directory so the
    // final rename/replace cannot cross filesystems.
    const auto tmpPath = withSuffix(path, ".tmp");
    const auto bakPath = withSuffix(path, ".bak");

    std::ofstream file(tmpPath, std::ios::binary);
    if (!file.is_open())
    {
        spdlog::error("ProjectSerializer: cannot open '{}' for writing", pathToUtf8(path));
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.flush();
    const bool writeSucceeded = file.good();
    file.close();

    if (!writeSucceeded || file.fail())
    {
        spdlog::error("ProjectSerializer: write error to '{}'", pathToUtf8(path));
        removeTemporaryFile(tmpPath);
        return false;
    }

    std::error_code stagingError;
    const auto stagedSize = std::filesystem::file_size(tmpPath, stagingError);
    if (stagingError || stagedSize != data.size())
    {
        spdlog::error("ProjectSerializer: staged write verification failed for '{}': {}",
                      pathToUtf8(path),
                      stagingError ? stagingError.message() : "size mismatch");
        removeTemporaryFile(tmpPath);
        return false;
    }

    // Atomically promote the fully written staging file over the destination.
    std::error_code statusError;
    const bool destinationExists = std::filesystem::exists(path, statusError);
    if (statusError)
    {
        spdlog::error("ProjectSerializer: cannot inspect destination '{}': {}",
                      pathToUtf8(path), statusError.message());
        removeTemporaryFile(tmpPath);
        return false;
    }

    std::error_code promotionError;
    if (!replaceStagedFile(tmpPath, path, bakPath, destinationExists, promotionError))
    {
        // Keep the promotion error independent from cleanup. The old code
        // reused one error_code, allowing a successful remove() to turn a
        // failed promotion into a reported success.
        const auto promotionMessage = promotionError.message();
        if (destinationExists)
            restoreMissingDestination(path, bakPath);
        removeTemporaryFile(tmpPath);
        spdlog::error("ProjectSerializer: failed to atomically finalize save to '{}': {}",
                      pathToUtf8(path), promotionMessage);
        return false;
    }

    std::string verificationError;
    if (!verifyFileContents(path, data, verificationError))
    {
        spdlog::error("ProjectSerializer: finalized save verification failed for '{}': {}",
                      pathToUtf8(path), verificationError);
        return false;
    }

    spdlog::info("Saved project to '{}' ({} bytes)", pathToUtf8(path), data.size());
    return true;
}

std::unique_ptr<Project> ProjectSerializer::load(const std::filesystem::path& path) const
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        spdlog::error("ProjectSerializer: cannot open '{}' for reading", pathToUtf8(path));
        return nullptr;
    }

    auto fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(fileSize));
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    auto project = deserialize(data);
    if (project)
    {
        project->setFilePath(path);
        spdlog::info("Loaded project from '{}' ({} bytes)", pathToUtf8(path), static_cast<size_t>(fileSize));
    }
    return project;
}

} // namespace rt
