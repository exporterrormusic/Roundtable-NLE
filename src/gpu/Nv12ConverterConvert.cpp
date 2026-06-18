/*
 * Nv12ConverterConvert.cpp — NV12 conversion functions for Nv12Converter.
 *
 * Extracted from Nv12Converter.cpp to reduce translation unit size.
 * Contains: convert(), convertSync(), ensureOutputSize(),
 *           convertSyncScaled(), convertFromVkBuffer()
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

//══════════════════════════════════════════════════════════════════════════
//  Conversion
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::convert(VkCommandBuffer cmd,
                             const uint8_t* yData, int yLinesize,
                             const uint8_t* uvData, int uvLinesize,
                             uint32_t width, uint32_t height,
                             std::vector<Texture::StagingCleanup>& stagingOut)
{
    if (!m_initialized) return false;

    // Auto-resize if dimensions changed
    if (width != m_config.width || height != m_config.height) {
        if (!resize(width, height)) return false;
    }

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;

    // ── Upload Y plane (R8, W × H) ─────────────────────────────────────
    {
        // Pack Y plane into contiguous buffer (skip linesize padding)
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(w) * h;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < h; ++row) {
            std::memcpy(yPacked.data() + row * w,
                        yData + row * yLinesize,
                        w);
        }

        Texture::StagingCleanup staging{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != w || m_yTexture.height() != h)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = w;
            cfg.height = h;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, staging))
            {
                spdlog::error("Nv12Converter: Y plane upload failed");
                return false;
            }
        } else {
            if (!m_yTexture.updateDataBatched(
                    yPacked.data(), ySize, cmd, staging))
            {
                spdlog::error("Nv12Converter: Y plane re-upload failed");
                return false;
            }
        }
        if (staging.buffer != VK_NULL_HANDLE)
            stagingOut.push_back(staging);
    }

    // ── Upload UV plane (RG8, W/2 × H/2) ───────────────────────────────
    {
        const uint32_t uvW = w / 2;
        const uint32_t uvH = h / 2;
        const VkDeviceSize uvSize = static_cast<VkDeviceSize>(uvW) * uvH * 2;
        std::vector<uint8_t> uvPacked(static_cast<size_t>(uvSize));
        for (uint32_t row = 0; row < uvH; ++row) {
            std::memcpy(uvPacked.data() + row * uvW * 2,
                        uvData + row * uvLinesize,
                        static_cast<size_t>(uvW) * 2);
        }

        Texture::StagingCleanup staging{};
        if (m_uvTexture.image() == VK_NULL_HANDLE ||
            m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
        {
            m_uvTexture.destroy();
            TextureConfig cfg;
            cfg.width  = uvW;
            cfg.height = uvH;
            cfg.format = VK_FORMAT_R8G8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uvTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uvPacked.data(), uvSize, cmd, staging))
            {
                spdlog::error("Nv12Converter: UV plane upload failed");
                return false;
            }
        } else {
            if (!m_uvTexture.updateDataBatched(
                    uvPacked.data(), uvSize, cmd, staging))
            {
                spdlog::error("Nv12Converter: UV plane re-upload failed");
                return false;
            }
        }
        if (staging.buffer != VK_NULL_HANDLE)
            stagingOut.push_back(staging);
    }

    // ── Barrier: transfer writes → compute reads ───────────────────────
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

    // ── Update descriptor set with current plane textures ───────────────
    {
        VkDescriptorImageInfo yInfo = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch compute shader ────────────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(w);
    pc.h = static_cast<int32_t>(h);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (w + 15) / 16;
    uint32_t gy = (h + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute writes → shader reads ─────────────────────────
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

bool Nv12Converter::convertSync(const uint8_t* yData, int yLinesize,
                                 const uint8_t* uvData, int uvLinesize,
                                 uint32_t width, uint32_t height)
{
    if (!m_initialized || !m_cmdPool) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    bool ok = convert(cmd, yData, yLinesize, uvData, uvLinesize,
                      width, height, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);

    for (auto& s : staging)
        s.destroy();

    return ok;
}

//══════════════════════════════════════════════════════════════════════════
//  ensureOutputSize — resize only the output texture (not input planes)
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::ensureOutputSize(uint32_t w, uint32_t h)
{
    if (m_outputTexture.image() != VK_NULL_HANDLE &&
        m_outputTexture.width() == w && m_outputTexture.height() == h)
        return true;

    GpuContext::get().scheduler().deviceWaitIdle();
    m_outputTexture.destroy();

    TextureConfig outCfg;
    outCfg.width  = w;
    outCfg.height = h;
    outCfg.format = VK_FORMAT_R8G8B8A8_UNORM;
    outCfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!m_outputTexture.create(m_allocator->handle(), m_device->handle(), outCfg))
        return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_outputTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Update config to reflect output dimensions (for readbackOutput)
    m_config.width  = w;
    m_config.height = h;
    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Phase 4.2 — ensureOutputSize16F: the RGBA16F output texture.
//  Tracks its OWN dimensions (m_output16W/H) and MUST NOT touch m_config —
//  the 8-bit readbackOutput reads m_config, so stomping it here would corrupt
//  an interleaved 8-bit readback (the export uses a dedicated converter, but
//  keeping them independent is the safe invariant).
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::ensureOutputSize16F(uint32_t w, uint32_t h)
{
    if (m_output16FTexture.image() != VK_NULL_HANDLE &&
        m_output16W == w && m_output16H == h)
        return true;

    GpuContext::get().scheduler().deviceWaitIdle();
    m_output16FTexture.destroy();

    TextureConfig outCfg;
    outCfg.width  = w;
    outCfg.height = h;
    outCfg.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    outCfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                  | VK_IMAGE_USAGE_SAMPLED_BIT
                  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!m_output16FTexture.create(m_allocator->handle(), m_device->handle(), outCfg))
        return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    m_output16FTexture.transitionLayout(
        cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    m_cmdPool->endSingleTime(cmd, m_queue);

    m_output16W = w;
    m_output16H = h;
    return true;
}

//══════════════════════════════════════════════════════════════════════════
//  Scaled NV12→BGRA (upload at srcW×srcH, output at dstW×dstH)
//══════════════════════════════════════════════════════════════════════════

// UPGRADE_PLAN Phase 4: this is the recording body of convertSyncScaled.
// Lifted into its own method so the GPU-resident prefetch path can record
// it into a caller-provided command buffer (alongside downstream
// vkCmdCopyImage to a per-frame destination texture) and submit once.
// convertSyncScaled below is now a thin wrapper that opens / closes its
// own single-time cmd around this call.  Behaviour is unchanged for the
// existing callers (convertSync, convertAndReadback*).
bool Nv12Converter::recordConvertScaled(
    VkCommandBuffer cmd,
    const uint8_t* yData, int yLinesize,
    const uint8_t* uvData, int uvLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<Texture::StagingCleanup>& stagingOut)
{
    if (!m_initialized) return false;
    if (cmd == VK_NULL_HANDLE) return false;

    // Ensure output texture is at target (downscaled) size.  Note this
    // can call vkDeviceWaitIdle + open its own single-time cmd via
    // m_cmdPool — those operations are independent of `cmd`.
    if (!ensureOutputSize(dstW, dstH)) return false;

    // ── Upload Y plane at source resolution ─────────────────────────────
    {
        const VkDeviceSize ySize = static_cast<VkDeviceSize>(srcW) * srcH;
        std::vector<uint8_t> yPacked(static_cast<size_t>(ySize));
        for (uint32_t row = 0; row < srcH; ++row)
            std::memcpy(yPacked.data() + row * srcW, yData + row * yLinesize, srcW);

        Texture::StagingCleanup stg{};
        if (m_yTexture.image() == VK_NULL_HANDLE ||
            m_yTexture.width() != srcW || m_yTexture.height() != srcH)
        {
            m_yTexture.destroy();
            TextureConfig cfg;
            cfg.width  = srcW;
            cfg.height = srcH;
            cfg.format = VK_FORMAT_R8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_yTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    yPacked.data(), ySize, cmd, stg)) {
                return false;
            }
        } else {
            if (!m_yTexture.updateDataBatched(yPacked.data(), ySize, cmd, stg)) {
                return false;
            }
        }
        if (stg.buffer != VK_NULL_HANDLE) stagingOut.push_back(stg);
    }

    // ── Upload UV plane at source resolution ────────────────────────────
    {
        const uint32_t uvW = srcW / 2;
        const uint32_t uvH = srcH / 2;
        const VkDeviceSize uvSize = static_cast<VkDeviceSize>(uvW) * uvH * 2;
        std::vector<uint8_t> uvPacked(static_cast<size_t>(uvSize));
        for (uint32_t row = 0; row < uvH; ++row)
            std::memcpy(uvPacked.data() + row * uvW * 2,
                        uvData + row * uvLinesize,
                        static_cast<size_t>(uvW) * 2);

        Texture::StagingCleanup stg{};
        if (m_uvTexture.image() == VK_NULL_HANDLE ||
            m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
        {
            m_uvTexture.destroy();
            TextureConfig cfg;
            cfg.width  = uvW;
            cfg.height = uvH;
            cfg.format = VK_FORMAT_R8G8_UNORM;
            cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!m_uvTexture.createFromDataBatched(
                    m_allocator->handle(), m_device->handle(), cfg,
                    uvPacked.data(), uvSize, cmd, stg)) {
                return false;
            }
        } else {
            if (!m_uvTexture.updateDataBatched(uvPacked.data(), uvSize, cmd, stg)) {
                return false;
            }
        }
        if (stg.buffer != VK_NULL_HANDLE) stagingOut.push_back(stg);
    }

    // ── Barrier: transfer → compute ─────────────────────────────────────
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

    // ── Update descriptors ──────────────────────────────────────────────
    {
        VkDescriptorImageInfo yInfo = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch at OUTPUT dimensions ───────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (dstW + 15) / 16;
    uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute → transfer ─────────────────────────────────────
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

bool Nv12Converter::convertSyncScaled(const uint8_t* yData, int yLinesize,
                                       const uint8_t* uvData, int uvLinesize,
                                       uint32_t srcW, uint32_t srcH,
                                       uint32_t dstW, uint32_t dstH)
{
    if (!m_initialized || !m_cmdPool) return false;

    // Same-size: delegate to non-scaled path
    if (srcW == dstW && srcH == dstH)
        return convertSync(yData, yLinesize, uvData, uvLinesize, srcW, srcH);

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;

    const bool ok = recordConvertScaled(cmd,
                                        yData, yLinesize,
                                        uvData, uvLinesize,
                                        srcW, srcH, dstW, dstH, staging);

    m_cmdPool->endSingleTime(cmd, m_queue);

    for (auto& s : staging)
        s.destroy();

    return ok;
}


//══════════════════════════════════════════════════════════════════════════
//  Convert from VkBuffer (GPU→GPU, zero PCIe transfers)
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::convertFromVkBuffer(VkCommandBuffer cmd,
                                         VkBuffer nv12Buffer,
                                         uint32_t width, uint32_t height,
                                         VkDeviceSize yOffset,  uint32_t yRowPitch,
                                         VkDeviceSize uvOffset, uint32_t uvRowPitch)
{
    if (!m_initialized) return false;

    // Auto-resize if dimensions changed
    if (width != m_config.width || height != m_config.height) {
        if (!resize(width, height)) return false;
    }

    const uint32_t w = m_config.width;
    const uint32_t h = m_config.height;

    // ── Ensure Y and UV textures exist ──────────────────────────────────
    if (m_yTexture.image() == VK_NULL_HANDLE ||
        m_yTexture.width() != w || m_yTexture.height() != h)
    {
        m_yTexture.destroy();
        TextureConfig cfg;
        cfg.width  = w;
        cfg.height = h;
        cfg.format = VK_FORMAT_R8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_yTexture.create(m_allocator->handle(), m_device->handle(), cfg))
            return false;
    }

    const uint32_t uvW = w / 2;
    const uint32_t uvH = h / 2;
    if (m_uvTexture.image() == VK_NULL_HANDLE ||
        m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
    {
        m_uvTexture.destroy();
        TextureConfig cfg;
        cfg.width  = uvW;
        cfg.height = uvH;
        cfg.format = VK_FORMAT_R8G8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_uvTexture.create(m_allocator->handle(), m_device->handle(), cfg))
            return false;
    }

    // ── Transition Y and UV textures to TRANSFER_DST ────────────────────
    VkImageLayout yOldLayout = m_yTexture.layout();
    VkImageLayout uvOldLayout = m_uvTexture.layout();

    m_yTexture.transitionLayout(cmd, yOldLayout,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_uvTexture.transitionLayout(cmd, uvOldLayout,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // ── Copy Y plane from VkBuffer → m_yTexture (R8, W×H) ──────────────
    {
        VkBufferImageCopy region{};
        region.bufferOffset    = yOffset;
        region.bufferRowLength = yRowPitch ? yRowPitch : 0; // 0 = tightly packed
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_yTexture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
    }

    // ── Copy UV plane from VkBuffer → m_uvTexture (RG8, W/2 × H/2) ─────
    {
        VkBufferImageCopy region{};
        region.bufferOffset    = uvOffset ? uvOffset
                                         : static_cast<VkDeviceSize>(w) * h;
        region.bufferRowLength = uvRowPitch ? (uvRowPitch / 2) : 0;
        // bufferRowLength is in TEXELS for the image format. RG8 = 2 bytes/texel.
        // If uvRowPitch is in bytes and format is RG8, row length in texels = pitch/2.
        // 0 means tightly packed.
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {uvW, uvH, 1};
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_uvTexture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);
    }

    // ── Transition Y and UV back to SHADER_READ_ONLY ────────────────────
    m_yTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_uvTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ── Barrier: transfer writes → compute reads ───────────────────────
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

    // ── Descriptor update + compute dispatch (shared with convert()) ────
    {
        VkDescriptorImageInfo yInfo  = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(w);
    pc.h = static_cast<int32_t>(h);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (w + 15) / 16;
    uint32_t gy = (h + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute writes → shader reads / transfer reads ─────────
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

//══════════════════════════════════════════════════════════════════════════
//  recordConvertFromBufferScaled — zero-copy NV12 → BGRA + downscale
//  See header.  Mirrors recordConvertScaled's structure but reads NV12
//  source planes from a Vulkan buffer (no CPU staging upload).
//══════════════════════════════════════════════════════════════════════════

bool Nv12Converter::recordConvertFromBufferScaled(
    VkCommandBuffer cmd,
    VkBuffer        nv12Buffer,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    VkDeviceSize yOffset,
    VkDeviceSize uvOffset)
{
    if (!m_initialized) return false;
    if (cmd == VK_NULL_HANDLE || nv12Buffer == VK_NULL_HANDLE) return false;

    if (!ensureOutputSize(dstW, dstH)) return false;

    // ── Y texture: R8, sized to srcW × srcH ─────────────────────────────
    if (m_yTexture.image() == VK_NULL_HANDLE ||
        m_yTexture.width() != srcW || m_yTexture.height() != srcH)
    {
        m_yTexture.destroy();
        TextureConfig cfg;
        cfg.width  = srcW;
        cfg.height = srcH;
        cfg.format = VK_FORMAT_R8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_yTexture.create(m_allocator->handle(),
                               m_device->handle(), cfg))
            return false;
    }

    // ── UV texture: RG8, sized to srcW/2 × srcH/2 ───────────────────────
    const uint32_t uvW = srcW / 2;
    const uint32_t uvH = srcH / 2;
    if (m_uvTexture.image() == VK_NULL_HANDLE ||
        m_uvTexture.width() != uvW || m_uvTexture.height() != uvH)
    {
        m_uvTexture.destroy();
        TextureConfig cfg;
        cfg.width  = uvW;
        cfg.height = uvH;
        cfg.format = VK_FORMAT_R8G8_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!m_uvTexture.create(m_allocator->handle(),
                                m_device->handle(), cfg))
            return false;
    }

    // ── Transition Y/UV: prev layout → TRANSFER_DST_OPTIMAL ─────────────
    const VkImageLayout yOldLayout  = m_yTexture.layout();
    const VkImageLayout uvOldLayout = m_uvTexture.layout();
    m_yTexture.transitionLayout(cmd, yOldLayout,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    m_uvTexture.transitionLayout(cmd, uvOldLayout,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // ── Copy Y plane: buffer offset 0 → texture ─────────────────────────
    {
        VkBufferImageCopy region{};
        region.bufferOffset                    = yOffset;
        region.bufferRowLength                 = 0;  // tightly packed
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageExtent                     = {srcW, srcH, 1};
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_yTexture.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // ── Copy UV plane: buffer offset srcW*srcH → texture ────────────────
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
        vkCmdCopyBufferToImage(cmd, nv12Buffer, m_uvTexture.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // ── Transition Y/UV: TRANSFER_DST → SHADER_READ_ONLY ────────────────
    m_yTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_uvTexture.transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // ── Barrier: transfer writes → compute reads ────────────────────────
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

    // ── Update descriptors (Y, UV, output) ──────────────────────────────
    {
        VkDescriptorImageInfo yInfo  = m_yTexture.descriptorInfo();
        VkDescriptorImageInfo uvInfo = m_uvTexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_outputTexture.imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &yInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &uvInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_descriptorSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[2].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);
    }

    // ── Dispatch at OUTPUT dimensions ──────────────────────────────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 15) / 16;
    const uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // ── Barrier: compute writes → shader reads / transfer reads ────────
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

} // namespace rt
