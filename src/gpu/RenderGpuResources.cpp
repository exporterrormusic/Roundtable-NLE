#include "RenderGpuResources.h"

#include "Compositor.h"
#include "EffectProcessor.h"
#include "GpuContext.h"
#include "Nv12Converter.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"

#include <spdlog/spdlog.h>

namespace rt {

namespace {

template <typename T>
std::shared_ptr<T> makeScopedHelper()
{
    return std::shared_ptr<T>(new T(), [](T* helper) {
        if (!helper) return;
        helper->shutdown(GpuTeardownMode::SessionScoped);
        delete helper;
    });
}

} // namespace

RenderGpuResources::RenderGpuResources() = default;

RenderGpuResources::~RenderGpuResources()
{
    shutdown();
}

uint64_t RenderGpuResources::sizeKey(uint32_t width, uint32_t height) noexcept
{
    return (static_cast<uint64_t>(width) << 32) | static_cast<uint64_t>(height);
}

bool RenderGpuResources::init()
{
    if (m_initialized)
        return true;

    auto& ctx = GpuContext::get();
    if (!ctx.isInitialized() || ctx.vkDevice() == VK_NULL_HANDLE)
        return false;

    auto& device = ctx.device();
    const auto& families = device.queueFamilies();
    const uint32_t computeFamily =
        families.compute.value_or(families.graphics.value_or(0));
    const uint32_t graphicsFamily =
        families.graphics.value_or(computeFamily);

    m_computeQueue = device.computeQueue()
        ? device.computeQueue() : device.graphicsQueue();
    m_graphicsQueue = device.graphicsQueue()
        ? device.graphicsQueue() : m_computeQueue;

    if (!m_computeCommandPool.create(ctx.vkDevice(), computeFamily)) {
        spdlog::error("RenderGpuResources: failed to create private compute command pool");
        shutdown();
        return false;
    }
    m_computeCommandPool.setQueueMutex(&ctx.computeQueueMutex());

    if (!m_graphicsCommandPool.create(ctx.vkDevice(), graphicsFamily)) {
        spdlog::error("RenderGpuResources: failed to create private graphics command pool");
        shutdown();
        return false;
    }
    m_graphicsCommandPool.setQueueMutex(&ctx.graphicsQueueMutex());

    m_initialized = true;
    spdlog::info("RenderGpuResources: isolated render-session resources initialized");
    return true;
}

void RenderGpuResources::shutdown(GpuTeardownMode mode)
{
    auto& ctx = GpuContext::get();
    const bool deviceHealthy =
        ctx.isInitialized() && ctx.gpuState() == GpuState::Healthy;

    if (m_initialized && deviceHealthy) {
        if (mode == GpuTeardownMode::DeviceWide) {
            // Direct owners without a CompositeEngine retain the conservative
            // contract, but pay for one synchronized device wait rather than
            // one wait in every helper destructor.
            ctx.scheduler().deviceWaitIdle();
        } else {
            constexpr uint64_t kSessionDrainTimeoutNs = 5'000'000'000ull;
            bool drained = true;
            m_spineRenderers.forEach([&](const auto&, const auto& renderer) {
                if (renderer)
                    drained = renderer->waitForFrame() && drained;
            });
            m_effectProcessors.forEach([&](uint64_t, const auto& processor) {
                if (processor) {
                    drained = processor->waitForOwnedWork(
                        kSessionDrainTimeoutNs) && drained;
                }
            });
            if (!drained && ctx.gpuState() == GpuState::Healthy) {
                spdlog::warn("RenderGpuResources: session fence drain failed; "
                             "falling back to device-wide idle");
                ctx.scheduler().deviceWaitIdle();
            }
        }
    }

    // Every helper-specific asynchronous path is now drained. Custom shared
    // deleters perform scoped teardown when the final lease is released.
    (void)m_nv12Converters.clear();
    (void)m_spineRenderers.clear();
    (void)m_transitionRenderers.clear();
    (void)m_effectProcessors.clear();
    (void)m_compositors.clear();

    m_graphicsCommandPool.destroy();
    m_computeCommandPool.destroy();
    m_graphicsQueue = VK_NULL_HANDLE;
    m_computeQueue = VK_NULL_HANDLE;
    m_initialized = false;
}

GpuGenerationWorkingSetStats
RenderGpuResources::generationCacheStats() const noexcept
{
    return {
        m_compositors.stats(),
        m_effectProcessors.stats(),
        m_spineRenderers.stats(),
        m_transitionRenderers.stats(),
        m_nv12Converters.stats()
    };
}

std::shared_ptr<Compositor> RenderGpuResources::compositor(
    uint32_t width, uint32_t height)
{
    if (!m_initialized)
        return nullptr;

    const uint64_t key = sizeKey(width, height);
    if (auto cached = m_compositors.find(key))
        return cached;

    auto compositor = makeScopedHelper<Compositor>();
    CompositorConfig config{};
    config.outputWidth = width;
    config.outputHeight = height;
    auto& ctx = GpuContext::get();
    if (!compositor->init(ctx.device(), ctx.allocator(),
                          m_computeCommandPool, m_computeQueue, config)) {
        spdlog::error("RenderGpuResources: isolated compositor init failed");
        return nullptr;
    }

    auto result = compositor;
    const auto retired = m_compositors.insert(
        key, std::move(compositor), estimatedGpuImageBytes(width, height, 4, 6));
    spdlog::info("RenderGpuResources: isolated compositor generation created "
                 "({}x{}, resident={}, retired={})", width, height,
                 m_compositors.size(), retired.size());
    return result;
}

std::shared_ptr<EffectProcessor> RenderGpuResources::effectProcessor(
    uint32_t width, uint32_t height)
{
    if (!m_initialized)
        return nullptr;

    const uint64_t key = sizeKey(width, height);
    if (auto cached = m_effectProcessors.find(key))
        return cached;

    auto processor = makeScopedHelper<EffectProcessor>();
    EffectProcessorConfig config{};
    config.width = width;
    config.height = height;
    auto& ctx = GpuContext::get();
    if (!processor->init(ctx.device(), ctx.allocator(), m_computeCommandPool,
                         m_computeQueue, config)) {
        spdlog::error("RenderGpuResources: isolated effect processor init failed ({}x{})",
                      width, height);
        return nullptr;
    }

    auto result = processor;
    const auto retired = m_effectProcessors.insert(
        key, std::move(processor), estimatedGpuImageBytes(width, height, 8, 3));
    spdlog::info("RenderGpuResources: isolated effect processor created "
                 "({}x{}, resident={}, retired={})", width, height,
                 m_effectProcessors.size(), retired.size());
    return result;
}

std::shared_ptr<TransitionRenderer> RenderGpuResources::transitionRenderer(
    uint32_t width, uint32_t height)
{
    if (!m_initialized)
        return nullptr;

    const uint64_t key = sizeKey(width, height);
    if (auto cached = m_transitionRenderers.find(key))
        return cached;

    auto renderer = makeScopedHelper<TransitionRenderer>();
    TransitionConfig config{};
    config.outputWidth = width;
    config.outputHeight = height;
    auto& ctx = GpuContext::get();
    if (!renderer->init(ctx.device(), ctx.allocator(),
                        m_computeCommandPool, m_computeQueue, config)) {
        spdlog::error("RenderGpuResources: isolated transition renderer init failed");
        return nullptr;
    }

    auto result = renderer;
    const auto retired = m_transitionRenderers.insert(
        key, std::move(renderer), estimatedGpuImageBytes(width, height, 4, 3));
    spdlog::info("RenderGpuResources: isolated transition generation created "
                 "({}x{}, resident={}, retired={})", width, height,
                 m_transitionRenderers.size(), retired.size());
    return result;
}

std::shared_ptr<SpineRenderer> RenderGpuResources::spineRenderer(
    uint32_t width, uint32_t height, const std::string& contentKey)
{
    if (!m_initialized)
        return nullptr;

    const SpineRendererCacheKey key{width, height, contentKey};
    if (auto cached = m_spineRenderers.find(key))
        return cached;

    auto renderer = makeScopedHelper<SpineRenderer>();
    SpineRendererConfig config{};
    config.renderWidth = width;
    config.renderHeight = height;

    auto& ctx = GpuContext::get();
    if (!renderer->init(ctx.device(), ctx.allocator(),
                        m_graphicsCommandPool, m_graphicsQueue, config)) {
        spdlog::error("RenderGpuResources: isolated Spine renderer init failed");
        return nullptr;
    }
    renderer->setQueueMutex(&ctx.graphicsQueueMutex());

    auto result = renderer;
    const auto retired = m_spineRenderers.insert(
        key, std::move(renderer), estimatedGpuImageBytes(width, height, 4, 3));
    spdlog::info("RenderGpuResources: isolated Spine generation created "
                 "({}x{}, key='{}', resident={}, retired={})", width, height,
                 contentKey, m_spineRenderers.size(), retired.size());
    return result;
}

std::shared_ptr<Nv12Converter> RenderGpuResources::nv12Converter(
    uint32_t width, uint32_t height)
{
    if (!m_initialized)
        return nullptr;

    const uint64_t key = sizeKey(width, height);
    if (auto cached = m_nv12Converters.find(key))
        return cached;

    auto converter = makeScopedHelper<Nv12Converter>();
    Nv12ConverterConfig config{};
    config.width = width;
    config.height = height;
    auto& ctx = GpuContext::get();
    if (!converter->init(ctx.device(), ctx.allocator(), m_computeCommandPool,
                         m_computeQueue, config)) {
        spdlog::warn("RenderGpuResources: isolated NV12 converter init failed ({}x{})",
                     width, height);
        return nullptr;
    }

    auto result = converter;
    const auto retired = m_nv12Converters.insert(
        key, std::move(converter), estimatedGpuImageBytes(width, height, 6, 2));
    spdlog::info("RenderGpuResources: isolated NV12 converter created "
                 "({}x{}, resident={}, retired={})", width, height,
                 m_nv12Converters.size(), retired.size());
    return result;
}

} // namespace rt
