/*
 * AutoSave.cpp — Folder-based periodic auto-save implementation.
 * Step 29
 *
 * Saves timestamped copies to a "Roundtable Auto-Save" folder next to the
 * project file.  Old copies are pruned to keep at most maxAutoSaves files.
 */

#include "project/AutoSave.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"

#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <vector>

namespace rt {

static constexpr const char* kAutoSaveFolderName = "Roundtable Auto-Save";

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

AutoSave::AutoSave()
    : m_lastSave(Clock::now())
{
    m_stats.status = AutoSaveStatus::Idle;
}

AutoSave::~AutoSave() = default;

// ═════════════════════════════════════════════════════════════════════════════
// Configuration
// ═════════════════════════════════════════════════════════════════════════════

void AutoSave::setInterval(Duration interval) noexcept
{
    m_interval = interval;
}

void AutoSave::setEnabled(bool enabled) noexcept
{
    m_enabled = enabled;
    if (!m_enabled) {
        m_stats.status = AutoSaveStatus::Disabled;
    } else if (m_stats.status == AutoSaveStatus::Disabled) {
        m_stats.status = AutoSaveStatus::Idle;
        m_lastSave = Clock::now(); // Reset timer when re-enabling
    }
}

void AutoSave::setProject(Project* project) noexcept
{
    m_project = project;
    m_lastSave = Clock::now(); // Reset timer when project changes
}

void AutoSave::setSerializer(ProjectSerializer* serializer) noexcept
{
    m_serializer = serializer;
}

void AutoSave::setStatusCallback(StatusCallback cb)
{
    m_statusCb = std::move(cb);
}

// ═════════════════════════════════════════════════════════════════════════════
// Tick
// ═════════════════════════════════════════════════════════════════════════════

bool AutoSave::tick()
{
    if (!m_enabled || !m_project) return false;

    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - m_lastSave);

    if (elapsed < m_interval) return false;

    // Only save if the project has been modified
    if (!m_project->isModified()) {
        m_lastSave = now; // Reset timer even if not modified
        return false;
    }

    return doSave();
}

bool AutoSave::saveNow()
{
    if (!m_project) return false;
    return doSave();
}

// ═════════════════════════════════════════════════════════════════════════════
// Auto-save folder paths
// ═════════════════════════════════════════════════════════════════════════════

std::filesystem::path AutoSave::autoSaveFolder(const std::filesystem::path& projectPath)
{
    if (projectPath.empty()) {
        // Unsaved project — auto-save to temp directory
        return std::filesystem::temp_directory_path() / kAutoSaveFolderName;
    }

    // "Roundtable Auto-Save" folder next to the project file
    return projectPath.parent_path() / kAutoSaveFolderName;
}

std::filesystem::path AutoSave::currentAutoSaveFolder() const
{
    if (!m_project) return {};
    return autoSaveFolder(m_project->filePath());
}

std::filesystem::path AutoSave::makeTimestampedPath(
    const std::filesystem::path& folder,
    const std::string& projectName)
{
    // Format: "ProjectName YYYY-MM-DD at HH.MM.SS.rtp"
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d at %02d.%02d.%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);

    std::string filename = projectName + " " + buf + ".rtp";
    return folder / filename;
}

// ═════════════════════════════════════════════════════════════════════════════
// Recovery detection
// ═════════════════════════════════════════════════════════════════════════════

std::filesystem::path AutoSave::findNewestAutoSave(
    const std::filesystem::path& projectPath)
{
    auto folder = autoSaveFolder(projectPath);
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) return {};

    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};

    for (auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = pathToUtf8(entry.path().extension());
        if (ext != ".rtp") continue;

        auto wt = entry.last_write_time(ec);
        if (ec) continue;
        if (newest.empty() || wt > newestTime) {
            newest = entry.path();
            newestTime = wt;
        }
    }

    return newest;
}

bool AutoSave::hasRecoverableAutoSave(const std::filesystem::path& projectPath)
{
    auto newest = findNewestAutoSave(projectPath);
    if (newest.empty()) return false;

    std::error_code ec;

    // If the project file doesn't exist, any auto-save is recoverable
    if (!std::filesystem::exists(projectPath, ec)) return true;

    // Auto-save is recoverable if it's newer than the project file
    auto asTime = std::filesystem::last_write_time(newest, ec);
    if (ec) return false;

    auto projTime = std::filesystem::last_write_time(projectPath, ec);
    if (ec) return true; // Can't read project time — assume auto-save is newer

    return asTime > projTime;
}

