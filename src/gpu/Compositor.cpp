/*
 * Compositor.cpp -- runtime compositing, readback, transforms.
 *
 * Init/shutdown --> CompositorInit.cpp
 */

#include <volk.h>
#include "Compositor.h"
#include "GpuContext.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cmath>


namespace fs = std::filesystem;

namespace rt {

// ── setLayers ───────────────────────────────────────────────────────────────

void Compositor::setLayers(const std::vector<CompositorLayer>& layers)
{
    m_layerCount = static_cast<uint32_t>(std::min(layers.size(),
                                                   static_cast<size_t>(kMaxCompositorLayers)));
    m_layers.assign(layers.begin(), layers.begin() + m_layerCount);
    m_layersDirty = true;
}

// ── setPairs ────────────────────────────────────────────────────────────────
//
// Flattens A/B track pairs into layers for the GPU compute shader.
// The caller (CompositeServiceFrame) is responsible for rendering transitions
// via TransitionRenderer before calling setPairs().  The pair metadata
// (pairIndex, isBackground) is preserved on each layer so future pair-aware
// shader optimisations can use it.

void Compositor::setPairs(const std::vector<ABPair>& pairs)
{
    const size_t maxPairs = std::min(pairs.size(), static_cast<size_t>(kMaxCompositorLayers / 2));
    m_layers.clear();
    m_layers.reserve(maxPairs * 2);

    for (size_t pi = 0; pi < maxPairs; ++pi) {
        const auto& pair = pairs[pi];

        // ── Background (Track A) ────────────────────────────────────────
        CompositorLayer bg = pair.background;
        bg.pairIndex    = static_cast<uint32_t>(pi);
        bg.isBackground = true;
        // NOTE: both background and foreground remain enabled so the GPU
        // compositor shader can alpha-blend foreground over background
        // correctly.  A foreground character with transparency should
        // reveal the background layer behind it.

        m_layers.push_back(bg);

        // ── Foreground (Track B) ────────────────────────────────────────
        CompositorLayer fg = pair.foreground;
        fg.pairIndex    = static_cast<uint32_t>(pi);
        fg.isBackground = false;
        m_layers.push_back(fg);
    }

    m_layerCount = static_cast<uint32_t>(m_layers.size());
    m_layersDirty = true;
}

// ── clearLayers ─────────────────────────────────────────────────────────────

void Compositor::clearLayers()
{
    m_layers.clear();
    m_layerCount  = 0;
    m_layersDirty = true;
}

// ── updateSSBO ──────────────────────────────────────────────────────────────

void Compositor::updateSSBO()
{
    LayerParamsGPU params{};
    params.layerCount = static_cast<int32_t>(m_layerCount);

    for (uint32_t i = 0; i < m_layerCount; ++i)
    {
        const auto& layer = m_layers[i];
        params.transform[i] = layer.transform;
        params.motionTransformStart[i] = layer.motionTransformStart;
        params.motionTransformEnd[i] = layer.motionTransformEnd;
        params.motionSampleCount[i] = std::clamp(layer.motionSampleCount, 1, 8);
        params.opacity[i]   = layer.opacity;
        params.blendMode[i] = static_cast<int32_t>(layer.blendMode);
        params.enabled[i]   = layer.enabled ? 1 : 0;
        params.cropRect[i]  = glm::vec4(layer.cropLeft, layer.cropRight,
                                        layer.cropTop, layer.cropBottom);
        params.contentRect[i] = glm::vec4(
            layer.contentLeft, layer.contentTop,
            layer.contentRight, layer.contentBottom);
        params.isPacked[i]  = layer.isPacked ? 1 : 0;
        params.isPMA[i]     = layer.isPMA ? 1 : 0;
        params.needsSwapRB[i] = layer.needsSwapRB ? 1 : 0;
        params.hasMask[i]   = layer.hasMask ? 1 : 0;
    }

    // Zero out unused slots
    for (uint32_t i = m_layerCount; i < kMaxCompositorLayers; ++i)
    {
        params.transform[i] = glm::mat4(1.0f);
        params.motionTransformStart[i] = glm::mat4(1.0f);
        params.motionTransformEnd[i] = glm::mat4(1.0f);
        params.motionSampleCount[i] = 1;
        params.opacity[i]   = 0.0f;
        params.blendMode[i] = 0;
        params.enabled[i]   = 0;
        params.cropRect[i]  = glm::vec4(0.0f);
        params.contentRect[i] = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
        params.isPacked[i]  = 0;
        params.isPMA[i]     = 0;
        params.needsSwapRB[i] = 0;
        params.hasMask[i]   = 0;
    }

    m_layerParamsBuffers[m_outputRingIdx].upload(&params, sizeof(params));
}

// ── updateDescriptorSet ─────────────────────────────────────────────────────

void Compositor::updateDescriptorSet()
{
    VkDevice dev = m_device->handle();
    VkDescriptorSet descriptorSet = m_descriptorSets[m_outputRingIdx];

    // ── Binding 0: output storage image ─────────────────────────────────
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView   = m_outputTexture->imageView();
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4] = {};

    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &outputInfo;

