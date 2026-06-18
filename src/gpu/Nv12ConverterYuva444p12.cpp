/*
 * Nv12ConverterYuva444p12.cpp — ProRes 4444 (yuva444p12le) conversion.
 *
 * ProRes 4444 decodes (via FFmpeg software decode — there is no NVDEC for
 * ProRes) to planar yuva444p12le: four FULL-resolution planes (Y, U, V, A),
 * each a 16-bit little-endian word with the 12-bit value in the LOW bits
 * (range 0..4095).  No chroma subsampling (4:4:4) and a real alpha plane.
 *
 * Structural mirror of Nv12ConverterP010.cpp, but with four R16_UNORM input
 * textures (all at full src resolution) bound to a 5-binding descriptor set
 * (4 samplers + 1 storage image) and dispatched through the
 * yuva444p12_to_bgra.comp pipeline.  There is no CUDA zero-copy variant —
 * ProRes never lives in NVDEC GPU memory — so only the CPU-staging upload
 * path exists.
 */

#include <volk.h>
#include "Nv12Converter.h"
#include "GpuContext.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

namespace rt {

// Pack one tightly-packed R16 plane, stripping FFmpeg's linesize padding.
// widthTexels is the plane width in pixels (full-res for 4:4:4).
static void packR16Plane(std::vector<uint8_t>& dst,
                         const uint8_t* src, int srcLineBytes,
                         uint32_t widthTexels, uint32_t height)
{
    const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(widthTexels) * 2;
    dst.resize(static_cast<size_t>(rowBytes) * height);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst.data() + row * rowBytes,
                    src + static_cast<ptrdiff_t>(row) * srcLineBytes,
                    static_cast<size_t>(rowBytes));
    }
}

// Upload one full-res R16 plane into `tex`, (re)creating it on size change.
static bool uploadR16Plane(Texture& tex, Allocator* alloc, Device* device,
                           const uint8_t* data, int linesize,
                           uint32_t w, uint32_t h,
                           VkCommandBuffer cmd,
                           std::vector<Texture::StagingCleanup>& stagingOut)
{
    std::vector<uint8_t> packed;
    packR16Plane(packed, data, linesize, w, h);

    Texture::StagingCleanup stg{};
    if (tex.image() == VK_NULL_HANDLE || tex.width() != w || tex.height() != h) {
        tex.destroy();
        TextureConfig cfg;
        cfg.width  = w;
        cfg.height = h;
        cfg.format = VK_FORMAT_R16_UNORM;
        cfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!tex.createFromDataBatched(alloc->handle(), device->handle(), cfg,
                                       packed.data(),
                                       static_cast<VkDeviceSize>(packed.size()),
                                       cmd, stg))
            return false;
    } else {
        if (!tex.updateDataBatched(packed.data(),
                                   static_cast<VkDeviceSize>(packed.size()),
                                   cmd, stg))
            return false;
    }
    if (stg.buffer != VK_NULL_HANDLE) stagingOut.push_back(stg);
    return true;
}

bool Nv12Converter::recordConvertYuva444p12Scaled(
    VkCommandBuffer cmd,
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    const uint8_t* aData, int aLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<Texture::StagingCleanup>& stagingOut)
{
    // 8-bit BGRA output (the original path) — out16f=false reproduces the
    // exact Vulkan command stream this function always emitted.
    return recordConvertYuva444p12ScaledImpl(
        cmd, yData, yLinesize, uData, uLinesize, vData, vLinesize,
        aData, aLinesize, srcW, srcH, dstW, dstH, /*out16f=*/false, stagingOut);
}

