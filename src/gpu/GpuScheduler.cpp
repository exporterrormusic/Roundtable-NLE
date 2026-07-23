/*
 * GpuScheduler.cpp — Central queue submission authority.
 * See GpuScheduler.h for the architecture rationale.
 */

#include "GpuScheduler.h"

#include <volk.h>
#include <spdlog/spdlog.h>

#include <cassert>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rt {

#ifdef _WIN32
// SEH-guarded vkQueueSubmit.  The NVIDIA Vulkan ICD (nvoglv64.dll)
// has been observed to raise STATUS_ACCESS_VIOLATION inside
// vkQueueSubmit when the device is mid-TDR or a stale command buffer
// is in flight.  Without SEH the violation propagates as an unhandled
// SEH exception and the process dies silently.  With SEH we catch it,
// return VK_ERROR_UNKNOWN, and let the caller surface device-lost
// through the normal path.
//
// Must be a free function (or extern "C") with no C++ destructors in
// scope — MSVC disallows mixing __try with stack-unwound objects.
static VkResult vkQueueSubmitSeh(VkQueue queue, const VkSubmitInfo* submitInfo,
                                  VkFence fence)
{
    __try {
        return vkQueueSubmit(queue, 1, submitInfo, fence);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return VK_ERROR_UNKNOWN;
    }
}

static VkResult vkQueueWaitIdleSeh(VkQueue queue)
{
    __try {
        return vkQueueWaitIdle(queue);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return VK_ERROR_UNKNOWN;
    }
}
#endif

bool GpuScheduler::init(
    VkDevice    device,
    VkQueue     graphicsQueue, std::mutex* graphicsQueueMutex,
    VkQueue     computeQueue,  std::mutex* computeQueueMutex,
    VkQueue     transferQueue, std::mutex* transferQueueMutex)
{
    if (m_device != VK_NULL_HANDLE) {
        spdlog::warn("GpuScheduler: init called twice — ignoring");
        return true;
    }
    if (device == VK_NULL_HANDLE || graphicsQueue == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: init requires at minimum a graphics queue");
        return false;
    }
    m_device = device;

    m_graphics.queue = graphicsQueue;
    m_graphics.mutex = graphicsQueueMutex;

    // Fall through to graphics if compute/transfer aren't separately
    // available.  Submissions still go through this scheduler so the
    // queue mutex is honored — they just land on the graphics queue.
    m_compute.queue  = (computeQueue != VK_NULL_HANDLE) ? computeQueue : graphicsQueue;
    m_compute.mutex  = (computeQueue != VK_NULL_HANDLE) ? computeQueueMutex : graphicsQueueMutex;

    m_transfer.queue = (transferQueue != VK_NULL_HANDLE) ? transferQueue : graphicsQueue;
    m_transfer.mutex = (transferQueue != VK_NULL_HANDLE) ? transferQueueMutex : graphicsQueueMutex;

    spdlog::info("GpuScheduler: initialized (graphics, compute={}, transfer={})",
                 (computeQueue != VK_NULL_HANDLE  && computeQueue  != graphicsQueue) ? "dedicated" : "shared",
                 (transferQueue != VK_NULL_HANDLE && transferQueue != graphicsQueue) ? "dedicated" : "shared");
    return true;
}

void GpuScheduler::shutdown()
{
    if (m_device == VK_NULL_HANDLE) return;
    spdlog::info("GpuScheduler: total submissions = {} (graphics={}, compute={}, transfer={})",
                 m_totalSubmissions.load(std::memory_order_relaxed),
                 m_graphics.submissions.load(std::memory_order_relaxed),
                 m_compute.submissions.load(std::memory_order_relaxed),
                 m_transfer.submissions.load(std::memory_order_relaxed));
    m_device = VK_NULL_HANDLE;
    // QueueSlot holds a std::atomic so it can't be copy-assigned with {};
    // clear fields in place instead.
    auto clearSlot = [](QueueSlot& s) {
        s.queue = VK_NULL_HANDLE;
        s.mutex = nullptr;
        s.submissions.store(0, std::memory_order_relaxed);
    };
    clearSlot(m_graphics);
    clearSlot(m_compute);
    clearSlot(m_transfer);
    m_totalSubmissions.store(0, std::memory_order_relaxed);
}

