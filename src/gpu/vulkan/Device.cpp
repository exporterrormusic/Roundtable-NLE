/*
 * Device.cpp — Physical + logical Vulkan device.
 * Step 2: Vulkan Initialization
 *
 * Enumerates GPUs, scores them (prefers RTX 4090 / discrete GPUs),
 * creates logical device with multiple queue families.
 */

#include <volk.h>
#include "vulkan/Device.h"
#include "vulkan/Instance.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <cstring>

namespace rt {

// ── Required device extensions ──────────────────────────────────────────────

std::vector<const char*> Device::requiredExtensions()
{
    return {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
#ifdef ROUNDTABLE_HAS_CUDA
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
#endif
    };
}

// ── Move semantics ──────────────────────────────────────────────────────────

Device::~Device()
{
    destroy();
}

Device::Device(Device&& other) noexcept
    : m_physicalDevice(other.m_physicalDevice),
      m_device(other.m_device),
      m_queueFamilies(other.m_queueFamilies),
      m_gpuInfo(std::move(other.m_gpuInfo)),
      m_memoryProperties(other.m_memoryProperties),
      m_graphicsQueue(other.m_graphicsQueue),
      m_computeQueue(other.m_computeQueue),
      m_transferQueue(other.m_transferQueue),
      m_presentQueue(other.m_presentQueue)
{
    other.m_physicalDevice = VK_NULL_HANDLE;
    other.m_device         = VK_NULL_HANDLE;
    other.m_graphicsQueue  = VK_NULL_HANDLE;
    other.m_computeQueue   = VK_NULL_HANDLE;
    other.m_transferQueue  = VK_NULL_HANDLE;
    other.m_presentQueue   = VK_NULL_HANDLE;
}

Device& Device::operator=(Device&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_physicalDevice   = other.m_physicalDevice;
        m_device           = other.m_device;
        m_queueFamilies    = other.m_queueFamilies;
        m_gpuInfo          = std::move(other.m_gpuInfo);
        m_memoryProperties = other.m_memoryProperties;
        m_graphicsQueue    = other.m_graphicsQueue;
        m_computeQueue     = other.m_computeQueue;
        m_transferQueue    = other.m_transferQueue;
        m_presentQueue     = other.m_presentQueue;

        other.m_physicalDevice = VK_NULL_HANDLE;
        other.m_device         = VK_NULL_HANDLE;
        other.m_graphicsQueue  = VK_NULL_HANDLE;
        other.m_computeQueue   = VK_NULL_HANDLE;
        other.m_transferQueue  = VK_NULL_HANDLE;
        other.m_presentQueue   = VK_NULL_HANDLE;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Device::create(const Instance& instance, VkSurfaceKHR surface)
{
    VkInstance vkInst = instance.handle();

    // ── Enumerate physical devices ──────────────────────────────────────
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInst, &deviceCount, nullptr);
    if (deviceCount == 0)
    {
        spdlog::error("No Vulkan-capable GPUs found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInst, &deviceCount, devices.data());

    spdlog::info("Found {} GPU(s):", deviceCount);

    // ── Score and select best device ────────────────────────────────────
    int bestScore = -1;
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;

    for (auto& dev : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        int score = rateDevice(dev, surface);

        spdlog::info("  [{}] {} (score: {})", props.deviceID, props.deviceName, score);

        if (score > bestScore)
        {
            bestScore  = score;
            bestDevice = dev;
        }
    }

    if (bestDevice == VK_NULL_HANDLE || bestScore < 0)
    {
        spdlog::error("No suitable GPU found");
        return false;
    }

    m_physicalDevice = bestDevice;
    queryGPUInfo(m_physicalDevice);
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memoryProperties);
    m_queueFamilies = findQueueFamilies(m_physicalDevice, surface);

    spdlog::info("Selected GPU: {} ({:.1f} GB VRAM)",
                 m_gpuInfo.name,
                 static_cast<double>(m_gpuInfo.vramSize) / (1024.0 * 1024.0 * 1024.0));

    // ── Create logical device ───────────────────────────────────────────
    std::set<uint32_t> uniqueFamilies;
    uniqueFamilies.insert(m_queueFamilies.graphics.value());
    if (m_queueFamilies.present.has_value())
        uniqueFamilies.insert(m_queueFamilies.present.value());
    if (m_queueFamilies.compute.has_value())
        uniqueFamilies.insert(m_queueFamilies.compute.value());
    if (m_queueFamilies.transfer.has_value())
        uniqueFamilies.insert(m_queueFamilies.transfer.value());

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties(
        queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        m_physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

    const uint32_t graphicsFamily = m_queueFamilies.graphics.value();
    const uint32_t presentFamily =
        m_queueFamilies.present.value_or(graphicsFamily);
    const bool canSeparatePresent = graphicsFamily == presentFamily &&
        graphicsFamily < queueFamilyProperties.size() &&
        queueFamilyProperties[graphicsFamily].queueCount >= 2;

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::array<float, 2> queuePriorities{1.0f, 1.0f};
    for (uint32_t family : uniqueFamilies)
    {
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        // When presentation and rendering share a family, request a second
        // queue handle if the device exposes one. vkQueuePresentKHR may block
        // for display cadence; using queue 1 for graphics submissions keeps
        // that wait from stalling the compositor's externally-synchronized
        // queue mutex.
        queueInfo.queueCount       =
            (family == graphicsFamily && canSeparatePresent) ? 2u : 1u;
        queueInfo.pQueuePriorities = queuePriorities.data();
        queueCreateInfos.push_back(queueInfo);
    }

    // ── Features ────────────────────────────────────────────────────────
    // Vulkan 1.3 features (dynamic rendering, synchronization2)
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering   = VK_TRUE;
    features13.synchronization2   = VK_TRUE;

    // Vulkan 1.2 features (timeline semaphores, descriptor indexing)
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType                                       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext                                       = &features13;
    features12.timelineSemaphore                           = VK_TRUE;
    features12.descriptorIndexing                          = VK_TRUE;
    features12.descriptorBindingPartiallyBound             = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.runtimeDescriptorArray                      = VK_TRUE;
    features12.shaderSampledImageArrayNonUniformIndexing   = VK_TRUE;
    features12.bufferDeviceAddress                         = VK_TRUE;

    // Vulkan 1.0 features
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy     = VK_TRUE;
    deviceFeatures.fillModeNonSolid      = VK_TRUE;
    deviceFeatures.wideLines             = VK_TRUE;
    deviceFeatures.independentBlend      = VK_TRUE;
    deviceFeatures.multiDrawIndirect     = VK_TRUE;
    deviceFeatures.fragmentStoresAndAtomics = VK_TRUE;

    auto extensions = requiredExtensions();

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext                   = &features12;
    createInfo.queueCreateInfoCount    = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos       = queueCreateInfos.data();
    createInfo.pEnabledFeatures        = &deviceFeatures;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create logical device (VkResult: {})", static_cast<int>(result));
        return false;
    }

    // Load device-level Vulkan functions for volk
    volkLoadDevice(m_device);

    // ── Retrieve queue handles ──────────────────────────────────────────
    // Queue 0 remains the presentation queue. Queue 1, when available in the
    // same family, is the graphics/render queue used by the scheduler.
    const uint32_t graphicsQueueIndex = canSeparatePresent ? 1u : 0u;
    vkGetDeviceQueue(m_device, graphicsFamily,
                     graphicsQueueIndex, &m_graphicsQueue);

    if (m_queueFamilies.present.has_value())
        vkGetDeviceQueue(m_device, presentFamily, 0, &m_presentQueue);
    else
        m_presentQueue = m_graphicsQueue;

    if (m_queueFamilies.compute.has_value()) {
        const uint32_t computeFamily = m_queueFamilies.compute.value();
        const uint32_t computeQueueIndex =
            (computeFamily == graphicsFamily && canSeparatePresent) ? 1u : 0u;
        vkGetDeviceQueue(m_device, computeFamily,
                         computeQueueIndex, &m_computeQueue);
    }
    else
        m_computeQueue = m_graphicsQueue;

    if (m_queueFamilies.transfer.has_value())
        vkGetDeviceQueue(m_device, m_queueFamilies.transfer.value(), 0, &m_transferQueue);
    else
        m_transferQueue = m_graphicsQueue;

    spdlog::info("Logical device created — queues: graphics={}, compute={}, transfer={}, present={}",
                 m_queueFamilies.graphics.value(),
                 m_queueFamilies.compute.value_or(m_queueFamilies.graphics.value()),
                 m_queueFamilies.transfer.value_or(m_queueFamilies.graphics.value()),
                 m_queueFamilies.present.value_or(m_queueFamilies.graphics.value()));
    spdlog::warn("[HW-DIAG] Vulkan render/present queues: {}",
                 m_graphicsQueue != m_presentQueue ? "separated" : "shared");

    if (m_queueFamilies.hasAsyncCompute())
        spdlog::info("  Async compute queue available (family {})", m_queueFamilies.compute.value());
    if (m_queueFamilies.hasDedicatedTransfer())
        spdlog::info("  Dedicated transfer queue available (family {})", m_queueFamilies.transfer.value());

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Device::destroy()
{
    if (m_device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
        spdlog::info("Vulkan logical device destroyed");
    }
    m_physicalDevice = VK_NULL_HANDLE;
}

// ── waitIdle ────────────────────────────────────────────────────────────────

void Device::waitIdle() const
{
    if (m_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(m_device);
    }
}

// ── rateDevice ──────────────────────────────────────────────────────────────

int Device::rateDevice(VkPhysicalDevice device, VkSurfaceKHR surface) const
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);

    // Must have graphics queue
    auto families = findQueueFamilies(device, surface);
    if (!families.isComplete())
        return -1;

    // Must support all required extensions
    uint32_t extCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, availableExts.data());

