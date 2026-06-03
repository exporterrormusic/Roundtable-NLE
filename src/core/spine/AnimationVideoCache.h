/*
 * AnimationVideoCache — manages pre-rendered Spine animation video files.
 *
 * Central registry mapping (character, outfit, animation) → pre-rendered
 * video files (.mp4 or .mov).  Provides:
 *   - Inventory scanning (discovers existing cached videos on startup)
 *   - Lookup by character identity and animation name
 *   - Background pre-rendering of missing animations via SpinePrerenderer
 *   - MediaPool integration for video decode during compositing
 *   - Staleness detection (re-renders when skeleton files change)
 *
 * Cache layout on disk:
 *   assets/converted/{FormatDir}/{CharName}/{outfit}/{animName}.ext
 *
 * Where FormatDir is one of: H264_Green, H264_Blue, H264_Custom, ProRes
 * and ext is .mp4 (chroma-key formats) or .mov (ProRes).
 *
 * Thread safety: all public methods are guarded by a mutex.
 * Background rendering happens on a worker thread; completed entries
 * are integrated via a callback on the main thread.
 */

#pragma once

#ifdef ROUNDTABLE_HAS_SPINE

#include "media/MediaPool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

class SpinePrerenderer;

/// Encoder format preference for new cache renders
enum class SpineCacheFormat : uint8_t
{
    GreenScreen,  ///< Character on bright green (#00FF00) background (.mp4)
    BlueScreen,   ///< Character on bright blue (#0000FF) background (.mp4)
    CustomColor,  ///< Character on user-chosen color background (.mp4)
    ProRes4444,   ///< ProRes 4444 (.mov) — intra-frame, native alpha
};

/// Information about a single cached animation video
struct AnimCacheEntry
{
    std::string           characterName;
    std::string           outfit;
    std::string           animationName;
    std::filesystem::path videoPath;        ///< Absolute path to .mov or .webm
    uint32_t              width{0};
    uint32_t              height{0};
    float                 duration{0.0f};   ///< Animation duration in seconds
    int64_t               frameCount{0};
    int                   fps{60};
    uint64_t              fileSizeBytes{0};
    MediaHandle           mediaHandle{0};   ///< Handle in MediaPool (0 = not opened)

    // Staleness tracking
    std::filesystem::file_time_type skelModTime{};
};

/// Status of a pre-render job
enum class PrerenderStatus : uint8_t
{
    Queued,
    InProgress,
    Completed,
    Failed
};

/// Callback when a pre-render job completes
/// (characterName, outfit, animationName, success)
using AnimCacheCompleteFn = std::function<void(const std::string&,
                                                const std::string&,
                                                const std::string&,
                                                bool)>;

class AnimationVideoCache
{
public:
    /// Create the cache.
    /// @param cacheDir  Root directory for cached videos
    ///                  (e.g., "assets/converted")
    /// @param assetsDir Root assets directory (containing "characters/")
    /// @param pool      MediaPool for video decode (non-owning)
    explicit AnimationVideoCache(MediaPool* pool = nullptr,
                                  const std::string& cacheDir = "assets/converted",
                                  const std::string& assetsDir = "assets");
    ~AnimationVideoCache();

    AnimationVideoCache(const AnimationVideoCache&) = delete;
    AnimationVideoCache& operator=(const AnimationVideoCache&) = delete;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set a callback for when background pre-renders complete.
    /// Called on the rendering thread — use QMetaObject::invokeMethod
    /// to bounce to the UI thread if needed.
    ///
    /// Thread-safe: the worker thread snapshots m_completeFn under m_mutex
    /// at the start of each job, so this setter must take the same lock to
    /// avoid a torn read of the std::function (D3).
    void setCompletionCallback(AnimCacheCompleteFn fn)
    {
        std::lock_guard lock(m_mutex);
        m_completeFn = std::move(fn);
    }

    /// Set the encoder format for new renders.
    /// Thread-safe: worker reads m_encoderFormat unlocked while rendering, so
    /// both the setter and the worker's read are serialised on m_mutex (D1).
    void setEncoderFormat(SpineCacheFormat fmt)
    {
        std::lock_guard lock(m_mutex);
        m_encoderFormat = fmt;
    }
    [[nodiscard]] SpineCacheFormat encoderFormat() const
    {
        std::lock_guard lock(m_mutex);
        return m_encoderFormat;
    }

