/*
 * GpuContext.cpp — Application-wide Vulkan context singleton.
 */

#include "GpuContext.h"
#include "Compositor.h"
#include "EffectProcessor.h"
#include "GpuResourceManager.h"
#include "GpuScheduler.h"
#include "Nv12Converter.h"
#include "SpineRenderer.h"
#include "TransitionRenderer.h"
#include "cuda/CudaVulkanInterop.h"
#include "cuda/CudaContext.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <vk_mem_alloc.h>
#include <volk.h>
#include <cstdlib>     // std::getenv (validation opt-in env vars)
#include <thread>
#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Singleton
// ═════════════════════════════════════════════════════════════════════════════

GpuContext& GpuContext::get() noexcept
{
    static GpuContext s_instance;
    return s_instance;
}

GpuContext::~GpuContext()
{
    shutdown();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lifecycle
// ═════════════════════════════════════════════════════════════════════════════

bool GpuContext::init(VkSurfaceKHR surface)
{
    if (m_initialized) return true;  // Already done

    spdlog::info("GpuContext: Initializing Vulkan...");

    // 1) Create Vulkan instance with surface extensions
    InstanceConfig instCfg;
    instCfg.extraExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    instCfg.extraExtensions.push_back("VK_KHR_win32_surface");
#endif

    // Allow Release builds to opt into validation via environment variable
    // for stress testing.  Set ROUNDTABLE_VALIDATION=1 to force validation
    // ON regardless of build config; set ROUNDTABLE_GPU_ASSISTED=1 to add
    // GPU-Assisted Validation (significant perf cost) when investigating
    // shader-side bugs; set ROUNDTABLE_VALIDATION_FATAL=0 to disable the
    // debug-break on validation errors during unattended stress runs.
    auto envFlag = [](const char* name, bool dflt) -> bool {
        if (const char* v = std::getenv(name)) {
            return !(v[0] == '\0' || v[0] == '0' || v[0] == 'f' || v[0] == 'F');
        }
        return dflt;
    };
    instCfg.enableValidation                = envFlag("ROUNDTABLE_VALIDATION",
                                                       instCfg.enableValidation);
    instCfg.enableGpuAssistedValidation     = envFlag("ROUNDTABLE_GPU_ASSISTED",
                                                       instCfg.enableGpuAssistedValidation);
    instCfg.validationErrorsFatal           = envFlag("ROUNDTABLE_VALIDATION_FATAL",
                                                       instCfg.validationErrorsFatal);
    // Sync validation is cheap; only override via env if explicitly disabled.
    instCfg.enableSynchronizationValidation = envFlag("ROUNDTABLE_SYNC_VALIDATION",
                                                       instCfg.enableSynchronizationValidation);

    if (!m_instance.create(instCfg)) {
        spdlog::error("GpuContext: Failed to create Vulkan instance");
        return false;
    }
    spdlog::info("GpuContext: Vulkan instance created (validation={}, sync={}, gpu-assisted={})",
                 m_instance.validationEnabled(),
                 instCfg.enableValidation && instCfg.enableSynchronizationValidation,
                 instCfg.enableValidation && instCfg.enableGpuAssistedValidation);

    // 2) Select physical device + create logical device
    if (!m_device.create(m_instance, surface)) {
        spdlog::error("GpuContext: Failed to create Vulkan device");
        m_instance.destroy();
        return false;
    }
    spdlog::info("GpuContext: Using GPU '{}' ({:.1f} GB VRAM)",
                 m_device.gpuInfo().name,
                 m_device.gpuInfo().vramSize / (1024.0 * 1024.0 * 1024.0));

    // 3) Create VMA allocator
    if (!m_allocator.create(m_instance, m_device)) {
        spdlog::error("GpuContext: Failed to create VMA allocator");
        m_device.destroy();
        m_instance.destroy();
        return false;
    }

    // 4) Create command pool for compute/graphics queue
    uint32_t computeFamily = m_device.queueFamilies().compute.value_or(
        m_device.queueFamilies().graphics.value_or(0));
    if (!m_cmdPool.create(m_device.handle(), computeFamily)) {
        spdlog::error("GpuContext: Failed to create command pool");
        m_allocator.destroy();
        m_device.destroy();
        m_instance.destroy();
        return false;
    }

    // 5) If graphics queue is on a different family, create a graphics
    //    command pool so SpineRenderer (which needs render passes) can
    //    submit to the graphics queue correctly.
    uint32_t graphicsFamily = m_device.queueFamilies().graphics.value_or(0);
    if (graphicsFamily != computeFamily) {
        if (!m_graphicsCmdPool.create(m_device.handle(), graphicsFamily)) {
            spdlog::error("GpuContext: Failed to create graphics command pool");
            m_cmdPool.destroy();
            m_allocator.destroy();
            m_device.destroy();
            m_instance.destroy();
            return false;
        }
        spdlog::info("GpuContext: Separate graphics command pool for family {}",
                     graphicsFamily);
    }

    // 6) Initialize the central GPU submission scheduler.  After this,
    //    new code should route every vkQueueSubmit through it.  Existing
    //    direct callers are migrated incrementally in subsequent P1
    //    commits.
    if (!m_scheduler.init(m_device.handle(),
                           m_device.graphicsQueue(), &m_graphicsQueueMutex,
                           m_device.computeQueue(),  &m_computeQueueMutex,
                           m_device.transferQueue(), &m_transferQueueMutex))
    {
        spdlog::error("GpuContext: Failed to init GpuScheduler");
        m_graphicsCmdPool.destroy();
        m_cmdPool.destroy();
        m_allocator.destroy();
        m_device.destroy();
        m_instance.destroy();
        return false;
    }

    m_initialized = true;
    m_gpuState.store(GpuState::Healthy, std::memory_order_release);
    spdlog::info("GpuContext: Vulkan initialization complete");

    // Initialize shared staging ring (64 MB — absorbs CompositeService's ring)
    m_resourceManager = std::make_unique<GpuResourceManager>();
    m_resourceManager->initStagingRing(m_allocator.handle(), 64u * 1024u * 1024u);

    // Eager-init TransitionRenderer at the default resolution so the
    // ~800 ms shader-compile stall happens here during startup instead
    // of on the first transition during playback.  Any later resize
    // request at a different resolution is handled via resize().
    transitionRenderer(1920, 1080);

    return true;
}

void GpuContext::shutdown()
{
    if (!m_initialized) return;

    // Invariant: shutdown() is called EXACTLY ONCE per process, during
    // App::~App.  It is NOT called from tryRecover() anymore (see comment
    // on tryRecover): destroying these subsystems mid-flight invalidates
    // every raw VkImage / VkSemaphore / VkPipeline handle held by external
    // consumers (VulkanViewport, FrameProducer::m_lastGoodFrame, cached
    // CompositeEngine state, etc.) and the next Vulkan call from any of
    // them crashes in nvoglv64.dll.  Keep it that way.

    spdlog::info("GpuContext: Shutting down Vulkan...");

    const bool deviceLost = gpuState() != GpuState::Healthy;

    // A lost device is not guaranteed to signal idle. Vulkan objects may be
    // destroyed after device loss, and no work is resumed in this process.
    if (m_device.handle() && !deviceLost)
        m_device.waitIdle();
    else if (m_device.handle())
        spdlog::warn("GpuContext: skipping device-idle wait after device loss");

    // Drain the inter-queue binary-semaphore pool before destroying the
    // device.  These semaphores are owned process-wide and shared between
    // CompositeEngine (acquire) and VulkanViewport (release); the only
    // safe time to destroy them is here, after normal waitIdle (or once a
    // lost device has been declared unusable) and before
    // m_device.destroy().
    {
        std::lock_guard lock(m_binarySemaphorePoolMutex);
        for (VkSemaphore sem : m_binarySemaphorePool) {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(m_device.handle(), sem, nullptr);
        }
        m_binarySemaphorePool.clear();
    }

    // Tear down the scheduler.  Just drops queue/mutex pointers — the
    // actual VkQueues live in Device and are destroyed below.  No GPU
    // work is drained here because normal shutdown waited above; fatal
    // shutdown deliberately abandons in-flight work on the lost device.
    m_scheduler.shutdown();

    // Destroy in reverse order.
    // m_resourceManager must be destroyed BEFORE the VMA allocator and
    // device because its staging ring calls vmaUnmapMemory/vmaDestroyBuffer.
    // If destroyed later (during ~GpuContext static destructor at CRT exit),
    // the VMA allocator is already freed → ACCESS_VIOLATION in nvoglv64.dll.
    m_resourceManager.reset();
    m_cudaVulkanInterop.reset();
    m_nv12Converters.clear();
    m_transitionRenderer.reset();
    m_spineRenderer.reset();
    m_effectProcessors.clear();
    m_compositor.reset();
    m_graphicsCmdPool.destroy();
    m_cmdPool.destroy();
    m_allocator.destroy();
    m_device.destroy();
    m_instance.destroy();

    m_effectProcessorRequests = 0;
    m_effectProcessorCacheHits = 0;
    m_effectProcessorCreations = 0;
    m_nv12ConverterRequests = 0;
    m_nv12ConverterCacheHits = 0;
    m_nv12ConverterCreations = 0;

    m_initialized = false;
    spdlog::info("GpuContext: Vulkan shutdown complete");
}

// ═════════════════════════════════════════════════════════════════════════════
//  Device-lost — fatal failure (was: tryRecover with full in-place re-init)
// ═════════════════════════════════════════════════════════════════════════════
//
// The previous implementation tore down the entire VkInstance / VkDevice
// inside a worker thread, slept 50ms, then re-init()'d.  Two fatal problems:
//
//   1. Every other component (VulkanViewport, FrameProducer's lastGoodFrame,
//      cached subsystems) still held raw VkImageView / VkSemaphore / VkFence
//      handles from the destroyed device.  The very next vkSomething() call
//      with one of those handles crashed inside the NVIDIA dispatch table
//      (nvoglv64.dll!vkGetInstanceProcAddr+0xf36db7 — exactly the crash in
//      the captured dump).
//
//   2. The 50ms sleep was nowhere near long enough for WDDM to release the
//      wedged device.  In the captured log the re-init returned
//      VK_ERROR_INITIALIZATION_FAILED, after which we marched on with a
//      VkInstance::handle() == VK_NULL_HANDLE singleton.
//
// Every major NLE (Premiere, Resolve, AE) treats device-lost as fatal.  Now
// we do too: transition to Failed, fire the callback (which is expected to
// present a modal restart dialog on the UI thread), and return false.
bool GpuContext::tryRecover()
{
    m_gpuState.store(GpuState::Failed, std::memory_order_release);

    spdlog::error("[GPU] Device lost — fatal. Application must be restarted.");

    bool expected = false;
    if (m_fatalFailureCallback &&
        m_fatalFailureFired.compare_exchange_strong(expected, true))
    {
        try {
            m_fatalFailureCallback();
        } catch (const std::exception& e) {
            spdlog::error("[GPU] Fatal-failure callback threw: {}", e.what());
        } catch (...) {
            spdlog::error("[GPU] Fatal-failure callback threw unknown exception");
        }
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared binary-semaphore pool (compositor→presenter inter-queue sync)
// ═════════════════════════════════════════════════════════════════════════════

VkSemaphore GpuContext::acquireBinarySemaphore()
{
    {
        std::lock_guard lock(m_binarySemaphorePoolMutex);
        if (!m_binarySemaphorePool.empty()) {
            VkSemaphore sem = m_binarySemaphorePool.back();
            m_binarySemaphorePool.pop_back();
            return sem;
        }
    }
    // Pool empty — allocate.  Outside the mutex so vkCreateSemaphore can't
    // re-enter through any logging/allocator path.
    if (!m_initialized || m_device.handle() == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore sem = VK_NULL_HANDLE;
    if (vkCreateSemaphore(m_device.handle(), &info, nullptr, &sem) != VK_SUCCESS) {
        spdlog::warn("GpuContext: vkCreateSemaphore failed for binary pool");
        return VK_NULL_HANDLE;
    }
    return sem;
}

void GpuContext::releaseBinarySemaphore(VkSemaphore sem)
{
    if (sem == VK_NULL_HANDLE) return;
    std::lock_guard lock(m_binarySemaphorePoolMutex);
    m_binarySemaphorePool.push_back(sem);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared Compositor
// ═════════════════════════════════════════════════════════════════════════════

ICompositor* GpuContext::compositor(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_compositor) {
        auto* vkComp = new Compositor();
        m_compositor.reset(vkComp);

        CompositorConfig cfg;
        cfg.outputWidth  = width;
        cfg.outputHeight = height;

        VkQueue queue = m_device.computeQueue()
                            ? m_device.computeQueue()
                            : m_device.graphicsQueue();

        if (!vkComp->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared Compositor");
            m_compositor.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: Shared Compositor created ({}x{})", width, height);
    }
    else if (static_cast<Compositor*>(m_compositor.get())->outputWidth() != width ||
             static_cast<Compositor*>(m_compositor.get())->outputHeight() != height) {
        m_compositor->resize(width, height);
    }

    return m_compositor.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared EffectProcessor
// ═════════════════════════════════════════════════════════════════════════════

EffectProcessor* GpuContext::effectProcessor(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);
    ++m_effectProcessorRequests;

    const uint64_t key = (static_cast<uint64_t>(width) << 32)
                       | static_cast<uint64_t>(height);
    auto it = m_effectProcessors.find(key);
    if (it != m_effectProcessors.end()) {
        ++m_effectProcessorCacheHits;
        if (m_effectProcessorRequests % 120 == 0) {
            const uint64_t misses = m_effectProcessorRequests - m_effectProcessorCacheHits;
            const double hitRate = m_effectProcessorRequests > 0
                ? (100.0 * static_cast<double>(m_effectProcessorCacheHits)
                   / static_cast<double>(m_effectProcessorRequests))
                : 0.0;
            spdlog::info("[PERF] GpuContext EffectProcessor cache: req={} hit={} miss={} hitRate={:.1f}% entries={}",
                         m_effectProcessorRequests, m_effectProcessorCacheHits,
                         misses, hitRate, m_effectProcessors.size());
        }
        return it->second.get();
    }

    auto processor = std::make_unique<EffectProcessor>();

    EffectProcessorConfig cfg;
    cfg.width  = width;
    cfg.height = height;

    VkQueue queue = m_device.computeQueue()
                        ? m_device.computeQueue()
                        : m_device.graphicsQueue();

    if (!processor->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
        spdlog::error("GpuContext: Failed to init shared EffectProcessor ({}x{})",
                      width, height);
        return nullptr;
    }

    spdlog::info("GpuContext: Shared EffectProcessor created ({}x{})", width, height);
    auto* result = processor.get();
    m_effectProcessors.emplace(key, std::move(processor));
    ++m_effectProcessorCreations;
    spdlog::info("[PERF] GpuContext EffectProcessor cache MISS: {}x{} -> created entry {} (requests={}, hits={}, creations={})",
                 width, height, m_effectProcessors.size(),
                 m_effectProcessorRequests, m_effectProcessorCacheHits,
                 m_effectProcessorCreations);

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared SpineRenderer
// ═════════════════════════════════════════════════════════════════════════════

SpineRenderer* GpuContext::spineRenderer(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_spineRenderer) {
        m_spineRenderer = std::make_unique<SpineRenderer>();

        SpineRendererConfig cfg;
        cfg.renderWidth  = width;
        cfg.renderHeight = height;

        // When using async compute for the compositor, the spine FBO must be
        // accessible from both the graphics and compute queue families.
        auto& qf = m_device.queueFamilies();
        if (qf.graphics.has_value() && qf.compute.has_value() &&
            qf.graphics.value() != qf.compute.value()) {
            cfg.concurrentQueueFamilies = {qf.graphics.value(),
                                           qf.compute.value()};
        }

        // SpineRenderer needs a graphics queue for vertex/fragment shaders
        VkQueue queue = m_device.graphicsQueue()
                            ? m_device.graphicsQueue()
                            : m_device.computeQueue();

        // Must use the graphics-family command pool so command buffers can
        // be submitted to the graphics queue (queue family must match).
        if (!m_spineRenderer->init(m_device, m_allocator, graphicsCmdPool(), queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared SpineRenderer");
            m_spineRenderer.reset();
            return nullptr;
        }

        // Plumb the graphics-queue mutex so Spine's submit serializes with
        // other graphics-queue users (VulkanViewport, EffectProcessor).
        m_spineRenderer->setQueueMutex(&graphicsQueueMutex());

        // When graphics and compute queue families differ, register the
        // compute queue so SpineRenderer's beginFrame() can drain any
        // in-flight compositor sampling of the shared framebuffer before
        // re-transitioning it to COLOR_ATTACHMENT.  See SpineRenderer.cpp
        // beginFrame() for the full hazard description.
        if (qf.graphics.has_value() && qf.compute.has_value() &&
            qf.graphics.value() != qf.compute.value()) {
            m_spineRenderer->setComputeQueue(m_device.computeQueue(),
                                             &computeQueueMutex());
            spdlog::info("GpuContext: Spine cross-queue sync enabled "
                         "(graphics family {} → compute family {})",
                         qf.graphics.value(), qf.compute.value());
        }

        spdlog::info("GpuContext: Shared SpineRenderer created ({}x{})", width, height);
    }
    else if (m_spineRenderer->framebuffer().width() != width ||
             m_spineRenderer->framebuffer().height() != height) {
        m_spineRenderer->resize(width, height);
    }

    return m_spineRenderer.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared TransitionRenderer
// ═════════════════════════════════════════════════════════════════════════════

TransitionRenderer* GpuContext::transitionRenderer(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

    if (!m_transitionRenderer) {
        m_transitionRenderer = std::make_unique<TransitionRenderer>();

        TransitionConfig cfg;
        cfg.outputWidth  = width;
        cfg.outputHeight = height;

        VkQueue queue = m_device.computeQueue()
                            ? m_device.computeQueue()
                            : m_device.graphicsQueue();

        if (!m_transitionRenderer->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
            spdlog::error("GpuContext: Failed to init shared TransitionRenderer");
            m_transitionRenderer.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: Shared TransitionRenderer created ({}x{})", width, height);
    }
    else if (m_transitionRenderer->outputWidth() != width ||
             m_transitionRenderer->outputHeight() != height) {
        m_transitionRenderer->resize(width, height);
    }

    return m_transitionRenderer.get();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared Nv12Converter
// ═════════════════════════════════════════════════════════════════════════════

Nv12Converter* GpuContext::nv12Converter(uint32_t width, uint32_t height)
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);
    ++m_nv12ConverterRequests;

    const uint64_t key = (static_cast<uint64_t>(width) << 32)
                       | static_cast<uint64_t>(height);
    auto it = m_nv12Converters.find(key);
    if (it != m_nv12Converters.end()) {
        ++m_nv12ConverterCacheHits;
        if (m_nv12ConverterRequests % 120 == 0) {
            const uint64_t misses = m_nv12ConverterRequests - m_nv12ConverterCacheHits;
            const double hitRate = m_nv12ConverterRequests > 0
                ? (100.0 * static_cast<double>(m_nv12ConverterCacheHits)
                   / static_cast<double>(m_nv12ConverterRequests))
                : 0.0;
            spdlog::info("[PERF] GpuContext Nv12Converter cache: req={} hit={} miss={} hitRate={:.1f}% entries={}",
                         m_nv12ConverterRequests, m_nv12ConverterCacheHits,
                         misses, hitRate, m_nv12Converters.size());
        }
        return it->second.get();
    }

    auto converter = std::make_unique<Nv12Converter>();

    Nv12ConverterConfig cfg;
    cfg.width  = width;
    cfg.height = height;

    VkQueue queue = m_device.computeQueue()
                        ? m_device.computeQueue()
                        : m_device.graphicsQueue();

    if (!converter->init(m_device, m_allocator, m_cmdPool, queue, cfg)) {
        spdlog::warn("GpuContext: Nv12Converter init failed for {}x{} — falling back to CPU sws_scale",
                     width, height);
        return nullptr;
    }

    spdlog::info("GpuContext: Shared Nv12Converter created ({}x{})", width, height);
    auto* result = converter.get();
    m_nv12Converters.emplace(key, std::move(converter));
    ++m_nv12ConverterCreations;
    spdlog::info("[PERF] GpuContext Nv12Converter cache MISS: {}x{} -> created entry {} (requests={}, hits={}, creations={})",
                 width, height, m_nv12Converters.size(),
                 m_nv12ConverterRequests, m_nv12ConverterCacheHits,
                 m_nv12ConverterCreations);

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shared CudaVulkanInterop
// ═════════════════════════════════════════════════════════════════════════════

CudaVulkanInterop* GpuContext::cudaVulkanInterop()
{
    if (!m_initialized) return nullptr;

    std::lock_guard lock(m_subsystemMutex);

#ifdef ROUNDTABLE_HAS_CUDA
    if (!m_cudaVulkanInterop) {
        // Need a CudaContext — create one if not yet available
        static CudaContext s_cudaCtx;
        if (!s_cudaCtx.isAvailable()) {
            if (!s_cudaCtx.init()) {
                spdlog::warn("GpuContext: CUDA init failed — "
                             "CudaVulkanInterop not available");
                return nullptr;
            }
        }

        m_cudaVulkanInterop = std::make_unique<CudaVulkanInterop>(s_cudaCtx);
        if (!m_cudaVulkanInterop->init(m_device.handle(),
                                       m_device.physicalDevice())) {
            spdlog::warn("GpuContext: CudaVulkanInterop init failed");
            m_cudaVulkanInterop.reset();
            return nullptr;
        }

        spdlog::info("GpuContext: CudaVulkanInterop created");
    }
    return m_cudaVulkanInterop.get();
#else
    return nullptr;
#endif
}

bool GpuContext::cudaAvailable() const noexcept
{
#ifdef ROUNDTABLE_HAS_CUDA
    // Quick check: can we load the NVIDIA driver DLL?
    HMODULE mod = LoadLibraryExW(L"nvcuda.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (mod) {
        FreeLibrary(mod);
        return true;
    }
    return false;
#else
    return false;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
//  Texture readback utility
// ═════════════════════════════════════════════════════════════════════════════

bool GpuContext::readbackTexture(void* texturePtr,
                                  uint32_t width, uint32_t height,
                                  std::vector<uint8_t>& outPixels)
{
    if (!m_initialized || !texturePtr) return false;

    // Serialise concurrent readbacks. m_cmdPool
    // is host-externally-synchronised; without this, the disk cache worker
    // and an export thread racing on a freshly-decoded GPU-resident frame
    // would crash in vkAllocateCommandBuffers / vkBeginCommandBuffer.
    std::lock_guard lk(m_readbackMutex);

    auto* tex = static_cast<Texture*>(texturePtr);
    const VkDeviceSize bufSz = static_cast<VkDeviceSize>(width) * height * 4;

    // Create CPU-visible staging buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = bufSz;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer sb{VK_NULL_HANDLE};
    VmaAllocation sa{nullptr};
    if (vmaCreateBuffer(m_allocator.handle(), &bci, &aci,
                        &sb, &sa, nullptr) != VK_SUCCESS)
        return false;

    // Record copy commands
    VkCommandBuffer cmd = m_cmdPool.beginSingleTime();

    tex->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent      = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, tex->image(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &rgn);

    tex->transitionLayout(cmd,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    m_cmdPool.endSingleTime(cmd, computeQueue());

    // Map, copy, cleanup
    outPixels.resize(static_cast<size_t>(bufSz));
    void* mapped = nullptr;
    vmaMapMemory(m_allocator.handle(), sa, &mapped);
    std::memcpy(outPixels.data(), mapped, bufSz);
    vmaUnmapMemory(m_allocator.handle(), sa);
    vmaDestroyBuffer(m_allocator.handle(), sb, sa);

    return true;
}

} // namespace rt
