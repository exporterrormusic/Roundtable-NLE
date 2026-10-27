/*
 * SpineEngine — Central Spine skeleton manager.
 *
 * Loads a skeleton from .skel (binary) + .atlas + .png, provides
 * frame-accurate pose evaluation, and extracts render-ready mesh data
 * (vertices, UVs, indices, colors, blend modes) for the GPU renderer.
 *
 * Lifecycle:
 *   1. Create SpineEngine
 *   2. Call loadSkeleton() with paths to .skel and .atlas files
 *   3. Set animation via animation().setBodyAnimation(...)
 *   4. Call update(dt) or evaluateAtTime(t) each frame
 *   5. Call extractMeshes() to get render data
 *
 * This class does NOT do any GPU work — that's SpineRenderer (Step 9).
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include "spine/SpineAtlas.h"
#include "spine/SpineAnimation.h"
#include "timeline/SpineClip.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

namespace spine {
    class Skeleton;
    class SkeletonData;
    class SkeletonClipping;
}

namespace rt {

/// Blend mode for a render batch (maps to Vulkan blend state)
enum class SpineBlendMode : uint8_t
{
    Normal,
    Additive,
    Multiply,
    Screen
};

/// A single vertex in the spine mesh
struct SpineVertex
{
    float x, y;           ///< Position (world space)
    float u, v;           ///< Texture coordinates
    float r, g, b, a;     ///< Vertex color (premultiplied alpha)
};

/// A batch of triangles that share the same texture page and blend mode.
/// The GPU renderer draws one draw call per batch.
struct SpineRenderBatch
{
    int                              texturePageIndex = -1;
    SpineBlendMode                   blendMode = SpineBlendMode::Normal;
    std::vector<SpineVertex>         vertices;
    std::vector<uint16_t>            indices;
    // Diagnostic only: name of the first slot that opened this batch (in draw
    // order).  Set on batch creation in extractMeshes(); not uploaded to the
    // GPU.  Used by the timeline Spine-blend signature logger to pinpoint which
    // slot/blend-mode transition causes a one-frame flicker.
    std::string                      debugFirstSlot;
};

/// Result of extractMeshes() — all render data for the current pose
struct SpineRenderData
{
    std::vector<SpineRenderBatch> batches;
    float boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;
};

/// An axis-aligned box in skeleton coordinates.
struct SpineBox
{
    float x = 0, y = 0, w = 0, h = 0;
};

/// The most a render grows to hold an animation's reach (a memory cap;
/// reach beyond it is cut off).
inline constexpr float kSpineMaxRenderGrowth = 2.0f;

/// Height for a render `outH` tall at the character's setup-pose size that
/// also holds what an animation reaches above and below (`reach`, its
/// framingBox), centred on the setup pose.  The character keeps her size
/// and place whatever she plays; only the render grows.  `outH` when the
/// animation stays inside the setup pose; always even.
[[nodiscard]] uint32_t spineRenderHeight(uint32_t outH, const SpineBox& setup,
                                         const SpineBox& reach);

/// Parsed, immutable atlas + skeleton data for one character asset.
/// Parsing is the expensive part of creating a SpineEngine (~50 ms for a
/// full character); spine-cpp treats SkeletonData/Atlas as read-only at
/// runtime, so every clip of the same character shares one instance and
/// only builds its own Skeleton + AnimationState.  Engines hold a
/// shared_ptr, so the data outlives every Skeleton that references it.
struct SpineParsedSkeleton
{
    SpineParsedSkeleton();
    ~SpineParsedSkeleton();
    SpineParsedSkeleton(const SpineParsedSkeleton&) = delete;
    SpineParsedSkeleton& operator=(const SpineParsedSkeleton&) = delete;

    /// Parse from in-memory .skel bytes + .atlas text.  Thread-safe (no
    /// shared state); returns nullptr on failure.
    static std::shared_ptr<SpineParsedSkeleton> parseFromBuffers(
        const std::vector<uint8_t>& skelBytes, const std::string& atlasText,
        const std::string& atlasDir, const std::string& skelPath,
        const std::string& atlasPath, float scale = 1.0f);

    /// Parse from .skel + .atlas files on disk; nullptr on failure.
    static std::shared_ptr<SpineParsedSkeleton> parseFromFiles(
        const std::string& skelPath, const std::string& atlasPath,
        float scale = 1.0f);

    /// What `animation` needs drawn: horizontally the setup pose; vertically
    /// also everything visible the animation reaches, so a raised arm is not
    /// cut off at the top of the timeline's render.  A character is SIZED
    /// and placed by the setup pose (framingBox("")) in COMPOSE, timeline and
    /// export alike; this box only sets how tall the render must be
    /// (spineRenderHeight).  It is constant per animation: live bounds
    /// follow every swinging cape and particle.  Measured once per animation
    /// on a skeleton of its own, then cached.  An empty or unknown name
    /// gives the setup pose.  Thread-safe.
    [[nodiscard]] SpineBox framingBox(const std::string& animation) const;

    struct SkelDataDeleter { void operator()(spine::SkeletonData* p) const; };

    // Declaration order matters: skelData's attachments reference atlas
    // regions, so skelData (declared after) is destroyed first.
    SpineAtlas                                            atlas;
    std::unique_ptr<spine::SkeletonData, SkelDataDeleter> skelData;
    std::string                                           skelPath;
    std::string                                           atlasPath;
    std::string                                           version;

private:
    mutable std::mutex                                    m_framingMutex;
    mutable std::unordered_map<std::string, SpineBox>     m_framingBoxes;
};

class SpineEngine
{
public:
    SpineEngine();
    ~SpineEngine();

    SpineEngine(const SpineEngine&) = delete;
    SpineEngine& operator=(const SpineEngine&) = delete;
    SpineEngine(SpineEngine&&) noexcept;
    SpineEngine& operator=(SpineEngine&&) noexcept;

    /// Load skeleton from .skel binary + .atlas files
    /// @param skelPath   Path to .skel binary file
    /// @param atlasPath  Path to .atlas text file
    /// @param scale      Skeleton scale (default 1.0)
    /// @return true on success
    bool loadSkeleton(const std::string& skelPath,
                      const std::string& atlasPath,
                      float scale = 1.0f);

    /// Load skeleton from in-memory buffers (avoids disk I/O).
    /// Used by the spine cache to create per-clip engines from
    /// already-loaded skeleton binary + atlas text data.
    /// @param skelBytes   Skeleton binary data (contents of .skel file)
    /// @param atlasText   Atlas text data (contents of .atlas file)
    /// @param atlasDir    Directory for atlas texture path resolution
    /// @param skelPath    Original skel path (for dedup/version detection)
    /// @param atlasPath   Original atlas path (for dedup)
    /// @param scale       Skeleton scale (default 1.0)
    /// @return true on success
    bool loadSkeletonFromBuffers(const std::vector<uint8_t>& skelBytes,
                                 const std::string& atlasText,
                                 const std::string& atlasDir,
                                 const std::string& skelPath,
                                 const std::string& atlasPath,
                                 float scale = 1.0f);

    /// Load from a SpineClip + asset base directory.
    /// Resolves character/outfit/stance to file paths automatically.
    /// @param clip      SpineClip with character info
    /// @param assetsDir Root assets directory (containing "characters/")
    /// @return true on success
    bool loadFromClip(const SpineClip& clip, const std::string& assetsDir);

    /// Load from a SpineClip using pre-cached in-memory buffers.
    /// Avoids all disk I/O — used after spine shared cache warm-up.
    bool loadFromClipBuffered(const SpineClip& clip,
                              const std::vector<uint8_t>& skelBytes,
                              const std::string& atlasText,
                              const std::string& atlasDir,
                              const std::string& skelPath,
                              const std::string& atlasPath);

    /// Fast path: build this engine's Skeleton + AnimationState on already
    /// parsed shared data (no parsing).  Applies the clip's settings.
    bool loadFromClipParsed(const SpineClip& clip,
                            std::shared_ptr<SpineParsedSkeleton> parsed);

    /// @return true if skeleton is loaded
    [[nodiscard]] bool isLoaded() const noexcept { return m_skeleton != nullptr; }

    /// @return the skeleton file path currently loaded (empty if none)
    [[nodiscard]] const std::string& loadedSkelPath() const noexcept { return m_loadedSkelPath; }
    [[nodiscard]] const std::string& loadedAtlasPath() const noexcept { return m_loadedAtlasPath; }

    // ── Skeleton info ───────────────────────────────────────────────────

    /// Get the skeleton version string from the .skel file
    [[nodiscard]] const std::string& version() const noexcept { return m_version; }

    /// Skeleton dimensions from the data
    [[nodiscard]] float skeletonWidth() const;
    [[nodiscard]] float skeletonHeight() const;

    /// Set skin by name (skins within a .skel file, not outfits)
    bool setSkin(const std::string& skinName);

    /// List available skins
    [[nodiscard]] std::vector<std::string> listSkins() const;

    /// Set skeleton position
    void setPosition(float x, float y);

    /// Set skeleton scale
    void setScale(float sx, float sy);

    // ── Animation access ────────────────────────────────────────────────

    [[nodiscard]] SpineAnimation& animation() noexcept { return m_animation; }
    [[nodiscard]] const SpineAnimation& animation() const noexcept { return m_animation; }

    // ── Frame evaluation ────────────────────────────────────────────────

    /// Update animation by delta time (real-time preview)
    void update(float dt);

    /// Evaluate pose at exact time (video/export mode)
    void evaluateAtTime(float bodyTime, float talkTime = 0.0f);

    // ── Mesh extraction ─────────────────────────────────────────────────

    /// Extract all render meshes for the current pose.
    /// Groups triangles by texture page and blend mode.
    [[nodiscard]] SpineRenderData extractMeshes();

    /// Omit named attachments during mesh extraction. This supports narrowly
    /// scoped per-character corrections without altering the source skeleton
    /// or disabling a blend mode globally.
    void setHiddenAttachmentNames(std::vector<std::string> names);

    /// Get axis-aligned bounding box of current pose
    void getBounds(float& x, float& y, float& w, float& h);

    /// The box to frame this character by while it plays `animation`; see
    /// SpineParsedSkeleton::framingBox.  Empty when nothing is loaded.
    [[nodiscard]] SpineBox framingBox(const std::string& animation) const
    {
        return m_parsed ? m_parsed->framingBox(animation) : SpineBox{};
    }

    // ── Atlas access ────────────────────────────────────────────────────

    [[nodiscard]] const SpineAtlas& atlas() const noexcept;

    /// The shared parsed data this engine was built on (nullptr if unloaded).
    /// Hand it to other engines' loadFromClipParsed() to skip re-parsing.
    [[nodiscard]] std::shared_ptr<SpineParsedSkeleton> parsedData() const noexcept
    {
        return m_parsed;
    }

    // ── Static utilities ────────────────────────────────────────────────

    /// Detect Spine version from a .skel binary file header
    /// @return Version string (e.g., "4.1.20") or empty on failure
    static std::string detectVersion(const std::string& skelPath);

    /// Resolve file paths for a character/outfit/stance combination
    struct ResolvedPaths {
        std::string skelPath;
        std::string atlasPath;
        std::string texturePath;
        bool valid = false;
    };

    static ResolvedPaths resolvePaths(const std::string& assetsDir,
                                       const std::string& character,
                                       const std::string& outfit,
                                       CharacterStance stance);

private:
    struct SkelDeleter     { void operator()(spine::Skeleton* p) const; };

    /// Replace current state with a Skeleton + AnimationState built on
    /// `parsed` (shared, never mutated).
    bool adoptParsed(std::shared_ptr<SpineParsedSkeleton> parsed);
    [[nodiscard]] spine::SkeletonData* skelData() const noexcept
    {
        return m_parsed ? m_parsed->skelData.get() : nullptr;
    }
    struct ClipperDeleter  { void operator()(spine::SkeletonClipping* p) const; };

    /// Build m_slotAlphaAnimated from the loaded skeleton data: marks every slot
    /// targeted by an alpha-bearing colour timeline (RGBA/Alpha/RGBA2) in any
    /// animation.  Such slots must NOT be alpha-culled in extractMeshes — a slot
    /// that fades through ~0 would otherwise pop in/out of the mesh and cause a
    /// one-frame GPU render glitch on the timeline (e.g. Modernia R_acc_light).
    void computeAlphaAnimatedSlots();

    std::vector<char>                                     m_slotAlphaAnimated;  ///< by slot index
    std::unordered_set<std::string>                       m_hiddenAttachmentNames;
    std::string                                           m_loadedSkelPath;
    std::string                                           m_loadedAtlasPath;
    // Declared before the skeleton/animation so it is destroyed after them.
    std::shared_ptr<SpineParsedSkeleton>                  m_parsed;
    std::unique_ptr<spine::Skeleton, SkelDeleter>         m_skeleton;
    std::unique_ptr<spine::SkeletonClipping, ClipperDeleter> m_clipper;
    SpineAnimation                                        m_animation;
    std::string                                           m_version;
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