std::unique_ptr<Project> AutoSave::loadLatestAutoSave(
    const std::filesystem::path& projectPath)
{
    auto asPath = findNewestAutoSave(projectPath);
    if (asPath.empty()) return nullptr;

    ProjectSerializer serializer;
    auto project = serializer.load(asPath);

    if (project) {
        // Set the original project path (not the auto-save path)
        project->setFilePath(projectPath);
        project->setModified(true); // Mark as modified since it's recovered
        spdlog::info("AutoSave: recovered project from '{}'", pathToUtf8(asPath));
    } else {
        spdlog::warn("AutoSave: failed to load auto-save from '{}'", pathToUtf8(asPath));
    }

    return project;
}

// ═════════════════════════════════════════════════════════════════════════════
// Pruning
// ═════════════════════════════════════════════════════════════════════════════

void AutoSave::pruneAutoSaves(const std::filesystem::path& folder, size_t maxKeep)
{
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) return;

    // Collect all auto-save files with their timestamps
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
    };
    std::vector<Entry> files;

    for (auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = pathToUtf8(entry.path().extension());
        if (ext != ".rtp") continue;

        auto wt = entry.last_write_time(ec);
        if (ec) continue;
        files.push_back({entry.path(), wt});
    }

    if (files.size() <= maxKeep) return;

    // Sort newest first
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.time > b.time; });

    // Remove oldest
    for (size_t i = maxKeep; i < files.size(); ++i) {
        std::filesystem::remove(files[i].path, ec);
        if (!ec) {
            spdlog::trace("AutoSave: pruned '{}'", pathToUtf8(files[i].path));
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Statistics
// ═════════════════════════════════════════════════════════════════════════════

AutoSave::Duration AutoSave::timeUntilNextSave() const noexcept
{
    if (!m_enabled) return Duration::max();

    auto now = Clock::now();
    auto elapsed = std::chrono::duration_cast<Duration>(now - m_lastSave);

    if (elapsed >= m_interval) return Duration::zero();
    return std::chrono::duration_cast<Duration>(m_interval - elapsed);
}

void AutoSave::resetTimer() noexcept
{
    m_lastSave = Clock::now();
}

// ═════════════════════════════════════════════════════════════════════════════
// Internal save implementation
// ═════════════════════════════════════════════════════════════════════════════

bool AutoSave::doSave()
{
    if (!m_project) return false;

    auto folder = currentAutoSaveFolder();
    if (folder.empty()) return false;

    // Ensure the auto-save folder exists
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        spdlog::warn("AutoSave: failed to create folder '{}': {}",
                     pathToUtf8(folder), ec.message());
        m_stats.failCount++;
        m_stats.status = AutoSaveStatus::Failed;
        if (m_statusCb)
            m_statusCb(AutoSaveStatus::Failed, "Failed to create auto-save folder");
        return false;
    }

    // Build timestamped save path
    auto savePath = makeTimestampedPath(folder, m_project->name());

    m_stats.status = AutoSaveStatus::Saving;

    // Get or create a serializer
    ProjectSerializer* ser = m_serializer;
    if (!ser) {
        if (!m_ownedSerializer)
            m_ownedSerializer = std::make_unique<ProjectSerializer>();
        ser = m_ownedSerializer.get();
    }

    // ProjectSerializer owns the single staging-and-replacement transaction.
    // A second auto-save staging layer previously reintroduced a non-atomic
    // copy fallback and could erase its error while cleaning up the temp file.
    const bool success = ser->save(*m_project, savePath);

    // Update stats
    m_lastSave = Clock::now();

    if (success) {
        m_stats.saveCount++;
        m_stats.lastSaveTime = m_lastSave;
        m_stats.status = AutoSaveStatus::Saved;
        spdlog::info("AutoSave: saved to '{}'", pathToUtf8(savePath));

        // Prune old auto-saves
        pruneAutoSaves(folder, m_maxAutoSaves);

        if (m_statusCb)
            m_statusCb(AutoSaveStatus::Saved, "Auto-saved successfully");
    } else {
        m_stats.failCount++;
        m_stats.status = AutoSaveStatus::Failed;
        spdlog::warn("AutoSave: failed to save to '{}'", pathToUtf8(savePath));

        if (m_statusCb)
            m_statusCb(AutoSaveStatus::Failed, "Auto-save failed");
    }

    return success;
}

} // namespace rt