    // ── Binding 1: layer textures ───────────────────────────────────────
    VkDescriptorImageInfo layerInfos[kMaxCompositorLayers];
    auto placeholderInfo = m_placeholderTexture.descriptorInfo();

    for (uint32_t i = 0; i < kMaxCompositorLayers; ++i)
    {
        if (i < m_layerCount && m_layers[i].textureInfo.imageView != VK_NULL_HANDLE)
        {
            layerInfos[i] = m_layers[i].textureInfo;
        }
        else
        {
            layerInfos[i] = placeholderInfo;
        }
    }

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = kMaxCompositorLayers;
    writes[1].pImageInfo      = layerInfos;

    // ── Binding 2: layer params SSBO ────────────────────────────────────
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_layerParamsBuffers[m_outputRingIdx].handle();
    bufferInfo.offset = 0;
    bufferInfo.range  = sizeof(LayerParamsGPU);

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = descriptorSet;
    writes[2].dstBinding      = 2;
    writes[2].dstArrayElement = 0;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo     = &bufferInfo;

    // ── Binding 3: mask textures ────────────────────────────────────────
    VkDescriptorImageInfo maskInfos[kMaxCompositorLayers];
    for (uint32_t i = 0; i < kMaxCompositorLayers; ++i)
    {
        if (i < m_layerCount && m_layers[i].hasMask &&
            m_layers[i].maskTextureInfo.imageView != VK_NULL_HANDLE)
        {
            maskInfos[i] = m_layers[i].maskTextureInfo;
        }
        else
        {
            maskInfos[i] = placeholderInfo;
        }
    }

    writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet          = descriptorSet;
    writes[3].dstBinding      = 3;
    writes[3].dstArrayElement = 0;
    writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = kMaxCompositorLayers;
    writes[3].pImageInfo      = maskInfos;

    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
}

// ── advanceOutputRing ───────────────────────────────────────────────────────

void Compositor::advanceOutputRing()
{
    m_outputRingIdx = (m_outputRingIdx + 1) % kOutputRing;
    // m_outputTexture is the alias every accessor / readback reads, so the
    // ring stays fully transparent to callers.  shared_ptr assignment
    // keeps the slot the presenter still holds (via gpuTextureOwner)
    // alive even though the ring has moved on.
    m_outputTexture = m_outputRing[m_outputRingIdx];
}

// ── composite ───────────────────────────────────────────────────────────────

bool Compositor::composite(VkCommandBuffer cmd)
{
    if (!m_initialized)
    {
        spdlog::error("Compositor: Not initialized");
        return false;
    }

    // Rotate to a fresh output texture so this composite does not
    // overwrite the slot the presenter (or a nested inner readback) is
    // still referencing.  Must happen BEFORE updateDescriptorSet() so
    // binding 0 points at the new slot.
    advanceOutputRing();

    // Each ring slot owns a separate SSBO.  Populate the slot selected for
    // this dispatch even when the logical layer list did not change: a slot
    // may not have been used since the last setLayers()/setPairs() call.
    // This also keeps repeated composites through the public API correct.
    updateSSBO();
    m_layersDirty = false;

    // Binding 0 and the SSBO binding both follow the selected ring slot, so
    // update only that slot's descriptor set.  Older in-flight sets remain
    // untouched.
    updateDescriptorSet();

    // Reset and write timestamp queries
    VkQueryPool queryPool = m_queryPools[m_outputRingIdx];
    VkDescriptorSet descriptorSet = m_descriptorSets[m_outputRingIdx];
    if (queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmd, queryPool, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
    }

    // Bind compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &descriptorSet,
                            0, nullptr);

    // Push constants
    CompositePushConstants pc;
    pc.width         = static_cast<int32_t>(m_config.outputWidth);
    pc.height        = static_cast<int32_t>(m_config.outputHeight);
    pc.hqSample      = m_hqSampling ? 1 : 0;
    pc.preserveAlpha = m_preserveAlpha ? 1 : 0;
    pc.outputSwizzleRB = m_outputSwizzleRB ? 1 : 0;
    pc._pad[0] = pc._pad[1] = pc._pad[2] = 0;
    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    // Dispatch compute shader
    uint32_t groupsX = (m_config.outputWidth  + kCompositeWorkgroupSize - 1) / kCompositeWorkgroupSize;
    uint32_t groupsY = (m_config.outputHeight + kCompositeWorkgroupSize - 1) / kCompositeWorkgroupSize;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // End timestamp
    if (queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
    }

    // Update stats
    m_stats.layerCount   = m_layerCount;
    m_stats.enabledLayers = 0;
    for (uint32_t i = 0; i < m_layerCount; ++i)
    {
        if (m_layers[i].enabled) m_stats.enabledLayers++;
    }
    m_stats.outputWidth  = m_config.outputWidth;
    m_stats.outputHeight = m_config.outputHeight;

    return true;
}

