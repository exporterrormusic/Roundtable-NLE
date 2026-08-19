/*
 * GpuWorkSubmission.cpp — Triple-buffered GPU work submission ring.
 *
 * See GpuWorkSubmission.h for architecture.
 */

#include "GpuWorkSubmission.h"
#include "GpuContext.h"
#include "GpuScheduler.h"

#include <volk.h>
#include <spdlog/spdlog.h>
#include <utility>

namespace {

// Resolve a raw VkQueue back to a GpuQueueKind for routing through
// the scheduler.  GpuWorkSubmission's public submit() API takes a
// VkQueue (legacy interface kept for source compatibility); the
// scheduler routes by kind.  Falls back to Graphics if the handle
// isn't recognized — shouldn't happen since every queue in this
// process comes from Device.
rt::GpuQueueKind kindForQueue(VkQueue queue) noexcept
{
    if (queue == VK_NULL_HANDLE) return rt::GpuQueueKind::Graphics;
    auto& gpu = rt::GpuContext::get();
    if (!gpu.scheduler().isInitialized()) return rt::GpuQueueKind::Graphics;
    if (queue == gpu.computeQueue())           return rt::GpuQueueKind::Compute;
    if (queue == gpu.device().transferQueue()) return rt::GpuQueueKind::Transfer;
    return rt::GpuQueueKind::Graphics;
}

} // namespace

namespace rt {

// ── Destructor ──────────────────────────────────────────────────────────────

GpuWorkSubmission::~GpuWorkSubmission()
{
    destroy();
}

// ── Move ────────────────────────────────────────────────────────────────────

GpuWorkSubmission::GpuWorkSubmission(GpuWorkSubmission&& other) noexcept
    : m_device(other.m_device)
    , m_cmdPool(other.m_cmdPool)
    , m_slots(other.m_slots)
    , m_globalSubmissionIndex(other.m_globalSubmissionIndex)
    , m_currentSlot(other.m_currentSlot)
    , m_recording(other.m_recording)
{
    other.m_device    = VK_NULL_HANDLE;
    other.m_cmdPool   = VK_NULL_HANDLE;
    other.m_slots     = {};
    other.m_globalSubmissionIndex = 0;
    other.m_currentSlot = 0;
    other.m_recording = false;
}

GpuWorkSubmission& GpuWorkSubmission::operator=(GpuWorkSubmission&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_device    = other.m_device;
        m_cmdPool   = other.m_cmdPool;
        m_slots     = other.m_slots;
        m_globalSubmissionIndex = other.m_globalSubmissionIndex;
        m_currentSlot = other.m_currentSlot;
        m_recording = other.m_recording;
        other.m_device    = VK_NULL_HANDLE;
        other.m_cmdPool   = VK_NULL_HANDLE;
        other.m_slots     = {};
        other.m_globalSubmissionIndex = 0;
        other.m_currentSlot = 0;
        other.m_recording = false;
    }
    return *this;
}

// ── init ────────────────────────────────────────────────────────────────────

bool GpuWorkSubmission::init(VkDevice device, VkCommandPool cmdPool)
{
    if (device == VK_NULL_HANDLE || cmdPool == VK_NULL_HANDLE)
        return false;

    m_device  = device;
    m_cmdPool = cmdPool;

    // Allocate command buffer array and create fences for all 3 slots
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = cmdPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kRingSize;

    VkCommandBuffer rawBuffers[kRingSize];
    if (vkAllocateCommandBuffers(device, &allocInfo, rawBuffers) != VK_SUCCESS)
        return false;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (int i = 0; i < kRingSize; ++i) {
        m_slots[i].cmdBuffer = rawBuffers[i];

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device, &fenceInfo, nullptr, &m_slots[i].fence) != VK_SUCCESS) {
            for (int j = 0; j < i; ++j) {
                vkDestroyFence(device, m_slots[j].fence, nullptr);
                if (m_slots[j].signalSemaphore != VK_NULL_HANDLE)
                    vkDestroySemaphore(device, m_slots[j].signalSemaphore, nullptr);
            }
            vkFreeCommandBuffers(device, cmdPool, kRingSize, rawBuffers);
            for (auto& s : m_slots) s = Slot{};
            return false;
        }

        // Create per-slot binary semaphore for inter-queue sync
        if (vkCreateSemaphore(device, &semInfo, nullptr, &m_slots[i].signalSemaphore) != VK_SUCCESS) {
            for (int j = 0; j < i; ++j) {
                vkDestroyFence(device, m_slots[j].fence, nullptr);
                vkDestroySemaphore(device, m_slots[j].signalSemaphore, nullptr);
            }
            vkDestroyFence(device, m_slots[i].fence, nullptr);
            vkFreeCommandBuffers(device, cmdPool, kRingSize, rawBuffers);
            for (auto& s : m_slots) s = Slot{};
            return false;
        }
    }

    m_currentSlot = 0;
    m_globalSubmissionIndex = 0;
    return true;
}

