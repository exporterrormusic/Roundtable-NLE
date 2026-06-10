/*
 * FrameSignature.cpp — see FrameSignature.h.
 *
 * NOT yet integrated into the render paths and NOT runtime-verified. Compiles
 * and follows the project's Vulkan conventions (Compositor descriptor pattern,
 * VMA host-visible buffer). Runtime verification needed: shader correctness +
 * that the same texture yields a stable signature across runs.
 */

#include "diag/FrameSignature.h"
#include "PathUtils.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#include <vk_mem_alloc.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace rt {

namespace {

struct SigPush {
    int32_t width;
    int32_t height;
};

fs::path findSignatureSpv() {
    const fs::path candidates[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path()
            / "build" / "shaders" / "frame_signature.comp.spv",
        fs::current_path() / "shaders" / "frame_signature.comp.spv",
        fs::current_path() / "build" / "shaders" / "frame_signature.comp.spv",
        fs::current_path().parent_path() / "shaders" / "frame_signature.comp.spv",
        fs::current_path().parent_path() / "build" / "shaders" / "frame_signature.comp.spv",
    };
    for (const auto& p : candidates)
        if (fs::exists(p)) return p;
    return {};
}

VkShaderModule loadSpv(VkDevice dev, const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return VK_NULL_HANDLE;
    const auto size = static_cast<size_t>(f.tellg());
    if (size == 0 || (size % 4) != 0) return VK_NULL_HANDLE;
    std::vector<uint32_t> code(size / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = code.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

} // namespace

FrameSignature::~FrameSignature() { destroy(); }

bool FrameSignature::init(GpuContext& ctx) {
    m_ctx    = &ctx;
    m_device = ctx.vkDevice();
    if (m_device == VK_NULL_HANDLE) return false;

    // ── Shader module ───────────────────────────────────────────────────
    const fs::path spv = findSignatureSpv();
    if (spv.empty()) {
        spdlog::error("FrameSignature: frame_signature.comp.spv not found");
        return false;
    }
    m_shader = loadSpv(m_device, spv);
    if (m_shader == VK_NULL_HANDLE) {
        spdlog::error("FrameSignature: failed to load {}", pathToUtf8(spv));
        return false;
    }

    // ── Descriptor set layout: b0 sampled image, b1 storage buffer ──────
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslCi.bindingCount = 2;
    dslCi.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &dslCi, nullptr, &m_dsl) != VK_SUCCESS) {
        spdlog::error("FrameSignature: descriptor set layout creation failed");
        return false;
    }

    // ── Descriptor pool + set ───────────────────────────────────────────
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolCi.maxSets       = 1;
    poolCi.poolSizeCount = 2;
    poolCi.pPoolSizes    = poolSizes;
    if (vkCreateDescriptorPool(m_device, &poolCi, nullptr, &m_pool) != VK_SUCCESS) {
        spdlog::error("FrameSignature: descriptor pool creation failed");
        return false;
    }
    VkDescriptorSetAllocateInfo dsAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsAi.descriptorPool     = m_pool;
    dsAi.descriptorSetCount = 1;
    dsAi.pSetLayouts        = &m_dsl;
    if (vkAllocateDescriptorSets(m_device, &dsAi, &m_ds) != VK_SUCCESS) {
        spdlog::error("FrameSignature: descriptor set allocation failed");
        return false;
    }

