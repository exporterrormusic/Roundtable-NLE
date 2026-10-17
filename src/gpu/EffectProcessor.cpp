/*
 * EffectProcessor.cpp — GPU compute-shader effects pipeline.
 *
 * Step 22: Effects System
 *
 * Applies effects using Vulkan compute shaders with ping-pong storage images.
 * Each effect is dispatched as a separate compute shader invocation.
 * The processor chains effects by alternating read/write between two images.
 *
 * Shader bindings (must match color_correct.comp, blur.comp, etc.):
 *   binding 0: writeonly image2D  (output storage image)
 *   binding 1: sampler2D          (input combined image sampler)
 *
 * Push constants: EffectPushConstants (width, height, paramCount, params[16])
 */

#include <volk.h>
#include "EffectProcessor.h"
#include "effects/LUT.h"
#include "effects/EffectRegistry.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#include <cstdlib>
#include <cstring>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace rt {

namespace {

/// 2x2x2 identity: with trilinear filtering it maps every colour to itself.
const std::shared_ptr<const LutCube>& identityLutCube()
{
    static const std::shared_ptr<const LutCube> cube = [] {
        auto identity = std::make_shared<LutCube>();
        identity->size = 2;
        identity->id = UINT64_MAX;   // loaded cubes count up from 1
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    identity->data.insert(identity->data.end(),
                                          {float(r), float(g), float(b)});
        return identity;
    }();
    return cube;
}

} // namespace

// =============================================================================
//  Construction / Destruction
// =============================================================================

EffectProcessor::EffectProcessor() = default;

EffectProcessor::~EffectProcessor()
{
    shutdown();
}

// =============================================================================
//  Helper: locate SPIR-V shader file
// =============================================================================

static fs::path findShader(const char* name)
{
    fs::path candidates[] = {
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "build" / "shaders" / name,
        fs::current_path() / "shaders" / name,
        fs::current_path().parent_path() / "shaders" / name,
        fs::current_path().parent_path() / "build" / "shaders" / name,
    };
    for (auto& p : candidates)
        if (fs::exists(p)) return p;
    return {};
}

// =============================================================================
//  Shared, size-independent pipeline set
// =============================================================================

namespace {
constexpr const char* kLutShader     = "lut.comp.spv";
constexpr const char* kMaskMixShader = "mask_mix.comp.spv";
// Ultra Key's own passes (the table names only its legacy chroma_key shader).
constexpr const char* kUltraKeyShaders[] = {
    "ultra_key_matte.comp.spv", "ultra_key_cleanup.comp.spv",
    "ultra_key_finalize.comp.spv", "ultra_key_combined.comp.spv",
    "ultra_key_light.comp.spv",
};

/// Shaders using the ordinary 2-binding layout: every effect's from the
/// table (LUT has its own layout) plus Ultra Key's passes.
std::vector<const char*> mainShaders()
{
    std::vector<const char*> names;
    for (const auto& def : allEffectDefs()) {
        if (!def.shader || std::strcmp(def.shader, kLutShader) == 0) continue;
        const bool seen = std::any_of(names.begin(), names.end(), [&](const char* n) {
            return std::strcmp(n, def.shader) == 0;
        });
        if (!seen) names.push_back(def.shader);
    }
    for (const char* name : kUltraKeyShaders) names.push_back(name);
    return names;
}
} // namespace

struct EffectPipelineSet
{
    VkDevice              device{VK_NULL_HANDLE};
    PipelineManager       manager;
    VkDescriptorSetLayout mainSetLayout{VK_NULL_HANDLE};  ///< bindings 0-1
    VkDescriptorSetLayout lutSetLayout{VK_NULL_HANDLE};   ///< bindings 0-2
    VkPipelineLayout      pipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout      lutPipelineLayout{VK_NULL_HANDLE};
    VkPipelineLayout      maskMixPipelineLayout{VK_NULL_HANDLE};
    std::unordered_map<std::string, VkPipeline> pipelines;  ///< by .spv name

    EffectPipelineSet() = default;
    EffectPipelineSet(const EffectPipelineSet&) = delete;
    EffectPipelineSet& operator=(const EffectPipelineSet&) = delete;

    ~EffectPipelineSet()
    {
        manager.destroy();  // pipelines + pipeline layouts + shader modules
        if (device != VK_NULL_HANDLE) {
            if (lutSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, lutSetLayout, nullptr);
            if (mainSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, mainSetLayout, nullptr);
        }
    }

    [[nodiscard]] VkPipeline get(const char* spvName) const
    {
        const auto it = pipelines.find(spvName);
        return it != pipelines.end() ? it->second : VK_NULL_HANDLE;
    }