// ── compositeSync ───────────────────────────────────────────────────────────

bool Compositor::compositeSync()
{
    if (!m_initialized) return false;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    bool result = composite(cmd);
    m_cmdPool->endSingleTime(cmd, m_queue);

    // Read back GPU timestamp results
    VkQueryPool queryPool = m_queryPools[m_outputRingIdx];
    if (result && queryPool != VK_NULL_HANDLE)
    {
        uint64_t timestamps[2] = {};
        VkResult qr = vkGetQueryPoolResults(
            m_device->handle(), queryPool,
            0, 2,
            sizeof(timestamps), timestamps,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

        if (qr == VK_SUCCESS)
        {
            m_stats.gpuTimeMs = static_cast<float>(timestamps[1] - timestamps[0])
                              * m_timestampPeriod / 1e6f;
        }
    }

    return result;
}

// ── resize ──────────────────────────────────────────────────────────────────

bool Compositor::resize(uint32_t width, uint32_t height)
{
    if (!m_initialized) return false;
    if (width == m_config.outputWidth && height == m_config.outputHeight) return true;

    GpuContext::get().scheduler().deviceWaitIdle();

    m_config.outputWidth  = width;
    m_config.outputHeight = height;

    for (auto& staging : m_readbackStaging)
        staging.destroy(); // recreated per slot on next readback
    // deviceWaitIdle() above guarantees no in-flight work references any
    // ring slot, so it is safe to drop them all and rebuild at the new
    // size.  createOutputTexture() repopulates the whole ring.
    for (auto& t : m_outputRing) t.reset();
    m_outputTexture.reset();
    m_outputRingIdx = 0;
    if (!createOutputTexture())
        return false;

    // Re-update descriptor set with new output image
    m_layersDirty = true;

    return true;
}

// ── outputDescriptorInfo ────────────────────────────────────────────────────

VkDescriptorImageInfo Compositor::outputDescriptorInfo() const
{
    return m_outputTexture->descriptorInfo();
}

// ── readbackOutput ──────────────────────────────────────────────────────────

bool Compositor::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;

    const uint32_t w = m_config.outputWidth;
    const uint32_t h = m_config.outputHeight;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4; // RGBA8
    auto& readbackStaging = m_readbackStaging[m_outputRingIdx];

    // (Re)create persistent staging buffer if needed (avoids per-frame alloc)
    if (readbackStaging.handle() == VK_NULL_HANDLE ||
        readbackStaging.size() < imageSize)
    {
        readbackStaging.destroy();
        if (!readbackStaging.create(m_allocator->handle(), imageSize,
                                    BufferUsage::Readback))
        {
            spdlog::error("Compositor: Failed to create readback staging buffer");
            return false;
        }
    }

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    // Transition output to TRANSFER_SRC
    m_outputTexture->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Copy image to staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};

    vkCmdCopyImageToBuffer(cmd, m_outputTexture->image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackStaging.handle(), 1, &region);

    // Transition back to GENERAL for next composite
    m_outputTexture->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    // Read from staging buffer
    outPixels.resize(static_cast<size_t>(imageSize));
    void* mapped = readbackStaging.map();
    if (!mapped)
    {
        spdlog::error("Compositor: Failed to map readback buffer");
        return false;
    }
    std::memcpy(outPixels.data(), mapped, static_cast<size_t>(imageSize));
    readbackStaging.unmap();

    return true;
}

// ── recordReadback ──────────────────────────────────────────────────────────
// Records readback commands (barrier + copy) into an external command buffer.
// After submit + wait, call mapAndCopyReadback() to retrieve the pixels.