// ── destroy ─────────────────────────────────────────────────────────────────

void GpuWorkSubmission::destroy()
{
    if (m_device != VK_NULL_HANDLE) {
        // A lost device may never signal its outstanding fences. Normal
        // teardown drains every slot; fatal teardown destroys handles without
        // an unbounded wait because the process cannot resume GPU work.
        if (GpuContext::get().gpuState() == GpuState::Healthy)
            waitForAll();
        else
            spdlog::warn("GpuWorkSubmission: skipping fence drain after device loss");

        for (int i = 0; i < kRingSize; ++i) {
            auto& s = m_slots[i];
            if (s.fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, s.fence, nullptr);
                s.fence = VK_NULL_HANDLE;
            }
            if (s.signalSemaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, s.signalSemaphore, nullptr);
                s.signalSemaphore = VK_NULL_HANDLE;
            }
        }
        if (m_cmdPool != VK_NULL_HANDLE) {
            VkCommandBuffer buffers[kRingSize];
            for (int i = 0; i < kRingSize; ++i)
                buffers[i] = m_slots[i].cmdBuffer;
            vkFreeCommandBuffers(m_device, m_cmdPool, kRingSize, buffers);
        }
    }
    m_device    = VK_NULL_HANDLE;
    m_cmdPool   = VK_NULL_HANDLE;
    m_slots     = {};
    m_recording = false;
    m_currentSlot = 0;
    m_globalSubmissionIndex = 0;
}

// ── beginRecording ──────────────────────────────────────────────────────────

bool GpuWorkSubmission::beginRecording()
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE)
        return false;

    // Wait for this slot's fence if it's still in-flight from a previous
    // submission.  This is the core of triple-buffering: the CPU may have
    // wrapped around and is reusing a slot whose GPU work hasn't finished.
    //
    // A5: bounded timeout.  vkWaitForFences(UINT64_MAX) deadlocks the
    // producer thread if the GPU hangs (driver TDR pre-state, infinite
    // loop in a shader, etc.).  100 ms is well above 60fps frame time
    // (16.7ms) but short enough to surface TDRs through the existing
    // fatal-failure path rather than freeze the app.
    if (s.inFlight) {
        // 1500 ms timeout.  Bumped from the original 100 ms because a
        // realistic frame at a shot boundary can spend 30-80 ms on a
        // synchronous Spine atlas upload + a couple of large texture
        // copies on the same queue as the previous composite — perfectly
        // healthy work that just happens to share the queue.  At 100 ms
        // we were misclassifying those bursts as a hang and triggering
        // the device-lost path ourselves, which then caused
        // FrameProducer to record into an unstarted command buffer.
        // Windows TDR is 2000 ms by default, so 1500 ms still surfaces
        // a genuine driver hang well before the OS resets the device.
        VkResult r = vkWaitForFences(m_device, 1, &s.fence, VK_TRUE,
                                      1'500'000'000ull);  // 1.5 s
        if (r == VK_TIMEOUT) {
            // GPU is genuinely stuck — Windows TDR will fire next.
            spdlog::warn("[GPU-SUBMIT] beginRecording: fence wait TIMED OUT after 1500ms slot={} — GPU is stuck",
                         m_currentSlot);
            return false;
        }
        if (r != VK_SUCCESS) {
            spdlog::error("[GPU-SUBMIT] beginRecording: vkWaitForFences failed VkResult={} slot={}",
                          static_cast<int>(r), m_currentSlot);
            return false;
        }
        s.inFlight = false;
    }

    vkResetCommandBuffer(s.cmdBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    if (vkBeginCommandBuffer(s.cmdBuffer, &beginInfo) != VK_SUCCESS)
        return false;

    m_recording = true;
    return true;
}

// ── endRecording ────────────────────────────────────────────────────────────

bool GpuWorkSubmission::endRecording()
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || !m_recording) {
        spdlog::warn("[GPU-SUBMIT] endRecording: bad state cmdBuffer={} recording={}",
                     (void*)s.cmdBuffer, m_recording);
        return false;
    }
    VkResult r = vkEndCommandBuffer(s.cmdBuffer);
    if (r != VK_SUCCESS) {
        spdlog::error("[GPU-SUBMIT] vkEndCommandBuffer failed: VkResult={}", static_cast<int>(r));
        m_recording = false;
        return false;
    }
    m_recording = false;
    return true;
}