    /// One set per device, alive while any processor holds it.  Built under
    /// a mutex so concurrent first-time processors (preview + export) don't
    /// both compile.
    static std::shared_ptr<EffectPipelineSet> acquire(Device& dev)
    {
        static std::mutex s_mtx;
        static std::weak_ptr<EffectPipelineSet> s_set;
        std::lock_guard lock(s_mtx);
        if (auto existing = s_set.lock();
            existing && existing->device == dev.handle())
            return existing;
        auto set = std::make_shared<EffectPipelineSet>();
        if (!set->build(dev))
            return nullptr;
        s_set = set;
        return set;
    }

private:
    bool build(Device& dev)
    {
        device = dev.handle();

        auto makeSetLayout = [&](uint32_t bindingCount,
                                 VkDescriptorSetLayout& out) {
            // binding 0: storage image (output); 1..N: combined samplers.
            VkDescriptorSetLayoutBinding b[3]{};
            for (uint32_t i = 0; i < bindingCount; ++i) {
                b[i].binding         = i;
                b[i].descriptorType  = i == 0
                    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].descriptorCount = 1;
                b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo ci{};
            ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            ci.bindingCount = bindingCount;
            ci.pBindings    = b;
            return vkCreateDescriptorSetLayout(device, &ci, nullptr, &out)
                == VK_SUCCESS;
        };
        if (!makeSetLayout(2, mainSetLayout)) {
            spdlog::error("EffectProcessor: Failed to create descriptor set layout");
            return false;
        }
        // LUT layout: binding 2 is the 3D LUT sampler (also used by mask mix).
        if (!makeSetLayout(3, lutSetLayout)) {
            spdlog::error("EffectProcessor: Failed to create LUT descriptor set layout");
            return false;
        }

        if (!manager.create(dev)) {
            spdlog::error("EffectProcessor: Failed to create pipeline manager");
            return false;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset     = 0;
        pushRange.size       = sizeof(EffectPushConstants);

        pipelineLayout = manager.createLayout({pushRange}, {mainSetLayout});
        if (pipelineLayout == VK_NULL_HANDLE) {
            spdlog::error("EffectProcessor: Failed to create pipeline layout");
            return false;
        }
        lutPipelineLayout     = manager.createLayout({pushRange}, {lutSetLayout});
        maskMixPipelineLayout = manager.createLayout({pushRange}, {lutSetLayout});

        // Non-fatal per shader: a missing one is skipped at dispatch time.
        auto load = [&](const char* spvName, VkPipelineLayout layout) {
            if (layout == VK_NULL_HANDLE) return;
            fs::path path = findShader(spvName);
            if (path.empty()) {
                spdlog::warn("EffectProcessor: shader {} not found — skipping", spvName);
                return;
            }
            VkShaderModule mod = manager.loadShader(path);
            if (mod == VK_NULL_HANDLE) {
                spdlog::warn("EffectProcessor: failed to load {}", spvName);
                return;
            }
            ComputePipelineConfig pc;
            pc.compShader = mod;
            pc.layout     = layout;
            VkPipeline p = manager.createComputePipeline(pc);
            if (p == VK_NULL_HANDLE)
                spdlog::warn("EffectProcessor: failed to create pipeline for {}", spvName);
            else
                pipelines.emplace(spvName, p);
        };
        for (const char* name : mainShaders())
            load(name, pipelineLayout);
        load(kLutShader, lutPipelineLayout);
        load(kMaskMixShader, maskMixPipelineLayout);

        spdlog::info("EffectProcessor: shared pipeline set built ({} pipelines)",
                     pipelines.size());
        return true;
    }
};

// =============================================================================
//  Lifecycle
// =============================================================================

bool EffectProcessor::init(Device& device,
                           Allocator& allocator,
                           CommandPool& cmdPool,
                           VkQueue computeQueue,
                           const EffectProcessorConfig& config)
{
    m_device    = &device;
    m_allocator = &allocator;
    m_cmdPool   = &cmdPool;
    m_queue     = computeQueue;
    m_config    = config;

    // Query timestamp period for GPU timing
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.physicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;

    if (!createStorageTextures()) {
        spdlog::error("EffectProcessor: failed to create storage textures");
        return false;
    }

    if (!createDescriptorResources()) {
        spdlog::error("EffectProcessor: failed to create descriptor resources");
        return false;
    }

    if (!createPipelines()) {
        spdlog::error("EffectProcessor: failed to create pipelines");
        return false;
    }

    // Create timestamp query pool (2 queries: begin + end)
    VkQueryPoolCreateInfo qpci{};
    qpci.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType   = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount  = 2;
    if (vkCreateQueryPool(device.handle(), &qpci, nullptr, &m_queryPool) != VK_SUCCESS)
        m_queryPool = VK_NULL_HANDLE; // non-fatal

    // Create a reusable fence for processSync (avoids per-call create/destroy)
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device.handle(), &fci, nullptr, &m_syncFence) != VK_SUCCESS)
        m_syncFence = VK_NULL_HANDLE; // non-fatal, will fall back to endSingleTime

    m_initialized = true;
    spdlog::info("EffectProcessor initialized ({}x{})", m_config.width, m_config.height);
    return true;
}

void EffectProcessor::shutdown(GpuTeardownMode mode)
{
    if (!m_device) return;

    VkDevice dev = m_device->handle();
    if (mode == GpuTeardownMode::DeviceWide && dev &&
        GpuContext::get().gpuState() == GpuState::Healthy)
        vkDeviceWaitIdle(dev);

    // Query pool
    if (m_queryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev, m_queryPool, nullptr);
        m_queryPool = VK_NULL_HANDLE;
    }

    // Reusable fence
    if (m_syncFence != VK_NULL_HANDLE) {
        vkDestroyFence(dev, m_syncFence, nullptr);
        m_syncFence = VK_NULL_HANDLE;
    }

    // LUT 3D textures (frames still in flight hold their own references)
    m_activeLut.reset();
    m_lutCache.clear();

    // Descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
        m_descriptorPool      = VK_NULL_HANDLE;
        m_descriptorSets[0]   = VK_NULL_HANDLE;
        m_descriptorSets[1]   = VK_NULL_HANDLE;
        m_sourceDescriptorSets.clear();
        m_lutDescriptorSets.clear();
        m_maskMixDescriptorSets.clear();
        m_descriptorRingCursor = 0;
        m_maskMixRingCursor   = 0;
        m_curSourceSet        = VK_NULL_HANDLE;
        m_curLutSet           = VK_NULL_HANDLE;
    }
    // Layouts + pipelines are borrowed from the shared set (destroyed by
    // it when the last processor lets go) — only drop our references.
    m_lutDescriptorSetLayout = VK_NULL_HANDLE;
    m_descriptorSetLayout    = VK_NULL_HANDLE;
    m_shared.reset();

    // Pipelines
    m_effectPipelines.fill(VK_NULL_HANDLE);
    m_blurPipeline         = VK_NULL_HANDLE;
    m_ultraKeyMattePipeline     = VK_NULL_HANDLE;
    m_ultraKeyCleanupPipeline   = VK_NULL_HANDLE;
    m_ultraKeyFinalizePipeline  = VK_NULL_HANDLE;
    m_ultraKeyCombinedPipeline  = VK_NULL_HANDLE;
    m_ultraKeyLightPipeline     = VK_NULL_HANDLE;
    m_maskMixPipeline        = VK_NULL_HANDLE;
    m_maskMixPipelineLayout  = VK_NULL_HANDLE;
    m_lutPipelineLayout      = VK_NULL_HANDLE;
    m_pipelineLayout       = VK_NULL_HANDLE;

    // Storage textures + placeholder
    m_storageTextures[0].destroy();
    m_storageTextures[1].destroy();
    m_placeholderTexture.destroy();

    m_initialized = false;
    m_device      = nullptr;
    m_allocator   = nullptr;
    m_cmdPool     = nullptr;
    m_queue       = VK_NULL_HANDLE;
    spdlog::info("EffectProcessor shut down");
}

bool EffectProcessor::waitForOwnedWork(uint64_t timeoutNs) const
{
    if (!m_device || m_syncFence == VK_NULL_HANDLE)
        return true;

    const VkResult status = vkGetFenceStatus(m_device->handle(), m_syncFence);
    if (status == VK_SUCCESS)
        return true;
    if (status != VK_NOT_READY) {
        if (status == VK_ERROR_DEVICE_LOST)
            GpuContext::get().signalDeviceLost();
        return false;
    }
    const VkResult waited = vkWaitForFences(
        m_device->handle(), 1, &m_syncFence, VK_TRUE, timeoutNs);
    if (waited == VK_ERROR_DEVICE_LOST)
        GpuContext::get().signalDeviceLost();
    return waited == VK_SUCCESS;
}