bool Nv12Converter::recordConvertYuva444p12ScaledImpl(
    VkCommandBuffer cmd,
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    const uint8_t* aData, int aLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    bool out16f,
    std::vector<Texture::StagingCleanup>& stagingOut)
{
    if (!m_initialized) return false;
    if (cmd == VK_NULL_HANDLE) return false;
    // 8-bit vs 16F differ only in the bound pipeline + output texture; the
    // four R16 input planes, descriptor set, and pipeline layout are shared.
    if (out16f) {
        if (!ensureYuva444p12Pipeline16F()) return false;
        if (!ensureOutputSize16F(dstW, dstH)) return false;
    } else {
        if (!ensureYuva444p12Pipeline()) return false;
        if (!ensureOutputSize(dstW, dstH)) return false;
    }
    const VkImageView outputView =
        out16f ? m_output16FTexture.imageView() : m_outputTexture.imageView();
    const VkPipeline  outputPipe =
        out16f ? m_yuva444Pipeline16F : m_yuva444Pipeline;

    // All four planes are full-resolution (4:4:4 + alpha).
    if (!uploadR16Plane(m_yuvaYTexture, m_allocator, m_device, yData, yLinesize,
                        srcW, srcH, cmd, stagingOut)) return false;
    if (!uploadR16Plane(m_yuvaUTexture, m_allocator, m_device, uData, uLinesize,
                        srcW, srcH, cmd, stagingOut)) return false;
    if (!uploadR16Plane(m_yuvaVTexture, m_allocator, m_device, vData, vLinesize,
                        srcW, srcH, cmd, stagingOut)) return false;
    if (!uploadR16Plane(m_yuvaATexture, m_allocator, m_device, aData, aLinesize,
                        srcW, srcH, cmd, stagingOut)) return false;

    // Barrier: transfer writes → compute reads.
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

    // Update descriptor set (4 samplers + output storage image).
    {
        VkDescriptorImageInfo yInfo = m_yuvaYTexture.descriptorInfo();
        VkDescriptorImageInfo uInfo = m_yuvaUTexture.descriptorInfo();
        VkDescriptorImageInfo vInfo = m_yuvaVTexture.descriptorInfo();
        VkDescriptorImageInfo aInfo = m_yuvaATexture.descriptorInfo();
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = outputView;
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        const VkDescriptorImageInfo* infos[4] = { &yInfo, &uInfo, &vInfo, &aInfo };
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 4; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = m_yuva444DescSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = infos[i];
        }
        writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet          = m_yuva444DescSet;
        writes[4].dstBinding      = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo      = &outInfo;

        vkUpdateDescriptorSets(m_device->handle(), 5, writes, 0, nullptr);
    }

    // Dispatch at OUTPUT dimensions.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, outputPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_yuva444PipeLayout, 0, 1, &m_yuva444DescSet,
                            0, nullptr);

    struct { int32_t w, h; } pc;
    pc.w = static_cast<int32_t>(dstW);
    pc.h = static_cast<int32_t>(dstH);
    vkCmdPushConstants(cmd, m_yuva444PipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    const uint32_t gx = (dstW + 15) / 16;
    const uint32_t gy = (dstH + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier: compute writes → shader/transfer reads.
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

bool Nv12Converter::convertYuva444p12SyncScaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    const uint8_t* aData, int aLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH)
{
    if (!m_initialized || !m_cmdPool) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    const bool ok = recordConvertYuva444p12Scaled(
        cmd, yData, yLinesize, uData, uLinesize, vData, vLinesize,
        aData, aLinesize, srcW, srcH, dstW, dstH, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);
    for (auto& s : staging) s.destroy();
    return ok;
}

bool Nv12Converter::convertAndReadbackYuva444p12Scaled(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    const uint8_t* aData, int aLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());
    if (!convertYuva444p12SyncScaled(yData, yLinesize, uData, uLinesize,
                                     vData, vLinesize, aData, aLinesize,
                                     srcW, srcH, dstW, dstH))
        return false;
    return readbackOutput(outPixels);
}

bool Nv12Converter::convertAndReadbackYuva444p12Scaled16F(
    const uint8_t* yData, int yLinesize,
    const uint8_t* uData, int uLinesize,
    const uint8_t* vData, int vLinesize,
    const uint8_t* aData, int aLinesize,
    uint32_t srcW, uint32_t srcH,
    uint32_t dstW, uint32_t dstH,
    std::vector<uint8_t>& outHalfRgba)
{
    if (!m_initialized || !m_cmdPool) return false;
    std::lock_guard<std::mutex> apiLock(m_apiMutex);
    std::lock_guard<std::mutex> qLock(GpuContext::get().computeQueueMutex());

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    std::vector<Texture::StagingCleanup> staging;
    const bool ok = recordConvertYuva444p12ScaledImpl(
        cmd, yData, yLinesize, uData, uLinesize, vData, vLinesize,
        aData, aLinesize, srcW, srcH, dstW, dstH, /*out16f=*/true, staging);
    m_cmdPool->endSingleTime(cmd, m_queue);
    for (auto& s : staging) s.destroy();
    if (!ok) return false;
    return readbackOutput16F(outHalfRgba);
}

} // namespace rt