// ── advanceSlot ─────────────────────────────────────────────────────────────

void GpuWorkSubmission::advanceSlot()
{
    m_currentSlot = (m_currentSlot + 1) % kRingSize;
    ++m_globalSubmissionIndex;
}

// ── submitAndWait ───────────────────────────────────────────────────────────

bool GpuWorkSubmission::submitAndWait(VkQueue queue, std::mutex* queueLock)
{
    if (!submit(queue, queueLock))
        return false;
    return waitForCompletion();
}

// ── submit ──────────────────────────────────────────────────────────────────

bool GpuWorkSubmission::submit(VkQueue queue, std::mutex* queueLock)
{
    return submit(queue, VK_NULL_HANDLE, queueLock);
}

// ── submit with timeline wait (cross-queue producer→compositor sync) ───────
//
// The compositor's VkSubmitInfo carries a VkTimelineSemaphoreSubmitInfo that
// waits on the prefetch shared timeline semaphore at the maximum
// value across all CachedFrames being sampled this frame.  Until that
// value is signalled, the GPU does not begin executing this
// submission's compute dispatches — so the compositor never samples a
// texture whose convert+copy is still in flight on the compute queue.

bool GpuWorkSubmission::submitWithTimelineWait(
    VkQueue queue,
    VkSemaphore externalSemaphore,
    VkSemaphore waitSem,
    uint64_t    waitValue,
    std::mutex* queueLock)
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || s.fence == VK_NULL_HANDLE)
        return false;

    vkResetFences(m_device, 1, &s.fence);

    // ── Signal: dedicated per-frame external semaphore (binary).
    //    Same as the plain submit() overload — the compositor's
    //    presenter waits on this semaphore.
    VkSemaphore signalSemaphores[1]{};
    uint32_t    signalCount = 0;
    if (externalSemaphore != VK_NULL_HANDLE) {
        signalSemaphores[0] = externalSemaphore;
        signalCount = 1;
    }

    // ── Wait: producer timeline semaphore at `waitValue`.
    //    Stage = COMPUTE_SHADER so the wait only blocks the compute
    //    dispatch (the composite shader); fixed-function stages can
    //    still queue up speculatively.
    const bool wantWait = (waitSem != VK_NULL_HANDLE) && (waitValue != 0);
    VkSemaphore          waitSems[1]   = { waitSem };
    VkPipelineStageFlags waitStages[1] = { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

    VkTimelineSemaphoreSubmitInfo tlInfo{};
    tlInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    if (wantWait) {
        tlInfo.waitSemaphoreValueCount = 1;
        tlInfo.pWaitSemaphoreValues    = &waitValue;
    }
    // No timeline signal — the external signal semaphore is binary.

    (void)queueLock;

    rt::GpuSubmission sub{};
    sub.cmd                  = s.cmdBuffer;
    sub.queue                = kindForQueue(queue);
    sub.signalSemaphores     = signalSemaphores;
    sub.signalSemaphoreCount = signalCount;
    sub.completionFence      = s.fence;
    sub.tag                  = "GpuWorkSubmission::submitWithTimelineWait";
    if (wantWait) {
        sub.pNext              = &tlInfo;
        sub.waitSemaphores     = waitSems;
        sub.waitStages         = waitStages;
        sub.waitSemaphoreCount = 1;
    }

    VkResult result = rt::GpuContext::get().scheduler().submit(sub);

    if (result != VK_SUCCESS) {
        spdlog::error("[GPU-SUBMIT] vkQueueSubmit (timeline wait) failed: "
                      "VkResult={} slot={} submission={} waitValue={}",
                      static_cast<int>(result), m_currentSlot,
                      m_globalSubmissionIndex, waitValue);
        return false;
    }

    s.inFlight = true;
    s.submissionIndex = m_globalSubmissionIndex;
    advanceSlot();
    return true;
}

// ── submit (with signal semaphore) ──────────────────────────────────────────

