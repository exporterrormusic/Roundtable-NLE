/*
 * SpineRenderer — Vulkan GPU renderer for Spine character meshes.
 *
 * Step 9: Takes the SpineRenderData produced by SpineEngine::extractMeshes()
 * and draws it using Vulkan with:
 *   - Dynamic vertex/index buffers (double-buffered for overlap)
 *   - Per-atlas-page texture binding via descriptor sets
 *   - Pre-multiplied alpha blending (PMA) pipeline
 *   - Additive / Multiply / Screen blend mode pipelines
 *   - Push-constant MVP + opacity
 *   - Offscreen Framebuffer rendering for compositing
 *   - Batch rendering: multiple skeletons in one renderFrame() call
 *   - Thread-safe skeleton evaluation (thread pool, one task per character)
 *
 * Lifecycle:
 *   1. Create SpineRenderer
 *   2. Call init() with Vulkan device, allocator, command pool, queue
 *   3. Call loadAtlasTextures() to upload PNG atlas pages to GPU
 *   4. Each frame:
 *      a. Call beginFrame()
 *      b. Call renderSkeleton() for each character (provides SpineRenderData)
 *      c. Call endFrame()
 *   5. Call shutdown() on destruction
 *
 * This class does NOT own SpineEngine instances — the caller manages those.
 */

#pragma once

#include "vulkan/Allocator.h"
#include "vulkan/Buffer.h"
#include "vulkan/CommandPool.h"

#include <mutex>
#include "vulkan/Device.h"
#include "vulkan/Framebuffer.h"
#include "vulkan/Pipeline.h"
#include "vulkan/Texture.h"

#include "spine/SpineEngine.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt {
struct CachedFrame;

// ── Configuration ───────────────────────────────────────────────────────────

struct SpineRendererConfig
{
    uint32_t maxVertices{65536};        ///< Max vertices per frame (auto-grows)
    uint32_t maxIndices{131072};        ///< Max indices per frame (auto-grows)
    uint32_t framesInFlight{2};        ///< Double-buffer count
    uint32_t renderWidth{1920};        ///< Offscreen framebuffer width
    uint32_t renderHeight{1080};       ///< Offscreen framebuffer height
    VkFormat colorFormat{VK_FORMAT_R8G8B8A8_UNORM};

    /// Queue families that need to sample the FBO (for zero-copy compositing).
    /// When non-empty, the FBO uses VK_SHARING_MODE_CONCURRENT.
    std::vector<uint32_t> concurrentQueueFamilies;
};

// ── Push constants (matches spine.vert) ─────────────────────────────────────

struct SpinePushConstants
{
    glm::mat4 mvp{1.0f};              ///< Model-View-Projection matrix
    float     opacity{1.0f};          ///< Layer opacity [0,1]
};
static_assert(sizeof(SpinePushConstants) <= 128,
              "Push constants must fit in 128 bytes (Vulkan minimum guarantee)");

// ── Per-frame GPU resources (double-buffered) ───────────────────────────────

struct SpineFrameResources
{
    Buffer       vertexBuffer;
    Buffer       indexBuffer;
    VkDeviceSize vertexCapacity{0};    ///< Current capacity in bytes
    VkDeviceSize indexCapacity{0};     ///< Current capacity in bytes
    size_t       vertexCount{0};       ///< Vertices written this frame
    size_t       indexCount{0};        ///< Indices written this frame
};

// ── Atlas texture handle ────────────────────────────────────────────────────

struct AtlasTextureSlot
{
    Texture         texture;
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    uint32_t        width{0};
    uint32_t        height{0};
    std::string     path;              ///< Source file path (for debugging)
};

// ── Render statistics ───────────────────────────────────────────────────────

struct SpineRenderStats
{
    uint32_t drawCalls{0};
    uint32_t vertexCount{0};
    uint32_t indexCount{0};
    uint32_t skeletonCount{0};
    uint32_t textureBindChanges{0};
    uint32_t blendModeChanges{0};
    float    gpuTimeMs{0.0f};          ///< GPU time from timestamp queries
};

// ═════════════════════════════════════════════════════════════════════════════

class SpineRenderer
{
public:
    SpineRenderer();
    ~SpineRenderer();

