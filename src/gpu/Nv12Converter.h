/*
 * Nv12Converter — GPU compute-shader NV12 → BGRA color conversion.
 *
 * Replaces CPU-side sws_scale for the most common NVDEC output format.
 * Uses BT.709 color matrix (HDTV standard, correct for most H.264/H.265).
 *
 * Usage:
 *   1. Call init() once with the Vulkan context.
 *   2. Call convert() or convertSync() per frame.
 *   3. Read the BGRA result from outputDescriptorInfo() (for GPU consumers)
 *      or readbackOutput() (for CPU consumers).
 *
 * The converter owns two persistent textures (Y plane R8, UV plane RG8)
 * and one BGRA output storage image.  Dimensions are auto-resized.
 */

#pragma once

#include "vulkan/Device.h"
#include "vulkan/Allocator.h"
#include "vulkan/CommandPool.h"
#include "vulkan/Texture.h"
#include "vulkan/Buffer.h"

#include <cstdint>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>

namespace rt {

struct Nv12ConverterConfig
{
    uint32_t width{1920};
    uint32_t height{1080};
};

class Nv12Converter
{
public:
    Nv12Converter();
    ~Nv12Converter();

    Nv12Converter(const Nv12Converter&) = delete;
    Nv12Converter& operator=(const Nv12Converter&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    bool init(Device& device, Allocator& allocator, CommandPool& cmdPool,
              VkQueue computeQueue, const Nv12ConverterConfig& config = {});
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    // ── Conversion ──────────────────────────────────────────────────────

    /// Upload NV12 planes and record conversion dispatch into an external
    /// command buffer.  After submit + wait:
    ///   - outputDescriptorInfo() samples the BGRA result
    ///   - readbackOutput() copies BGRA pixels to CPU
    ///
    /// @param cmd        External command buffer (must be in recording state).
    /// @param yData      Y plane pixels (W × H bytes, one byte per pixel).
    /// @param yLinesize  Row stride of Y plane in bytes.
    /// @param uvData     UV plane pixels (W/2 × H/2 × 2 bytes, interleaved CB/CR).
    /// @param uvLinesize Row stride of UV plane in bytes.
    /// @param width      Frame width in pixels.
    /// @param height     Frame height in pixels.
    /// @param stagingOut Staging buffers that must be freed AFTER cmd completes.
    /// @return true on success.
    bool convert(VkCommandBuffer cmd,
                 const uint8_t* yData, int yLinesize,
                 const uint8_t* uvData, int uvLinesize,
                 uint32_t width, uint32_t height,
                 std::vector<Texture::StagingCleanup>& stagingOut);

    /// Synchronous version — allocates its own command buffer, submits, waits.
    bool convertSync(const uint8_t* yData, int yLinesize,
                     const uint8_t* uvData, int uvLinesize,
                     uint32_t width, uint32_t height);

    /// Synchronous NV12→BGRA with integrated downscale.
    /// Input planes are srcW × srcH; output BGRA is dstW × dstH.
    /// The compute shader samples input textures with bilinear filtering
    /// at normalized coordinates, producing a downscaled BGRA result.
    bool convertSyncScaled(const uint8_t* yData, int yLinesize,
                           const uint8_t* uvData, int uvLinesize,
                           uint32_t srcW, uint32_t srcH,
                           uint32_t dstW, uint32_t dstH);

    /// Record (only) NV12→BGRA + downscale into a caller-provided command
    /// buffer.  Does NOT submit and does NOT wait.  The caller is
    /// responsible for:
    ///   - calling ensureOutputSize(dstW, dstH) beforehand (or using the
    ///     overload below that does it for them);
    ///   - keeping the returned staging buffers alive until the submitted
    ///     command buffer completes, then calling StagingCleanup::destroy();
    ///   - synchronising the call across threads via the same m_apiMutex
    ///     that convertAndReadback* takes — this method does NOT lock.
    ///
    /// Used by the GPU-resident prefetch path (UPGRADE_PLAN Phase 4) so
    /// the prefetch worker can record convert + downstream vkCmdCopyImage
    /// into a single per-worker command buffer and submit once with a
    /// compositor-bound signal semaphore.
    bool recordConvertScaled(VkCommandBuffer cmd,
                             const uint8_t* yData, int yLinesize,
                             const uint8_t* uvData, int uvLinesize,
                             uint32_t srcW, uint32_t srcH,
                             uint32_t dstW, uint32_t dstH,
                             std::vector<Texture::StagingCleanup>& stagingOut);

    // ── YUV420P (3-plane) conversion ─────────────────────────────────

    /// Synchronous YUV420P→BGRA conversion.  3 separate planes.
    bool convertYuv420pSync(const uint8_t* yData, int yLinesize,
                            const uint8_t* uData, int uLinesize,
                            const uint8_t* vData, int vLinesize,
                            uint32_t width, uint32_t height);

    /// Synchronous YUV420P→BGRA with integrated downscale.
    bool convertYuv420pSyncScaled(const uint8_t* yData, int yLinesize,
                                  const uint8_t* uData, int uLinesize,
                                  const uint8_t* vData, int vLinesize,
                                  uint32_t srcW, uint32_t srcH,
                                  uint32_t dstW, uint32_t dstH);

    /// Record-only YUV420P→BGRA + downscale.  See recordConvertScaled
    /// for the full contract.
    bool recordConvertYuv420pScaled(VkCommandBuffer cmd,
                                    const uint8_t* yData, int yLinesize,
                                    const uint8_t* uData, int uLinesize,
                                    const uint8_t* vData, int vLinesize,
                                    uint32_t srcW, uint32_t srcH,
                                    uint32_t dstW, uint32_t dstH,
                                    std::vector<Texture::StagingCleanup>& stagingOut);

    /// Convert NV12 data from a Vulkan buffer (GPU→GPU, no CPU staging).
    /// Used for CUDA-Vulkan interop: NVDEC writes NV12 to a shared VkBuffer,
    /// then this method copies to Y/UV textures and dispatches the compute
    /// shader — all in GPU memory, zero PCIe transfers.
    ///
    /// @param cmd          External command buffer (recording state).
    /// @param nv12Buffer   VkBuffer containing NV12 data.
    /// @param width        Frame width in pixels.
    /// @param height       Frame height in pixels.
    /// @param yOffset      Byte offset of Y plane in the buffer.
    /// @param yRowPitch    Row pitch of Y plane in bytes (0 = tightly packed = width).
    /// @param uvOffset     Byte offset of UV plane in the buffer.
    /// @param uvRowPitch   Row pitch of UV plane in bytes (0 = tightly packed = width).
    /// @return true on success.
    bool convertFromVkBuffer(VkCommandBuffer cmd,
                             VkBuffer nv12Buffer,
                             uint32_t width, uint32_t height,
                             VkDeviceSize yOffset  = 0, uint32_t yRowPitch  = 0,
                             VkDeviceSize uvOffset = 0, uint32_t uvRowPitch = 0);

    /// Record (only) NV12 → BGRA with integrated downscale, reading the
    /// NV12 source planes from a Vulkan buffer (zero-copy from CUDA via
    /// CudaVulkanInterop) into the converter's internal Y/UV textures,
    /// then dispatching the convert shader into the output texture sized
    /// to dstW × dstH.  Caller's responsibilities mirror
    /// recordConvertScaled: lock apiMutex(), do not submit, supply the
    /// command buffer.
    ///
    /// Buffer layout (matches CudaVulkanInterop::copyNv12FromCuda):
    ///   Y plane  at yOffset,  tightly packed (pitch = srcW).
    ///   UV plane at uvOffset, tightly packed (pitch = srcW).
    /// Pass uvOffset = srcW*srcH for the standard NV12 layout the
    /// interop produces.
    bool recordConvertFromBufferScaled(VkCommandBuffer cmd,
                                       VkBuffer nv12Buffer,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH,
                                       VkDeviceSize yOffset  = 0,
                                       VkDeviceSize uvOffset = 0);

    // ── P010 (10-bit NV12) conversion — UPGRADE_PLAN item 4 ──────────
    //
    // P010 is NV12 with 10-bit samples stored in the upper bits of 16-bit
    // words.  NVDEC outputs P010 for HEVC 10-bit and AV1 10-bit streams;
    // software FFmpeg outputs AV_PIX_FMT_P010LE for the same.  Plane sizes
    // match NV12 (Y at W×H, UV at W/2 × H/2) but each sample is 2 bytes.
    //
    // The shader (p010_to_bgra.comp) applies a 10-bit-aware studio-swing
    // rescale before the BT.709 matrix; otherwise the GPU path is
    // structurally identical to NV12 — just R16/R16G16 textures.

    /// Record-only P010→BGRA + downscale.  CPU staging upload path.
    /// Linesizes are in BYTES (each plane stride from FFmpeg).
    bool recordConvertP010Scaled(VkCommandBuffer cmd,
                                  const uint8_t* yData, int yLinesize,
                                  const uint8_t* uvData, int uvLinesize,
                                  uint32_t srcW, uint32_t srcH,
                                  uint32_t dstW, uint32_t dstH,
                                  std::vector<Texture::StagingCleanup>& stagingOut);

    /// Record-only P010→BGRA + downscale, reading P010 source from a
    /// Vulkan buffer populated by CudaVulkanInterop::copyP010FromCuda.
    /// Buffer layout: Y at yOffset (W*H*2 bytes, tightly packed),
    ///                UV at uvOffset (W*H bytes for 4:2:0 chroma, tightly packed).
    bool recordConvertP010FromBufferScaled(VkCommandBuffer cmd,
                                            VkBuffer p010Buffer,
                                            uint32_t srcW, uint32_t srcH,
                                            uint32_t dstW, uint32_t dstH,
                                            VkDeviceSize yOffset  = 0,
                                            VkDeviceSize uvOffset = 0);

    /// Synchronous P010→BGRA with integrated downscale.  Convenience for
    /// the non-prefetch fallback path (CPU sws_scale alternative).
    bool convertP010SyncScaled(const uint8_t* yData, int yLinesize,
                                const uint8_t* uvData, int uvLinesize,
                                uint32_t srcW, uint32_t srcH,
                                uint32_t dstW, uint32_t dstH);

    /// Locked convert + readback (CPU pixels) for the prefetch worker
    /// fallback path.  Same pattern as convertAndReadbackNV12Scaled.
    bool convertAndReadbackP010Scaled(const uint8_t* yData, int yLinesize,
                                       const uint8_t* uvData, int uvLinesize,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH,
                                       std::vector<uint8_t>& outPixels);

    /// Phase 4.2 — convert P010 to RGBA16F (half-float) and read the raw
    /// halfs back to CPU (8 bytes/px, straight RGBA, non-linear BT.709 R'G'B').
    /// Feeds the export 16F passthrough packer (Rgba16fPack) so a 10-bit HEVC/
    /// AV1 master exports to real 10-bit instead of transiting 8-bit BGRA.
    /// Same locking discipline as the 8-bit variant; falls back (returns false)
    /// if the 16F shader/pipeline is unavailable.
    bool convertAndReadbackP010Scaled16F(const uint8_t* yData, int yLinesize,
                                          const uint8_t* uvData, int uvLinesize,
                                          uint32_t srcW, uint32_t srcH,
                                          uint32_t dstW, uint32_t dstH,
                                          std::vector<uint8_t>& outHalfRgba);

    // ── YUVA444P12 (ProRes 4444 — 4:4:4 10/12-bit + alpha) ───────────
    //
    // FFmpeg decodes ProRes 4444 to planar yuva444p12le: four FULL-resolution
    // planes (Y, U, V, A) with the 12-bit value in the LOW bits of a 16-bit
    // word (range 0..4095) — unlike P010 which uses the HIGH bits.  4:4:4 has
    // no chroma subsampling, and the real alpha plane retires the packed-alpha
    // stacked-tile hack for video characters (Wells).  Linesizes are in BYTES.
    bool recordConvertYuva444p12Scaled(VkCommandBuffer cmd,
                                       const uint8_t* yData, int yLinesize,
                                       const uint8_t* uData, int uLinesize,
                                       const uint8_t* vData, int vLinesize,
                                       const uint8_t* aData, int aLinesize,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH,
                                       std::vector<Texture::StagingCleanup>& stagingOut);

    bool convertYuva444p12SyncScaled(const uint8_t* yData, int yLinesize,
                                     const uint8_t* uData, int uLinesize,
                                     const uint8_t* vData, int vLinesize,
                                     const uint8_t* aData, int aLinesize,
                                     uint32_t srcW, uint32_t srcH,
                                     uint32_t dstW, uint32_t dstH);

    bool convertAndReadbackYuva444p12Scaled(const uint8_t* yData, int yLinesize,
                                            const uint8_t* uData, int uLinesize,
                                            const uint8_t* vData, int vLinesize,
                                            const uint8_t* aData, int aLinesize,
                                            uint32_t srcW, uint32_t srcH,
                                            uint32_t dstW, uint32_t dstH,
                                            std::vector<uint8_t>& outPixels);

    /// Phase 4.2 — convert ProRes 4444 (yuva444p12le) to RGBA16F and read the
    /// raw halfs back to CPU (8 bytes/px, straight RGBA + real straight alpha).
    /// Feeds the export 16F passthrough packer; see the P010 16F variant.
    bool convertAndReadbackYuva444p12Scaled16F(const uint8_t* yData, int yLinesize,
                                               const uint8_t* uData, int uLinesize,
                                               const uint8_t* vData, int vLinesize,
                                               const uint8_t* aData, int aLinesize,
                                               uint32_t srcW, uint32_t srcH,
                                               uint32_t dstW, uint32_t dstH,
                                               std::vector<uint8_t>& outHalfRgba);

    // ── Resize ──────────────────────────────────────────────────────────

    bool resize(uint32_t width, uint32_t height);

    // ── Output access ───────────────────────────────────────────────────

    [[nodiscard]] uint32_t outputWidth()  const noexcept { return m_config.width; }
    [[nodiscard]] uint32_t outputHeight() const noexcept { return m_config.height; }
    [[nodiscard]] VkDescriptorImageInfo outputDescriptorInfo() const;

    /// Raw output texture — needed for GPU→GPU image copy when producing
    /// GPU-resident CachedFrames (avoids CPU readback + re-upload).
    [[nodiscard]] const Texture& outputTexture() const noexcept { return m_outputTexture; }

    /// Read back BGRA pixels to CPU.  Synchronous (allocates cmd + waits).
    bool readbackOutput(std::vector<uint8_t>& outPixels);

    // ── Thread-safe combined convert + readback ────────────────────────
    //
    // Holds an internal mutex AND the GpuContext compute-queue mutex across
    // the convert + readback pair, so the result cannot be clobbered by a
    // racing caller between submit and copy-back. Prefetch workers (off the
    // compositor thread) use these to do NV12/YUV420P → BGRA + downscale on
    // the GPU instead of CPU sws_scale, then materialise the result back to
    // CPU pixels (still needed for FrameCache + disk cache + the compositor
    // upload path). Returns false on any failure — caller falls back to
    // sws_scale.
    bool convertAndReadbackNV12Scaled(const uint8_t* yData, int yLinesize,
                                       const uint8_t* uvData, int uvLinesize,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH,
                                       std::vector<uint8_t>& outPixels);

    bool convertAndReadbackYuv420pScaled(const uint8_t* yData, int yLinesize,
                                          const uint8_t* uData, int uLinesize,
                                          const uint8_t* vData, int vLinesize,
                                          uint32_t srcW, uint32_t srcH,
                                          uint32_t dstW, uint32_t dstH,
                                          std::vector<uint8_t>& outPixels);

    /// Public accessor on the converter's internal API mutex (the one that
    /// convertAndReadback* lock).  Required by external callers (e.g. the
    /// GPU-resident prefetch path, UPGRADE_PLAN Phase 4) that want to
    /// record into a caller-provided command buffer alongside downstream
    /// commands and submit once — the lock protects the shared internal
    /// textures (m_yTexture/m_uvTexture/m_uTexture/m_vTexture/
    /// m_outputTexture) and the descriptor sets from concurrent recorders.
    [[nodiscard]] std::mutex& apiMutex() const noexcept { return m_apiMutex; }

private:
    bool createTextures();
    bool createDescriptorResources();
    bool createPipeline();
    bool createYuv420pPipeline();
    bool createP010Pipeline();   // UPGRADE_PLAN item 4
    bool createYuva444p12Pipeline(); // ProRes 4444 (4:4:4 + alpha)
    bool ensureOutputSize(uint32_t w, uint32_t h);
    bool ensureP010Pipeline();   // lazy — first P010 frame triggers init
    bool ensureYuva444p12Pipeline(); // lazy — first ProRes 4444 frame

    // ── Phase 4.2 — RGBA16F output variants (export passthrough) ──────────
    // Additive: share the input textures / descriptor sets / pipeline layouts
    // with the 8-bit paths, differing only in the output texture
    // (R16G16B16A16_SFLOAT) and the bound pipeline.  ensureOutputSize16F
    // tracks its OWN dimensions (m_output16W/H) and must NOT touch m_config
    // (which readbackOutput reads).  The record bodies are shared with the
    // 8-bit path via the *Impl helpers below (out16f selects the output
    // texture + pipeline), so the 8-bit Vulkan command stream is unchanged.
    bool ensureOutputSize16F(uint32_t w, uint32_t h);
    bool ensureP010Pipeline16F();
    bool ensureYuva444p12Pipeline16F();
    bool readbackOutput16F(std::vector<uint8_t>& outHalfRgba);
    bool recordConvertP010ScaledImpl(
        VkCommandBuffer cmd,
        const uint8_t* yData, int yLinesize,
        const uint8_t* uvData, int uvLinesize,
        uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH,
        bool out16f, std::vector<Texture::StagingCleanup>& stagingOut);
    bool recordConvertYuva444p12ScaledImpl(
        VkCommandBuffer cmd,
        const uint8_t* yData, int yLinesize,
        const uint8_t* uData, int uLinesize,
        const uint8_t* vData, int vLinesize,
        const uint8_t* aData, int aLinesize,
        uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH,
        bool out16f, std::vector<Texture::StagingCleanup>& stagingOut);

    Device*      m_device{nullptr};
    Allocator*   m_allocator{nullptr};
    CommandPool* m_cmdPool{nullptr};
    VkQueue      m_queue{VK_NULL_HANDLE};

    Nv12ConverterConfig m_config;
    bool m_initialized{false};

    // Input planes (uploaded per frame)
    Texture m_yTexture;   // R8 UNORM, W × H
    Texture m_uvTexture;  // RG8 UNORM, W/2 × H/2 (NV12)
    Texture m_uTexture;   // R8 UNORM,  W/2 × H/2 (YUV420P Cb)
    Texture m_vTexture;   // R8 UNORM,  W/2 × H/2 (YUV420P Cr)

    // Output BGRA image (shared by NV12 and YUV420P)
    Texture m_outputTexture; // RGBA8 (BGRA swizzle in shader), W × H

    // NV12 descriptors
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // NV12 pipeline
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkShaderModule   m_shaderModule{VK_NULL_HANDLE};

    // YUV420P descriptors + pipeline
    VkDescriptorSetLayout m_yuv420pDescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_yuv420pDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_yuv420pDescSet{VK_NULL_HANDLE};
    VkPipelineLayout      m_yuv420pPipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_yuv420pPipeline{VK_NULL_HANDLE};
    VkShaderModule        m_yuv420pShaderModule{VK_NULL_HANDLE};

    // P010 (10-bit NV12) — separate textures so the converter can flip
    // between 8-bit and 10-bit streams without recreating R8/R16 images.
    // Descriptor set layout matches NV12 (2 samplers + 1 storage image),
    // but a distinct VkDescriptorSet keeps the R16 image views bound.
    Texture               m_p010YTexture;   // R16_UNORM,   W × H
    Texture               m_p010UvTexture;  // R16G16_UNORM, W/2 × H/2
    VkDescriptorSetLayout m_p010DescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_p010DescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_p010DescSet{VK_NULL_HANDLE};
    VkPipelineLayout      m_p010PipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_p010Pipeline{VK_NULL_HANDLE};
    VkShaderModule        m_p010ShaderModule{VK_NULL_HANDLE};

    // YUVA444P12 (ProRes 4444) — four FULL-resolution R16 planes + alpha.
    // No subsampled UV (4:4:4), plus a dedicated alpha plane.
    Texture               m_yuvaYTexture;   // R16_UNORM, W × H
    Texture               m_yuvaUTexture;   // R16_UNORM, W × H
    Texture               m_yuvaVTexture;   // R16_UNORM, W × H
    Texture               m_yuvaATexture;   // R16_UNORM, W × H (alpha)
    VkDescriptorSetLayout m_yuva444DescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_yuva444DescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_yuva444DescSet{VK_NULL_HANDLE};
    VkPipelineLayout      m_yuva444PipeLayout{VK_NULL_HANDLE};
    VkPipeline            m_yuva444Pipeline{VK_NULL_HANDLE};
    VkShaderModule        m_yuva444ShaderModule{VK_NULL_HANDLE};

    // Phase 4.2 — RGBA16F output (export passthrough).  Its OWN size
    // bookkeeping (NOT m_config), and one extra pipeline per high-bit-depth
    // format reusing the matching 8-bit descriptor set + pipeline layout.
    Texture        m_output16FTexture;             // VK_FORMAT_R16G16B16A16_SFLOAT
    uint32_t       m_output16W{0};
    uint32_t       m_output16H{0};
    VkPipeline     m_p010Pipeline16F{VK_NULL_HANDLE};
    VkShaderModule m_p010ShaderModule16F{VK_NULL_HANDLE};
    VkPipeline     m_yuva444Pipeline16F{VK_NULL_HANDLE};
    VkShaderModule m_yuva444ShaderModule16F{VK_NULL_HANDLE};

    // Serialises convertAndReadback* against concurrent prefetch-worker
    // calls. The plain convertSync*/readbackOutput methods are NOT
    // protected by this — callers using them directly must serialise
    // externally (today the only direct caller is dead code).
    mutable std::mutex m_apiMutex;
};

} // namespace rt
