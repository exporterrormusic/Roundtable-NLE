/*
 * ShotPreset — data model for character shot compositions.
 *
 * Step 18: Shot Composer
 *
 * Ported from the Python shot_manager.py's BackgroundState, CharacterState,
 * Shot, and ShotManager classes.
 *
 * A ShotPreset describes a complete scene composition:
 *   - Multiple characters with position/scale/outfit/stance/animation
 *   - Multiple backgrounds with position/scale/opacity
 *   - Z-ordering via a layerOrder list
 *   - Camera transform (zoom, pan)
 *
 * The ShotPresetManager handles persistence — saving/loading presets
 * as JSON files in a directory.
 */

#pragma once

#include "timeline/SpineClip.h"  // CharacterStance

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rt {

// ─── Layer reference ────────────────────────────────────────────────────────

/// Type of layer in a shot composition
enum class LayerType : uint8_t
{
    Background,
    Character
};

/// Identifies a specific layer by type and index
struct LayerRef
{
    LayerType type   = LayerType::Character;
    int       index  = 0;

    bool operator==(const LayerRef& o) const noexcept
    {
        return type == o.type && index == o.index;
    }
    bool operator!=(const LayerRef& o) const noexcept { return !(*this == o); }
};

// ─── Background state ───────────────────────────────────────────────────────

/// State of a background layer in a shot preset
struct BackgroundState
{
    std::string path;                    ///< Relative path to background image/video
    float       posX         = 0.5f;     ///< Horizontal position (0–1, center = 0.5)
    float       posY         = 0.5f;     ///< Vertical position (0–1, center = 0.5)
    float       scale        = 1.0f;     ///< Scale multiplier
    float       opacity      = 1.0f;     ///< Opacity (0–1)
    int         nativeWidth  = 1920;     ///< Source image width
    int         nativeHeight = 1080;     ///< Source image height
    bool        visible      = true;     ///< Layer visibility
    std::string layerType    = "image"; ///< "image" or "video"
    float       inPoint      = 0.0f;     ///< Video in point (seconds)
    float       outPoint     = 0.0f;     ///< Video out point (seconds, 0 = full)

    // Crop (percentage 0–100)
    float       cropLeft     = 0.0f;
    float       cropRight    = 0.0f;
    float       cropTop      = 0.0f;
    float       cropBottom   = 0.0f;

    float       blur         = 0.0f;     ///< Gaussian blur radius (0–100)

    [[nodiscard]] bool isVideo() const noexcept { return layerType == "video"; }
};

// ─── Character state ────────────────────────────────────────────────────────

/// State of a character layer in a shot preset
struct CharacterState
{
    std::string     characterName;                           ///< Character identifier
    std::string     outfit      = "default";                 ///< Active outfit name
    CharacterStance stance      = CharacterStance::Default;  ///< Which skeleton variant
    std::string     animation   = "idle";                    ///< Body animation name
    bool            isTalking   = false;                     ///< Talking overlay active

    // Video character support (empty strings = normal Spine character)
    std::string     videoMutePath;   ///< Video file when not talking
    std::string     videoTalkPath;   ///< Video file when talking

    // PNG puppet support (empty puppetFolder = not a puppet). A puppet is a
    // Veadotube-style 4-image character; on the timeline it expands into a
    // PngPuppetClip. puppetFolder is the on-disk folder under the puppets root
    // (assets/png_characters/<folder>) and puppetVariant selects the variant.
    std::string     puppetFolder;
    std::string     puppetVariant = "default";

    // Transform (all in normalized [0–1] or scale-factor space)
    float           posX        = 0.5f;     ///< Horizontal position (0–1)
    float           posY        = 0.75f;    ///< Vertical position (0–1, typically lower)
    float           scale       = 1.0f;     ///< Scale multiplier
    float           rotation    = 0.0f;     ///< Rotation in degrees
    bool            flipX       = false;    ///< Horizontal flip
    bool            flipY       = false;    ///< Vertical flip
    float           opacity     = 1.0f;     ///< Opacity (0–1)

    // Crop (0.0 = no crop, 0.5 = crop 50% from that side)
    float           cropLeft    = 0.0f;
    float           cropRight   = 0.0f;
    float           cropTop     = 0.0f;
    float           cropBottom  = 0.0f;

    float           blur        = 0.0f;     ///< Gaussian blur radius (0–100)

    bool            visible     = true;     ///< Layer visibility

    /// True if this character uses video files instead of Spine
    [[nodiscard]] bool isVideoCharacter() const noexcept
    {
        return !videoMutePath.empty() || !videoTalkPath.empty();
    }

    /// True if this character is a PNG puppet (4-image Veadotube-style).
    [[nodiscard]] bool isPuppet() const noexcept
    {
        return !puppetFolder.empty();
    }

    /// Return the active video path based on talking state
    [[nodiscard]] const std::string& activeVideoPath() const noexcept
    {
        return isTalking ? videoTalkPath : videoMutePath;
    }
};

// ─── Shot preset ────────────────────────────────────────────────────────────

/// A complete shot composition preset
class ShotPreset
{
public:
    ShotPreset() = default;
    explicit ShotPreset(const std::string& name);

    // ── Name ────────────────────────────────────────────────────────────
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    void setName(const std::string& n) { m_name = n; }

    // ── Show (namespace) ───────────────────────────────────────────────────
    /// The single "show" this shot belongs to — its namespace. A shot is
    /// identified by (show, name): the same name can exist independently in
    /// different shows (e.g. "CROWN (Default)" in "Roundtable Talk" vs
    /// "Kingdom Connection"). Empty = the shared "No Show" namespace.
    [[nodiscard]] const std::string& show() const noexcept { return m_show; }
    void setShow(const std::string& s) { m_show = s; }

    // Compat wrappers: shots used to carry multiple show "tags"; the model is
    // now a single show (namespace). These keep older call sites working.
    [[nodiscard]] std::vector<std::string> shows() const {
        return m_show.empty() ? std::vector<std::string>{}
                              : std::vector<std::string>{m_show};
    }
    void setShows(const std::vector<std::string>& v) {
        m_show = v.empty() ? std::string{} : v.front();
    }

    /// Case-insensitive equality to this shot's show.
    [[nodiscard]] bool hasShow(const std::string& show) const;

    // ── Backgrounds ─────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<BackgroundState>& backgrounds() const noexcept
    {
        return m_backgrounds;
    }

    [[nodiscard]] int backgroundCount() const noexcept
    {
        return static_cast<int>(m_backgrounds.size());
    }

    int addBackground(const BackgroundState& bg);
    bool removeBackground(int index);

    BackgroundState* background(int index);
    [[nodiscard]] const BackgroundState* background(int index) const;

    // ── Characters ──────────────────────────────────────────────────────
    [[nodiscard]] const std::vector<CharacterState>& characters() const noexcept
    {
        return m_characters;
    }

    [[nodiscard]] int characterCount() const noexcept
    {
        return static_cast<int>(m_characters.size());
    }

    int addCharacter(const CharacterState& ch);
    bool removeCharacter(int index);

    CharacterState* character(int index);
    [[nodiscard]] const CharacterState* character(int index) const;

    // ── Layer ordering ──────────────────────────────────────────────────
    [[nodiscard]] const std::vector<LayerRef>& layerOrder() const noexcept
    {
        return m_layerOrder;
    }

    [[nodiscard]] int layerCount() const noexcept
    {
        return static_cast<int>(m_layerOrder.size());
    }

    /// Swap two layers in the z-order
    bool swapLayers(int indexA, int indexB);

    /// Move a layer up in z-order (toward front)
    bool moveLayerUp(int index);

    /// Move a layer down in z-order (toward back)
    bool moveLayerDown(int index);

    /// Move a layer to the front (top of z-order)
    bool moveLayerToFront(int index);

    /// Move a layer to the back (bottom of z-order)
    bool moveLayerToBack(int index);

    /// Move a layer from one position to another in the z-order.
    bool moveLayerTo(int from, int to);

    /// Find the layer order index for a particular layer ref
    [[nodiscard]] int findLayerIndex(LayerRef ref) const;

    // ── Camera ──────────────────────────────────────────────────────────
    [[nodiscard]] float cameraZoom() const noexcept { return m_cameraZoom; }
    void setCameraZoom(float z) noexcept { m_cameraZoom = z; }

    [[nodiscard]] float cameraX() const noexcept { return m_cameraX; }
    void setCameraX(float x) noexcept { m_cameraX = x; }

    [[nodiscard]] float cameraY() const noexcept { return m_cameraY; }
    void setCameraY(float y) noexcept { m_cameraY = y; }

    // ── Serialization ───────────────────────────────────────────────────
    /// Serialize the preset to a JSON string.
    [[nodiscard]] std::string toJson() const;

    /// Deserialize from a JSON string.  Returns nullopt on parse failure.
    static std::optional<ShotPreset> fromJson(const std::string& json);

    // ── Utility ─────────────────────────────────────────────────────────
    /// Create a minimal shot with a single character at default position.
    static ShotPreset createDefault(const std::string& characterName);

    /// Rebuild m_layerOrder to match current backgrounds/characters.
    /// Removes stale references and adds any missing layers.
    void ensureLayerOrder();

private:

    std::string                m_name;
    std::string                  m_show;      ///< Show namespace this shot belongs to
    std::vector<BackgroundState> m_backgrounds;
    std::vector<CharacterState>  m_characters;
    std::vector<LayerRef>        m_layerOrder;

    float m_cameraZoom = 1.0f;
    float m_cameraX    = 0.0f;
    float m_cameraY    = 0.0f;
};

// ─── Shot preset manager ────────────────────────────────────────────────────

/// Manages persistence of shot presets (load/save/delete from a directory).
class ShotPresetManager
{
public:
    enum class CharacterGroup {
        Normal,
        Favorite,
        Hidden
    };

    ShotPresetManager() = default;

    /// Scan a directory for saved presets (*.json).
    /// @return Number of presets loaded.
    int scan(const std::filesystem::path& presetsDir);

    // ── Identity ────────────────────────────────────────────────────────
    // A shot is identified by (show, name). On disk it lives in a per-show
    // subdirectory (No-Show shots live in the root). In memory and in clip
    // references it is addressed by a "key": "<show>/<name>", or just "<name>"
    // for No-Show shots.
    [[nodiscard]] static std::string makeKey(const std::string& show,
                                             const std::string& name);
    static void splitKey(const std::string& key, std::string& show,
                         std::string& name);

    /// Save a preset to disk (creates/overwrites the file for its show+name).
    bool save(const ShotPreset& preset);

    /// Load a preset by key ("show/name" or "name"). For a bare name with no
    /// show, falls back to searching every show (back-compat for old clip
    /// references / projects). Returns nullopt if not found.
    [[nodiscard]] std::optional<ShotPreset> load(const std::string& key) const;

    /// Load a preset within a specific show namespace.
    [[nodiscard]] std::optional<ShotPreset> load(const std::string& show,
                                                 const std::string& name) const;

    /// Delete a preset by key (or by show+name).
    bool remove(const std::string& key);
    bool remove(const std::string& show, const std::string& name);

    /// List all preset keys ("show/name" / "name").
    [[nodiscard]] std::vector<std::string> presetNames() const;

    /// Bare shot names within a single show namespace.
    [[nodiscard]] std::vector<std::string> namesForShow(const std::string& show) const;

    /// All presets (for iteration without per-name loads).
    [[nodiscard]] const std::vector<std::pair<std::string, ShotPreset>>&
        allPresets() const noexcept { return m_presets; }

    /// Number of known presets.
    [[nodiscard]] int presetCount() const noexcept
    {
        return static_cast<int>(m_presets.size());
    }

    /// Get the directory being managed.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept
    {
        return m_directory;
    }

    /// Check if a preset exists (by key, or within a specific show namespace).
    [[nodiscard]] bool hasPreset(const std::string& key) const;
    [[nodiscard]] bool hasPreset(const std::string& show, const std::string& name) const;

    /// Resolve the default shot for a character.
    ///
    /// Priority:
    ///   1. Resolve any alias display name → real character name (see setAlias).
    ///   2. Check `_defaults.json` (set via the "SET DEFAULT" button in ShotComposer)
    ///      for an explicit character → shot name mapping.
    ///   3. Fall back to the naming convention `"{characterName} (Default)"`.
    ///   4. Fall back to case-insensitive match on `"{charactername} (default)"`.
    ///
    /// Returns nullopt if no default shot is found.
    [[nodiscard]] std::optional<ShotPreset> resolveDefaultShot(
        const std::string& characterName) const;

    /// Show-aware default-shot resolution. When `show` is non-empty, a
    /// per-show default (from `_show_defaults.json`) is checked FIRST, then it
    /// falls back to the global default and naming convention (the behaviour of
    /// the single-argument overload). This lets the same character have a
    /// different default shot per show (e.g. Crown in "Roundtable Talk" vs
    /// "Kingdom Connection").
    [[nodiscard]] std::optional<ShotPreset> resolveDefaultShot(
        const std::string& characterName, const std::string& show) const;

    /// Set or clear the per-show default shot for a character. Empty shotName
    /// clears it. Persists to `_show_defaults.json`.
    void setShowDefaultShot(const std::string& show,
                            const std::string& characterName,
                            const std::string& shotName);

    /// Set or clear a character display-name alias. Persists to `_aliases.json`
    /// in the preset directory. A script line for `displayName` (or its
    /// case-insensitive match) will resolve to `realName`'s default shot.
    /// Pass an empty `displayName` to remove the alias for `realName`.
    void setAlias(const std::string& realName, const std::string& displayName);

    /// Return the display name configured for a real character name, or the
    /// real name itself when no alias has been set.
    [[nodiscard]] std::string displayNameFor(const std::string& realName) const;

    /// Reverse lookup: given a display name (case-insensitive), return the
    /// real character name. Returns the input if no alias matches.
    [[nodiscard]] std::string realNameFor(const std::string& displayName) const;

    /// Full alias map: realName → displayName.
    [[nodiscard]] const std::map<std::string, std::string>& aliases() const noexcept
    {
        return m_aliases;
    }

    /// Place a character in the normal, favorite, or hidden COMPOSE group.
    /// This never removes or changes saved shots that use the character.
    void setCharacterGroup(const std::string& realName, CharacterGroup group);

    /// Return the saved COMPOSE group for a real character name.
    [[nodiscard]] CharacterGroup characterGroup(const std::string& realName) const;

    // ── Show registry ─────────────────────────────────────────────────────
    /// Registered show names. This is a superset of the shows actually used by
    /// shots — it lets a user create a show (via the "+" button) before any
    /// shot is assigned to it. Persisted to `_shows.json` in the preset dir.
    [[nodiscard]] const std::vector<std::string>& knownShows() const noexcept
    {
        return m_knownShows;
    }

    /// Register a show name (no-op if it already exists, case-insensitive).
    void addShow(const std::string& show);

    /// Unregister a show name (case-insensitive). Does not touch shots.
    void removeShow(const std::string& show);

    /// Rename a registered show (case-insensitive match). Does not touch shots.
    void renameShow(const std::string& oldName, const std::string& newName);

    /// Set (or clear, if path empty) the thumbnail image path for a show.
    void setShowThumbnail(const std::string& show, const std::string& path);

    /// Thumbnail image path for a show (case-insensitive), or "" if none.
    [[nodiscard]] std::string showThumbnail(const std::string& show) const;

private:
    /// Generate the file path for a preset (per-show subdir; root = No Show).
    [[nodiscard]] std::filesystem::path pathForPreset(const std::string& show,
                                                      const std::string& name) const;

    /// Load/save the alias map from `_aliases.json`.
    void loadAliases();
    void saveAliases() const;

    /// Load/save favorite/hidden character groups from `_character_groups.json`.
    void loadCharacterGroups();
    void saveCharacterGroups() const;

    /// Load/save the registered show list from `_shows.json`.
    void loadShows();
    void saveShows() const;

    /// Load/save per-show thumbnail paths from `_show_thumbnails.json`.
    void loadShowThumbnails();
    void saveShowThumbnails() const;

    /// Load/save per-show default-shot map from `_show_defaults.json`.
    void loadShowDefaults();
    void saveShowDefaults() const;

    std::filesystem::path                             m_directory;
    std::vector<std::pair<std::string, ShotPreset>>   m_presets; ///< name → preset
    std::map<std::string, std::string>                m_aliases; ///< realName → displayName
    std::map<std::string, CharacterGroup>             m_characterGroups; ///< realName → non-normal group
    std::vector<std::string>                          m_knownShows; ///< registered show names
    std::map<std::string, std::string>                m_showThumbnails; ///< lowercased show → image path
    /// lowercased show → (real character name → shot name). Per-show defaults.
    std::map<std::string, std::map<std::string, std::string>> m_showDefaults;
};

} // namespace rt
