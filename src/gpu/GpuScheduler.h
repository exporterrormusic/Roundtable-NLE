/*
 * GpuScheduler — Central authority for all vkQueueSubmit calls.
 *
 * P1 of CLAUDE_IMPROVEMENT_PLAN.  The single most important
 * architectural change for stability: no other code in the project
 * should call vkQueueSubmit directly.
 *
 * Why centralize?
 *
 *   Until P1, 24 files independently called vkQueueSubmit.  Each
 *   subsystem held its own VkQueue handle and submitted whenever
 *   it pleased.  Queue mutexes were owned ad-hoc; some callers
 *   acquired them, others didn't.  Cross-queue dependencies were
 *   ad-hoc (vkQueueWaitIdle sledgehammers, or worse, nothing).
 *   The May 2026 SpineRenderer cross-queue TDR was one symptom of
 *   this pattern: every fix is a per-caller patch instead of a
 *   structural guarantee.
 *
 *   After P1, every command-buffer submission flows through this
 *   class.  External synchronization on each VkQueue is enforced
 *   here, in one place, by a per-queue mutex held by GpuContext.
 *   Cross-queue dependencies are expressed via semaphores attached
 *   to GpuSubmission — never by callers calling vkQueueWaitIdle.
 *
 * v1 design (this revision):
 *
 *   submit() is synchronous.  The vkQueueSubmit happens on the
 *   calling thread, under the appropriate queue mutex held by
 *   GpuContext.  This is intentionally simple: the goal of v1 is
 *   consolidation, not threading rework.
 *
 *   Future revisions will spawn one dedicated thread per VkQueue
 *   and turn submit() into a non-blocking enqueue.  That belongs
 *   in a later P1.x once every caller is routed through here and
 *   the API has stabilized.
 *
 * Migration policy:
 *
 *   - New code: always submit via GpuScheduler::submit().
 *   - Existing call sites are migrated incrementally; the eventual
 *     end state is a build-time guard that #defines vkQueueSubmit
 *     to a compile error in non-scheduler TUs.  Not yet enforced.
 */

#pragma once

#include <vulkan/vulkan_core.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace rt {

/// Logical queue family selector.  The scheduler maps these to actual
/// VkQueue handles + mutexes at init time.
enum class GpuQueueKind : uint8_t {
    Graphics,   ///< Render passes, graphics pipelines, presentation.
    Compute,    ///< Compute shaders, async work that can overlap graphics.
    Transfer,   ///< Pure copies / uploads.  Falls back to Graphics if no
                ///<  dedicated transfer family exists.
};

/// Description of a single submission.  Lifetime of all referenced data
/// (pNext chain, semaphore arrays, stage masks) is the caller's
/// responsibility — it must remain valid until submit() returns.
struct GpuSubmission {
    VkCommandBuffer cmd        = VK_NULL_HANDLE;
    GpuQueueKind    queue      = GpuQueueKind::Graphics;

    /// Optional pNext chain (e.g. VkTimelineSemaphoreSubmitInfo).
    const void*     pNext      = nullptr;

    /// Wait semaphores + matching stage masks.  Array length must match
    /// waitSemaphoreCount; passing nullptr with count 0 is fine.
    const VkSemaphore*           waitSemaphores      = nullptr;
    const VkPipelineStageFlags*  waitStages          = nullptr;
    uint32_t                     waitSemaphoreCount  = 0;

    /// Semaphores to signal on completion.
    const VkSemaphore*           signalSemaphores    = nullptr;
    uint32_t                     signalSemaphoreCount = 0;

    /// Optional fence to signal on completion.  VK_NULL_HANDLE if the
    /// caller doesn't need to know when the work finished.
    VkFence         completionFence = VK_NULL_HANDLE;

    /// Optional human-readable tag for diagnostics.  Not used in v1,
    /// reserved for the future per-queue logging.
    const char*     tag             = nullptr;
};