    // ── Pipeline layout + compute pipeline ──────────────────────────────
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(SigPush);
    VkPipelineLayoutCreateInfo plCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCi.setLayoutCount         = 1;
    plCi.pSetLayouts            = &m_dsl;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device, &plCi, nullptr, &m_layout) != VK_SUCCESS) {
        spdlog::error("FrameSignature: pipeline layout creation failed");
        return false;
    }

    VkComputePipelineCreateInfo cpCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpCi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCi.stage.module = m_shader;
    cpCi.stage.pName  = "main";
    cpCi.layout       = m_layout;
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &m_pipeline) != VK_SUCCESS) {
        spdlog::error("FrameSignature: compute pipeline creation failed");
        return false;
    }

    // ── Host-visible storage buffer (the SSBO sig[8]), persistently mapped ─
    VkBufferCreateInfo bufCi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufCi.size  = sizeof(uint32_t) * 8;
    bufCi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo allocCi{};
    allocCi.usage = VMA_MEMORY_USAGE_AUTO;
    allocCi.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocation alloc{};
    VmaAllocationInfo allocInfo{};
    if (vmaCreateBuffer(ctx.allocator().handle(), &bufCi, &allocCi,
                        &m_sigBuf, &alloc, &allocInfo) != VK_SUCCESS) {
        spdlog::error("FrameSignature: signature buffer allocation failed");
        return false;
    }
    m_sigAlloc  = alloc;
    m_sigMapped = static_cast<uint32_t*>(allocInfo.pMappedData);
    if (!m_sigMapped) {
        spdlog::error("FrameSignature: signature buffer not host-mapped");
        return false;
    }

    // Bind the (fixed) storage buffer into the descriptor set once.
    VkDescriptorBufferInfo bufInfo{m_sigBuf, 0, sizeof(uint32_t) * 8};
    VkWriteDescriptorSet bufWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    bufWrite.dstSet          = m_ds;
    bufWrite.dstBinding      = 1;
    bufWrite.descriptorCount = 1;
    bufWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bufWrite.pBufferInfo     = &bufInfo;
    vkUpdateDescriptorSets(m_device, 1, &bufWrite, 0, nullptr);

    // ── Command pool (transient, graphics family) + buffer + fence ──────
    VkCommandPoolCreateInfo cpCreate{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpCreate.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                              | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpCreate.queueFamilyIndex = ctx.graphicsQueueFamilyIndex();
    if (vkCreateCommandPool(m_device, &cpCreate, nullptr, &m_cmdPool) != VK_SUCCESS) {
        spdlog::error("FrameSignature: command pool creation failed");
        return false;
    }
    VkCommandBufferAllocateInfo cbAi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAi.commandPool        = m_cmdPool;
    cbAi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &cbAi, &m_cmd) != VK_SUCCESS) {
        spdlog::error("FrameSignature: command buffer allocation failed");
        return false;
    }
    VkFenceCreateInfo fenceCi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(m_device, &fenceCi, nullptr, &m_fence) != VK_SUCCESS) {
        spdlog::error("FrameSignature: fence creation failed");
        return false;
    }

    m_ready = true;
    spdlog::info("FrameSignature: ready");
    return true;
}

bool FrameSignature::compute(VkImageView view, VkSampler sampler,
                             uint32_t width, uint32_t height,
                             std::array<uint32_t, 8>& outSig,
                             VkImageLayout layout) {
    if (!m_ready || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE ||
        width == 0 || height == 0)
        return false;

    // Zero the SSBO on the host before the dispatch (host writes to coherent
    // memory are visible to the device at the subsequent queue submit).
    std::memset(m_sigMapped, 0, sizeof(uint32_t) * 8);

    // Point binding 0 at the supplied output texture.
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = sampler;
    imgInfo.imageView   = view;
    imgInfo.imageLayout = layout;
    VkWriteDescriptorSet imgWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    imgWrite.dstSet          = m_ds;
    imgWrite.dstBinding      = 0;
    imgWrite.descriptorCount = 1;
    imgWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    imgWrite.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(m_device, 1, &imgWrite, 0, nullptr);

    vkResetCommandBuffer(m_cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_cmd, &begin);

    // Acquire barrier: make a prior composite's writes to the sampled texture
    // visible to our compute read. GpuScheduler serialises graphics-queue
    // submits (so the composite executed first); this provides the *memory*
    // visibility across that submission boundary — needed for the async
    // preview path (the export path also fenced-waited, so it's belt-and-braces
    // there).
    VkMemoryBarrier acquire{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    acquire.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
                          | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &acquire, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout,
                            0, 1, &m_ds, 0, nullptr);
    SigPush push{static_cast<int32_t>(width), static_cast<int32_t>(height)};
    vkCmdPushConstants(m_cmd, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push), &push);
    const uint32_t gx = (width  + 15u) / 16u;
    const uint32_t gy = (height + 15u) / 16u;
    vkCmdDispatch(m_cmd, gx, gy, 1);

    // Make the compute writes to the SSBO visible to the host read.
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer              = m_sigBuf;
    barrier.offset              = 0;
    barrier.size                = sizeof(uint32_t) * 8;
    vkCmdPipelineBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);

    vkEndCommandBuffer(m_cmd);

    // All queue submits MUST go through GpuScheduler (raw vkQueueSubmit is
    // macro-poisoned). Submit on the Graphics queue — it matches our command
    // pool's family and graphics queues are guaranteed to support compute.
    vkResetFences(m_device, 1, &m_fence);
    GpuSubmission sub{};
    sub.cmd             = m_cmd;
    sub.queue           = GpuQueueKind::Graphics;
    sub.completionFence = m_fence;
    sub.tag             = "FrameSignature";
    if (GpuContext::get().scheduler().submit(sub) != VK_SUCCESS) {
        spdlog::error("FrameSignature: queue submit failed");
        return false;
    }
    if (vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, 2'000'000'000ull) != VK_SUCCESS) {
        spdlog::error("FrameSignature: fence wait timed out");
        return false;
    }

    for (int i = 0; i < 8; ++i) outSig[static_cast<size_t>(i)] = m_sigMapped[i];
    return true;
}

