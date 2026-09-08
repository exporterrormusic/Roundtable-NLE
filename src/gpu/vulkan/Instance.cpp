/*
 * Instance.cpp — Vulkan instance with debug validation.
 * Step 2: Vulkan Initialization
 */

#include <volk.h>
#include "vulkan/Instance.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rt {

// ── Validation layer name ───────────────────────────────────────────────────
static constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

// Static flag used by debugCallback (Vulkan's userData slot isn't plumbed).
bool Instance::s_errorsFatal = true;
std::atomic<uint64_t> Instance::s_validationErrorCount{0};

// ── Debug callback ──────────────────────────────────────────────────────────
VKAPI_ATTR VkBool32 VKAPI_CALL Instance::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*                                       /*userData*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        // Implicit overlay/capture layers can emit loader GENERAL errors
        // through this callback before our instance exists. Keep logging
        // those, but the renderer-health counter tracks validation-layer
        // findings only so machine-level overlay faults do not masquerade as
        // application synchronization/resource errors.
        const bool isValidationError =
            (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
        if (isValidationError)
            s_validationErrorCount.fetch_add(1, std::memory_order_acq_rel);
        // Use critical so errors show even when the log level is turned up.
        // Keep loader/overlay failures visibly distinct from validation-layer
        // findings produced by the application's Vulkan usage.
        if (isValidationError)
            spdlog::critical("[Vulkan VALIDATION ERROR] {}", callbackData->pMessage);
        else
            spdlog::critical("[Vulkan ERROR] {}", callbackData->pMessage);

        // If a debugger is attached, break here so the developer lands
        // exactly on the offending API call.  Stack trace shows which
        // Roundtable code triggered the validation error.
        if (s_errorsFatal && isValidationError) {
#ifdef _WIN32
            if (IsDebuggerPresent()) {
                __debugbreak();
            }
#elif defined(__GNUC__) || defined(__clang__)
            __builtin_trap();
#endif
        }
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        spdlog::warn("[Vulkan] {}", callbackData->pMessage);
    }
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
    {
        spdlog::info("[Vulkan] {}", callbackData->pMessage);
    }
    else
    {
        spdlog::debug("[Vulkan] {}", callbackData->pMessage);
    }

    return VK_FALSE; // Don't abort the call
}

// ── Constructor / Destructor / Move ─────────────────────────────────────────

Instance::~Instance()
{
    destroy();
}

Instance::Instance(Instance&& other) noexcept
    : m_instance(other.m_instance),
      m_debugMessenger(other.m_debugMessenger),
      m_validationEnabled(other.m_validationEnabled)
{
    other.m_instance         = VK_NULL_HANDLE;
    other.m_debugMessenger   = VK_NULL_HANDLE;
    other.m_validationEnabled = false;
}

Instance& Instance::operator=(Instance&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_instance           = other.m_instance;
        m_debugMessenger     = other.m_debugMessenger;
        m_validationEnabled  = other.m_validationEnabled;
        other.m_instance         = VK_NULL_HANDLE;
        other.m_debugMessenger   = VK_NULL_HANDLE;
        other.m_validationEnabled = false;
    }
    return *this;
}

// ── create ──────────────────────────────────────────────────────────────────

bool Instance::create(const InstanceConfig& config)
{
    if (m_instance != VK_NULL_HANDLE)
    {
        spdlog::warn("Instance::create called when already initialized");
        return true;
    }

    // ── Initialize volk (runtime Vulkan function loader) ────────────────
    // Must be called before ANY Vulkan function.  With VK_NO_PROTOTYPES
    // all Vulkan symbols are null function pointers until volk resolves them.
    VkResult volkResult = volkInitialize();
    if (volkResult != VK_SUCCESS)
    {
        spdlog::error("volkInitialize failed (VkResult: {}) — no Vulkan driver?",
                      static_cast<int>(volkResult));
        return false;
    }

    s_validationErrorCount.store(0, std::memory_order_release);

    // Check validation layer support
    m_validationEnabled = config.enableValidation && checkValidationLayerSupport();
    if (config.enableValidation && !m_validationEnabled)
    {
        spdlog::warn("Validation layers requested but not available — continuing without");
    }
    s_errorsFatal = m_validationEnabled && config.validationErrorsFatal;

    // ── Application info ────────────────────────────────────────────────
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = config.appName.c_str();
    appInfo.applicationVersion = config.appVersion;
    appInfo.pEngineName        = "ROUNDTABLE Engine";
    appInfo.engineVersion      = VK_MAKE_API_VERSION(0, 2, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_3;

    // ── Extensions ──────────────────────────────────────────────────────
    std::vector<const char*> extensions = config.extraExtensions;

    // Always request surface extensions on Windows
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    extensions.push_back("VK_KHR_win32_surface");
#endif

    if (m_validationEnabled)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // ── Instance creation ───────────────────────────────────────────────
    VkInstanceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo        = &appInfo;
    createInfo.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    // Validation layer
    std::vector<const char*> layers;
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};

    // Storage for the validation-features chain (must outlive vkCreateInstance).
    std::vector<VkValidationFeatureEnableEXT> enabledValidationFeatures;
    VkValidationFeaturesEXT validationFeatures{};

    if (m_validationEnabled)
    {
        layers.push_back(VALIDATION_LAYER);
        createInfo.enabledLayerCount   = static_cast<uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();

        // Attach debug messenger to instance create/destroy
        debugCreateInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        // Build the validation-features chain.  These extend the
        // VK_LAYER_KHRONOS_validation layer's coverage:
        //   • Synchronization validation — catches missing barriers,
        //     cross-queue read/write hazards, layout-transition races.
        //     Exactly the class of bug behind the May 2026 Spine TDR.
        //   • GPU-Assisted Validation — instruments shaders to catch
        //     out-of-bounds buffer/image accesses and stale-descriptor
        //     reads.  Significant perf cost (2-3x), opt-in only.
        if (config.enableSynchronizationValidation) {
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        }
        if (config.enableGpuAssistedValidation) {
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        }

        if (!enabledValidationFeatures.empty()) {
            validationFeatures.sType =
                VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
            validationFeatures.enabledValidationFeatureCount =
                static_cast<uint32_t>(enabledValidationFeatures.size());
            validationFeatures.pEnabledValidationFeatures =
                enabledValidationFeatures.data();
            validationFeatures.pNext = &debugCreateInfo;
            createInfo.pNext = &validationFeatures;
        } else {
            createInfo.pNext = &debugCreateInfo;
        }
    }

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS)
    {
        spdlog::error("Failed to create Vulkan instance (VkResult: {})", static_cast<int>(result));
        return false;
    }

    // Load instance-level Vulkan functions (vkEnumeratePhysicalDevices, etc.)
    volkLoadInstance(m_instance);

    spdlog::info("Vulkan instance created (API 1.3, validation: {})",
                 m_validationEnabled ? "ON" : "OFF");

    // ── Debug messenger ─────────────────────────────────────────────────
    if (m_validationEnabled)
    {
        setupDebugMessenger();
    }

    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void Instance::destroy()
{
    if (m_debugMessenger != VK_NULL_HANDLE)
    {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (func)
        {
            func(m_instance, m_debugMessenger, nullptr);
        }
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
        spdlog::info("Vulkan instance destroyed");
    }
}

// ── checkValidationLayerSupport ─────────────────────────────────────────────

bool Instance::checkValidationLayerSupport() const
{
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const auto& layer : availableLayers)
    {
        if (std::strcmp(layer.layerName, VALIDATION_LAYER) == 0)
        {
            return true;
        }
    }
    return false;
}

// ── setupDebugMessenger ─────────────────────────────────────────────────────

void Instance::setupDebugMessenger()
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;

    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));

    if (func)
    {
        VkResult result = func(m_instance, &createInfo, nullptr, &m_debugMessenger);
        if (result == VK_SUCCESS)
        {
            spdlog::info("Vulkan debug messenger attached");
        }
        else
        {
            spdlog::warn("Failed to create debug messenger (VkResult: {})",
                         static_cast<int>(result));
        }
    }
}

} // namespace rt

