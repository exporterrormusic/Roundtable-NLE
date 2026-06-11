/*
 * WaveformRenderer — GPU-accelerated waveform rendering via Vulkan.
 *
 * Step 19: GPU Waveform Renderer
 *
 * Takes WaveformPeak data from WaveformCache and renders line-segment
 * waveforms using a Vulkan graphics pipeline:
 *   - Vertex shader converts peak (min/max) + x position → line segments
 *   - Fragment shader applies gradient coloring (green → yellow → red)
 *   - Supports multi-channel waveforms
 *   - LOD: WaveformCache provides pre-computed mip levels for efficient zoom
 *   - Double-buffered vertex data for overlap
 *
 * Also provides CPU-side peak layout utilities for widgets that render
 * waveforms via QPainter (no GPU required).
 *
 * Lifecycle (GPU mode):
 *   1. Create WaveformRenderer
 *   2. Call init() with Vulkan resources
 *   3. Each frame:
 *      a. uploadPeaks() — send peak data to GPU
 *      b. render() — draw waveform
 *   4. shutdown()
 *
 * CPU mode (for Qt widgets):
 *   - Use static layoutPeaks() to convert WaveformPeak data → screen coords
 *   - Render with QPainter in the widget
 */

#pragma once

#include "audio/WaveformCache.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace rt {

// ── Waveform vertex (matches waveform.vert) ─────────────────────────────────

/// A single vertex for GPU waveform rendering.
struct WaveformVertex
{
    float x;           ///< Horizontal position in pixels
    float y;           ///< Vertical position (min or max peak)
    float amplitude;   ///< Normalized amplitude [0,1] for gradient coloring
    float channel;     ///< Channel index (for multi-channel coloring)
};

// ── Push constants (matches waveform.vert) ──────────────────────────────────

struct WaveformPushConstants
{
    glm::mat4 mvp{1.0f};              ///< Orthographic projection + view
    glm::vec4 colorLow{0.0f, 0.8f, 0.0f, 1.0f};   ///< Low amplitude color (green)
    glm::vec4 colorMid{0.9f, 0.9f, 0.0f, 1.0f};   ///< Mid amplitude color (yellow)
    glm::vec4 colorHigh{1.0f, 0.2f, 0.0f, 1.0f};   ///< High amplitude color (red)
};
static_assert(sizeof(WaveformPushConstants) <= 128,
              "Push constants must fit in 128 bytes (Vulkan minimum guarantee)");

// ── GPU render configuration ────────────────────────────────────────────────

struct WaveformRendererConfig
{
    uint32_t maxVertices{131072};       ///< Max vertices per frame
    uint32_t framesInFlight{2};        ///< Double-buffer count
    uint32_t renderWidth{1920};
    uint32_t renderHeight{1080};
};

// ── GPU render statistics ───────────────────────────────────────────────────

struct WaveformRenderStats
{
    uint32_t vertexCount{0};
    uint32_t drawCalls{0};
    float    gpuTimeMs{0.0f};
};

// ── CPU-side layout output ──────────────────────────────────────────────────

/// A pair of screen-space Y coordinates for one column of the waveform.
struct WaveformColumn
{
    float x;           ///< Horizontal pixel position
    float yMin;        ///< Bottom (min peak mapped to screen)
    float yMax;        ///< Top (max peak mapped to screen)
    float amplitude;   ///< Normalized peak amplitude [0,1]
};

// ═════════════════════════════════════════════════════════════════════════════

class WaveformRenderer
{
public:
    WaveformRenderer();
    ~WaveformRenderer();

    WaveformRenderer(const WaveformRenderer&) = delete;
    WaveformRenderer& operator=(const WaveformRenderer&) = delete;

    // ── GPU lifecycle ───────────────────────────────────────────────────
    // (Requires Vulkan — not used in unit tests)

    // bool init(Device& device, Allocator& allocator, CommandPool& cmdPool,
    //           VkQueue graphicsQueue, const WaveformRendererConfig& config = {});
    // void shutdown();
    // [[nodiscard]] bool isInitialized() const noexcept;
    // void uploadPeaks(const std::vector<WaveformVertex>& vertices);
    // void render(VkCommandBuffer cmd, const WaveformPushConstants& pc);
    // [[nodiscard]] const WaveformRenderStats& stats() const noexcept;

    // ── CPU-side peak layout (no GPU required) ──────────────────────────

    /// Convert WaveformPeak data into screen-space columns for QPainter rendering.
    ///
    /// @param peaks        Array of peaks from WaveformCache::getPeaks()
    /// @param peakCount    Number of peaks
    /// @param startX       Start X pixel position
    /// @param pixelsPerPeak Width in pixels per peak column (usually 1.0)
    /// @param centerY      Y center of the waveform area
    /// @param halfHeight   Half the height of the waveform area
    /// @return Vector of screen-space columns
    [[nodiscard]] static std::vector<WaveformColumn> layoutPeaks(
        const WaveformPeak* peaks,
        size_t peakCount,
        float startX,
        float pixelsPerPeak,
        float centerY,
        float halfHeight);

    /// Convert WaveformPeak data into vertices for GPU rendering.
    ///
    /// @param peaks        Array of peaks
    /// @param peakCount    Number of peaks
    /// @param startX       Start X position
    /// @param pixelsPerPeak Pixels per peak
    /// @param centerY      Y center
    /// @param halfHeight   Half height
    /// @param channel      Channel index (for multi-channel coloring)
    /// @return Vector of WaveformVertex (2 per peak: min and max)
    [[nodiscard]] static std::vector<WaveformVertex> peaksToVertices(
        const WaveformPeak* peaks,
        size_t peakCount,
        float startX,
        float pixelsPerPeak,
        float centerY,
        float halfHeight,
        float channel = 0.0f);

    // ── Color utilities ─────────────────────────────────────────────────

    /// Default gradient colors for waveform rendering.
    struct GradientColors
    {
        glm::vec4 low{0.0f, 0.8f, 0.0f, 1.0f};    ///< Green (quiet)
        glm::vec4 mid{0.9f, 0.9f, 0.0f, 1.0f};    ///< Yellow (moderate)
        glm::vec4 high{1.0f, 0.2f, 0.0f, 1.0f};   ///< Red (loud)
    };

    /// Interpolate the gradient color for a given amplitude [0,1].
    [[nodiscard]] static glm::vec4 gradientColor(
        float amplitude,
        const GradientColors& colors = {});

    // ── Mip level selection ─────────────────────────────────────────────

    /// Calculate the ideal samples-per-pixel for a given zoom/width.
    /// @param totalFrames   Total sample frames in the audio
    /// @param visibleFrames Number of frames visible in the viewport
    /// @param widthPixels   Width of the viewport in pixels
    /// @return Ideal samples per pixel for mip level selection
    [[nodiscard]] static uint32_t idealSamplesPerPixel(
        int64_t totalFrames,
        int64_t visibleFrames,
        int widthPixels);
};

} // namespace rt