bool Compositor::recordReadback(VkCommandBuffer cmd)
{
    if (!m_initialized) return false;

    const uint32_t w = m_config.outputWidth;
    const uint32_t h = m_config.outputHeight;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;
    auto& readbackStaging = m_readbackStaging[m_outputRingIdx];

    // Ensure persistent readback staging buffer exists
    if (readbackStaging.handle() == VK_NULL_HANDLE ||
        readbackStaging.size() < imageSize)
    {
        readbackStaging.destroy();
        if (!readbackStaging.create(m_allocator->handle(), imageSize,
                                    BufferUsage::Readback))
        {
            spdlog::error("Compositor: Failed to create readback staging buffer");
            return false;
        }
    }

    // Memory barrier: composite writes → transfer reads
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Transition output to TRANSFER_SRC
    m_outputTexture->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // Copy image to staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};

    vkCmdCopyImageToBuffer(cmd, m_outputTexture->image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackStaging.handle(), 1, &region);

    // Transition back to GENERAL for next composite
    m_outputTexture->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);

    return true;
}

// ── mapAndCopyReadback ──────────────────────────────────────────────────────

bool Compositor::mapAndCopyReadback(std::vector<uint8_t>& outPixels)
{
    return mapAndCopyReadbackSlot(m_outputRingIdx, outPixels);
}

bool Compositor::mapAndCopyReadbackSlot(uint32_t slot,
                                        std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;
    if (slot >= kOutputRing) return false;
    auto& readbackStaging = m_readbackStaging[slot];

    // Guard against destroyed staging buffer (e.g. compositor resized
    // between recordReadback() and this deferred call via lazyReadback).
    if (readbackStaging.handle() == VK_NULL_HANDLE) return false;

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(m_config.outputWidth)
                                   * m_config.outputHeight * 4;

    outPixels.resize(static_cast<size_t>(imageSize));
    void* mapped = readbackStaging.map();
    if (!mapped) {
        spdlog::error("Compositor: Failed to map readback staging buffer");
        return false;
    }
    std::memcpy(outPixels.data(), mapped, static_cast<size_t>(imageSize));
    readbackStaging.unmap();
    return true;
}

// ── buildLayerTransform ─────────────────────────────────────────────────────

glm::mat4 Compositor::buildLayerTransform(float posX, float posY,
                                           float scaleX, float scaleY,
                                           float rotationDeg)
{
    // Build inverse transform: maps output UV → layer UV
    // We want: layer_uv = (output_uv - position) / scale
    // With GLM right-multiply convention: M = M * op
    // Operations apply right-to-left to input vector.
    // So we build: M = Scale(1/s) * Translate(-pos)
    // which GLM creates as: m = scale(translate(I, -pos), 1/s)

    glm::mat4 m(1.0f);

    // Step 1: Scale goes first in code (applied second to the vector)
    if (scaleX != 0.0f && scaleY != 0.0f)
    {
        m = glm::scale(m, glm::vec3(1.0f / scaleX, 1.0f / scaleY, 1.0f));
    }

    // Step 2: Translate goes second in code (applied first to the vector)
    m = glm::translate(m, glm::vec3(-posX, -posY, 0.0f));

    // Step 3: rotation (inverse = negative angle, around layer center)
    if (rotationDeg != 0.0f)
    {
        float halfX = 0.5f;
        float halfY = 0.5f;
        // Rotate around center of layer
        m = glm::translate(m, glm::vec3(halfX, halfY, 0.0f));
        m = glm::rotate(m, glm::radians(-rotationDeg), glm::vec3(0.0f, 0.0f, 1.0f));
        m = glm::translate(m, glm::vec3(-halfX, -halfY, 0.0f));
    }

    return m;
}

// ── buildViewportTransform ──────────────────────────────────────────────────