    SpineRenderer(const SpineRenderer&) = delete;
    SpineRenderer& operator=(const SpineRenderer&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /// Initialize the renderer with Vulkan resources.
    /// @param device       The Vulkan device (for queue, physical device info)
    /// @param allocator    VMA allocator for buffer/image allocation
    /// @param cmdPool      Command pool for one-shot uploads
    /// @param graphicsQueue Queue for rendering + transfers
    /// @param config       Renderer configuration
    /// @return true on success
    bool init(Device& device,
              Allocator& allocator,
              CommandPool& cmdPool,
              VkQueue graphicsQueue,
              const SpineRendererConfig& config = {});

    /// Shut down and release all GPU resources.
    void shutdown();

    /// @return true if initialized
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    /// Set an external mutex to lock around graphics queue submissions.
    /// Required when SpineRenderer is used from a background thread that
    /// shares the graphics queue with the main thread.
    void setQueueMutex(std::mutex* mtx) noexcept { m_queueMutex = mtx; }

    /// Register the compositor's compute queue so beginFrame() can drain
    /// any in-flight compute work that may still be sampling the shared
    /// framebuffer.  Required when the device exposes a separate
    /// async-compute queue family (e.g. NVIDIA family 2): the compositor
    /// samples m_framebuffer from the compute queue while Spine writes to
    /// it from the graphics queue, and the two are otherwise unordered.
    /// Pass VK_NULL_HANDLE / nullptr to disable.
    void setComputeQueue(VkQueue q, std::mutex* mtx) noexcept {
        m_computeQueue = q;
        m_computeQueueMutex = mtx;
    }

    // ── Texture management ──────────────────────────────────────────────

    /// Upload an atlas PNG texture from CPU pixel data.
    /// @param pageIndex  Index matching SpineRenderBatch::texturePageIndex
    /// @param pixelData  RGBA8 pixel data
    /// @param width      Texture width
    /// @param height     Texture height
    /// @param path       Source file path (for debugging)
    /// @return true on success
    bool uploadAtlasTexture(int pageIndex,
                            const void* pixelData,
                            uint32_t width, uint32_t height,
                            const std::string& path = "");

    /// Load atlas textures from PNG files listed in SpineAtlas pages.
    /// Uses stb_image to load each PNG.
    /// @param atlas  The loaded SpineAtlas
    /// @return Number of pages successfully loaded
    int loadAtlasTextures(const SpineAtlas& atlas);

    /// Check if a texture page is loaded.
    [[nodiscard]] bool hasTexture(int pageIndex) const;

    /// Release a specific texture page.
    void releaseTexture(int pageIndex);

    /// Release all loaded textures.
    void releaseAllTextures();

    // ── Frame rendering ─────────────────────────────────────────────────

    /// Begin a new frame. Returns false when the GPU/FBO is not ready.
    bool beginFrame();

    /// Render a single skeleton's mesh data to the offscreen framebuffer.
    /// May be called multiple times between beginFrame/endFrame for batch rendering.
    /// @param renderData  Mesh data from SpineEngine::extractMeshes()
    /// @param mvp         Model-View-Projection matrix for this skeleton
    /// @param opacity     Layer opacity [0,1]
    bool renderSkeleton(const SpineRenderData& renderData,
                        const glm::mat4& mvp,
                        float opacity = 1.0f);

    /// End the frame and submit all draw commands. Returns false on failure.
    bool endFrame();

    // ── Offscreen framebuffer ───────────────────────────────────────────

    /// Get the offscreen framebuffer (for compositing).
    [[nodiscard]] Framebuffer& framebuffer() noexcept { return m_framebuffer; }
    [[nodiscard]] const Framebuffer& framebuffer() const noexcept { return m_framebuffer; }

    /// Descriptor info for sampling the FBO output directly in another
    /// pipeline (e.g. the Compositor).  Avoids costly GPU→CPU readback.
    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const;

    /// Wait for the most recent endFrame() submission to complete.
    /// Use this before sampling the framebuffer from another queue.
    bool waitForFrame() const;

    /// Read back the current FBO pixels to a CachedFrame.
    /// Call AFTER waitForFrame(). Returns nullptr on failure.
    /// Used for multi-character export: render character 1, readback,
    /// beginFrame+render character 2 (zero-copy), composite both.
    std::shared_ptr<struct CachedFrame> readbackPixels();

    /// Resize the offscreen render target.
    bool resize(uint32_t width, uint32_t height);

    // ── Projection helpers ──────────────────────────────────────────────

    /// Build an orthographic projection matrix matching Spine's
    /// coordinate system (Y-up, origin at center).
    /// @param width   Viewport width in pixels
    /// @param height  Viewport height in pixels
    /// @param centerX Camera center X
    /// @param centerY Camera center Y
    /// @param zoom    Zoom factor (1.0 = no zoom)
    [[nodiscard]] static glm::mat4 orthoProjection(
        float width, float height,
        float centerX = 0.0f, float centerY = 0.0f,
        float zoom = 1.0f);

    /// Build a model matrix for a skeleton at a given position/scale.
    [[nodiscard]] static glm::mat4 modelMatrix(
        float posX, float posY,
        float scaleX = 1.0f, float scaleY = 1.0f,
        float rotationDeg = 0.0f);

    // ── Statistics ──────────────────────────────────────────────────────

    /// Get stats for the last completed frame.
    [[nodiscard]] const SpineRenderStats& lastFrameStats() const noexcept { return m_stats; }

    /// Reset accumulated stats.
    void resetStats() noexcept { m_stats = {}; }

private:
    // ── Setup ───────────────────────────────────────────────────────────
    bool createPipelines();
    bool createDescriptorPool();
    bool createFrameResources();
    bool createTimestampQueries();

    // ── Per-frame helpers ───────────────────────────────────────────────
    void ensureBufferCapacity(SpineFrameResources& frame,
                              size_t vertexBytes, size_t indexBytes);
    void bindPipelineForBlendMode(VkCommandBuffer cmd, SpineBlendMode mode);

    VkDescriptorSet allocateDescriptorSet(const Texture& tex);

    // ── State ───────────────────────────────────────────────────────────
    bool m_initialized{false};
    SpineRendererConfig m_config;

    // Vulkan handles (non-owning)
    VkDevice      m_vkDevice{VK_NULL_HANDLE};
    VkQueue       m_graphicsQueue{VK_NULL_HANDLE};
    VmaAllocator  m_vmaAllocator{nullptr};
    CommandPool*  m_cmdPool{nullptr};
    std::mutex*   m_queueMutex{nullptr};  ///< Optional: lock around vkQueueSubmit

    // Compositor's compute queue.  Used by beginFrame() to drain in-flight
    // compute work before transitioning the shared framebuffer back to
    // COLOR_ATTACHMENT — see setComputeQueue() doc.
    VkQueue       m_computeQueue{VK_NULL_HANDLE};
    std::mutex*   m_computeQueueMutex{nullptr};

    // Pipelines (one per blend mode)
    PipelineManager     m_pipelineMgr;
    VkPipelineLayout    m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline          m_pipelineNormal{VK_NULL_HANDLE};
    VkPipeline          m_pipelineAdditive{VK_NULL_HANDLE};
    VkPipeline          m_pipelineMultiply{VK_NULL_HANDLE};
    VkPipeline          m_pipelineScreen{VK_NULL_HANDLE};
    VkShaderModule      m_vertShader{VK_NULL_HANDLE};
    VkShaderModule      m_fragShader{VK_NULL_HANDLE};

    // Descriptor set layout + pool
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};

    // Double-buffered frame resources
    std::vector<SpineFrameResources> m_frameResources;
    uint32_t m_currentFrame{0};

    // Offscreen framebuffer
    Framebuffer m_framebuffer;

    // Linear sampler for reading the FBO output in downstream pipelines
    VkSampler m_outputSampler{VK_NULL_HANDLE};

    // Atlas textures (indexed by page)
    std::unordered_map<int, AtlasTextureSlot> m_atlasTextures;

    // Timestamp queries
    VkQueryPool m_timestampPool{VK_NULL_HANDLE};
    float       m_timestampPeriod{0.0f};

    // Render state (valid between beginFrame/endFrame)
    VkCommandBuffer  m_activeCmdBuffer{VK_NULL_HANDLE};
    VkFence          m_frameFence{VK_NULL_HANDLE};
    SpineBlendMode   m_currentBlendMode{SpineBlendMode::Normal};
    int              m_currentTexturePageIndex{-1};

    // Stats
    SpineRenderStats m_stats;
};

} // namespace rt
