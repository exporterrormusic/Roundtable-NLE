/*
 * Nv12ConverterP010.cpp — P010 (10-bit NV12) conversion methods.
 *
 * UPGRADE_PLAN item 4: Format-agnostic GPU-resident decode.
 *
 * P010 is NV12's 10-bit sibling.  NVDEC outputs P010 for HEVC 10-bit and
 * AV1 10-bit streams; software FFmpeg outputs AV_PIX_FMT_P010LE for the
 * same.  Plane layout matches NV12 (Y at full W×H, UV at W/2 × H/2,
 * 4:2:0 chroma subsampling) but each sample is a 16-bit word with the
 * 10-bit value stored in the upper 10 bits (bottom 6 bits zero).
 *
 * The Vulkan format that matches this exactly is R16_UNORM (and
 * R16G16_UNORM for the interleaved UV plane).  The sampler returns the
 * uint16 normalized as data/65535, which puts the 10-bit data at
 * approximately data/1024 — accuracy the BT.709 matrix tolerates without
 * issue, with a 10-bit-aware studio-swing rescale in p010_to_bgra.comp.
 *
 * Structural mirror of Nv12ConverterConvert.cpp:
 *   - recordConvertP010Scaled       (CPU staging upload)
 *   - recordConvertP010FromBufferScaled (CUDA zero-copy buffer copy)
 *   - convertP010SyncScaled         (self-contained sync helper)
 *   - convertAndReadbackP010Scaled  (locked convert + CPU readback)
 *
 * The shared output texture (m_outputTexture, BGRA8) and the descriptor
 * write pattern match the NV12 path — only the input plane textures
 * (R16/R16G16 instead of R8/R8G8) and the bound pipeline differ.
 */

#include <volk.h>
#include "Nv12Converter.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────
// Internal helper: pack a single 16-bit plane (Y is R16, UV is R16G16) into
// a contiguous staging buffer, skipping FFmpeg's linesize padding.  Width
// is the plane width in TEXELS (Y: pixel count, UV: chroma pair count).
// bytesPerTexel must be 2 for R16 and 4 for R16G16.
// ─────────────────────────────────────────────────────────────────────────
static void pack16BitPlane(std::vector<uint8_t>& dst,
                            const uint8_t* src, int srcLineBytes,
                            uint32_t widthTexels, uint32_t height,
                            uint32_t bytesPerTexel)
{
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(widthTexels)
                                * bytesPerTexel;
    dst.resize(static_cast<size_t>(rowBytes) * height);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst.data() + row * rowBytes,
                    src + static_cast<ptrdiff_t>(row) * srcLineBytes,
                    static_cast<size_t>(rowBytes));
    }
}

bool Nv12Converter::recordConvertP010Scaled(
    VkCommandBuffer cmd,
    const uint8_t* yData,  int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<Texture::StagingCleanup>& stagingOut)
{
    // 8-bit BGRA output (the original path) — out16f=false reproduces the
    // exact Vulkan command stream this function always emitted.
    return recordConvertP010ScaledImpl(cmd, yData, yLinesize, uvData, uvLinesize,
                                       srcW, srcH, dstW, dstH,
                                       /*out16f=*/false, stagingOut);
}

bool Nv12Converter::recordConvertP010ScaledImpl(
    VkCommandBuffer cmd,
    const uint8_t* yData,  int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    bool out16f,
    std::vector<Texture::StagingCleanup>& stagingOut)
{
    if (!m_initialized) return false;
    if (cmd == VK_NULL_HANDLE) return false;
    // The 8-bit and 16F paths differ ONLY in which pipeline + output texture
    // they target; the R16 input planes, descriptor set, and pipeline layout
    // are shared (the layout is format-agnostic).
    if (out16f) {
        if (!ensureP010Pipeline16F()) return false;
        if (!ensureOutputSize16F(dstW, dstH)) return false;
    } else {
        if (!ensureP010Pipeline()) return false;
        if (!ensureOutputSize(dstW, dstH)) return false;
    }
    const VkImageView outputView =
        out16f ? m_output16FTexture.imageView() : m_outputTexture.imageView();
    const VkPipeline  outputPipe =
        out16f ? m_p010Pipeline16F : m_p010Pipeline;

    // ── Y texture: R16_UNORM at srcW × srcH ───────────────────────────
    {
        std::vector<uint8_t> packed;
        pack16BitPlane(packed, yData, yLinesize, srcW, srcH, 2);

        Texture::StagingCleanup stg{};
        if (m_p010YTexture.image() == VK_NULL_HANDLE ||
            m_p010YTexture.width() != srcW || m_p010YTexture.height() != srcH)
        {
            m_p010YTexture.destroy();
            TextureConfig cfg;
            cfg.width  = srcW;
            cfg.height = srcH;
            cfg.format = VK_FORMAT_R16_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_p010YTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    packed.data(),
                    static_cast<VkDeviceSize>(packed.size()),
                    cmd, stg))
                return false;
        } else {
            if (!m_p010YTexture.updateDataBatched(
                    packed.data(),
                    static_cast<VkDeviceSize>(packed.size()),
                    cmd, stg))
                return false;
        }
        if (stg.buffer != VK_NULL_HANDLE) stagingOut.push_back(stg);
    }

    // ── UV texture: R16G16_UNORM at srcW/2 × srcH/2 ───────────────────
    {
        const uint32_t uvW = srcW / 2;
        const uint32_t uvH = srcH / 2;
        std::vector<uint8_t> packed;
        // 4 bytes per UV texel (R16+G16)
        pack16BitPlane(packed, uvData, uvLinesize, uvW, uvH, 4);

        Texture::StagingCleanup stg{};
        if (m_p010UvTexture.image() == VK_NULL_HANDLE ||
            m_p010UvTexture.width() != uvW || m_p010UvTexture.height() != uvH)
        {
            m_p010UvTexture.destroy();
            TextureConfig cfg;
            cfg.width  = uvW;
            cfg.height = uvH;
            cfg.format = VK_FORMAT_R16G16_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_p010UvTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    packed.data(),
                    static_cast<VkDeviceSize>(packed.size()),
                    cmd, stg))
                return false;
        } else {
            if (!m_p010UvTexture.updateDataBatched(
                    packed.data(),
                    static_cast<VkDeviceSize>(packed.size()),
                    cmd, stg))
                return false;
        }
        if (stg.buffer != VK_NULL_HANDLE) stagingOut.push_back(stg);
    }

    // ── Barrier: transfer writes → compute reads ──────────────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ── Update P010 descriptor set ────────────────────────────────────
    {
        VkDescriptorImageInfo yInfo  = m_p010YTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_p010UvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = outputView;
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_p010DescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_p010DescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_p010DescSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch at OUTPUT dimensions ─────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, outputPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_p010PipeLayout, 0, 1, &m_p010DescSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_p010PipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 15) / 16;
    const uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute writes → shader/transfer reads ───────────────
    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    return true;
}

