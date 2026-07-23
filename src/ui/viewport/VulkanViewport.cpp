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

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  Helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


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

    // The render fence covers graphics execution, but not the presentation
    // engine's wait on renderFinished. Drain only the present queue before
    // destroying its swapchain or replacing the per-image semaphores. This
    // is substantially narrower than the former device-wide idle.
    VkResult presentIdle = gpu.scheduler().queueWaitIdle(
        gpu.device().presentQueue());
    if (presentIdle != VK_SUCCESS) {
        if (presentIdle == VK_ERROR_DEVICE_LOST ||
            presentIdle == VK_ERROR_UNKNOWN) {
            gpu.signalDeviceLost();
            gpu.tryRecover();
        }
        return;
    }

    // Destroy old framebuffers
    for (auto fb : m_framebuffers)
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    m_framebuffers.clear();

    // Recreate swapchain — use cached dimensions (thread-safe)
    uint32_t w = m_cachedWidth.load();
    uint32_t h = m_cachedHeight.load();
    if (!m_swapchain->recreate(gpu.device(), w, h))
        return;
    if (!createPresentSemaphores()) {
        spdlog::error("VulkanViewport: failed to recreate present semaphores");
        return;
    }

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
