/*
 * VulkanViewport.cpp â€” Zero-copy GPU viewport via native swapchain.
 *
 * Uses GpuContext's Vulkan device to create a Win32 surface + swapchain,
 * then renders the compositor output via a fullscreen textured quad.
 * Falls back to CPU QPainter if GPU init fails.
 */

#include "viewport/VulkanViewport.h"
#include "Theme.h"
#include "GpuContext.h"
#include "GpuScheduler.h"
#include "vulkan/Swapchain.h"
#include "vulkan/Texture.h"
#include "media/FrameCache.h"

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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

#if 0 // Currently unused
static std::vector<uint32_t> loadSpirv(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> spirv(static_cast<size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

static VkShaderModule createShaderModule(VkDevice device,
                                          const std::vector<uint32_t>& spirv)
{
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode    = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}
#endif // unused shader helpers

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Construction / Destruction
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

VulkanViewport::VulkanViewport(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(160, 90);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Dark background for CPU fallback path
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Theme::colors().surface0);
    setPalette(pal);

    // Force this widget to become a native window (HWND) BEFORE initGpu().
    // createWindowContainer() inside initGpu() parents the Vulkan surface
    // HWND to the nearest native ancestor.  If VulkanViewport itself has
    // an HWND, the surface becomes a direct child and Win32 automatically
    // clips it to VulkanViewport's bounds — preventing the surface from
    // overflowing into sibling widgets (control bar, transport, etc.).
    setAttribute(Qt::WA_NativeWindow);
    winId();  // materialise the HWND now

    // Try to init GPU surface
    if (GpuContext::get().isInitialized()) {
        initGpu();
    }

    if (!m_gpuActive) {
        spdlog::info("VulkanViewport: GPU display not available, using CPU fallback");
    }

    // Resize debounce — see kResizeDebounceMs in the header.
    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    m_resizeDebounceTimer->setInterval(kResizeDebounceMs);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, [this]() {
        if (!m_gpuActive) return;
        m_swapchainDirty = true;
        if (m_sourceView != VK_NULL_HANDLE)
            refresh();
        else
            presentClearFrame();
    });
}

