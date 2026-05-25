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
    ShotPresetManager() = default;

    /// Scan a directory for saved presets (*.json).
    /// @return Number of presets loaded.
    int scan(const std::filesystem::path& presetsDir);

    /// Save a preset to disk (creates/overwrites the file).
    bool save(const ShotPreset& preset);

    /// Load a preset by name.  Returns nullopt if not found.
    [[nodiscard]] std::optional<ShotPreset> load(const std::string& name) const;

    /// Delete a preset by name from disk.
    bool remove(const std::string& name);

    /// List all available preset names.
    [[nodiscard]] std::vector<std::string> presetNames() const;

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

    /// Check if a preset with the given name exists.
    [[nodiscard]] bool hasPreset(const std::string& name) const;

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

private:
    /// Generate the file path for a preset name.
    [[nodiscard]] std::filesystem::path pathForPreset(const std::string& name) const;

    /// Load/save the alias map from `_aliases.json`.
    void loadAliases();
    void saveAliases() const;

    std::filesystem::path                             m_directory;
    std::vector<std::pair<std::string, ShotPreset>>   m_presets; ///< name → preset
    std::map<std::string, std::string>                m_aliases; ///< realName → displayName
};

} // namespace rt