glm::mat4 Compositor::buildViewportTransform(uint32_t srcW, uint32_t srcH,
                                              uint32_t outW, uint32_t outH,
                                              float posXPx, float posYPx,
                                              float scaleX, float scaleY,
                                              float rotDeg,
                                              bool containFit,
                                              float anchorXPx,
                                              float anchorYPx,
                                              int srcRotationDeg)
{
    // Source DISPLAY rotation (clockwise, 0/90/180/270): portrait phone footage
    // is stored as landscape pixels + a rotate flag.  A 90°/270° turn swaps the
    // displayed aspect, so the contain/cover fit must use the DISPLAY dims
    // (srcW/srcH swapped).  The sampled UV is mapped back into the source
    // texture's native orientation as the FINAL step (see below).  0 = no
    // rotation → every value below is byte-identical to the legacy path.
    const int   srcRot      = ((srcRotationDeg % 360) + 360) % 360;
    const bool  quarterTurn = (srcRot == 90 || srcRot == 270);
    const float dispW = quarterTurn ? static_cast<float>(srcH) : static_cast<float>(srcW);
    const float dispH = quarterTurn ? static_cast<float>(srcW) : static_cast<float>(srcH);

    // Cover/contain fit: scale source to fill (cover) or fit within (contain)
    // the output rectangle.  Resolution-independent — the composited image
    // looks the same whether the output is 1920×1080 or 960×540.
    const float scaleToFitW = static_cast<float>(outW) / dispW;
    const float scaleToFitH = static_cast<float>(outH) / dispH;
    const float fitScale = containFit
        ? std::min(scaleToFitW, scaleToFitH)
        : std::max(scaleToFitW, scaleToFitH);

    const float fittedW = dispW * fitScale;
    const float fittedH = dispH * fitScale;
    const float baseOffX = (static_cast<float>(outW) - fittedW) * 0.5f;
    const float baseOffY = (static_cast<float>(outH) - fittedH) * 0.5f;

    const float cx = static_cast<float>(outW) * 0.5f;
    const float cy = static_cast<float>(outH) * 0.5f;

    // Guard against degenerate scale
    if (std::abs(scaleX) < 0.001f || std::abs(scaleY) < 0.001f)
        return glm::mat4(0.0f); // degenerate — shader will sample nothing

    // Build the inverse transform:
    //   output_uv → output_pixel → subtract centre+pos → inv-rotate →
    //   inv-scale → add centre → subtract baseOff → normalise to DISPLAY UV →
    //   [source rotation] re-orient into the source texture's native UV.
    //
    // GLM right-multiplies columns, so operations are in code-order
    // (the first operation listed acts last on the vector).  Initialising `m`
    // with the source-rotation UV matrix (instead of identity) makes that
    // re-orientation act LAST, after step 7 has produced display UV ∈ [0,1]².
    // Both spaces are normalised, so this is pure orientation bookkeeping:
    //   90° CW: (u,v)→(v,1−u)   180°: (u,v)→(1−u,1−v)   270° CW: (u,v)→(1−v,u)
    // (Columns below are GLM column-major, verified against those mappings.)
    glm::mat4 m(1.0f);
    if (srcRot == 90)
        m = glm::mat4( 0.f,-1.f, 0.f, 0.f,   1.f, 0.f, 0.f, 0.f,
                       0.f, 0.f, 1.f, 0.f,   0.f, 1.f, 0.f, 1.f);
    else if (srcRot == 180)
        m = glm::mat4(-1.f, 0.f, 0.f, 0.f,   0.f,-1.f, 0.f, 0.f,
                       0.f, 0.f, 1.f, 0.f,   1.f, 1.f, 0.f, 1.f);
    else if (srcRot == 270)
        m = glm::mat4( 0.f, 1.f, 0.f, 0.f,  -1.f, 0.f, 0.f, 0.f,
                       0.f, 0.f, 1.f, 0.f,   1.f, 0.f, 0.f, 1.f);

    // 7. Normalise to layer UV [0,1]
    m = glm::scale(m, glm::vec3(1.0f / fittedW, 1.0f / fittedH, 1.0f));

    // 6. Subtract base offset
    m = glm::translate(m, glm::vec3(-baseOffX, -baseOffY, 0.0f));

    // 5. Shift back to output-pixel origin (centre + anchor)
    // Anchor is a pivot offset relative to the layer's centre: rotation
    // and scale below pivot around (cx + anchor), not around (cx).
    // With anchor=(0,0) this collapses to legacy behaviour.
    m = glm::translate(m, glm::vec3(cx + anchorXPx, cy + anchorYPx, 0.0f));

    // 4. Inverse user scale
    m = glm::scale(m, glm::vec3(1.0f / scaleX, 1.0f / scaleY, 1.0f));

    // 3. Inverse user rotation (around the anchor — origin is at the
    // anchor after step 2 below shifts there)
    if (rotDeg != 0.0f)
        m = glm::rotate(m, glm::radians(-rotDeg), glm::vec3(0.0f, 0.0f, 1.0f));

    // 2. Subtract output centre + user position + anchor so subsequent
    // rotate/scale pivot around the anchor point in output-pixel space.
    m = glm::translate(m, glm::vec3(-cx - posXPx - anchorXPx,
                                    -cy - posYPx - anchorYPx, 0.0f));

    // 1. Un-normalise output UV → output pixel
    m = glm::scale(m, glm::vec3(static_cast<float>(outW),
                                 static_cast<float>(outH), 1.0f));

    return m;
}

} // namespace rt