GpuScheduler::QueueSlot& GpuScheduler::slotFor(GpuQueueKind kind) noexcept
{
    switch (kind) {
        case GpuQueueKind::Graphics: return m_graphics;
        case GpuQueueKind::Compute:  return m_compute;
        case GpuQueueKind::Transfer: return m_transfer;
    }
    return m_graphics;  // unreachable; quiets compiler.
}

const GpuScheduler::QueueSlot& GpuScheduler::slotFor(GpuQueueKind kind) const noexcept
{
    switch (kind) {
        case GpuQueueKind::Graphics: return m_graphics;
        case GpuQueueKind::Compute:  return m_compute;
        case GpuQueueKind::Transfer: return m_transfer;
    }
    return m_graphics;
}

uint64_t GpuScheduler::submissionsOn(GpuQueueKind kind) const noexcept
{
    return slotFor(kind).submissions.load(std::memory_order_relaxed);
}

VkResult GpuScheduler::submit(const GpuSubmission& sub)
{
    if (m_device == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: submit() called before init()");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (sub.cmd == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: submit() with VK_NULL_HANDLE command buffer (tag={})",
                      sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    if (sub.waitSemaphoreCount > 0 && (!sub.waitSemaphores || !sub.waitStages)) {
        spdlog::error("GpuScheduler: submit() with waitSemaphoreCount={} but null arrays (tag={})",
                      sub.waitSemaphoreCount, sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    if (sub.signalSemaphoreCount > 0 && !sub.signalSemaphores) {
        spdlog::error("GpuScheduler: submit() with signalSemaphoreCount={} but null array (tag={})",
                      sub.signalSemaphoreCount, sub.tag ? sub.tag : "<none>");
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    QueueSlot& slot = slotFor(sub.queue);
    if (slot.queue == VK_NULL_HANDLE) {
        spdlog::error("GpuScheduler: no queue available for kind={} (tag={})",
                      static_cast<int>(sub.queue), sub.tag ? sub.tag : "<none>");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = sub.pNext;
    submitInfo.waitSemaphoreCount   = sub.waitSemaphoreCount;
    submitInfo.pWaitSemaphores      = sub.waitSemaphores;
    submitInfo.pWaitDstStageMask    = sub.waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &sub.cmd;
    submitInfo.signalSemaphoreCount = sub.signalSemaphoreCount;
    submitInfo.pSignalSemaphores    = sub.signalSemaphores;

    VkResult result;
#ifdef _WIN32
    // Always SEH-guard on Windows.  Cost is negligible; the safety net
    // converts driver-side access violations (nvoglv64.dll mid-TDR)
    // into a returnable error code instead of unhandled SEH process
    // death.  The pre-P1 code had this guard only in VulkanViewport's
    // presentFrame path; now every submission gets it for free.
    if (slot.mutex) {
        std::lock_guard lock(*slot.mutex);
        result = vkQueueSubmitSeh(slot.queue, &submitInfo, sub.completionFence);
    } else {
        result = vkQueueSubmitSeh(slot.queue, &submitInfo, sub.completionFence);
    }
    if (result == VK_ERROR_UNKNOWN) {
        spdlog::error("[GpuScheduler] SEH access violation in vkQueueSubmit "
                      "(kind={} tag={}) — treating as device lost",
                      static_cast<int>(sub.queue),
                      sub.tag ? sub.tag : "<none>");
    }
#else
    if (slot.mutex) {
        std::lock_guard lock(*slot.mutex);
        result = vkQueueSubmit(slot.queue, 1, &submitInfo, sub.completionFence);
    } else {
        result = vkQueueSubmit(slot.queue, 1, &submitInfo, sub.completionFence);
    }
#endif

    slot.submissions.fetch_add(1, std::memory_order_relaxed);
    m_totalSubmissions.fetch_add(1, std::memory_order_relaxed);

    if (result != VK_SUCCESS) {
        // -4 = VK_ERROR_DEVICE_LOST.  Surface the code so the rest of
        // the stack (CompositeEngine recovery, etc.) can react.
        spdlog::error("[GpuScheduler] vkQueueSubmit failed: VkResult={} kind={} tag={}",
                      static_cast<int>(result),
                      static_cast<int>(sub.queue),
                      sub.tag ? sub.tag : "<none>");
    }
    return result;
}

void GpuScheduler::deviceWaitIdle()
{
    if (m_device == VK_NULL_HANDLE) return;

    // Dedupe mutex pointers — when a queue family is missing the slot
    // falls back to the graphics mutex (see init()), so the same mutex
    // can appear in multiple slots.  std::scoped_lock's deadlock-avoid
    // algorithm requires distinct mutexes.
    std::mutex* a = m_graphics.mutex;
    std::mutex* b = (m_compute.mutex  && m_compute.mutex  != a) ? m_compute.mutex  : nullptr;
    std::mutex* c = (m_transfer.mutex && m_transfer.mutex != a
                                     && m_transfer.mutex != b) ? m_transfer.mutex : nullptr;

    if (a && b && c) {
        std::scoped_lock lock(*a, *b, *c);
        vkDeviceWaitIdle(m_device);
    } else if (a && b) {
        std::scoped_lock lock(*a, *b);
        vkDeviceWaitIdle(m_device);
    } else if (a) {
        std::lock_guard lock(*a);
        vkDeviceWaitIdle(m_device);
    } else {
        vkDeviceWaitIdle(m_device);
    }
}

VkResult GpuScheduler::present(VkQueue queue, const VkPresentInfoKHR* info)
{
    if (queue == VK_NULL_HANDLE || info == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Resolve which queue mutex owns this handle.  Present queue is
    // typically the graphics queue (Device.cpp falls back to it when
    // no dedicated present family exists), so we'll usually lock
    // m_graphics.mutex.  When the present family is genuinely separate,
    // no other code submits to that queue and there's no race — fall
    // through with no lock in that case.
    std::mutex* mtx = nullptr;
    if (queue == m_graphics.queue)      mtx = m_graphics.mutex;
    else if (queue == m_compute.queue)  mtx = m_compute.mutex;
    else if (queue == m_transfer.queue) mtx = m_transfer.mutex;

    if (mtx) {
        std::lock_guard lock(*mtx);
        return vkQueuePresentKHR(queue, info);
    }
    return vkQueuePresentKHR(queue, info);
}

VkResult GpuScheduler::queueWaitIdle(VkQueue queue)
{
    if (queue == VK_NULL_HANDLE)
        return VK_ERROR_INITIALIZATION_FAILED;

    std::mutex* mtx = nullptr;
    if (queue == m_graphics.queue)      mtx = m_graphics.mutex;
    else if (queue == m_compute.queue)  mtx = m_compute.mutex;
    else if (queue == m_transfer.queue) mtx = m_transfer.mutex;

    auto wait = [queue]() {
#ifdef _WIN32
        return vkQueueWaitIdleSeh(queue);
#else
        return vkQueueWaitIdle(queue);
#endif
    };

    VkResult result = VK_SUCCESS;
    if (mtx) {
        std::lock_guard lock(*mtx);
        result = wait();
    } else {
        result = wait();
    }
    if (result != VK_SUCCESS) {
        spdlog::error("[GpuScheduler] vkQueueWaitIdle failed: VkResult={}",
                      static_cast<int>(result));
    }
    return result;
}

} // namespace rt