VulkanViewport::~VulkanViewport()
{
    removeCursorSubclass();  // restore original WNDPROC before teardown
    shutdownGpu();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  GPU Initialization
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Resize handling
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::handleResize()
{
    if (!m_gpuActive || !m_swapchain) return;

    auto& gpu = GpuContext::get();
    VkDevice device = gpu.vkDevice();

    // Wait only on the viewport's own in-flight submit before tearing down
    // its framebuffers/swapchain.  Previously we called vkDeviceWaitIdle()
    // here, which stalled the compute queue (compositor mid-frame) and the
    // transfer queue (upload manager).  Under a dock-animation resize storm
    // this happened many times per second and triggered VK_ERROR_DEVICE_LOST.
    // The viewport is the only consumer of these swapchain images, so its
    // own fence + the upload-slot fences are the only synchronization
    // points that matter for swapchain recreate.
    if (m_inFlightFence != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &m_inFlightFence, VK_TRUE, 100'000'000);
    }
    // Drain CPU-upload slots — their command buffers reference textures
    // that we keep, not swapchain images, but their submits go through the
    // graphics queue and we want them off the queue before we destroy
    // framebuffers (the present submit waits on m_imageAvailable from a new
    // swapchain image).
    for (auto& slot : m_uploadSlots) {
        if (slot.fence != VK_NULL_HANDLE)
            vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 100'000'000);
    }

    // Destroy old framebuffers
    for (auto fb : m_framebuffers)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();

    // Recreate swapchain — use cached dimensions (thread-safe)
    uint32_t w = m_cachedWidth.load();
    uint32_t h = m_cachedHeight.load();
    m_swapchain->recreate(gpu.device(), w, h);

    // Recreate framebuffers
    createFramebuffers();
    m_swapchainDirty = false;

    // Preserve the source image across the swapchain rebuild.  The
    // composite texture lives independently of the swapchain framebuffers
    // — sampling a texture inside the shader doesn't care about the
    // framebuffer extent — and m_sourceTextureOwner (shared_ptr) keeps
    // the VkImage alive until at least the next compositor::resize().
    //
    // Wiping the view here used to be the source of the paused-resize
    // echo: each WM_SIZE that triggered a recreate left presentFrame
    // with no source to display, so the swapchain held its last image
    // and DWM stretched it into the growing HWND.  Keeping the view
    // lets the inline refresh() in resizeEvent re-present that same
    // texture into the NEW-size framebuffer every drag tick, so the
    // image stays sharp and centered as the window grows.
    //
    // m_imageDirty=true forces the descriptor set to be rewritten on
    // the next present in case anything underneath got reshuffled.
    m_imageDirty = true;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
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
    // waitSemaphore — points at a destroyed device.  Refusing here means
    // ProgramMonitor falls back to the CPU display path, which the safe-mode
    // compositor populates with valid pixels.
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
            spdlog::warn("[VKVIEWPORT-FIT] widget={}x{} sw={}x{} offset=({:.0f},{:.0f}) baseY={:.1f} baseH={:.1f}",
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
    submitInfo.pSignalSemaphores    = &m_renderFinished;

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
        sub.signalSemaphores     = &m_renderFinished;
        sub.completionFence      = m_inFlightFence;
        sub.tag                  = "VulkanViewport::presentFrame";

        VkResult submitRes = rt::GpuContext::get().scheduler().submit(sub);
#ifdef _WIN32
        if (submitRes == VK_ERROR_UNKNOWN) {
            spdlog::error("[VIEWPORT] Access violation in vkQueueSubmit "
                          "— marking GPU Failed.");
            GpuContext::get().signalDeviceLost();
            GpuContext::get().tryRecover();
            return;
        }
#else
        (void)submitRes;
#endif

        // Present runs on the present queue (not the scheduler's set yet
        // — the swapchain owns it directly).  Future P1.x revision can
        // pull this into the scheduler too once we model present as a
        // submission kind.
        m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);
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
    sub.signalSemaphores     = &m_renderFinished;
    sub.completionFence      = m_inFlightFence;
    sub.tag                  = "VulkanViewport::clearPresent";
    rt::GpuContext::get().scheduler().submit(sub);

    m_swapchain->present(gpu.device().presentQueue(), imageIndex, m_renderFinished);

    emit frameDisplayed();
}

void VulkanViewport::refresh()
{
    if (m_gpuActive && m_sourceView != VK_NULL_HANDLE) {
        presentFrame();
    } else {
        update();
    }
}