bool Nv12Converter::recordConvertP010FromBufferScaled(
    VkCommandBuffer cmd,
    VkBuffer        p010Buffer,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    VkDeviceSize yOffset,
    VkDeviceSize uvOffset)
{
    if (!m_initialized) return false;
    if (cmd == VK_NULL_HANDLE || p010Buffer == VK_NULL_HANDLE) return false;
    if (!ensureP010Pipeline()) return false;
    if (!ensureOutputSize(dstW, dstH)) return false;

    // ── Ensure Y texture: R16_UNORM at srcW × srcH ────────────────────
    if (m_p010YTexture.image() == VK_NULL_HANDLE ||
        m_p010YTexture.width() != srcW || m_p010YTexture.height() != srcH)
    {
        m_p010YTexture.destroy();
        TextureConfig cfg;
        cfg.width  = srcW;
        cfg.height = srcH;
        cfg.format = VK_FORMAT_R16_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_p010YTexture.create(m_allocator->handle(),
                                    m_device->handle(), cfg))
            return false;
    }

    // ── Ensure UV texture: R16G16_UNORM at srcW/2 × srcH/2 ────────────
    const uint32_t uvW = srcW / 2;
    const uint32_t uvH = srcH / 2;
    if (m_p010UvTexture.image() == VK_NULL_HANDLE ||
        m_p010UvTexture.width() != uvW || m_p010UvTexture.height() != uvH)
    {
        m_p010UvTexture.destroy();
        TextureConfig cfg;
        cfg.width  = uvW;
        cfg.height = uvH;
        cfg.format = VK_FORMAT_R16G16_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_p010UvTexture.create(m_allocator->handle(),
                                     m_device->handle(), cfg))
            return false;
    }

    // ── Transition Y/UV → TRANSFER_DST_OPTIMAL ────────────────────────
    const VkImageLayout yOldLayout  = m_p010YTexture.layout();
    const VkImageLayout uvOldLayout = m_p010UvTexture.layout();
    m_p010YTexture.transitionLayout(cmd, yOldLayout,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_p010UvTexture.transitionLayout(cmd, uvOldLayout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // ── Copy Y plane (W × H × 2 bytes tight-packed) ──────────────────
    {
        VkBufferImageCopy region{};
        region.bufferOffset                    = yOffset;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageExtent                     = {srcW, srcH, 1};
        vkCmdCopyBufferToImage(cmd, p010Buffer, m_p010YTexture.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // ── Copy UV plane (W/2 × H/2 × 4 bytes tight-packed) ─────────────
    {
        VkBufferImageCopy region{};
        region.bufferOffset                    = uvOffset;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageExtent                     = {uvW, uvH, 1};
        vkCmdCopyBufferToImage(cmd, p010Buffer, m_p010UvTexture.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // ── Transition Y/UV → SHADER_READ_ONLY_OPTIMAL ───────────────────
    m_p010YTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_p010UvTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    // ── Update descriptors ────────────────────────────────────────────
    {
        VkDescriptorImageInfo yInfo  = m_p010YTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_p010UvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_p010DescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_p010DescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_p010DescSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch ─────────────────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_p010Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_p010PipeLayout, 0, 1, &m_p010DescSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_p010PipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 15) / 16;
    const uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
              | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    return true;
}

bool Nv12Converter::convertP010SyncScaled(const uint8_t* yData,  int yLinesize,
                                           const uint8_t* uvData, int uvLinesize,
                                           uint32_t srcW, uint32_t srcH,
                                           uint32_t dstW, uint32_t dstH)
{
    if (!m_initialized || !m_cmdPool) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    const bool ok = recordConvertP010Scaled(cmd, yData, yLinesize,
                                             uvData, uvLinesize,
                                             srcW, srcH, dstW, dstH, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);
    for (auto& s : staging) s.destroy();
    return ok;
}

bool Nv12Converter::convertAndReadbackP010Scaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());
    if (!convertP010SyncScaled(yData, yLinesize, uvData, uvLinesize,
                                srcW, srcH, dstW, dstH))
        return false;
    return readbackOutput(outPixels);
}

bool Nv12Converter::convertAndReadbackP010Scaled16F(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outHalfRgba)
{
    if (!m_initialized || !m_cmdPool) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    const bool ok = recordConvertP010ScaledImpl(
        cmd, yData, yLinesize, uvData, uvLinesize,
        srcW, srcH, dstW, dstH, /*out16f=*/true, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);
    for (auto& s : staging) s.destroy();
    if (!ok) return false;
    return readbackOutput16F(outHalfRgba);
}

} // namespace rt
