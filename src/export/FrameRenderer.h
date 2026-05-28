/*
 * FrameRenderer — Evaluates the timeline at a given frame and produces
 * a composited RGBA image via the GPU compositor.
 *
 * For each frame:
 *   1. Evaluate which clips are active at the current time
 *   2. Render each clip layer (Spine characters, video frames, titles, etc.)
 *   3. Apply per-clip effects
 *   4. Composite all layers via GPU compute shader
 *   5. Read back the composited pixels (or keep on GPU for NVENC zero-copy)
 *
 * The FrameRenderer is stateless per-frame — it does not cache between frames.
 * Caching is handled by SpineAnimationCache and the video decoder seek cache.
 */

#pragma once

#include <volk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt {

// Forward declarations
class Timeline;
class Project;
class Compositor;
class MediaPool;
class MediaSourceService;
class VideoUploader;
class EffectProcessor;
class VideoDecoder;
class Texture;
struct CachedFrame;

// ── Configuration ───────────────────────────────────────────────────────────

struct FrameRendererConfig
{
    uint32_t outputWidth{1920};
    uint32_t outputHeight{1080};
    double   fps{30.0};
    int      fpsNum{30};
    int      fpsDen{1};

    /// If true, render to GPU buffer only (for NVENC zero-copy).
    /// If false, also readback to CPU (for software encoding or image sequence).
    bool     gpuOnly{false};
};

// ── Rendered frame ──────────────────────────────────────────────────────────

/// Result of rendering a single frame.
struct RenderedFrame
{
    std::vector<uint8_t> pixels;       ///< BGRA pixels (empty if gpuOnly)
    uint32_t             width{0};
    uint32_t             height{0};
    int64_t              frameIndex{0};
    double               timestamp{0.0};  ///< Seconds

    /// True if valid (pixels are populated or GPU buffer is ready).
    [[nodiscard]] bool isValid() const noexcept { return width > 0 && height > 0; }
};

// ── Frame renderer statistics ───────────────────────────────────────────────

struct FrameRenderStats
{
    int64_t  framesRendered{0};
    double   totalRenderTimeMs{0.0};
    double   avgFrameTimeMs{0.0};
    double   lastFrameTimeMs{0.0};
    int      activeLayers{0};        ///< Layers in last composited frame
};

// ═════════════════════════════════════════════════════════════════════════════

class FrameRenderer
{
public:
    FrameRenderer();
    ~FrameRenderer();

    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Initialize renderer. Compositor must be initialized first.
    bool init(const FrameRendererConfig& config,
              Compositor* compositor = nullptr);

    /// Set optional MediaPool for decoding video frames.
    void setMediaPool(MediaPool* pool) { m_mediaPool = pool; }

    /// Set the shared MediaSourceService for source access.
    void setMediaSourceService(MediaSourceService* service) { m_mediaSources = service; }

    /// Set optional VideoUploader for GPU texture uploads.
    void setVideoUploader(VideoUploader* uploader) { m_videoUploader = uploader; }

    /// Set optional EffectProcessor for GPU per-clip effects.
    void setEffectProcessor(EffectProcessor* fx) { m_effectProcessor = fx; }

    /// Set optional Project for resolving nested SequenceClips.
    void setProject(const Project* proj) { m_project = proj; }

    /// Shutdown and release resources.
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Rendering ───────────────────────────────────────────────────────

    /// Render a single frame from the timeline at the given frame index.
    /// \param timeline   The timeline to evaluate
    /// \param frameIndex Zero-based frame number
    /// \return Rendered frame data (or invalid frame on error)
    [[nodiscard]] RenderedFrame renderFrame(const Timeline& timeline,
                                            int64_t frameIndex);

    /// Render at a specific time in seconds.
    [[nodiscard]] RenderedFrame renderAtTime(const Timeline& timeline,
                                             double timeSeconds);

    // ── Range rendering ─────────────────────────────────────────────────

    /// Render a range of frames, calling the callback for each.
    /// \return Number of frames successfully rendered.
    using FrameCallback = std::function<bool(const RenderedFrame& frame)>;
    int64_t renderRange(const Timeline& timeline,
                        int64_t startFrame, int64_t endFrame,
                        const FrameCallback& callback);

    // ── Configuration ───────────────────────────────────────────────────

    [[nodiscard]] const FrameRendererConfig& config() const noexcept { return m_config; }

    /// Change output resolution (re-creates compositor output if needed).
    bool setResolution(uint32_t width, uint32_t height);

    // ── Statistics ──────────────────────────────────────────────────────

    [[nodiscard]] const FrameRenderStats& stats() const noexcept { return m_stats; }
    void resetStats();

    // ── Error ───────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

private:
    /// Convert frame index to TimeTick.
    int64_t frameToTick(int64_t frameIndex) const noexcept;

    /// Evaluate timeline at tick, build compositor layers.
    /// When \p setOnCompositor is false, layers are built into \p outLayers
    /// but NOT submitted to the compositor (used for nested sequence recursion).
    int evaluateLayers(const Timeline& timeline, int64_t tick, int depth = 0,
                       bool setOnCompositor = true);

    /// Render a nested sequence timeline to a CPU BGRA frame.
    /// Composites the inner timeline, reads back the result, and returns it.
    /// Returns nullptr on failure.
    std::shared_ptr<CachedFrame> renderNestedFrame(
        const Timeline& nested, int64_t tick, int depth);

    /// After processSync(), snapshot the effect output to a per-layer texture
    /// so subsequent layers' effect processing doesn't overwrite this layer's
    /// result in EffectProcessor's shared ping-pong storage textures.
    /// Returns the snapshot descriptor info on success, or the fallback
    /// (EffectProcessor's outputDescriptorInfo) on failure.
    VkDescriptorImageInfo snapshottedEffectOutput(
        size_t layerIndex,
        const VkDescriptorImageInfo& fallback);

    static constexpr int kMaxNestDepth = 8;

    FrameRendererConfig m_config;
    const Project*      m_project{nullptr};         // Non-owning (for nested sequences)
    Compositor*         m_compositor{nullptr};      // Non-owning
    MediaPool*          m_mediaPool{nullptr};       // Non-owning
    MediaSourceService* m_mediaSources{nullptr};   // Shared service
    VideoUploader*      m_videoUploader{nullptr};   // Non-owning
    EffectProcessor*    m_effectProcessor{nullptr};  // Non-owning
    FrameRenderStats    m_stats;
    std::string         m_lastError;

    // Per-layer effect output snapshots — prevents multiple layers from
    // sharing EffectProcessor's internal ping-pong storage textures.
    // Indexed by layer index, lazy-allocated on first use.
    mutable std::vector<std::unique_ptr<Texture>> m_effectSnapshots;

    bool                m_initialized{false};
};

} // namespace rt