void VulkanViewport::scheduleDeferredRedraw()
{
    // Coalesce: if a redraw is already queued, don't queue another.
    bool expected = false;
    if (!m_redrawQueued.compare_exchange_strong(expected, true))
        return;

    QMetaObject::invokeMethod(this, [this]() {
        m_redrawQueued.store(false, std::memory_order_release);
        if (m_gpuActive)
            refresh();
        else
            update();
    }, Qt::QueuedConnection);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Qt Events
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::paintEvent(QPaintEvent* event)
{
    static thread_local int s_paintDepth = 0;
    if (++s_paintDepth > 5) {
        --s_paintDepth;
        QWidget::paintEvent(event);
        return;
    }

    if (m_gpuActive) { --s_paintDepth; return; } // GPU path handles its own rendering

    QPainter painter(this);
    painter.fillRect(rect(), Theme::colors().surface0);

    if (m_cpuImage.isNull()) { --s_paintDepth; return; }

    // Aspect-ratio-correct fit
    QSizeF imgSize(m_cpuImage.width(), m_cpuImage.height());
    QSizeF widgetSize(width(), height());
    imgSize.scale(widgetSize, Qt::KeepAspectRatio);

    QRectF drawRect(
        (width() - imgSize.width()) / 2.0,
        (height() - imgSize.height()) / 2.0,
        imgSize.width(), imgSize.height());

    painter.drawImage(drawRect, m_cpuImage);

    --s_paintDepth;
}

#ifdef _WIN32
namespace {
// HWND → owning VulkanViewport, so the subclass WndProc can find the
// desired cursor.  Only ever touched on the UI thread.
std::unordered_map<HWND, VulkanViewport*> g_cursorSubclassMap;

// Build a Win32 HCURSOR for a QCursor.
//   - Custom (pixmap) cursors  → CreateIconIndirect from the bitmap
//                                (owned = caller must DestroyIcon).
//   - Standard shape cursors   → shared system cursor via LoadCursorW
//                                (owned = false, never destroy).
HCURSOR buildHCursor(const QCursor& c, bool& owned)
{
    owned = false;
    const QPixmap pm = c.pixmap();
    if (!pm.isNull()) {
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        const int w = img.width();
        const int h = img.height();

        BITMAPV5HEADER bi{};
        bi.bV5Size        = sizeof(BITMAPV5HEADER);
        bi.bV5Width       = w;
        bi.bV5Height      = -h;          // top-down
        bi.bV5Planes      = 1;
        bi.bV5BitCount    = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask     = 0x00FF0000;
        bi.bV5GreenMask   = 0x0000FF00;
        bi.bV5BlueMask    = 0x000000FF;
        bi.bV5AlphaMask   = 0xFF000000;

        HDC hdc = GetDC(nullptr);
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (color && bits) {
            // QImage ARGB32 little-endian byte order is B,G,R,A — matches
            // the 32bpp BGRA DIB above, so a straight copy is correct.
            for (int y = 0; y < h; ++y)
                std::memcpy(static_cast<uint8_t*>(bits) + y * w * 4,
                            img.scanLine(y), static_cast<size_t>(w) * 4);

            HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
            ICONINFO ii{};
            ii.fIcon    = FALSE;          // FALSE ⇒ cursor (uses hotspot)
            ii.xHotspot = static_cast<DWORD>(c.hotSpot().x());
            ii.yHotspot = static_cast<DWORD>(c.hotSpot().y());
            ii.hbmMask  = mask;
            ii.hbmColor = color;
            HCURSOR hc = reinterpret_cast<HCURSOR>(CreateIconIndirect(&ii));
            if (mask)  DeleteObject(mask);
            if (color) DeleteObject(color);
            if (hc) { owned = true; return hc; }
        }
        if (color) DeleteObject(color);
        // fall through to a shape fallback if bitmap path failed
    }

    const wchar_t* id = IDC_ARROW;
    switch (c.shape()) {
        case Qt::SizeFDiagCursor: id = IDC_SIZENWSE; break;
        case Qt::SizeBDiagCursor: id = IDC_SIZENESW; break;
        case Qt::SizeHorCursor:   id = IDC_SIZEWE;   break;
        case Qt::SizeVerCursor:   id = IDC_SIZENS;   break;
        case Qt::SizeAllCursor:   id = IDC_SIZEALL;  break;
        case Qt::OpenHandCursor:
        case Qt::ClosedHandCursor:
        case Qt::PointingHandCursor: id = IDC_HAND;  break;
        case Qt::CrossCursor:     id = IDC_CROSS;    break;
        default:                  id = IDC_ARROW;    break;
    }
    return LoadCursorW(nullptr, id);   // shared — owned stays false
}

LRESULT CALLBACK cursorSubclassProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    auto it = g_cursorSubclassMap.find(hwnd);
    VulkanViewport* self = (it != g_cursorSubclassMap.end()) ? it->second : nullptr;
    WNDPROC orig = self ? reinterpret_cast<WNDPROC>(self->origWndProc()) : nullptr;

    if (self && msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        if (auto hc = reinterpret_cast<HCURSOR>(self->winCursor())) {
            ::SetCursor(hc);
            return TRUE;   // handled — stop Windows resetting to arrow
        }
    }
    if (orig)
        return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

void VulkanViewport::installCursorSubclass()
{
    if (!m_nativeWindow || m_origWndProc) return;
    HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
    if (!hwnd) return;
    g_cursorSubclassMap[hwnd] = this;
    m_origWndProc = reinterpret_cast<void*>(SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&cursorSubclassProc)));
}