    auto required = requiredExtensions();
    for (const char* req : required)
    {
        bool found = false;
        for (const auto& ext : availableExts)
        {
            if (std::strcmp(ext.extensionName, req) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            spdlog::debug("  Device {} missing extension: {}", props.deviceName, req);
            return -1;
        }
    }

    // Score
    int score = 0;

    // Strongly prefer discrete GPUs
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        score += 10000;
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
        score += 1000;

    // Prefer more VRAM
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
    {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            score += static_cast<int>(memProps.memoryHeaps[i].size / (1024 * 1024)); // MB
        }
    }

    // Prefer NVIDIA (vendor ID 0x10DE) for NVENC/NVDEC
    if (props.vendorID == 0x10DE)
        score += 5000;

    // Bonus for async compute
    if (families.hasAsyncCompute())
        score += 500;

    // Bonus for dedicated transfer
    if (families.hasDedicatedTransfer())
        score += 300;

    // Bonus for sampler anisotropy
    if (features.samplerAnisotropy)
        score += 100;

    return score;
}

// ── findQueueFamilies ───────────────────────────────────────────────────────

QueueFamilyIndices Device::findQueueFamilies(VkPhysicalDevice device,
                                              VkSurfaceKHR surface) const
{
    QueueFamilyIndices indices;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);

    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

    for (uint32_t i = 0; i < familyCount; ++i)
    {
        const auto& family = families[i];

        // Graphics queue
        if (family.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            if (!indices.graphics.has_value())
                indices.graphics = i;
        }

        // Dedicated compute queue (compute but NOT graphics)
        if ((family.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(family.queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            indices.compute = i;
        }

        // Dedicated transfer queue (transfer but NOT graphics/compute)
        if ((family.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(family.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            !(family.queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            indices.transfer = i;
        }

        // Present queue
        if (surface != VK_NULL_HANDLE)
        {
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport && !indices.present.has_value())
            {
                indices.present = i;
            }
        }
    }

    // Fallback: use graphics queue for compute/transfer/present if no dedicated found
    if (!indices.compute.has_value() && indices.graphics.has_value())
        indices.compute = indices.graphics;
    if (!indices.transfer.has_value() && indices.graphics.has_value())
        indices.transfer = indices.graphics;
    if (!indices.present.has_value() && indices.graphics.has_value())
        indices.present = indices.graphics;

    return indices;
}

// ── queryGPUInfo ────────────────────────────────────────────────────────────

void Device::queryGPUInfo(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);

    m_gpuInfo.name          = props.deviceName;
    m_gpuInfo.vendorId      = props.vendorID;
    m_gpuInfo.deviceId      = props.deviceID;
    m_gpuInfo.deviceType    = props.deviceType;
    m_gpuInfo.apiVersion    = props.apiVersion;
    m_gpuInfo.driverVersion = props.driverVersion;

    m_gpuInfo.maxComputeWorkGroupSize[0] = props.limits.maxComputeWorkGroupSize[0];
    m_gpuInfo.maxComputeWorkGroupSize[1] = props.limits.maxComputeWorkGroupSize[1];
    m_gpuInfo.maxComputeWorkGroupSize[2] = props.limits.maxComputeWorkGroupSize[2];

    // Calculate total VRAM
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);
    m_gpuInfo.vramSize = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i)
    {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            m_gpuInfo.vramSize += memProps.memoryHeaps[i].size;
        }
    }

    // Check Vulkan 1.2 feature support
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features12.pNext = &features13;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features12;

    vkGetPhysicalDeviceFeatures2(device, &features2);

    m_gpuInfo.supportsTimelineSemaphore  = features12.timelineSemaphore == VK_TRUE;
    m_gpuInfo.supportsDescriptorIndexing = features12.descriptorIndexing == VK_TRUE;
    m_gpuInfo.supportsDynamicRendering   = features13.dynamicRendering == VK_TRUE;
    m_gpuInfo.supportsSynchronization2   = features13.synchronization2 == VK_TRUE;
}

// ── findMemoryType ──────────────────────────────────────────────────────────

uint32_t Device::findMemoryType(uint32_t typeFilter,
                                 VkMemoryPropertyFlags properties) const
{
    for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeFilter & (1 << i)) &&
            (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    spdlog::error("Failed to find suitable memory type (filter: {:#x}, props: {:#x})",
                  typeFilter, static_cast<uint32_t>(properties));
    return 0;
}

// ── findSupportedFormat ─────────────────────────────────────────────────────

VkFormat Device::findSupportedFormat(const std::vector<VkFormat>& candidates,
                                      VkImageTiling tiling,
                                      VkFormatFeatureFlags features) const
{
    for (VkFormat format : candidates)
    {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features)
        {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features)
        {
            return format;
        }
    }

    spdlog::error("Failed to find a supported format from {} candidates", candidates.size());
    return VK_FORMAT_UNDEFINED;
}

} // namespace rt