class GpuScheduler
{
public:
    GpuScheduler() = default;
    ~GpuScheduler() = default;

    GpuScheduler(const GpuScheduler&) = delete;
    GpuScheduler& operator=(const GpuScheduler&) = delete;

    /// Bind the scheduler to the Vulkan device's queues and mutexes.
    /// The queues continue to live in Device; the mutexes in GpuContext.
    /// Pass VK_NULL_HANDLE / nullptr for any unavailable queue (e.g. no
    /// dedicated transfer family); submissions to that kind will be
    /// redirected to the graphics queue.
    bool init(VkDevice    device,
              VkQueue     graphicsQueue, std::mutex* graphicsQueueMutex,
              VkQueue     computeQueue,  std::mutex* computeQueueMutex,
              VkQueue     transferQueue, std::mutex* transferQueueMutex);

    /// Release references.  No GPU work is drained here — GpuContext
    /// is expected to have called vkDeviceWaitIdle first.
    void shutdown();

    [[nodiscard]] bool isInitialized() const noexcept {
        return m_device != VK_NULL_HANDLE;
    }

    /// THE ONLY supported way to submit GPU work.  Selects the queue
    /// by kind, locks the matching mutex, calls vkQueueSubmit, returns
    /// the VkResult.  v1: synchronous.
    ///
    /// External synchronization on the chosen VkQueue is the
    /// scheduler's invariant — callers must NOT lock the queue mutex
    /// themselves.
    VkResult submit(const GpuSubmission& sub);

    /// THE ONLY supported way to call vkQueuePresentKHR.  Per Vulkan spec
    /// vkQueuePresentKHR is "queue use" and requires the same external
    /// synchronization on the VkQueue handle as vkQueueSubmit — without
    /// this lock the presenter thread races the producer thread's submit
    /// on the shared graphics+present queue, which on NVIDIA shows up as
    /// "vkQueueSubmit(): THREADING ERROR" in validation and as a
    /// hardware-level "subchannel mismatch" graphics-engine fault →
    /// VK_ERROR_DEVICE_LOST in production.  Resolves which queue mutex
    /// matches `queue` by handle and locks it for the duration of the
    /// call.
    VkResult present(VkQueue queue, const VkPresentInfoKHR* info);

    /// Wait for one queue to become idle while honoring the same external
    /// synchronization used by submit() and present(). Swapchain teardown
    /// needs this stronger guarantee: a submit fence does not prove that the
    /// presentation engine has finished consuming its wait semaphore.
    VkResult queueWaitIdle(VkQueue queue);

    /// THE ONLY supported way to call vkDeviceWaitIdle during operation.
    /// Per Vulkan spec vkDeviceWaitIdle is "queue use" on EVERY queue
    /// in the device — so calling it while another thread submits to any
    /// queue is the same threading violation as the submit-vs-present
    /// race that caused the May 2026 TDR.  This wrapper locks every
    /// distinct queue mutex before the wait, dedup'd when queues share
    /// a family.  Shutdown paths that have already drained all worker
    /// threads can keep calling vkDeviceWaitIdle directly — but for any
    /// resize/recreate or live wait, route through here.
    void deviceWaitIdle();

    [[nodiscard]] uint64_t totalSubmissions() const noexcept {
        return m_totalSubmissions.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t submissionsOn(GpuQueueKind kind) const noexcept;

private:
    struct QueueSlot {
        VkQueue              queue = VK_NULL_HANDLE;
        std::mutex*          mutex = nullptr;     // External; lives in GpuContext.
        std::atomic<uint64_t> submissions{0};
    };

    VkDevice            m_device = VK_NULL_HANDLE;
    QueueSlot           m_graphics;
    QueueSlot           m_compute;
    QueueSlot           m_transfer;
    std::atomic<uint64_t> m_totalSubmissions{0};

    QueueSlot& slotFor(GpuQueueKind kind) noexcept;
    const QueueSlot& slotFor(GpuQueueKind kind) const noexcept;
};

} // namespace rt