    /// Set the chroma key background color (used for GreenScreen/BlueScreen/CustomColor formats).
    /// Thread-safe: worker reads these unlocked while rendering (D1).
    void setChromaKeyColor(uint8_t r, uint8_t g, uint8_t b)
    {
        std::lock_guard lock(m_mutex);
        m_chromaKeyR = r;
        m_chromaKeyG = g;
        m_chromaKeyB = b;
    }
    [[nodiscard]] uint8_t chromaKeyR() const { std::lock_guard lk(m_mutex); return m_chromaKeyR; }
    [[nodiscard]] uint8_t chromaKeyG() const { std::lock_guard lk(m_mutex); return m_chromaKeyG; }
    [[nodiscard]] uint8_t chromaKeyB() const { std::lock_guard lk(m_mutex); return m_chromaKeyB; }

    /// Set the MediaPool (can be set after construction, non-owning).
    void setMediaPool(MediaPool* pool) { m_mediaPool = pool; }

    // ── Inventory ───────────────────────────────────────────────────────

    /// Scan the cache directory for existing pre-rendered videos.
    /// Call on startup to populate the inventory.
    void scanCacheDirectory();

    /// @return Number of cached animation entries
    [[nodiscard]] size_t entryCount() const;

    // ── Lookup ──────────────────────────────────────────────────────────

    /// Check if a pre-rendered video exists for the given animation.
    [[nodiscard]] bool hasVideo(const std::string& characterName,
                                const std::string& outfit,
                                const std::string& animationName) const;

    /// Get the cache entry for an animation (nullptr if not cached).
    [[nodiscard]] const AnimCacheEntry* getEntry(const std::string& characterName,
                                                  const std::string& outfit,
                                                  const std::string& animationName) const;

    /// Get or open a MediaHandle for a cached video.
    /// Returns InvalidMedia if the video doesn't exist in cache.
    [[nodiscard]] MediaHandle getMediaHandle(const std::string& characterName,
                                              const std::string& outfit,
                                              const std::string& animationName);

    /// Check if any pre-rendered video exists for a character (any outfit/anim).
    [[nodiscard]] bool hasAnyForCharacter(const std::string& characterName) const;

    /// Count how many cached entries exist for a character (any outfit/anim).
    [[nodiscard]] size_t countForCharacter(const std::string& characterName) const;

    /// Count how many cached entries exist for a specific character + outfit.
    [[nodiscard]] size_t countForCharacterOutfit(const std::string& characterName,
                                                  const std::string& outfit) const;

    /// Return codec name for a character+outfit's cached files (e.g. "ProRes 4444", "DNxHD", "HEVC").
    /// Returns empty string if no cached entries exist for this character+outfit.
    [[nodiscard]] std::string codecForCharacterOutfit(const std::string& characterName,
                                                      const std::string& outfit) const;

    /// Get a decoded frame from a cached animation video.
    /// @param characterName  Character name
    /// @param outfit         Outfit name
    /// @param animationName  Animation name
    /// @param frameNumber    Frame index within the loop
    /// @return Decoded frame, or nullptr if not cached
    [[nodiscard]] std::shared_ptr<CachedFrame> getFrame(
        const std::string& characterName,
        const std::string& outfit,
        const std::string& animationName,
        int64_t frameNumber);

    // ── Pre-rendering ───────────────────────────────────────────────────

    /// Queue a pre-render job for a single animation.
    /// If already cached or in-progress, this is a no-op.
    /// @param talking  If true, renders with talk track blended → saved as {anim}_talk.mov
    void queueRender(const std::string& characterName,
                     const std::string& outfit,
                     const std::string& animationName,
                     bool talking = false);

    /// Queue pre-renders for all animations of a character/outfit.
    /// Discovers animation list from the skeleton file.
    void queueAllAnimations(const std::string& characterName,
                            const std::string& outfit);

    /// @return true if any background pre-renders are in progress
    [[nodiscard]] bool isRendering() const;

    /// @return number of jobs pending (queued + in-progress)
    [[nodiscard]] size_t pendingCount() const;

    /// @return true if any pending jobs exist for this character+outfit
    [[nodiscard]] bool hasPendingForCharacterOutfit(const std::string& characterName,
                                                     const std::string& outfit) const;

    /// @return human-readable description of the currently rendering job,
    ///         or empty string if idle.
    [[nodiscard]] std::string currentJobDescription() const;

    /// Wait for all queued pre-renders to complete (blocking).
    void waitForAll();

    /// Cancel all pending pre-render jobs.
    void cancelAll();

    // ── Maintenance ─────────────────────────────────────────────────────

    /// Close all media handles and clear the inventory.
    void clear();