// =============================================================================
//  Processing
// =============================================================================

bool EffectProcessor::process(VkCommandBuffer cmd,
                              const VkDescriptorImageInfo& sourceImage,
                              const std::vector<EffectStack::EffectSnapshot>& effects,
                              const std::vector<VkDescriptorImageInfo>* effectMasks)
{
    if (!m_initialized) return false;
    if (effects.empty()) return true;

    m_stats.effectsApplied = static_cast<int>(effects.size());
    m_currentOutput = 0;

    // One LUT descriptor set per call, so one cube per call: the first LUT
    // effect's, as before. Its texture comes from the cache, not a per-frame
    // destroy + convert + blocking upload, and nothing reads the live effect.
    // A LUT effect with no cube loaded passes the image through (identity
    // cube) instead of sampling the 2D placeholder through a 3D sampler.
    m_activeLut.reset();
    for (const auto& snap : effects) {
        if (snap.type == EffectType::LUT) {
            m_activeLut = lutTextureFor(snap.lut ? snap.lut : identityLutCube());
            break;
        }
    }

    // Claim this call's ring slot for the source + LUT descriptor sets.  A
    // single shared set cannot be reused across the multiple process() calls
    // the render graph records into one command buffer (one per effected
    // layer), nor across the kRingSize frames that overlap in flight: the
    // binding-1 update is immediate on the CPU, so every dispatch would
    // sample whichever source was bound LAST.  Round-robin over the ring so
    // each call has its own set; a slot is only reused after the owning
    // frame has completed.
    m_curSourceSet = m_sourceDescriptorSets[m_descriptorRingCursor];
    m_curLutSet    = m_lutDescriptorSets[m_descriptorRingCursor];
    m_descriptorRingCursor =
        (m_descriptorRingCursor + 1) % kDescriptorRing;

    // Point this slot's binding 1 (input sampler) at the caller's source.
    {
        VkDescriptorImageInfo srcInfo = sourceImage;
        // Ensure layout is set for shader read
        if (srcInfo.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
            srcInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_curSourceInfo = srcInfo;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_curSourceSet;
        writes[0].dstBinding      = 1; // input sampler at binding 1
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &srcInfo;

        // Also point the LUT slot binding 1 at the same source, so a first
        // LUT effect (if any) can read from the external source.
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_curLutSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &srcInfo;

        vkUpdateDescriptorSets(m_device->handle(), 2, writes, 0, nullptr);
    }

    // Timestamp begin
    if (m_queryPool)
        vkCmdResetQueryPool(cmd, m_queryPool, 0, 2);
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, 0);

    // For each effect, dispatch compute shader
    int sourceIdx = -1;  // -1 = use external source via the current ring slot
    int targetIdx = 0;

    // The external source info, normalized for shader read — needed as the
    // "original" input when the FIRST effect in the chain is masked.
    VkDescriptorImageInfo externalSrc = sourceImage;
    if (externalSrc.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        externalSrc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    for (size_t i = 0; i < effects.size(); ++i) {
        const auto& snap = effects[i];

        // Premiere effect masks: capture this effect's input so the mix
        // pass can blend original/effected after the dispatch.
        const VkDescriptorImageInfo* maskInfo = nullptr;
        if (effectMasks && i < effectMasks->size() &&
            (*effectMasks)[i].imageView != VK_NULL_HANDLE &&
            m_maskMixPipeline != VK_NULL_HANDLE)
        {
            maskInfo = &(*effectMasks)[i];
        }
        VkDescriptorImageInfo origInfo{};
        if (maskInfo) {
            if (sourceIdx < 0) {
                origInfo = externalSrc;
            } else {
                origInfo.sampler     = m_storageTextures[sourceIdx].sampler();
                origInfo.imageView   = m_storageTextures[sourceIdx].imageView();
                origInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            }
        }
        int resultIdx = -1;  // storage index holding this effect's output

        // Ultra Key — prefer light (no cleanup), then combined, fall back to 3-pass
        if (snap.type == EffectType::ChromaKey &&
            (m_ultraKeyLightPipeline    != VK_NULL_HANDLE ||
             m_ultraKeyCombinedPipeline != VK_NULL_HANDLE ||
             m_ultraKeyMattePipeline    != VK_NULL_HANDLE))
        {
            // OutputMode=3 (Original) — bypass the key entirely
            bool isOriginal = (snap.params.size() > 3 &&
                               static_cast<int>(snap.params[3] + 0.5f) == 3);
            if (isOriginal) {
                // Pass through: copy source to target unmodified
                copyImage(cmd, sourceIdx, targetIdx);
                sourceIdx = targetIdx;
                targetIdx = 1 - targetIdx;
                maskInfo = nullptr;  // passthrough — mix would be a no-op
            } else {
                int finalTarget = dispatchUltraKey(cmd, snap.params, sourceIdx, targetIdx);
                resultIdx = finalTarget;
                sourceIdx = finalTarget;
                targetIdx = 1 - finalTarget;
            }
        }
        else if (snap.type == EffectType::Blur) {
            const int finalTarget = dispatchGaussianBlur(
                cmd, snap.params, sourceIdx, targetIdx,
                maskInfo != nullptr && sourceIdx >= 0);
            if (finalTarget < 0) {
                spdlog::warn("EffectProcessor: failed to dispatch Gaussian blur");
                maskInfo = nullptr;
            } else {
                resultIdx = finalTarget;
                sourceIdx = finalTarget;
                targetIdx = 1 - finalTarget;
            }
        }
        else if (!dispatchEffect(cmd, snap.type, snap.params, sourceIdx, targetIdx)) {
            spdlog::warn("EffectProcessor: failed to dispatch effect type {}",
                         static_cast<int>(snap.type));
            maskInfo = nullptr;  // nothing was applied — nothing to mask
        } else {
            resultIdx = targetIdx;
            // Ping-pong
            sourceIdx = targetIdx;
            targetIdx = 1 - targetIdx;
        }

        if (maskInfo && resultIdx >= 0)
            dispatchMaskMix(cmd, origInfo, *maskInfo, resultIdx);
    }

    // Timestamp end
    if (m_queryPool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, 1);

    m_currentOutput = sourceIdx;
    return true;
}

bool EffectProcessor::processSync(const VkDescriptorImageInfo& sourceImage,
                                  const std::vector<EffectStack::EffectSnapshot>& effects)
{
    if (!m_initialized || !m_cmdPool) return false;
    if (effects.empty()) return true;

    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
    bool ok = process(cmd, sourceImage, effects);
    vkEndCommandBuffer(cmd);

    // Use persistent fence (reset + reuse) to avoid per-call create/destroy.
    // A5: 100ms bounded fence waits.  These calls are on the synchronous
    // single-shot EffectProcessor path (compositeSync, test code) — the
    // per-frame DAG path does not reach here.  Even so, an infinite wait
    // could deadlock the test runner or export pipeline if the GPU hangs.
    if (ok && m_syncFence != VK_NULL_HANDLE) {
        constexpr uint64_t kFenceTimeoutNs = 100'000'000ull;
        if (vkWaitForFences(m_device->handle(), 1, &m_syncFence,
                             VK_TRUE, kFenceTimeoutNs) == VK_TIMEOUT)
        {
            spdlog::error("[EFFECT] pre-submit fence timeout — bailing");
            m_cmdPool->freeBuffer(cmd);
            return false;
        }
        vkResetFences(m_device->handle(), 1, &m_syncFence);

        // P1.1: route through GpuScheduler.  EffectProcessor's m_queue
        // is the compute queue (set at init); scheduler dispatches to
        // Compute kind explicitly so it owns the locking discipline.
        rt::GpuSubmission sub{};
        sub.cmd             = cmd;
        sub.queue           = rt::GpuQueueKind::Compute;
        sub.completionFence = m_syncFence;
        sub.tag             = "EffectProcessor::sync";
        rt::GpuContext::get().scheduler().submit(sub);
        if (vkWaitForFences(m_device->handle(), 1, &m_syncFence,
                             VK_TRUE, kFenceTimeoutNs) == VK_TIMEOUT)
        {
            spdlog::error("[EFFECT] post-submit fence timeout — GPU hang");
            m_cmdPool->freeBuffer(cmd);
            return false;
        }

        m_cmdPool->freeBuffer(cmd);
    } else {
        // Fallback: use CommandPool's single-time path
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // Read back GPU timestamps
    if (ok && m_queryPool) {
        uint64_t ts[2]{};
        VkResult qr = vkGetQueryPoolResults(
            m_device->handle(), m_queryPool,
            0, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qr == VK_SUCCESS)
            m_stats.gpuTimeMs = static_cast<float>(ts[1] - ts[0])
                              * m_timestampPeriod / 1e6f;
    }

    return ok;
}

// ═══════════════════════════════════════════════════════════════════════════

// =============================================================================
//  Snapshot output
// =============================================================================

bool EffectProcessor::snapshotOutputSync(Texture& dst)
{
    if (!m_initialized || !m_device || !m_cmdPool) return false;
    VkImage srcImg = outputImage();
    if (srcImg == VK_NULL_HANDLE) return false;
    if (dst.image() == VK_NULL_HANDLE ||
        dst.width() != m_config.width || dst.height() != m_config.height)
        return false;

    VkDevice dev = m_device->handle();
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = srcImg;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = dst.image();
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {m_config.width, m_config.height, 1};
    vkCmdCopyImage(cmd, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier restoreSrc{};
    restoreSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    restoreSrc.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    restoreSrc.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    restoreSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restoreSrc.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    restoreSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    restoreSrc.image = srcImg;
    restoreSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier dstReadable{};
    dstReadable.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstReadable.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstReadable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dstReadable.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstReadable.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dstReadable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstReadable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstReadable.image = dst.image();
    dstReadable.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier post[2] = {restoreSrc, dstReadable};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, post);

    vkEndCommandBuffer(cmd);
    if (m_syncFence != VK_NULL_HANDLE) {
        vkResetFences(dev, 1, &m_syncFence);
        rt::GpuSubmission sub{};
        sub.cmd = cmd; sub.queue = rt::GpuQueueKind::Compute;
        sub.completionFence = m_syncFence; sub.tag = "EffectProcessor::snapshot";
        rt::GpuContext::get().scheduler().submit(sub);
        vkWaitForFences(dev, 1, &m_syncFence, VK_TRUE, 100'000'000ull);
        m_cmdPool->freeBuffer(cmd);
    } else {
        m_cmdPool->endSingleTime(cmd, m_queue);
    }
    return true;
}

//  Resize
// =============================================================================

bool EffectProcessor::resize(uint32_t width, uint32_t height)
{
    if (width == m_config.width && height == m_config.height) return true;
    m_config.width  = width;
    m_config.height = height;

    if (m_initialized) {
        // Wait only on our compute queue rather than draining the entire
        // device — avoids stalling the graphics queue when processing
        // mixed-resolution clips back-to-back.
        vkQueueWaitIdle(m_queue);
        m_storageTextures[0].destroy();
        m_storageTextures[1].destroy();
        if (!createStorageTextures()) return false;

        // Re-point BOTH sides of the pair. Effect #1 uses the external-source
        // ring, but effect #2 samples binding 1 here. Leaving that binding on
        // the destroyed pre-resize image produces tiled/sliced corruption.
        updatePingPongDescriptorBindings();
        // Re-point the source/LUT ring sets at the new storage[0].
        initDescriptorRingBindings();
    }
    return true;
}

// =============================================================================
//  LUT 3D texture management
// =============================================================================

std::shared_ptr<Texture> EffectProcessor::lutTextureFor(
    const std::shared_ptr<const LutCube>& cube)
{
    if (!m_initialized || !m_device || !cube || cube->size < 2) return nullptr;
    const size_t numVoxels =
        static_cast<size_t>(cube->size) * cube->size * cube->size;
    if (cube->data.size() != numVoxels * 3) return nullptr;

    for (auto& entry : m_lutCache) {
        if (entry.cubeId == cube->id) {
            entry.lastUse = ++m_lutUseClock;
            return entry.texture;
        }
    }

    // LUT data is size^3 × 3 floats (RGB). We need to convert to RGBA8.
    const size_t dataSize = numVoxels * 4; // RGBA8 = 4 bytes per voxel
    std::vector<uint8_t> rgbaData(dataSize);
    const auto& lutData = cube->data;
    for (size_t i = 0; i < numVoxels; ++i) {
        rgbaData[i * 4 + 0] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 0] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 1] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 1] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 2] = static_cast<uint8_t>(
            std::clamp(lutData[i * 3 + 2] * 255.0f, 0.0f, 255.0f));
        rgbaData[i * 4 + 3] = 255; // alpha = opaque
    }

    TextureConfig cfg;
    cfg.width       = static_cast<uint32_t>(cube->size);
    cfg.height      = static_cast<uint32_t>(cube->size);
    cfg.depth       = static_cast<uint32_t>(cube->size);
    cfg.format      = VK_FORMAT_R8G8B8A8_UNORM;
    cfg.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    cfg.filter      = VK_FILTER_LINEAR;
    cfg.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    auto texture = std::make_shared<Texture>();
    if (!texture->createFromData(
            m_allocator->handle(), m_device->handle(), cfg,
            rgbaData.data(), static_cast<VkDeviceSize>(dataSize),
            *m_cmdPool, m_queue))
    {
        spdlog::error("EffectProcessor: Failed to create LUT 3D texture ({}x{}x{})",
                      cube->size, cube->size, cube->size);
        return nullptr;
    }

    // Least recently used goes. Dropping it here is safe: every frame that
    // sampled it retained its own reference (activeLUT3D()) until its fence.
    if (m_lutCache.size() >= kLutCacheSize) {
        auto oldest = std::min_element(m_lutCache.begin(), m_lutCache.end(),
            [](const LutCacheEntry& a, const LutCacheEntry& b) { return a.lastUse < b.lastUse; });
        m_lutCache.erase(oldest);
    }
    m_lutCache.push_back({cube->id, texture, ++m_lutUseClock});
    spdlog::info("EffectProcessor: uploaded {}x{}x{} LUT 3D texture",
                 cube->size, cube->size, cube->size);
    return texture;
}