bool GpuWorkSubmission::submit(VkQueue queue, VkSemaphore externalSemaphore,
                                std::mutex* queueLock)
{
    Slot& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || s.fence == VK_NULL_HANDLE)
        return false;

    vkResetFences(m_device, 1, &s.fence);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &s.cmdBuffer;

    // Signal the dedicated per-frame semaphore (external) for inter-queue
    // sync.  The external semaphore is created by CompositeEngine's pool and
    // stored in CachedFrame — the presenter waits on it and destroys it after
    // consumption.  This replaces the old per-slot semaphore approach which
    // had a ring-buffer wrap race (per-slot semaphore re-signaled before the
    // presenter consumed it).
    //
    // When no external semaphore is provided, do not signal a binary
    // semaphore. The old per-slot fallback was never consumed by same-queue
    // callers and could be signaled again while still signaled.
    VkSemaphore signalSemaphores[1]{};
    uint32_t signalCount = 0;
    if (externalSemaphore != VK_NULL_HANDLE) {
        signalSemaphores[0] = externalSemaphore;
        signalCount = 1;
    }
    submitInfo.signalSemaphoreCount = signalCount;
    submitInfo.pSignalSemaphores    = signalSemaphores;

    // P1.1: route through GpuScheduler.  The legacy `queueLock`
    // parameter is now unused — the scheduler owns the queue mutex.
    // Kept in the signature for source compatibility with callers
    // that still pass it during the migration window.
    (void)queueLock;

    rt::GpuSubmission sub{};
    sub.cmd                  = s.cmdBuffer;
    sub.queue                = kindForQueue(queue);
    sub.signalSemaphores     = signalSemaphores;
    sub.signalSemaphoreCount = signalCount;
    sub.completionFence      = s.fence;
    sub.tag                  = "GpuWorkSubmission::submit";

    VkResult result = rt::GpuContext::get().scheduler().submit(sub);

    if (result != VK_SUCCESS) {
        // -4 = VK_ERROR_DEVICE_LOST (TDR / driver reset), -2/-3 = OOM,
        // -8 = INITIALIZATION_FAILED.  Scheduler already logs the
        // generic failure; this line preserves the slot/submission
        // context that CompositeEngine recovery looks for.
        spdlog::error("[GPU-SUBMIT] vkQueueSubmit failed: VkResult={} slot={} submission={}",
                      static_cast<int>(result), m_currentSlot, m_globalSubmissionIndex);
        return false;
    }

    s.inFlight = true;
    s.submissionIndex = m_globalSubmissionIndex;
    advanceSlot();
    return true;
}

// ── waitForCompletion ───────────────────────────────────────────────────────

bool GpuWorkSubmission::waitForCompletion(uint64_t timeoutNs)
{
    // Wait for the previously submitted slot (the one we just advanced from)
    int prevSlot = (m_currentSlot - 1 + kRingSize) % kRingSize;
    auto& s = m_slots[prevSlot];
    if (s.fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return false;
    if (!s.inFlight)
        return true; // Already completed
    VkResult res = vkWaitForFences(m_device, 1, &s.fence, VK_TRUE, timeoutNs);
    if (res == VK_SUCCESS)
        s.inFlight = false;
    return res == VK_SUCCESS;
}

// ── waitForAll ──────────────────────────────────────────────────────────────

void GpuWorkSubmission::waitForAll()
{
    if (m_device == VK_NULL_HANDLE)
        return;

    // Collect all fences that are in-flight
    VkFence fences[kRingSize];
    int count = 0;
    for (int i = 0; i < kRingSize; ++i) {
        if (m_slots[i].inFlight && m_slots[i].fence != VK_NULL_HANDLE) {
            fences[count++] = m_slots[i].fence;
            m_slots[i].inFlight = false;
        }
    }
    if (count > 0)
        vkWaitForFences(m_device, count, fences, VK_TRUE, UINT64_MAX);
}

// ── isComplete ──────────────────────────────────────────────────────────────

bool GpuWorkSubmission::isComplete() const
{
    // Check the most recently submitted slot
    int prevSlot = (m_currentSlot - 1 + kRingSize) % kRingSize;
    const auto& s = m_slots[prevSlot];
    if (s.fence == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE)
        return true;
    if (!s.inFlight)
        return true;
    return vkGetFenceStatus(m_device, s.fence) == VK_SUCCESS;
}

// ── addBarrier ──────────────────────────────────────────────────────────────

void GpuWorkSubmission::addBarrier(VkPipelineStageFlags srcStage,
                                    VkPipelineStageFlags dstStage,
                                    VkAccessFlags srcAccess,
                                    VkAccessFlags dstAccess)
{
    auto& s = slot();
    if (s.cmdBuffer == VK_NULL_HANDLE || !m_recording)
        return;

    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(s.cmdBuffer,
                         srcStage, dstStage,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

} // namespace rt
