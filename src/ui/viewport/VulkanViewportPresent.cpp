/*
 * VulkanViewportPresent.cpp -- GPU display + swapchain present pipeline.
 * Extracted from VulkanViewport.cpp (behavior-preserving).
 */

#include "viewport/VulkanViewport.h"
#include "Theme.h"
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "vulkan/Swapchain.h"
#include "vulkan/Texture.h"
#include "cache/FrameCache.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <QPainter>
#include <QVBoxLayout>
#include <QCursor>
#include <QRegion>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace rt {

//  GPU Display
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::displayGpuImage(VkImageView imageView, VkSampler sampler,
                                      uint32_t imgWidth, uint32_t imgHeight,
                                      VkSemaphore waitSemaphore,
                                      std::shared_ptr<void> textureOwner)
{
    m_frameWidth  = imgWidth;
    m_frameHeight = imgHeight;

    if (!m_gpuActive) {
        spdlog::warn("[DIAG-VIEWPORT] displayGpuImage: gpuActive=false, skipping");
        return;
    }

    // If GpuContext has transitioned to Failed (VK_ERROR_DEVICE_LOST), every
    // Vulkan handle in the caller's hands — including imageView and
    // waitSemaphore — is no longer safe to use. Refuse presentation while
    // the fatal recovery dialog stops this session.
    if (!GpuContext::get().isOperational()) {
        return;
    }

    // Defer source image storage to presentFrame() so it happens AFTER
    // the previous frame's fence has signaled, under m_presentMtx.
    // Without this deferral, the compositor can resize and destroy the
    // output texture between the store here and the fence wait inside
    // presentFrame, leaving m_sourceView/m_sourceSampler as dangling
    // handles — GPU reads freed memory → nvoglv64.dll ACCESS_VIOLATION.
    m_pendingView      = imageView;
    m_pendingSampler   = sampler;
    m_pendingW         = imgWidth;
    m_pendingH         = imgHeight;
    m_pendingValid     = true;
    m_pendingTextureOwner = std::move(textureOwner);  // keep texture alive

    // DIAG: log viewport present attempts
    {
        static int s_vpLog = 0;
        if (++s_vpLog % 5 == 0) {
            spdlog::info("[DIAG-VIEWPORT] displayGpuImage: view=0x{:X} "
                         "sampler=0x{:X} pending=true {}x{}",
                         reinterpret_cast<uint64_t>(imageView),
                         reinterpret_cast<uint64_t>(sampler),
                         imgWidth, imgHeight);
        }
    }

    presentFrame(waitSemaphore);
}

// ── SEH-safe Vulkan wrappers ───────────────────────────────────────────────
// These are plain free functions (no C++ object unwinding) so MSVC allows
// __try/__except.  NVIDIA App (nvspcap64.dll) hooks these Vulkan entry
// points and can crash with a null-pointer deref.

#ifdef _WIN32
static VkResult sehWaitForFences(VkDevice device, VkFence fence,
                                  uint64_t timeoutNs)
{
    __try
    {
        return vkWaitForFences(device, 1, &fence, VK_TRUE, timeoutNs);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return VK_ERROR_UNKNOWN;
    }
}

// sehQueueSubmit moved into GpuScheduler::submit (P1.2).
#endif