void VulkanViewport::removeCursorSubclass()
{
    if (!m_nativeWindow || !m_origWndProc) return;
    HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
    if (hwnd) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(
                              reinterpret_cast<WNDPROC>(m_origWndProc)));
        g_cursorSubclassMap.erase(hwnd);
    }
    m_origWndProc = nullptr;
    if (m_winCursor && m_winCursorOwned)
        DestroyIcon(reinterpret_cast<HICON>(m_winCursor));
    m_winCursor = nullptr;
    m_winCursorOwned = false;
}
#else
void VulkanViewport::installCursorSubclass() {}
void VulkanViewport::removeCursorSubclass() {}
#endif

void VulkanViewport::setViewportCursor(const QCursor& cursor)
{
    m_desiredCursor = cursor;

    // Qt-level set (covers non-Windows + keeps the widget tree consistent).
    if (m_nativeWindow)    m_nativeWindow->setCursor(cursor);
    if (m_windowContainer) m_windowContainer->setCursor(cursor);
    setCursor(cursor);

#ifdef _WIN32
    // Build the native HCURSOR the WM_SETCURSOR subclass will force onto
    // the surface.  Replace (and free, if we own it) the previous one.
    if (m_winCursor && m_winCursorOwned)
        DestroyIcon(reinterpret_cast<HICON>(m_winCursor));
    bool owned = false;
    m_winCursor = reinterpret_cast<void*>(buildHCursor(cursor, owned));
    m_winCursorOwned = owned;

    // Apply immediately so it changes without waiting for the next move.
    if (m_winCursor)
        ::SetCursor(reinterpret_cast<HCURSOR>(m_winCursor));
#endif
}

QSize VulkanViewport::nativeSurfaceSize() const
{
#ifdef _WIN32
    if (m_nativeWindow) {
        HWND hwnd = reinterpret_cast<HWND>(m_nativeWindow->winId());
        if (hwnd) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            int w = cr.right  - cr.left;
            int h = cr.bottom - cr.top;
            if (w > 0 && h > 0)
                return QSize(w, h);
        }
    }
#endif
    return size();  // fallback to QWidget logical size
}

void VulkanViewport::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;

    QWidget::resizeEvent(event);

    const QSize surfaceSize = nativeSurfaceSize();
    // Cache dimensions for thread-safe handleResize (called from render thread)
    m_cachedWidth.store(std::max(1u, static_cast<uint32_t>(surfaceSize.width())));
    m_cachedHeight.store(std::max(1u, static_cast<uint32_t>(surfaceSize.height())));

    // Debounce swapchain recreation. Dock animations / drags emit dozens of
    // resize events per second; recreating the swapchain on each one wedges
    // the driver. Restart the timer — when the user stops resizing for
    // kResizeDebounceMs, the slot will rebuild the swapchain once.
    if (m_gpuActive && m_resizeDebounceTimer) {
        m_resizeDebounceTimer->start();
    }
    emit resized();

    // Same rationale as moveEvent: on Windows the modal WM_SIZE loop runs
    // its own pump that doesn't drain Qt's posted-event queue, so the
    // debounce timer above won't fire until the user RELEASES the drag.
    // Without an inline re-present, the swapchain shows its last-presented
    // frame frozen in place for the entire duration of the drag — visible
    // as an "echo" of stale UI in the area that the HWND has grown into.
    // ProgramMonitor suffers from this because its updateDisplay() routes
    // through the async pipeline (FrameProducer → FramePresenter via
    // queued connection), so the fresh composite never reaches the UI
    // thread during the modal loop.  SourceMonitor uses the CPU Viewport
    // (no Vulkan swapchain) and so doesn't exhibit the issue.
    //
    // The inline refresh() will see m_swapchainDirty / needsRecreation
    // (set by acquireNextImage detecting the extent mismatch), call
    // handleResize() to rebuild the swapchain at the new size, then
    // re-present the existing m_sourceView.  handleResize() now
    // preserves the source view across rebuild, so the composite stays
    // visible throughout the drag instead of getting stretched by DWM.
    if (m_gpuActive && m_sourceView != VK_NULL_HANDLE) {
        m_swapchainDirty = true;
        refresh();
    }

    s_inResize = false;
}

