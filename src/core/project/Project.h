/*
 * Project — the top-level container for all project data.
 *
 * A Project owns one or more Sequences (Timelines), an asset database,
 * settings, and a file path.
 * Serialized to a versioned binary format for <100ms load/save.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

#include "project/Settings.h"

namespace rt {

// Forward declarations
class Timeline;
class AssetDatabase;
class CommandStack;

/// The top-level project container.
class Project
{
public:
    Project();
    ~Project();

    // Non-copyable, movable
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) noexcept;
    Project& operator=(Project&&) noexcept;

    // ── Identity ────────────────────────────────────────────────────────
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(const std::string& name) { m_name = name; }

    [[nodiscard]] const std::filesystem::path& filePath() const noexcept { return m_filePath; }
    void setFilePath(const std::filesystem::path& path) { m_filePath = path; }

    [[nodiscard]] bool isModified() const noexcept { return m_modified.load(std::memory_order_relaxed); }
    void setModified(bool v = true) noexcept { m_modified.store(v, std::memory_order_relaxed); }

    // ── Show assignment ─────────────────────────────────────────────────
    /// The "show" this project belongs to (e.g. "Roundtable Talk"). Used to
    /// resolve per-show default character shots. Empty = unassigned.
    [[nodiscard]] const std::string& show() const noexcept { return m_show; }
    void setShow(const std::string& show) { m_show = show; }

    // ── Sequences (Timelines) ───────────────────────────────────────────
    /// Returns the currently active sequence (backward-compat alias).
    [[nodiscard]] Timeline* timeline() noexcept;
    [[nodiscard]] const Timeline* timeline() const noexcept;

    /// Number of sequences in the project.
    [[nodiscard]] size_t sequenceCount() const noexcept { return m_sequences.size(); }

    /// Access a sequence by index.
    [[nodiscard]] Timeline* sequence(size_t index) noexcept;
    [[nodiscard]] const Timeline* sequence(size_t index) const noexcept;

    /// Index of the active sequence.
    [[nodiscard]] size_t activeSequenceIndex() const noexcept { return m_activeSequence; }

    /// Switch the active sequence. Returns the new active Timeline*.
    Timeline* setActiveSequence(size_t index);

    /// Add a new empty sequence. Returns the new sequence and its index.
    Timeline* addSequence(const std::string& name = "");

    /// Add a pre-built sequence (takes ownership). Returns the new sequence.
    Timeline* addSequence(std::unique_ptr<Timeline> seq);

    /// Duplicate an existing sequence. Returns the new copy and its index.
    Timeline* duplicateSequence(size_t srcIndex);

    /// Remove a sequence by index. Cannot remove the last one.
    bool removeSequence(size_t index);

    /// Extract a sequence (remove + return ownership). For undo support.
    std::unique_ptr<Timeline> extractSequence(size_t index);

    /// Insert a previously extracted sequence at a specific index. For undo support.
    void insertSequence(size_t index, std::unique_ptr<Timeline> seq);

    /// Generate a unique sequence name (e.g. "Sequence 2", "Sequence 3").
    [[nodiscard]] std::string nextSequenceName() const;

    /// Sequence indices currently open as timeline tabs. This is project UI
    /// state, persisted so reopening a project restores only the tabs the user
    /// left open. The collection is always normalized and non-empty.
    [[nodiscard]] const std::vector<size_t>& openSequenceIndices() const noexcept
    {
        return m_openSequenceIndices;
    }
    void setOpenSequenceIndices(std::vector<size_t> indices);

    // ── Assets ──────────────────────────────────────────────────────────
    [[nodiscard]] AssetDatabase* assets() noexcept { return m_assets.get(); }
    [[nodiscard]] const AssetDatabase* assets() const noexcept { return m_assets.get(); }

    // ── Settings ────────────────────────────────────────────────────────
    // settings() returns the ACTIVE sequence's settings (each sequence owns
    // its own — independent, like Premiere Pro). Falls back to the project
    // default template when there are no sequences. Defined in Project.cpp
    // because it dereferences a Timeline.
    [[nodiscard]] const Settings& settings() const noexcept;
    [[nodiscard]] Settings& settings() noexcept;

    /// Project-level default settings template — used to seed NEW sequences
    /// and as the fallback before any sequence exists. NOT the active
    /// sequence's settings (use settings() for that).
    [[nodiscard]] const Settings& defaultSettings() const noexcept { return m_settings; }
    [[nodiscard]] Settings& defaultSettings() noexcept { return m_settings; }

    // ── Command stack (undo/redo) ───────────────────────────────────────
    [[nodiscard]] CommandStack* commandStack() noexcept { return m_commands.get(); }
    [[nodiscard]] const CommandStack* commandStack() const noexcept { return m_commands.get(); }

    // ── Metadata ────────────────────────────────────────────────────────
    [[nodiscard]] uint32_t formatVersion() const noexcept { return m_formatVersion; }

    // ── Bin state (media files + folder structure) ──────────────────────
    struct BinFolder {
        std::string              name;
        bool                     expanded = true;
        std::vector<std::string> childKeys; ///< file paths or sequence names
    };

    [[nodiscard]] const std::vector<std::filesystem::path>& binFiles() const noexcept { return m_binFiles; }
    void setBinFiles(std::vector<std::filesystem::path> files) { m_binFiles = std::move(files); }

    [[nodiscard]] const std::vector<BinFolder>& binFolders() const noexcept { return m_binFolders; }
    void setBinFolders(std::vector<BinFolder> folders) { m_binFolders = std::move(folders); }

    /// Rich per-instance bin entry (format v14+). Carries its own stable
    /// identity, display name, and label colour so footage can appear
    /// multiple times in the bin as independent "master clips" that
    /// survive save/reload (Premiere-style duplicates).
    struct BinItem {
        uint64_t              id = 0;
        std::filesystem::path path;
        std::string           displayName;
        uint32_t              labelColor = 0xFF888888;
    };

    [[nodiscard]] const std::vector<BinItem>& binItems() const noexcept { return m_binItems; }
    void setBinItems(std::vector<BinItem> items) { m_binItems = std::move(items); }

    // ── AudioSync state (opaque blob serialized by the AudioSync panel) ─
    [[nodiscard]] const std::vector<uint8_t>& audioSyncBlob() const noexcept { return m_audioSyncBlob; }
    void setAudioSyncBlob(std::vector<uint8_t> blob) { m_audioSyncBlob = std::move(blob); }

    // ── Factory ─────────────────────────────────────────────────────────
    /// Create a new empty project with defaults (one sequence with V1+A1).
    static std::unique_ptr<Project> createNew(const std::string& name = "Untitled");

private:
    std::string                    m_name{"Untitled"};
    std::string                    m_show;            ///< Show assignment (per-show defaults)
    std::filesystem::path          m_filePath;
    std::atomic<bool>                   m_modified{false};
    uint32_t                       m_formatVersion{2};  // v2 = C++ rewrite
    Settings                       m_settings;
    std::vector<std::unique_ptr<Timeline>> m_sequences;
    size_t                         m_activeSequence{0};
    std::vector<size_t>            m_openSequenceIndices{0};
    std::unique_ptr<AssetDatabase> m_assets;
    std::unique_ptr<CommandStack>  m_commands;
    std::vector<std::filesystem::path> m_binFiles;
    std::vector<BinFolder>         m_binFolders;
    std::vector<BinItem>           m_binItems;   // v14+ rich bin model
    std::vector<uint8_t>           m_audioSyncBlob;
};

} // namespace rt
