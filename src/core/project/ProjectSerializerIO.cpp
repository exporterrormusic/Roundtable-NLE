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

#include <fstream>
#include "PathUtils.h"

#include <spdlog/spdlog.h>

namespace rt {

// ═══════════════════════════════════════════════════════════════════════════
// File I/O wrappers
// ═══════════════════════════════════════════════════════════════════════════

bool ProjectSerializer::save(const Project& project, const std::filesystem::path& path) const
{
    auto data = serialize(project);

    // Atomic write pattern: write to .tmp, then rename over final path
    // Use native() to avoid Unicode mangling on Windows (narrow string conversion)
    auto tmpPath = std::filesystem::path(path.native() + L".tmp");

    std::ofstream file(tmpPath, std::ios::binary);
    if (!file.is_open())
    {
        spdlog::error("ProjectSerializer: cannot open '{}' for writing", pathToUtf8(path));
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    file.close();

    if (!file.good())
    {
        spdlog::error("ProjectSerializer: write error to '{}'", pathToUtf8(path));
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    // Create .bak of previous version (if it exists)
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        auto bakPath = std::filesystem::path(path.native() + L".bak");
        std::filesystem::copy_file(path, bakPath,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            spdlog::warn("ProjectSerializer: failed to create backup: {}", ec.message());
    }

    // Rename .tmp → final (atomic on most filesystems)
    std::filesystem::rename(tmpPath, path, ec);
    if (ec)
    {
        // Fallback: copy + delete
        std::filesystem::copy_file(tmpPath, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmpPath, ec);
        if (ec)
        {
            spdlog::error("ProjectSerializer: failed to finalize save to '{}'", pathToUtf8(path));
            return false;
        }
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