void VulkanViewport::presentFrame(VkSemaphore waitSemaphore)
{
    std::lock_guard lock(m_presentMtx);

    if (!m_gpuActive || !m_swapchain) {
        m_pendingValid = false;
        recycleSemaphores();
        return;
    }
    if (!m_pendingValid && m_sourceView == VK_NULL_HANDLE) {
        recycleSemaphores();
        return;
    }

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Wait for previous frame — bounded timeout.
    {
        auto fenceStart = std::chrono::steady_clock::now();
        VkResult fenceRes;

#ifdef _WIN32
        fenceRes = sehWaitForFences(device, m_inFlightFence, 16'000'000);
        if (fenceRes == VK_ERROR_UNKNOWN)
        {
            // SEH caught an access violation inside vkWaitForFences.  Either
            // a third-party hook (NVIDIA App, OBS) NULL-deref'd, or our own
            // device is dead.  Either way the only safe action is to mark
            // the device as Failed and let the fatal-failure path show the
            // restart dialog — we used to set m_swapchainDirty and limp
            // along forever pretending to work.
            spdlog::error("[VIEWPORT] Access violation in vkWaitForFences "
                          "— marking GPU Failed.");
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();  // fires fatal callback
            return;
        }
#else
        fenceRes = vkWaitForFences(device, 1, &m_inFlightFence,
                                    VK_TRUE, 16'000'000);
#endif

        if (fenceRes == VK_TIMEOUT) {
            spdlog::warn("[DIAG-VIEWPORT] presentFrame: fence TIMEOUT (16ms)");
            return;
        }
        auto fenceEnd = std::chrono::steady_clock::now();
        double fenceMs = std::chrono::duration<double, std::milli>(fenceEnd - fenceStart).count();
        static int s_fenceLog = 0;
        if (fenceMs > 2.0 || ++s_fenceLog % 30 == 0) {
            spdlog::info("[DIAG-VIEWPORT] presentFrame: fenceWait={:.1f}ms", fenceMs);
        }
    }

    // Recycle consumed semaphores (kept alive to avoid handle-reuse race
    // with compositor thread — nvoglv64.dll NULL-deref bug).
    recycleSemaphores();

    // Handle resize inline.  handleResize() preserves the source view
    // (m_sourceTextureOwner keeps the composite texture alive across
    // the swapchain rebuild), so subsequent presents in this same call
    // sample the existing frame into the NEW-size framebuffer.  This is
    // what lets the inline refresh() in resizeEvent re-present cleanly
    // at every WM_SIZE during a drag, instead of holding the old-size
    // swapchain and letting DWM stretch (which produced visible echoes).
    if (m_swapchainDirty || m_swapchain->needsRecreation()) {
        handleResize();
        if (!m_swapchain || m_swapchain->needsRecreation()) {
            m_pendingValid = false;
            return;
        }
    }

    // Apply pending source image — SAFE because:
    // 1. m_presentMtx is held (serialized with other present callers)
    // 2. The previous frame's fence has signaled (waited above) — any
    //    GPU work referencing the OLD descriptor (old handles) is done
    // 3. compositor::resize() -> scheduler.deviceWaitIdle() waits for
    //    ALL in-flight work including this viewport's submissions, so
    //    the source texture stays alive until at least the NEXT resize
    if (m_pendingValid) {
        bool changed = (m_pendingView != m_sourceView ||
                        m_pendingSampler != m_sourceSampler);
        m_sourceView    = m_pendingView;
        m_sourceSampler = m_pendingSampler;
        m_srcW          = m_pendingW;
        m_srcH          = m_pendingH;
        if (changed) m_imageDirty = true;
        m_pendingValid = false;
        // Swap the texture owner reference — the old texture (from the
        // previous frame) can now be freed since the GPU has signaled
        // the previous fence (waited above) and won't read from it again.
        m_sourceTextureOwner = std::move(m_pendingTextureOwner);
    }

    if (m_sourceView == VK_NULL_HANDLE) {
        // No source image available even after applying pending.
        recycleSemaphores();
        return;
    }

    // Acquire swapchain image
    uint32_t imageIndex = m_swapchain->acquireNextImage(device, m_imageAvailable,
                                                         5'000'000); // 5ms
    if (imageIndex == UINT32_MAX) {
        handleResize();
        return;
    }
    if (imageIndex >= m_renderFinishedByImage.size()) {
        spdlog::error("VulkanViewport: acquired image {} without a present semaphore",
                      imageIndex);
        m_swapchainDirty = true;
        return;
    }
    VkSemaphore renderFinished = m_renderFinishedByImage[imageIndex];

    vkResetFences(device, 1, &m_inFlightFence);

    // Update descriptor set if source image changed
    if (m_imageDirty) {
        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler     = m_sourceSampler ? m_sourceSampler : m_fallbackSampler;
        imgInfo.imageView   = m_sourceView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_descriptorSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        m_imageDirty = false;
    }

    // Record command buffer
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    // Begin render pass
    VkClearValue clearVal{};
    clearVal.color = {{0.07f, 0.07f, 0.09f, 1.0f}};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffers[imageIndex];
    rpBegin.renderArea.extent = m_swapchain->extent();
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &clearVal;

    vkCmdBeginRenderPass(m_commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Apply zoom/pan: expanding the VkViewport beyond the swapchain extent
    // makes the triangle render larger, and the scissor clips to the window.
    float swW = static_cast<float>(m_swapchain->extent().width);
    float swH = static_cast<float>(m_swapchain->extent().height);

    // The native HWND's client area (==swapchain extent) can be LARGER than
    // the Qt widget rect — on Windows, when a parent widget like a dock
    // title bar partially covers this widget, Qt clips the HWND visually
    // via SetWindowRgn but the HWND keeps its full client area. If we
    // center the video in the full swapchain, the top padding falls into
    // the clipped-by-title-bar area and the video appears with its top
    // flush against the title bar. Compute the widget's offset within the
    // HWND and center within the WIDGET area, not the swapchain area.
    float widgetW = static_cast<float>(width());
    float widgetH = static_cast<float>(height());
    float offsetX = 0.0f;
    float offsetY = 0.0f;
#ifdef _WIN32
    if (m_nativeWindow) {
        HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
        if (hwnd) {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            QPoint widgetGlobal = mapToGlobal(QPoint(0, 0));
            offsetX = static_cast<float>(widgetGlobal.x() - wr.left);
            offsetY = static_cast<float>(widgetGlobal.y() - wr.top);
        }
    }
#endif
    // Fall back to swapchain extent if widget size lookup fails.
    if (widgetW <= 0.0f) { widgetW = swW; offsetX = 0.0f; }
    if (widgetH <= 0.0f) { widgetH = swH; offsetY = 0.0f; }

    // 5% padding on EVERY side — shrink both axes of the WIDGET area by
    // kFitPadding first, then aspect-fit into the smaller box.
    constexpr float kFitPadding = 0.90f;  // 5% margin per side
    const float availW = widgetW * kFitPadding;
    const float availH = widgetH * kFitPadding;
    const float srcW   = (m_srcW > 0) ? static_cast<float>(m_srcW) : 16.0f;
    const float srcH   = (m_srcH > 0) ? static_cast<float>(m_srcH) :  9.0f;
    const float scaleX = availW / srcW;
    const float scaleY = availH / srcH;
    const float scale  = std::min(scaleX, scaleY);
    const float baseW  = srcW * scale;
    const float baseH  = srcH * scale;
    const float baseX  = offsetX + (widgetW - baseW) * 0.5f;
    const float baseY  = offsetY + (widgetH - baseH) * 0.5f;

    {
        static int s_vkDiag = 0;
        if (++s_vkDiag < 6) {
            spdlog::debug("[VKVIEWPORT-FIT] widget={}x{} sw={}x{} offset=({:.0f},{:.0f}) baseY={:.1f} baseH={:.1f}",
                         (int)widgetW, (int)widgetH, (int)swW, (int)swH,
                         offsetX, offsetY, baseY, baseH);
        }
    }

    // Compute the actual on-screen video rect with zoom/pan applied.
    // (Same math as VkViewport below — keep these two in sync.)
    const float zoomedX = baseX + m_viewPanX + (1.0f - m_viewZoom) * baseW * 0.5f;
    const float zoomedY = baseY + m_viewPanY + (1.0f - m_viewZoom) * baseH * 0.5f;
    const float zoomedW = baseW * m_viewZoom;
    const float zoomedH = baseH * m_viewZoom;

    // Premiere-style monitor: clear the FRAME-AREA rectangle to BLACK
    // over the grey background clear, so any transparent regions of the
    // composited image read as black instead of the surrounding letterbox
    // grey. Uses the zoomed/panned rect so the black box tracks the video
    // as the user zooms or pans the viewport (not a static frame).
    // Clamps to the swapchain extent — vkCmdClearAttachments needs the
    // rect inside the framebuffer.
    {
        const float clampedX = std::max(0.0f, zoomedX);
        const float clampedY = std::max(0.0f, zoomedY);
        const float clampedR = std::min(swW, zoomedX + zoomedW);
        const float clampedB = std::min(swH, zoomedY + zoomedH);
        if (clampedR > clampedX && clampedB > clampedY) {
            VkClearAttachment clearAtt{};
            clearAtt.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            clearAtt.colorAttachment = 0;
            clearAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkClearRect clearRect{};
            clearRect.rect.offset    = {
                static_cast<int32_t>(std::floor(clampedX)),
                static_cast<int32_t>(std::floor(clampedY))
            };
            clearRect.rect.extent    = {
                static_cast<uint32_t>(std::ceil(clampedR - clampedX)),
                static_cast<uint32_t>(std::ceil(clampedB - clampedY))
            };
            clearRect.baseArrayLayer = 0;
            clearRect.layerCount     = 1;
            vkCmdClearAttachments(m_commandBuffer, 1, &clearAtt, 1, &clearRect);
        }
    }

    // Bind pipeline and draw fullscreen quad
    vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport vp{};
    vp.x        = zoomedX;
    vp.y        = zoomedY;
    vp.width    = zoomedW;
    vp.height   = zoomedH;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;

    // Store normalized (0-1) rect — render thread only writes floats,
    // UI thread reads them in computeFrameRect().  No widget access here.
    m_gpuFrameRect = QRectF(
        static_cast<double>(vp.x     / swW),
        static_cast<double>(vp.y     / swH),
        static_cast<double>(vp.width / swW),
        static_cast<double>(vp.height/ swH));

    vkCmdSetViewport(m_commandBuffer, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.extent = m_swapchain->extent();
    vkCmdSetScissor(m_commandBuffer, 0, 1, &scissor);

    vkCmdBindDescriptorSets(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    vkCmdDraw(m_commandBuffer, 3, 1, 0, 0); // fullscreen triangle

    vkCmdEndRenderPass(m_commandBuffer);
    vkEndCommandBuffer(m_commandBuffer);

    // Submit
    VkPipelineStageFlags waitStages[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // m_imageAvailable
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT           // compositor semaphore
    };
    VkSemaphore waitSemaphores[2] = { m_imageAvailable, VK_NULL_HANDLE };
    uint32_t waitCount = 1;
    if (waitSemaphore != VK_NULL_HANDLE) {
        waitSemaphores[1] = waitSemaphore;
        waitCount = 2;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = waitCount;
    submitInfo.pWaitSemaphores      = waitSemaphores;
    submitInfo.pWaitDstStageMask    = waitStages;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &m_commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &renderFinished;

    {
        // P1.2: route through GpuScheduler.  The SEH guard that used
        // to live here is now built into the scheduler, so a driver-
        // side access violation still surfaces as VK_ERROR_UNKNOWN
        // (which the scheduler logs as "SEH access violation"); we
        // pick that up here and escalate to device-lost.
        rt::GpuSubmission sub{};
        sub.cmd                  = m_commandBuffer;
        sub.queue                = rt::GpuQueueKind::Graphics;
        sub.waitSemaphoreCount   = submitInfo.waitSemaphoreCount;
        sub.waitSemaphores       = submitInfo.pWaitSemaphores;
        sub.waitStages           = submitInfo.pWaitDstStageMask;
        sub.signalSemaphoreCount = 1;
        sub.signalSemaphores     = &renderFinished;
        sub.completionFence      = m_inFlightFence;
        sub.tag                  = "VulkanViewport::presentFrame";

        VkResult submitRes = rt::GpuContext::get().scheduler().submit(sub);
        if (submitRes != VK_SUCCESS) {
            spdlog::error("[VIEWPORT] present submit failed ({}) — marking GPU Failed.",
                          static_cast<int>(submitRes));
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();
            return;
        }

        // Present runs on the present queue (not the scheduler's set yet
        // — the swapchain owns it directly).  Future P1.x revision can
        // pull this into the scheduler too once we model present as a
        // submission kind.
        VkResult presentRes = m_swapchain->present(
            gpu.device().presentQueue(), imageIndex, renderFinished);
        if (presentRes == VK_ERROR_DEVICE_LOST || presentRes == VK_ERROR_UNKNOWN) {
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();
            return;
        }
    }

    // Recycle the just-used inter-queue semaphore by storing it so it
    // stays alive (never destroyed mid-frame).  Semaphores are destroyed
    // only during shutdownGpu() to avoid a handle-reuse race with the
    // compositor thread (nvoglv64.dll NULL-deref at +0xf39ed4).
    if (waitSemaphore != VK_NULL_HANDLE)
        m_recycledSemaphores.push_back(waitSemaphore);

    emit frameDisplayed();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Deferred semaphore recycling
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::recycleSemaphores()
{
    if (m_recycledSemaphores.empty())
        return;
    // Return semaphores to GpuContext's shared binary-semaphore pool so
    // CompositeEngine's next acquireFrameSemaphore() can reuse them
    // instead of allocating a fresh VkSemaphore every frame (~60/sec of
    // leaked handles over a session — eventually starves the driver).
    //
    // SAFETY: recycleSemaphores() is called at the top of presentFrame(),
    // immediately AFTER vkWaitForFences on m_inFlightFence has succeeded.
    // That fence corresponds to the PREVIOUS frame's submit, whose wait
    // stage already consumed any semaphore that was pushed into this list
    // from a still-earlier frame.  Anything currently in
    // m_recycledSemaphores is therefore in the unsignaled state and safe
    // to hand to the compositor for re-signaling.
    //
    // We deliberately do NOT destroy them: that was the path that
    // previously caused the nvoglv64.dll NULL-deref handle-reuse race.
    // Pool reuse avoids destroy/create entirely, so the race can't fire.
    auto& gpu = GpuContext::get();
    for (VkSemaphore sem : m_recycledSemaphores)
        gpu.releaseBinarySemaphore(sem);
    m_recycledSemaphores.clear();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  CPU Fallback Display
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::displayFrame(std::shared_ptr<CachedFrame> frame)
{
    if (!frame || !frame->ensurePixels()) {
        // Don't auto-clear — let the caller (ProgramMonitor) decide
        // whether to clear or keep the previous frame.
        return;
    }

    auto uploadT0 = std::chrono::steady_clock::now();

    m_frameWidth  = frame->width;
    m_frameHeight = frame->height;

    if (m_gpuActive) {
        // GPU-active mode: upload CPU pixels to a private texture and display
        // via the normal GPU path.  This is used during async playback where
        // the compositor output texture is not safe to sample directly (the
        // render thread may overwrite it).  Cost: ~1ms for 960Ã—540 BGRA.
        //
        // THREAD SAFETY: We use VulkanViewport's own m_commandPool (graphics
        // queue family) for the upload instead of GpuContext::cmdPool() which
        // the async render thread uses concurrently for compositing.
        auto& ctx = GpuContext::get();
        VkDevice device = ctx.vkDevice();
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(frame->pixels.size());

        if (m_uploadSlots.empty()) {
            spdlog::warn("VulkanViewport: no upload slots available");
            return;
        }

        auto& slot = m_uploadSlots[m_nextUploadSlot];
        m_nextUploadSlot = (m_nextUploadSlot + 1) % m_uploadSlots.size();

        VkResult slotFenceRes = vkWaitForFences(device, 1, &slot.fence,
                                                VK_TRUE, 2'000'000);
        if (slotFenceRes == VK_TIMEOUT) {
            return;
        }

        // Free staging buffer from previous upload on this slot (now safe —
        // fence signaled means GPU finished reading).
        if (slot.pendingStagingBuffer != VK_NULL_HANDLE && slot.pendingStagingAllocator) {
            vmaDestroyBuffer(static_cast<VmaAllocator>(slot.pendingStagingAllocator),
                             slot.pendingStagingBuffer,
                             static_cast<VmaAllocation>(slot.pendingStagingAlloc));
            slot.pendingStagingBuffer    = VK_NULL_HANDLE;
            slot.pendingStagingAlloc     = nullptr;
            slot.pendingStagingAllocator = nullptr;
        }

        if (!slot.texture || slot.texture->width() != frame->width ||
            slot.texture->height() != frame->height) {
            slot.texture = std::make_unique<Texture>();
        }

        const bool needCreate = slot.texture->image() == VK_NULL_HANDLE ||
                                slot.texture->width() != frame->width ||
                                slot.texture->height() != frame->height;

        VkCommandBuffer cmd = slot.commandBuffer;
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        Texture::StagingCleanup staging{};
        bool uploadOk = false;
        if (needCreate) {
            // Use R8G8B8A8 (not B8G8R8A8) so the sampler sees the same
            // byteâ†’component mapping as the compositor output image.
            // quad.frag's .bgra swizzle then corrects both paths identically.
            uploadOk = slot.texture->createFromDataBatched(
                ctx.allocator().handle(), device,
                TextureConfig{
                    .width  = frame->width,
                    .height = frame->height,
                    .format = VK_FORMAT_R8G8B8A8_UNORM,
                    .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                },
                frame->pixels.data(), dataSize, cmd, staging);
        } else {
            uploadOk = slot.texture->updateDataBatched(
                frame->pixels.data(), dataSize, cmd, staging);
        }

        vkEndCommandBuffer(cmd);

        if (uploadOk) {
            vkResetFences(device, 1, &slot.fence);
            {
                // P1.2: route through GpuScheduler (was: raw vkQueueSubmit
                // under ctx.graphicsQueueMutex()).
                rt::GpuSubmission sub{};
                sub.cmd             = cmd;
                sub.queue           = rt::GpuQueueKind::Graphics;
                sub.completionFence = slot.fence;
                sub.tag             = "VulkanViewport::uploadCpu";
                VkResult submitResult = rt::GpuContext::get().scheduler().submit(sub);
                if (submitResult != VK_SUCCESS) {
                    spdlog::warn("VulkanViewport: upload submit failed (VkResult={})", static_cast<int>(submitResult));
                    uploadOk = false;
                }
            }

            if (uploadOk) {
                // Bounded wait for upload — avoids blocking the UI thread
                // indefinitely.  If we timeout, the staging buffer stays
                // alive in the slot until the fence is signaled on next reuse.
                VkResult uploadWait = vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 5'000'000); // 5ms
                if (uploadWait == VK_TIMEOUT) {
                    // Upload still in-flight — keep staging alive in slot for
                    // deferred cleanup when the fence is signaled next time.
                    slot.pendingStagingBuffer    = staging.buffer;
                    slot.pendingStagingAlloc     = staging.allocation;
                    slot.pendingStagingAllocator = staging.allocator;
                    staging.buffer = VK_NULL_HANDLE; // prevent destroy() from freeing
                    staging.allocation = nullptr;
                    staging.destroy();
                    return;
                }
            } else {
                // Submit failed — re-signal the fence so this slot stays usable.
                // Submit an empty command buffer to signal the fence.
                VkCommandBuffer emptyCmd = slot.commandBuffer;
                vkResetCommandBuffer(emptyCmd, 0);
                VkCommandBufferBeginInfo emptyBegin{};
                emptyBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                emptyBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(emptyCmd, &emptyBegin);
                vkEndCommandBuffer(emptyCmd);
                // P1.2: route through GpuScheduler.  Empty command
                // buffer just re-signals the slot's fence so the slot
                // stays usable after a failed upload.
                rt::GpuSubmission emptySub{};
                emptySub.cmd             = emptyCmd;
                emptySub.queue           = rt::GpuQueueKind::Graphics;
                emptySub.completionFence = slot.fence;
                emptySub.tag             = "VulkanViewport::uploadFenceResignal";
                rt::GpuContext::get().scheduler().submit(emptySub);
            }
        }

        staging.destroy();

        if (!uploadOk) {
            spdlog::warn("VulkanViewport: CPU upload texture data failed");
            return;
        }

        // Display the upload texture via the normal GPU path
        displayGpuImage(slot.texture->imageView(),
                        slot.texture->sampler(),
                        frame->width, frame->height);
        {
            double uploadMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uploadT0).count();
            if (uploadMs > 3.0) {
                spdlog::info("[PERF] VulkanViewport::displayFrame(CPU): {:.1f}ms  {}x{}  slot={}",
                             uploadMs, frame->width, frame->height, m_nextUploadSlot);
            }
        }
        return;
    }

    uint32_t stride = frame->stride > 0 ? frame->stride : frame->width * 4;
    m_cpuFrameRef = std::move(frame);
    m_cpuImage = QImage(m_cpuFrameRef->pixels.data(),
                        static_cast<int>(m_cpuFrameRef->width),
                        static_cast<int>(m_cpuFrameRef->height),
                        static_cast<int>(stride),
                        QImage::Format_ARGB32);
    update();
}

void VulkanViewport::displayFrame(const CachedFrame& frame)
{
    if (frame.pixels.empty()) {
        clearFrame();
        return;
    }

    m_frameWidth  = frame.width;
    m_frameHeight = frame.height;

    if (!m_gpuActive) {
        uint32_t stride = frame.stride > 0 ? frame.stride : frame.width * 4;
        m_cpuImage = QImage(static_cast<int>(frame.width),
                            static_cast<int>(frame.height),
                            QImage::Format_ARGB32);
        for (uint32_t y = 0; y < frame.height; ++y) {
            std::memcpy(m_cpuImage.scanLine(static_cast<int>(y)),
                        frame.pixels.data() + y * stride,
                        static_cast<size_t>(frame.width) * 4);
        }
        m_cpuFrameRef.reset();
        update();
    }
}

void VulkanViewport::clearFrame()
{
    m_cpuImage = QImage();
    m_cpuFrameRef.reset();
    m_frameWidth  = 0;
    m_frameHeight = 0;
    m_sourceView  = VK_NULL_HANDLE;
    m_sourceTextureOwner.reset();
    m_pendingTextureOwner.reset();

    if (m_gpuActive) {
        presentClearFrame();
    } else {
        update();
    }
}

void VulkanViewport::presentClearFrame()
{
    std::lock_guard lock(m_presentMtx);

    if (!m_gpuActive || !m_swapchain) return;

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    {
        VkResult fenceRes = vkWaitForFences(device, 1, &m_inFlightFence,
                                            VK_TRUE, 2'000'000);
        if (fenceRes == VK_TIMEOUT) {
            return;
        }
    }

    // Recycle consumed semaphores (kept alive to avoid handle-reuse race).
    recycleSemaphores();

    if (m_swapchainDirty || m_swapchain->needsRecreation()) {
        handleResize();
        if (!m_swapchain || m_swapchain->needsRecreation()) return;
    }

    uint32_t imageIndex = m_swapchain->acquireNextImage(device, m_imageAvailable,
                                                         5'000'000); // 5ms
    if (imageIndex == UINT32_MAX) {
        handleResize();
        return;
    }
    if (imageIndex >= m_renderFinishedByImage.size()) {
        spdlog::error("VulkanViewport: acquired image {} without a present semaphore",
                      imageIndex);
        m_swapchainDirty = true;
        return;
    }
    VkSemaphore renderFinished = m_renderFinishedByImage[imageIndex];

    vkResetFences(device, 1, &m_inFlightFence);

    // Record command buffer with clear-only render pass (no draw)
    vkResetCommandBuffer(m_commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(m_commandBuffer, &beginInfo);

    VkClearValue clearVal{};
    clearVal.color = {{0.07f, 0.07f, 0.09f, 1.0f}};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffers[imageIndex];
    rpBegin.renderArea.extent = m_swapchain->extent();
    rpBegin.clearValueCount   = 1;
    rpBegin.pClearValues      = &clearVal;

    vkCmdBeginRenderPass(m_commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Mirror the WITH-video path: clear the FRAME-AREA rectangle to black
    // so the user sees a Premiere-style black video frame with grey
    // letterbox even when no source image is loaded yet (e.g. project
    // opened to an empty position on the timeline). Only kicks in once we
    // know the sequence dimensions (m_srcW/m_srcH from a prior frame).
    // Honours the user's current zoom/pan so the black box tracks the
    // video viewport — same math as presentFrame.
    if (m_srcW > 0 && m_srcH > 0) {
        const float swW = static_cast<float>(m_swapchain->extent().width);
        const float swH = static_cast<float>(m_swapchain->extent().height);
        // See presentFrame() for why we center in the WIDGET area, not the
        // swapchain extent (HWND extends behind dock title bar on Win32).
        float widgetW = static_cast<float>(width());
        float widgetH = static_cast<float>(height());
        float offsetX = 0.0f;
        float offsetY = 0.0f;
#ifdef _WIN32
        if (m_nativeWindow) {
            HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
            if (hwnd) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                QPoint widgetGlobal = mapToGlobal(QPoint(0, 0));
                offsetX = static_cast<float>(widgetGlobal.x() - wr.left);
                offsetY = static_cast<float>(widgetGlobal.y() - wr.top);
            }
        }
#endif
        if (widgetW <= 0.0f) { widgetW = swW; offsetX = 0.0f; }
        if (widgetH <= 0.0f) { widgetH = swH; offsetY = 0.0f; }

        constexpr float kFitPadding = 0.90f;
        const float availW = widgetW * kFitPadding;
        const float availH = widgetH * kFitPadding;
        const float srcW   = static_cast<float>(m_srcW);
        const float srcH   = static_cast<float>(m_srcH);
        const float scale  = std::min(availW / srcW, availH / srcH);
        const float baseW  = srcW * scale;
        const float baseH  = srcH * scale;
        const float baseX  = offsetX + (widgetW - baseW) * 0.5f;
        const float baseY  = offsetY + (widgetH - baseH) * 0.5f;
        const float zoomedX = baseX + m_viewPanX + (1.0f - m_viewZoom) * baseW * 0.5f;
        const float zoomedY = baseY + m_viewPanY + (1.0f - m_viewZoom) * baseH * 0.5f;
        const float zoomedW = baseW * m_viewZoom;
        const float zoomedH = baseH * m_viewZoom;
        const float clampedX = std::max(0.0f, zoomedX);
        const float clampedY = std::max(0.0f, zoomedY);
        const float clampedR = std::min(swW, zoomedX + zoomedW);
        const float clampedB = std::min(swH, zoomedY + zoomedH);
        if (clampedR > clampedX && clampedB > clampedY) {
            VkClearAttachment clearAtt{};
            clearAtt.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
            clearAtt.colorAttachment = 0;
            clearAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkClearRect clearRect{};
            clearRect.rect.offset    = {
                static_cast<int32_t>(std::floor(clampedX)),
                static_cast<int32_t>(std::floor(clampedY))
            };
            clearRect.rect.extent    = {
                static_cast<uint32_t>(std::ceil(clampedR - clampedX)),
                static_cast<uint32_t>(std::ceil(clampedB - clampedY))
            };
            clearRect.baseArrayLayer = 0;
            clearRect.layerCount     = 1;
            vkCmdClearAttachments(m_commandBuffer, 1, &clearAtt, 1, &clearRect);
        }
    }

    vkCmdEndRenderPass(m_commandBuffer);
    vkEndCommandBuffer(m_commandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    // P1.2: route through GpuScheduler.  This is the empty-clear path
    // used when no source frame is available — wait on image-available
    // and signal render-finished so the swapchain present chain stays
    // intact.
    rt::GpuSubmission sub{};
    sub.cmd                  = m_commandBuffer;
    sub.queue                = rt::GpuQueueKind::Graphics;
    sub.waitSemaphoreCount   = 1;
    sub.waitSemaphores       = &m_imageAvailable;
    sub.waitStages           = &waitStage;
    sub.signalSemaphoreCount = 1;
    sub.signalSemaphores     = &renderFinished;
    sub.completionFence      = m_inFlightFence;
    sub.tag                  = "VulkanViewport::clearPresent";
    VkResult submitRes = rt::GpuContext::get().scheduler().submit(sub);
    if (submitRes != VK_SUCCESS) {
        spdlog::error("[VIEWPORT] clear submit failed ({}) — marking GPU Failed.",
                      static_cast<int>(submitRes));
        GpuContext::get().signalDeviceLost();
        GpuContext::get().tryRecover();
        return;
    }

    VkResult presentRes = m_swapchain->present(
        gpu.device().presentQueue(), imageIndex, renderFinished);
    if (presentRes == VK_ERROR_DEVICE_LOST || presentRes == VK_ERROR_UNKNOWN) {
        GpuContext::get().signalDeviceLost();
        GpuContext::get().tryRecover();
        return;
    }

    emit frameDisplayed();
}
} // namespace rt
