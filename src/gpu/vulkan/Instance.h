/*
 * Instance — Vulkan instance creation with validation layers.
 *
 * Step 2: Creates VkInstance with optional debug/validation layers.
 * In debug builds, attaches VK_EXT_debug_utils for error reporting.
 */

#pragma once

#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rt {

/// Configuration for Vulkan instance creation.
struct InstanceConfig
{
    std::string appName    = "ROUNDTABLE NLE";
    uint32_t    appVersion = VK_MAKE_API_VERSION(0, 2, 0, 0);
#ifdef ROUNDTABLE_DEBUG
    bool        enableValidation = true;
#else
    bool        enableValidation = false;
#endif

    /// Synchronization validation catches cross-queue hazards, missing
    /// pipeline barriers, and similar — exactly the class of bug that
    /// produced the May 2026 SpineRenderer cross-queue TDR.  Low runtime
    /// cost (a few % overhead on the validation thread); ON by default
    /// whenever validation is enabled.
    bool        enableSynchronizationValidation = true;

    /// GPU-Assisted Validation instruments shaders to catch out-of-bounds
    /// buffer/image reads, stale descriptor reads, and similar.  Has a
    /// significant perf cost (often 2-3x slower frame times) so OFF by
    /// default; enable per session via the ROUNDTABLE_GPU_ASSISTED=1
    /// environment variable when investigating shader-side bugs.
    bool        enableGpuAssistedValidation = false;

    /// When true, validation messages at ERROR severity trigger a
    /// debug-break (DebuggerPresent path) and elevate the log line to
    /// spdlog::critical so it cannot be silently ignored.  Warnings
    /// still just log.  ON by default with validation; flip to false
    /// for unattended stress runs where you only want the log.
    bool        validationErrorsFatal = true;

    /// Extra instance extensions to request (e.g. surface extensions)
    std::vector<const char*> extraExtensions;
};

/// RAII wrapper for VkInstance + debug messenger.
class Instance
{
public:
    Instance() = default;
    ~Instance();

    // Non-copyable
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) noexcept;
    Instance& operator=(Instance&&) noexcept;

    /// Create the Vulkan instance.
    /// @return true on success.
    bool create(const InstanceConfig& config = {});

    /// Destroy instance and debug messenger.
    void destroy();

    /// Get the raw VkInstance handle.
    [[nodiscard]] VkInstance handle() const noexcept { return m_instance; }

    /// Whether validation layers are active.
    [[nodiscard]] bool validationEnabled() const noexcept { return m_validationEnabled; }

    /// Number of ERROR-severity validation messages received since the most
    /// recent Instance::create(). Useful for unattended stress verification.
    [[nodiscard]] uint64_t validationErrorCount() const noexcept
    {
        return s_validationErrorCount.load(std::memory_order_acquire);
    }

    /// Implicit conversion to VkInstance.
    operator VkInstance() const noexcept { return m_instance; }

private:
    VkInstance               m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    bool                     m_validationEnabled{false};

    /// True if InstanceConfig::validationErrorsFatal was set when this
    /// instance was created; consulted by debugCallback to decide
    /// whether to debug-break on ERROR severity messages.  Stored as
    /// a static so the static-method callback can read it without an
    /// instance pointer (Vulkan validation provides a userData slot
    /// but we don't currently plumb it).
    static bool              s_errorsFatal;
    static std::atomic<uint64_t> s_validationErrorCount;

    bool checkValidationLayerSupport() const;
    void setupDebugMessenger();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
        VkDebugUtilsMessageTypeFlagsEXT             type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void*                                       userData);
};

} // namespace rt