void FrameSignature::destroy() {
    if (m_device == VK_NULL_HANDLE) return;

    // Guard against teardown-after-device-destroy. FrameSignatureLog is a
    // function-local static, so when the harness is enabled its destructor
    // runs at CRT exit (execute_onexit_table) — which is AFTER App::~App has
    // already called GpuContext::shutdown() → vkDestroyDevice(). At that point
    // m_device is a dangling handle and vkDestroyDevice already implicitly
    // freed every child object below; calling vkDestroyFence / vmaDestroyBuffer
    // / vkDestroyPipeline on the dead device dereferences freed driver state
    // and crashes inside nvoglv64.dll (see logs/crash_log.txt, 2026-05-30
    // 02:14:50 and 02:22:54). GpuContext nulls its device handle on shutdown,
    // so a mismatch means the device is gone — there is nothing left to free.
    if (GpuContext::get().vkDevice() != m_device) {
        m_fence    = VK_NULL_HANDLE; m_cmdPool  = VK_NULL_HANDLE;
        m_sigBuf   = VK_NULL_HANDLE; m_sigAlloc = nullptr; m_sigMapped = nullptr;
        m_pipeline = VK_NULL_HANDLE; m_layout   = VK_NULL_HANDLE;
        m_pool     = VK_NULL_HANDLE; m_dsl      = VK_NULL_HANDLE;
        m_shader   = VK_NULL_HANDLE; m_device   = VK_NULL_HANDLE;
        m_ready    = false;
        return;
    }

    if (m_fence)    { vkDestroyFence(m_device, m_fence, nullptr); m_fence = VK_NULL_HANDLE; }
    if (m_cmdPool)  { vkDestroyCommandPool(m_device, m_cmdPool, nullptr); m_cmdPool = VK_NULL_HANDLE; }
    if (m_sigBuf && m_ctx) {
        vmaDestroyBuffer(m_ctx->allocator().handle(), m_sigBuf,
                         static_cast<VmaAllocation>(m_sigAlloc));
        m_sigBuf = VK_NULL_HANDLE; m_sigAlloc = nullptr; m_sigMapped = nullptr;
    }
    if (m_pipeline) { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_layout)   { vkDestroyPipelineLayout(m_device, m_layout, nullptr); m_layout = VK_NULL_HANDLE; }
    if (m_pool)     { vkDestroyDescriptorPool(m_device, m_pool, nullptr); m_pool = VK_NULL_HANDLE; }
    if (m_dsl)      { vkDestroyDescriptorSetLayout(m_device, m_dsl, nullptr); m_dsl = VK_NULL_HANDLE; }
    if (m_shader)   { vkDestroyShaderModule(m_device, m_shader, nullptr); m_shader = VK_NULL_HANDLE; }
    m_ready = false;
}

} // namespace rt