    /// Remove a single cache entry (e.g. for corrupt/0-byte files).
    /// Also releases the media handle and deletes the file on disk.
    void removeEntry(const std::string& characterName,
                     const std::string& outfit,
                     const std::string& animationName);

    /// Remove all cache entries and files for a character.
    /// Releases media handles and deletes the cache directory from disk.
    void removeAllForCharacter(const std::string& characterName);

    /// Remove all cache entries and files for a single character outfit.
    /// Releases media handles and deletes the outfit subdirectory.  Does NOT
    /// touch the underlying Spine/Live2D assets in `assets/characters/`.
    void removeAllForCharacterOutfit(const std::string& characterName,
                                     const std::string& outfit);

    /// Check if cached videos are stale (skeleton modified after render)
    /// and queue re-renders for any stale entries.
    void recheckStaleness();

    /// Migrate legacy HEVC packed-alpha .mp4 cache files to ProRes 4444 .mov.
    /// Deletes old .mp4 files and queues background re-renders in ProRes format.
    /// Safe to call multiple times (no-op if no .mp4 files exist).
    void migrateToProRes();

    /// @return The root cache directory where animation video files are stored.
    [[nodiscard]] const std::filesystem::path& cacheDirectory() const noexcept
    {
        return m_cacheDir;
    }

    /// Map a SpineCacheFormat to its subdirectory name under the cache root.
    /// GreenScreen  → "H264_Green"
    /// BlueScreen   → "H264_Blue"
    /// CustomColor  → "H264_Custom"
    /// ProRes4444   → "ProRes"
    [[nodiscard]] static std::string formatDirName(SpineCacheFormat fmt) noexcept
    {
        switch (fmt) {
        case SpineCacheFormat::GreenScreen:  return "H264_Green";
        case SpineCacheFormat::BlueScreen:   return "H264_Blue";
        case SpineCacheFormat::CustomColor:  return "H264_Custom";
        case SpineCacheFormat::ProRes4444:   return "ProRes";
        }
        return "ProRes";
    }

private:
    /// Build cache key: "charName|outfit|animName"
    static std::string makeKey(const std::string& charName,
                                const std::string& outfit,
                                const std::string& animName);

    /// Build cache file path for a given animation and format.
    std::filesystem::path cachePath(const std::string& charName,
                                     const std::string& outfit,
                                     const std::string& animName,
                                     SpineCacheFormat fmt) const;

    /// Resolve (and lazily open) the MediaHandle for an entry.
    /// PRECONDITION: m_mutex is already held by the caller.  Keeping the
    /// lock held across both the handle lookup AND the subsequent decode in
    /// getFrame() prevents removeEntry()/clear() from releasing the handle
    /// mid-decode (a use-after-release in MediaPool — D2).
    MediaHandle getMediaHandleLocked(const std::string& characterName,
                                     const std::string& outfit,
                                     const std::string& animationName);

    /// Worker function for background render threads.
    /// Each worker owns its own SpinePrerenderer and processes jobs
    /// from the shared m_jobQueue concurrently.
    void workerLoop();

    /// Holds a single render job description (queued from main thread).
    struct RenderJob {
        std::string charName;
        std::string outfit;
        std::string animName;
        bool        isTalking{false};  ///< Render with talk track blended
    };

    // Configuration
    std::filesystem::path       m_cacheDir;
    std::string                 m_assetsDir;
    MediaPool*                   m_mediaPool{nullptr};
    AnimCacheCompleteFn         m_completeFn;
    SpineCacheFormat            m_encoderFormat{SpineCacheFormat::GreenScreen};

    // Chroma key background color (default: bright green #00FF00)
    uint8_t                     m_chromaKeyR{0};
    uint8_t                     m_chromaKeyG{255};
    uint8_t                     m_chromaKeyB{0};

    // Inventory: key → entry
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, AnimCacheEntry> m_entries;

    // In-progress / queued renders — concurrent worker thread pool
    std::unordered_set<std::string> m_pendingKeys;
    std::deque<RenderJob>           m_jobQueue;
    std::condition_variable         m_jobCv;
    std::vector<std::thread>        m_workerThreads;
    bool                            m_stopWorker{false};
    std::string                     m_currentJobDesc;  ///< Set by worker thread

    // Outfits explicitly deleted while a render may be in-flight.
    // Worker threads check this set before re-adding a completed render
    // to m_entries, and delete the rendered file instead if the outfit
    // was deleted mid-render.  Format: "charName|outfit"
    std::unordered_set<std::string> m_deletingOutfits;

    // Prerenderers are owned by each worker thread locally — not stored here.
};

} // namespace rt

#endif // ROUNDTABLE_HAS_SPINE
