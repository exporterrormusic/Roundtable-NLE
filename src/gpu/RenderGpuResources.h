#pragma once

#include "GpuGenerationCache.h"
#include "GpuTeardownMode.h"
#include "SpineRendererCacheKey.h"
#include "vulkan/CommandPool.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace rt {

class Compositor;
class EffectProcessor;
class Nv12Converter;
class SpineRenderer;
class TransitionRenderer;

/// Mutable Vulkan helpers owned by one render consumer.
///
/// The logical device, allocator, queues, scheduler, and queue locks remain
/// process-wide in GpuContext. Command pools and stateful render helpers are
/// private because Vulkan command pools require external synchronization and
/// the helpers retain per-frame descriptors, images, and configuration.
class RenderGpuResources final
{
public:
    RenderGpuResources();
    ~RenderGpuResources();

    RenderGpuResources(const RenderGpuResources&) = delete;
    RenderGpuResources& operator=(const RenderGpuResources&) = delete;

    [[nodiscard]] bool init();
    void shutdown(GpuTeardownMode mode = GpuTeardownMode::DeviceWide);

    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    [[nodiscard]] std::shared_ptr<Compositor> compositor(uint32_t width, uint32_t height);
    [[nodiscard]] std::shared_ptr<EffectProcessor> effectProcessor(uint32_t width, uint32_t height);
    [[nodiscard]] std::shared_ptr<TransitionRenderer> transitionRenderer(uint32_t width, uint32_t height);
    [[nodiscard]] std::shared_ptr<SpineRenderer> spineRenderer(
        uint32_t width, uint32_t height,
        const std::string& contentKey = std::string{});
    [[nodiscard]] std::shared_ptr<Nv12Converter> nv12Converter(uint32_t width, uint32_t height);

    /// Read-only snapshot for diagnostics after this session's render worker
    /// has stopped. RenderGpuResources itself remains single-worker-owned.
    [[nodiscard]] GpuGenerationWorkingSetStats generationCacheStats() const noexcept;

    [[nodiscard]] CommandPool& graphicsCommandPool() noexcept { return m_graphicsCommandPool; }
    [[nodiscard]] VkQueue graphicsQueue() const noexcept { return m_graphicsQueue; }

private:
    [[nodiscard]] static uint64_t sizeKey(uint32_t width, uint32_t height) noexcept;

    // Pools are declared before their users so reverse-order destruction
    // releases every helper before destroying its command pool.
    CommandPool m_computeCommandPool;
    CommandPool m_graphicsCommandPool;
    VkQueue m_computeQueue{VK_NULL_HANDLE};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};

    // Bounded generation caches. Eviction drops only the cache lease; exact
    // command-slot/readback leases defer destruction until use is complete.
    GpuGenerationCache<Compositor> m_compositors{{3, 192ull * 1024ull * 1024ull}};
    GpuGenerationCache<EffectProcessor> m_effectProcessors{{4, 192ull * 1024ull * 1024ull}};
    GpuGenerationCache<TransitionRenderer> m_transitionRenderers{{3, 128ull * 1024ull * 1024ull}};
    GpuGenerationCache<SpineRenderer, SpineRendererCacheKey,
                       SpineRendererCacheKeyHash>
        m_spineRenderers{{6, 768ull * 1024ull * 1024ull}};
    GpuGenerationCache<Nv12Converter> m_nv12Converters{{6, 256ull * 1024ull * 1024ull}};

    bool m_initialized{false};
};

} // namespace rt