void VulkanViewport::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);

    // Vulkan native surfaces don't follow Qt's normal paint/expose loop:
    // moving the widget (e.g. dock rearrange, window drag) does NOT
    // fire resizeEvent or paintEvent, so the last-presented swapchain
    // content stays on screen at the OLD position, producing a visible
    // "echo" of stale UI alongside the new position.
    //
    // We have to refresh synchronously here — posting via
    // scheduleDeferredRedraw is too late, because on Windows the OS
    // modal move loop runs its own pump and Qt's posted-event queue
    // isn't drained until the drag ENDS.  By the time the deferred
    // event fires the user has already seen the echo for the whole
    // duration of the move.  Calling refresh() inline re-presents the
    // existing m_sourceView every move tick, which is cheap (a single
    // Vulkan submit of an unchanged texture) and matches the OS's
    // native compositing cadence.
    if (m_gpuActive && m_sourceView != VK_NULL_HANDLE) {
        refresh();
    }
}

QSize VulkanViewport::sizeHint() const
{
    return QSize(640, 360);
}

QSize VulkanViewport::minimumSizeHint() const
{
    return QSize(160, 90);
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Zoom & Pan
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void VulkanViewport::resetZoomPan()
{
    m_viewZoom = 1.0f;
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::setViewPan(float px, float py)
{
    m_viewPanX = px;
    m_viewPanY = py;
    scheduleDeferredRedraw();
}

void VulkanViewport::setViewZoom(float zoom)
{
    m_viewZoom = std::clamp(zoom, 0.1f, 20.0f);
    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::zoomToFill()
{
    float swW = static_cast<float>(width());
    float swH = static_cast<float>(height());
    float imgAspect = (m_srcW > 0 && m_srcH > 0)
        ? static_cast<float>(m_srcW) / static_cast<float>(m_srcH)
        : 16.0f / 9.0f;
    float winAspect = swW / std::max(swH, 1.0f);

    // Fill = zoom enough so the shorter dimension fills the window
    if (winAspect > imgAspect)
        m_viewZoom = winAspect / imgAspect;  // window wider → zoom to fill width
    else
        m_viewZoom = imgAspect / winAspect;  // window taller → zoom to fill height

    m_viewPanX = 0.0f;
    m_viewPanY = 0.0f;
    scheduleDeferredRedraw();
    emit viewZoomChanged(m_viewZoom);
}

void VulkanViewport::wheelEvent(QWheelEvent* event)
{
    float delta = static_cast<float>(event->angleDelta().y());
    if (delta == 0.0f) {
        event->ignore();
        return;
    }

    float factor = (delta > 0) ? 1.1f : (1.0f / 1.1f);
    float newZoom = std::clamp(m_viewZoom * factor, 0.1f, 20.0f);

    // Zoom toward mouse position â€” pan is relative to center
    // (rendering: vp.x = centerX - zoom*baseW/2 + panX)
    QPointF mousePos = event->position();
    float mx = static_cast<float>(mousePos.x()) - width()  * 0.5f;
    float my = static_cast<float>(mousePos.y()) - height() * 0.5f;

    float zoomRatio = newZoom / m_viewZoom;
    m_viewPanX += (1.0f - zoomRatio) * (mx - m_viewPanX);
    m_viewPanY += (1.0f - zoomRatio) * (my - m_viewPanY);

    m_viewZoom = newZoom;

    scheduleDeferredRedraw();

    emit viewZoomChanged(m_viewZoom);
    event->accept();
}

void VulkanViewport::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        resetZoomPan();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

bool VulkanViewport::eventFilter(QObject* watched, QEvent* event)
{
    // The native QWindow receives all input inside createWindowContainer.
    // Intercept wheel + double-click events and forward to our handlers.
    if (watched == m_nativeWindow) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            const QSize surfaceSize = nativeSurfaceSize();
            m_cachedWidth.store(std::max(1u, static_cast<uint32_t>(surfaceSize.width())));
            m_cachedHeight.store(std::max(1u, static_cast<uint32_t>(surfaceSize.height())));
            if (m_gpuActive && isVisible() && m_resizeDebounceTimer) {
                // Same debounce as QWidget::resizeEvent — drags fire many
                // resizes per second; recreating the swapchain on every one
                // wedges the driver.
                m_resizeDebounceTimer->start();
            }
            emit resized();
        }
        if (event->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(event));
            return true;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            mouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace rt