// =============================================================================
//  Output access
// =============================================================================

VkImage EffectProcessor::outputImage() const noexcept
{
    return m_storageTextures[m_currentOutput].image();
}

VkImageView EffectProcessor::outputImageView() const noexcept
{
    return m_storageTextures[m_currentOutput].imageView();
}

VkDescriptorImageInfo EffectProcessor::outputDescriptorInfo() const
{
    VkDescriptorImageInfo info{};
    info.sampler     = m_storageTextures[m_currentOutput].sampler();
    info.imageView   = outputImageView();
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return info;
}

bool EffectProcessor::readbackOutput(std::vector<uint8_t>& outPixels)
{
    if (!m_initialized) return false;

    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(m_config.width)
                               * m_config.height * 4;

    // Create staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf{VK_NULL_HANDLE};
    VmaAllocation stagingAlloc{nullptr};
    if (vmaCreateBuffer(m_allocator->handle(), &bci, &aci,
                        &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS)
        return false;

    // Copy image → buffer
    VkCommandBuffer cmd = m_cmdPool->beginSingleTime();

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {m_config.width, m_config.height, 1};
    vkCmdCopyImageToBuffer(cmd, m_storageTextures[m_currentOutput].image(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    m_storageTextures[m_currentOutput].transitionLayout(
        cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    m_cmdPool->endSingleTime(cmd, m_queue);

    // Map and copy to CPU
    void* mapped = nullptr;
    vmaMapMemory(m_allocator->handle(), stagingAlloc, &mapped);
    outPixels.resize(static_cast<size_t>(bufSize));
    std::memcpy(outPixels.data(), mapped, bufSize);
    vmaUnmapMemory(m_allocator->handle(), stagingAlloc);

    vmaDestroyBuffer(m_allocator->handle(), stagingBuf, stagingAlloc);
    return true;
}

// =============================================================================
//  Internal — create ping-pong storage textures
// =============================================================================

bool EffectProcessor::createStorageTextures()
{
    for (int i = 0; i < 2; ++i) {
        TextureConfig cfg;
        cfg.width  = m_config.width;
        cfg.height = m_config.height;
        cfg.format = m_config.format;
        cfg.usage  = VK_IMAGE_USAGE_STORAGE_BIT
                   | VK_IMAGE_USAGE_SAMPLED_BIT
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        if (!m_storageTextures[i].create(m_allocator->handle(),
                                          m_device->handle(), cfg))
        {
            spdlog::error("EffectProcessor: Failed to create storage texture {}", i);
            return false;
        }

        // Transition to GENERAL for compute shader read/write
        VkCommandBuffer cmd = m_cmdPool->beginSingleTime();
        m_storageTextures[i].transitionLayout(
            cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        m_cmdPool->endSingleTime(cmd, m_queue);
    }

    // The placeholder is dimension-independent. Keep it across resize;
    // recreating it would leak the old image and invalidate descriptors.
    if (m_placeholderTexture.image() == VK_NULL_HANDLE) {
        TextureConfig phCfg;
        phCfg.width  = 1;
        phCfg.height = 1;
        phCfg.format = m_config.format;
        phCfg.usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        uint32_t transparent = 0;
        if (!m_placeholderTexture.createFromData(
                m_allocator->handle(), m_device->handle(), phCfg,
                &transparent, 4, *m_cmdPool, m_queue))
        {
            spdlog::error("EffectProcessor: Failed to create placeholder texture");
            return false;
        }
    }

    return true;
}

// =============================================================================
//  Internal — create descriptor set layout, pool, and sets
// =============================================================================

bool EffectProcessor::createDescriptorResources()
{
    VkDevice dev = m_device->handle();

    // Descriptor-set layouts (and all pipelines) come from the per-device
    // shared set; see EffectPipelineSet above.
    m_shared = EffectPipelineSet::acquire(*m_device);
    if (!m_shared) {
        spdlog::error("EffectProcessor: shared pipeline set unavailable");
        return false;
    }
    m_descriptorSetLayout    = m_shared->mainSetLayout;
    m_lutDescriptorSetLayout = m_shared->lutSetLayout;

    // Pool sizing:
    //   ping-pong[0], ping-pong[1]                       = 2 main sets
    //   source ring (kDescriptorRing main-layout sets)
    //   LUT ring    (kDescriptorRing LUT-layout sets)
    //   main set = 1 storage + 1 sampler ; LUT set = 1 storage + 2 samplers.
    // See EffectProcessor.h (m_sourceDescriptorSets) for why the rings exist.
    const uint32_t kMainSets    = 2u + kDescriptorRing;   // ping-pong + source ring
    const uint32_t kLutSets     = kDescriptorRing;
    const uint32_t kMaskMixSets = kDescriptorRing;         // effect-mask mix ring
    const uint32_t kTotalSets   = kMainSets + kLutSets + kMaskMixSets;

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = kMainSets + kLutSets + kMaskMixSets;   // 1 storage per set
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kMainSets + 2u * (kLutSets + kMaskMixSets); // main:1, LUT/mix:2

    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolCI.maxSets       = kTotalSets;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(dev, &poolCI, nullptr,
                                &m_descriptorPool) != VK_SUCCESS)
    {
        spdlog::error("EffectProcessor: Failed to create descriptor pool "
                      "(storage={}, samplers={}, sets={})",
                      poolSizes[0].descriptorCount,
                      poolSizes[1].descriptorCount,
                      poolCI.maxSets);
        return false;
    }

    // Allocate the main-layout sets: 2 ping-pong + kDescriptorRing source.
    std::vector<VkDescriptorSetLayout> mainLayouts(kMainSets, m_descriptorSetLayout);
    std::vector<VkDescriptorSet>       mainSets(kMainSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = kMainSets;
    allocInfo.pSetLayouts        = mainLayouts.data();

    if (vkAllocateDescriptorSets(dev, &allocInfo, mainSets.data()) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate {} main descriptor sets "
                      "(pool maxSets={})", kMainSets, poolCI.maxSets);
        return false;
    }
    m_descriptorSets[0] = mainSets[0];  // target = storageTexture[0]
    m_descriptorSets[1] = mainSets[1];  // target = storageTexture[1]
    m_sourceDescriptorSets.assign(mainSets.begin() + 2, mainSets.end());

    // Allocate the LUT ring from the LUT-specific layout.
    std::vector<VkDescriptorSetLayout> lutLayouts(kLutSets, m_lutDescriptorSetLayout);
    m_lutDescriptorSets.assign(kLutSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo lutAllocInfo{};
    lutAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    lutAllocInfo.descriptorPool     = m_descriptorPool;
    lutAllocInfo.descriptorSetCount = kLutSets;
    lutAllocInfo.pSetLayouts        = lutLayouts.data();

    if (vkAllocateDescriptorSets(dev, &lutAllocInfo,
                                 m_lutDescriptorSets.data()) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate {} LUT descriptor sets "
                      "(pool maxSets={})", kLutSets, poolCI.maxSets);
        return false;
    }
    m_descriptorRingCursor = 0;

    // Allocate the mask-mix ring (LUT layout: storage + 2 samplers).
    std::vector<VkDescriptorSetLayout> mixLayouts(kMaskMixSets, m_lutDescriptorSetLayout);
    m_maskMixDescriptorSets.assign(kMaskMixSets, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo mixAllocInfo{};
    mixAllocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    mixAllocInfo.descriptorPool     = m_descriptorPool;
    mixAllocInfo.descriptorSetCount = kMaskMixSets;
    mixAllocInfo.pSetLayouts        = mixLayouts.data();

    if (vkAllocateDescriptorSets(dev, &mixAllocInfo,
                                 m_maskMixDescriptorSets.data()) != VK_SUCCESS) {
        spdlog::error("EffectProcessor: Failed to allocate {} mask-mix descriptor sets "
                      "(pool maxSets={})", kMaskMixSets, poolCI.maxSets);
        return false;
    }
    m_maskMixRingCursor = 0;

    // ── Initialize descriptor sets for the ping-pong pair ───────────────
    // Each set has:
    //   binding 0 → storageTexture[i]  (output)
    //   binding 1 → storageTexture[1-i] (input, from previous ping-pong)
    //
    // The source descriptor set (sets[2]) has:
    //   binding 0 → storageTexture[0] (output for first effect)
    //   binding 1 → external source (will be updated per-call)

    updatePingPongDescriptorBindings();

    // Source + LUT rings: binding 0 → storageTexture[0], binding 1 (input) →
    // placeholder, LUT binding 2 → placeholder.  Per-call bindings (the
    // external source, and the live 3D LUT) are written in process() /
    // dispatchEffect on the slot that call claims.
    initDescriptorRingBindings();

    return true;
}

void EffectProcessor::updatePingPongDescriptorBindings()
{
    if (!m_device) return;
    const VkDevice dev = m_device->handle();

    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[i].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo inInfo{};
        inInfo.sampler     = m_storageTextures[1 - i].sampler();
        inInfo.imageView   = m_storageTextures[1 - i].imageView();
        inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSets[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSets[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &inInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }
}

// =============================================================================
//  Internal — (re)point the static bindings of the source/LUT descriptor rings
// =============================================================================

void EffectProcessor::initDescriptorRingBindings()
{
    if (!m_device) return;
    VkDevice dev = m_device->handle();

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = m_storageTextures[0].imageView();
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const VkDescriptorImageInfo phInfo = m_placeholderTexture.descriptorInfo();

    // Source ring: binding 0 = output (storage[0]), binding 1 = placeholder.
    for (VkDescriptorSet set : m_sourceDescriptorSets) {
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &phInfo;

        vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
    }

    // LUT + mask-mix rings: binding 0 = output, binding 1 = placeholder,
    // binding 2 = placeholder.  dispatchEffect / dispatchMaskMix rewrite
    // all three per dispatch.
    std::vector<VkDescriptorSet> threeBindingSets;
    threeBindingSets.reserve(m_lutDescriptorSets.size() +
                             m_maskMixDescriptorSets.size());
    threeBindingSets.insert(threeBindingSets.end(),
                            m_lutDescriptorSets.begin(), m_lutDescriptorSets.end());
    threeBindingSets.insert(threeBindingSets.end(),
                            m_maskMixDescriptorSets.begin(), m_maskMixDescriptorSets.end());
    for (VkDescriptorSet set : threeBindingSets) {
        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &phInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = set;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &phInfo;

        vkUpdateDescriptorSets(dev, 3, writes, 0, nullptr);
    }
}

// =============================================================================
//  Internal — create compute pipelines (one per effect type)
// =============================================================================

bool EffectProcessor::createPipelines()
{
    if (!m_shared)
        return false;
    const EffectPipelineSet& s = *m_shared;
    m_pipelineLayout        = s.pipelineLayout;
    m_lutPipelineLayout     = s.lutPipelineLayout;
    m_maskMixPipelineLayout = s.maskMixPipelineLayout;
    auto loadPipeline = [&](const char* spvName) { return s.get(spvName); };

    for (const auto& def : allEffectDefs())
        if (def.shader)
            m_effectPipelines[static_cast<size_t>(def.type)] = loadPipeline(def.shader);
    m_blurPipeline              = loadPipeline("blur.comp.spv");
    m_ultraKeyMattePipeline     = loadPipeline("ultra_key_matte.comp.spv");
    m_ultraKeyCleanupPipeline   = loadPipeline("ultra_key_cleanup.comp.spv");
    m_ultraKeyFinalizePipeline  = loadPipeline("ultra_key_finalize.comp.spv");
    m_ultraKeyCombinedPipeline  = loadPipeline("ultra_key_combined.comp.spv");
    m_ultraKeyLightPipeline     = loadPipeline("ultra_key_light.comp.spv");

    // Effect-mask mix — 3-binding LUT descriptor layout (maskMixPipelineLayout).
    m_maskMixPipeline = loadPipeline(kMaskMixShader);
    if (m_maskMixPipeline == VK_NULL_HANDLE)
        spdlog::warn("EffectProcessor: mask_mix pipeline unavailable — "
                     "effect masks will be ignored");

    spdlog::debug("EffectProcessor: pipelines created (colorCorrect={}, blur={}, lumetri={})",
                 getPipeline(EffectType::ColorCorrect) != VK_NULL_HANDLE,
                 m_blurPipeline != VK_NULL_HANDLE,
                 getPipeline(EffectType::ColorGrading) != VK_NULL_HANDLE);
    return true;
}

// =============================================================================
//  Internal — dispatch a single effect
// =============================================================================

bool EffectProcessor::dispatchEffect(VkCommandBuffer cmd,
                                     EffectType type,
                                     const std::vector<float>& params,
                                     int sourceIdx, int targetIdx)
{
    VkPipeline pipeline = getPipeline(type);
    if (pipeline == VK_NULL_HANDLE) return false;

    // Select descriptor set:
    //   sourceIdx == -1 → use m_curSourceSet (external source image)
    //   sourceIdx ==  0 or 1 → use m_descriptorSets[targetIdx]
    //     (descriptorSets[targetIdx] has binding0 = storage[targetIdx],
    //      binding1 = storage[1-targetIdx] which is the source)
    //
    // For LUT effects, use the LUT-specific descriptor set which also has
    // binding 2 pointing to the 3D LUT texture.
    VkDescriptorSet ds;
    if (type == EffectType::LUT) {
        // Use LUT descriptor set – update bindings 0+1 for current ping-pong state
        VkDescriptorImageInfo outInfo{};
        outInfo.imageView   = m_storageTextures[targetIdx].imageView();
        outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo inInfo{};
        if (sourceIdx == -1) {
            // First effect: the caller's source, as process() bound it for
            // the other effects (this used to bind the placeholder, so a
            // LUT as the first effect graded a blank image).
            inInfo = m_curSourceInfo;
        } else {
            inInfo.sampler     = m_storageTextures[1 - targetIdx].sampler();
            inInfo.imageView   = m_storageTextures[1 - targetIdx].imageView();
            inInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        VkDescriptorImageInfo lutInfo = m_activeLut
            ? m_activeLut->descriptorInfo()
            : m_placeholderTexture.descriptorInfo();

        VkWriteDescriptorSet writes[3]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_curLutSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo      = &outInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_curLutSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo      = &inInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = m_curLutSet;
        writes[2].dstBinding      = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo      = &lutInfo;

        vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);

        ds = m_curLutSet;
    } else if (sourceIdx == -1) {
        // First effect (non-LUT): source is external image (the current ring
        // slot's binding 1 was set in process()).  binding 0 = storage[0].
        ds = m_curSourceSet;
    } else {
        // Subsequent effects: ping-pong.
        // m_descriptorSets[targetIdx] has binding0 = storage[targetIdx] (output),
        // binding1 = storage[1-targetIdx] = storage[sourceIdx] (input).
        ds = m_descriptorSets[targetIdx];
    }

    const VkPipelineLayout dispatchLayout =
        type == EffectType::LUT ? m_lutPipelineLayout : m_pipelineLayout;
    if (dispatchLayout == VK_NULL_HANDLE) return false;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            dispatchLayout, 0, 1, &ds, 0, nullptr);

    // Push constants
    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, dispatchLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    // Dispatch: 16×16 workgroups
    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Pipeline barrier: compute write → compute read for next effect
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    return true;
}

int EffectProcessor::dispatchGaussianBlur(
    VkCommandBuffer cmd, const std::vector<float>& params,
    int sourceIdx, int targetIdx, bool preserveInternalSource)
{
    // Radius below the shader threshold is a pass-through. Keep that case to
    // one dispatch so a disabled Blur does not add redundant GPU work.
    const float radius = params.empty() ? 0.0f : params[0];
    std::vector<float> passParams = params;
    // Snapshots created before Repeat Edge Pixels was added contain only
    // Radius. Treat those exactly like the historical clamp-to-edge blur.
    if (passParams.empty())
        passParams.push_back(0.0f);
    if (passParams.size() < 2)
        passParams.resize(2, 1.0f);
    // Slot 2 is renderer-internal and is never serialized as an effect param.
    if (passParams.size() < 3)
        passParams.resize(3, 0.0f);

    if (preserveInternalSource) {
        passParams[2] = 2.0f; // exact legacy circular 2D kernel
        return dispatchEffect(cmd, EffectType::Blur, passParams,
                              sourceIdx, targetIdx)
            ? targetIdx : -1;
    }

    passParams[2] = 0.0f; // horizontal
    if (!dispatchEffect(cmd, EffectType::Blur, passParams,
                        sourceIdx, targetIdx)) {
        return -1;
    }
    if (radius < 0.5f)
        return targetIdx;

    const int horizontalOutput = targetIdx;
    const int verticalOutput = 1 - horizontalOutput;
    passParams[2] = 1.0f; // vertical
    if (!dispatchEffect(cmd, EffectType::Blur, passParams,
                        horizontalOutput, verticalOutput)) {
        return -1;
    }
    return verticalOutput;
}

VkPipeline EffectProcessor::getPipeline(EffectType type) const
{
    const auto i = static_cast<size_t>(type);
    return i < m_effectPipelines.size() ? m_effectPipelines[i] : VK_NULL_HANDLE;
}

// ═════════════════════════════════════════════════════════════════════════
//  Ultra Key — 3-pass dispatch
// ═════════════════════════════════════════════════════════════════════════

void EffectProcessor::dispatchPass(VkCommandBuffer cmd, VkPipeline pipeline,
                                   VkDescriptorSet ds,
                                   const std::vector<float>& params)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &ds, 0, nullptr);

    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = static_cast<int32_t>(std::min(params.size(), size_t{28}));
    for (int i = 0; i < pc.paramCount; ++i)
        pc.params[i] = params[static_cast<size_t>(i)];

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier between passes — use image barrier for correct
    // storage-image → sampled-image cache coherency on all GPUs.
    VkMemoryBarrier memBarrier{};
    memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

void EffectProcessor::copyImage(VkCommandBuffer cmd, int sourceIdx, int targetIdx)
{
    if (targetIdx < 0 || targetIdx > 1)
        return;

    if (sourceIdx < 0) {
        // Source is the external image (first effect in chain).
        // Use dispatchPass with a passthrough: the current ring slot's source
        // set reads from the external source (binding 1) and writes to
        // storage[targetIdx] (binding 0).
        // We use the color correct pipeline with identity params as a passthrough.
        std::vector<float> identity(28, 0.0f);
        identity[0] = 1.0f;  // brightness = 1 (neutral)
        identity[1] = 1.0f;  // contrast = 1 (neutral)
        identity[2] = 1.0f;  // saturation = 1 (neutral)
        dispatchPass(cmd, getPipeline(EffectType::ColorCorrect), m_curSourceSet, identity);
    } else if (sourceIdx == targetIdx) {
        // Same source and target, no copy needed.
        return;
    } else {
        // Storage-to-storage copy
        VkImage srcImage = m_storageTextures[sourceIdx].image();
        VkImage dstImage = m_storageTextures[targetIdx].image();

        // Transition source to TRANSFER_SRC
        VkImageMemoryBarrier srcBarrier{};
        srcBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.image                           = srcImage;
        srcBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        srcBarrier.subresourceRange.levelCount     = 1;
        srcBarrier.subresourceRange.layerCount     = 1;
        srcBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        srcBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_READ_BIT;

        // Transition destination to TRANSFER_DST
        VkImageMemoryBarrier dstBarrier{};
        dstBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBarrier.oldLayout                       = VK_IMAGE_LAYOUT_GENERAL;
        dstBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.image                           = dstImage;
        dstBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        dstBarrier.subresourceRange.levelCount     = 1;
        dstBarrier.subresourceRange.layerCount     = 1;
        dstBarrier.srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        dstBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;

        VkImageMemoryBarrier barriers[2] = {srcBarrier, dstBarrier};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount     = 1;
        copyRegion.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount     = 1;
        copyRegion.extent.width                  = m_config.width;
        copyRegion.extent.height                 = m_config.height;
        copyRegion.extent.depth                  = 1;

        vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

        // Transition both back to GENERAL
        srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        VkImageMemoryBarrier restore[2] = {srcBarrier, dstBarrier};
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, restore);
    }
}

// =============================================================================
//  Internal — effect-mask mix (Premiere: masks limit where an effect applies)
// =============================================================================

void EffectProcessor::dispatchMaskMix(VkCommandBuffer cmd,
                                      const VkDescriptorImageInfo& origInfo,
                                      const VkDescriptorImageInfo& maskInfo,
                                      int targetIdx)
{
    if (m_maskMixPipeline == VK_NULL_HANDLE ||
        m_maskMixPipelineLayout == VK_NULL_HANDLE ||
        m_maskMixDescriptorSets.empty() ||
        targetIdx < 0 || targetIdx > 1)
        return;

    // Claim a ring slot — one per dispatch, so concurrently-recorded and
    // in-flight mixes never share a mutable binding (same reasoning as the
    // source ring in process()).
    VkDescriptorSet ds = m_maskMixDescriptorSets[m_maskMixRingCursor];
    m_maskMixRingCursor =
        (m_maskMixRingCursor + 1) %
        static_cast<uint32_t>(m_maskMixDescriptorSets.size());

    VkDescriptorImageInfo fxInfo{};
    fxInfo.imageView   = m_storageTextures[targetIdx].imageView();
    fxInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo orig = origInfo;
    if (orig.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        orig.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo mask = maskInfo;
    if (mask.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        mask.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = ds;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo      = &fxInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = ds;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &orig;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = ds;
    writes[2].dstBinding      = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo      = &mask;

    vkUpdateDescriptorSets(m_device->handle(), 3, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_maskMixPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_maskMixPipelineLayout, 0, 1, &ds, 0, nullptr);

    EffectPushConstants pc{};
    pc.width      = static_cast<int32_t>(m_config.width);
    pc.height     = static_cast<int32_t>(m_config.height);
    pc.paramCount = 0;
    vkCmdPushConstants(cmd, m_maskMixPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);

    uint32_t gx = (m_config.width  + 15) / 16;
    uint32_t gy = (m_config.height + 15) / 16;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier: mix write → downstream read (next effect / composite)
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

int EffectProcessor::dispatchUltraKey(VkCommandBuffer cmd,
                                      const std::vector<float>& params,
                                      int sourceIdx, int targetIdx)
{
    VkDescriptorSet ds = (sourceIdx == -1) ? m_curSourceSet
                                           : m_descriptorSets[targetIdx];

    // Determine whether matte cleanup (choke/soften/contrast) is needed.
    // Indices match ChromaKey::Param: Choke=10, Soften=11, Contrast=12.
    bool needsCleanup = false;
    if (params.size() > 12) {
        float chokeVal    = params[10];
        float softenVal   = params[11];
        float contrastVal = params[12];
        needsCleanup = (chokeVal > 0.01f || softenVal > 0.01f ||
                        contrastVal > 0.01f);
    }

    // ── Experimental single-pass path (opt-in via env var) ─────────────
    static bool singlePass = []() {
        const char* v = std::getenv("ROUNDTABLE_ULTRAKEY_SINGLEPASS");
        return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
    }();

    if (singlePass) {
        if (!needsCleanup && m_ultraKeyLightPipeline != VK_NULL_HANDLE) {
            dispatchPass(cmd, m_ultraKeyLightPipeline, ds, params);
            return targetIdx;
        }
        if (m_ultraKeyCombinedPipeline != VK_NULL_HANDLE) {
            dispatchPass(cmd, m_ultraKeyCombinedPipeline, ds, params);
            return targetIdx;
        }
    }

    // ── Proven 3-pass pipeline (matte → cleanup → finalize) ────────────
    // Always runs all 3 passes to maintain correct output-slot parity.
    // The cleanup pass is lightweight when params are at defaults
    // (choke=0, soften=0, contrast=0) — it just reads/clamps alpha.

    // Pass 1: Matte generation  (source → targetIdx)
    dispatchPass(cmd, m_ultraKeyMattePipeline, ds, params);

    int pass1Output = targetIdx;
    int pass2Target = 1 - pass1Output;

    // Pass 2: Matte cleanup  (pass1Output → pass2Target)
    {
        VkDescriptorSet ds2 = m_descriptorSets[pass2Target];
        dispatchPass(cmd, m_ultraKeyCleanupPipeline, ds2, params);
    }

    int pass2Output = pass2Target;
    int pass3Target = 1 - pass2Output;

    // Pass 3: Finalize (spill suppress + color correct + output mode)
    //         (pass2Output → targetIdx, maintaining slot parity)
    {
        VkDescriptorSet ds3 = m_descriptorSets[pass3Target];
        dispatchPass(cmd, m_ultraKeyFinalizePipeline, ds3, params);
    }

    return pass3Target;
}

} // namespace rt
