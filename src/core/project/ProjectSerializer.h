/*
 * ProjectSerializer — binary serialization for .rtp project files.
 *
 * File format:
 *   [Header]    Magic(8) + FormatVersion(4) + SectionCount(4) + Reserved(16)
 *   [Sections]  Each: TypeTag(4) + DataSize(4) + Data(N)
 *
 * Sections:
 *   0x01 = Settings (resolution, fps, audio, export)
 *   0x02 = Timeline metadata (name, playhead, in/out)
 *   0x03 = Tracks (type, name, muted, solo, locked, height)
 *   0x04 = Clips (per-track, fully self-describing)
 *   0x05 = Keyframe data (per-clip property tracks)
 *   0x06 = Asset entries
 *   0x07 = Character assets
 *   0x08 = Markers
 *   0x09 = Transitions
 *
 * All multi-byte values are little-endian. Strings are length-prefixed (uint32).
 *
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <string>

namespace rt {

class Project;

class ProjectSerializer
{
public:
    ProjectSerializer() = default;
    ~ProjectSerializer() = default;

    // ── Save / Load ─────────────────────────────────────────────────────
    /// Save a project to a .rtp binary file.
    /// Returns true on success.
    [[nodiscard]] bool save(const Project& project, const std::filesystem::path& path) const;

    /// Load a project from a .rtp binary file.
    /// Returns nullptr on failure.
    [[nodiscard]] std::unique_ptr<Project> load(const std::filesystem::path& path) const;

    // ── In-memory round-trip (for testing) ──────────────────────────────
    /// Serialize to a byte buffer.
    [[nodiscard]] std::vector<uint8_t> serialize(const Project& project) const;

    /// Deserialize from a byte buffer.
    [[nodiscard]] std::unique_ptr<Project> deserialize(const std::vector<uint8_t>& data) const;

    // ── Lightweight metadata read (no full deserialization) ────────────
    struct Metadata {
        uint32_t    resW{1920};
        uint32_t    resH{1080};
        double      fps{30.0};
        std::string name;
    };

    /// Read only the Settings + Timeline-name sections from a .rtp
    /// file.  Much faster than load() — ideal for project list display.
    [[nodiscard]] static bool readMetadata(const std::filesystem::path& path, Metadata& out);

    /// Read just the project's associated show (Section_ProjectMeta) without
    /// deserializing the project. Returns "" if unset or unreadable. Scans
    /// section headers only, so it stays cheap even for large project files.
    [[nodiscard]] static std::string readProjectShow(const std::filesystem::path& path);

    // ── Format info ─────────────────────────────────────────────────────
    static constexpr uint8_t  MAGIC[8] = {'R','N','D','T','B','L','v','2'};
    static constexpr uint32_t FORMAT_VERSION = 26;  // v26 = PngPuppetClip (Veadotube-style 4-image PNG puppet character) type-specific fields
                                                    // v25 = per-sequence Settings (resolution/fps/colour/audio independent per sequence)
                                                    // v24 = persist beat-reactive effect onset times + audio source id
                                                    // v23 = persist Project.show (per-show default shots)
                                                    // v22 = persist AudioClip audiofx chain (ParametricEQ / Dynamics)
                                                    // v21 = persist Track.isCaptionTrack (subtitle track pinned on top) + CaptionClip type-specific fields
                                                    // v20 = persist Track.isPermanentDivider so user-added dividers don't get hijacked when whoever sits at the V/A boundary is greedily promoted
                                                    // v19 = clip anchorX/anchorY tracks (rotation/scale pivot)
                                                    // v18 = persist Track.isDivider flag (V/A separator)
                                                    // v17 = Sequences section correctly writes v16+ transition fields
                                                    // v16 = transition clip-link + edit-point positioning (bug: Sequences section omitted them)
                                                    // v15 = spatial keyframe handles (motion path)

    /// Section types
    enum SectionType : uint32_t
    {
        Section_Settings    = 0x01,
        Section_Timeline    = 0x02,
        Section_Tracks      = 0x03,
        Section_Clips       = 0x04,
        Section_Keyframes   = 0x05,
        Section_Assets      = 0x06,
        Section_Characters  = 0x07,
        Section_Markers     = 0x08,
        Section_Transitions = 0x09,
        Section_Sequences   = 0x0A,   ///< Multi-sequence support (v4+)
        Section_BinState    = 0x0B,   ///< Project bin media files + folders
        Section_AudioSync   = 0x0C,   ///< AudioSync panel state (v13+)
        Section_ProjectMeta = 0x0D,   ///< Project-level metadata: show assignment (v23+)
    };
};

} // namespace rt
