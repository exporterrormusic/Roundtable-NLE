/*
 * VulkanViewport — Zero-copy GPU viewport for displaying compositor output.
 *
 * Uses GpuContext's VkDevice to create a native Win32 surface + swapchain,
 * then renders the compositor output via a fullscreen textured quad.
 *
 * Key difference from the old QVulkanWindow approach: shares the SAME VkDevice
 * as the compositor, so VkImageViews can be directly sampled without cross-
 * device copies.  Falls back to CPU (QPainter) if Vulkan init fails.
 *
 * Usage:
 *   auto* vp = new VulkanViewport(parent);
 *   // After compositor runs:
 *   vp->displayGpuImage(compositor->outputImageView(),
 *                       compositor->outputSampler(),
 *                       compositor->outputWidth(),
 *                       compositor->outputHeight());
 */

#pragma once

#include <QWidget>
#include <QWindow>
#include <QCursor>
#include <QImage>
#include <QRectF>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QTimer>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace rt {

struct CachedFrame;
class  Swapchain;
class  Texture;

/// Widget wrapper that provides both GPU and CPU display paths.
/// Drop-in replacement for Viewport in ProgramMonitor.
class VulkanViewport : public QWidget
{
    Q_OBJECT

public:
    explicit VulkanViewport(QWidget* parent = nullptr);
    ~VulkanViewport() override;

    // ── GPU display ─────────────────────────────────────────────────────

    /// Display the compositor output directly (zero-copy GPU path).
    /// @param waitSemaphore  Optional VkSemaphore (timeline) to wait on before
    ///                       reading the image.  Used for inter-queue sync
    ///                       between compute (compositor) and graphics (present).
    /// @param textureOwner   Shared ownership handle that keeps the source
    ///                       texture alive until the GPU finishes sampling it.
    ///                       From CachedFrame::gpuTextureOwner.
    void displayGpuImage(VkImageView imageView, VkSampler sampler,
                         uint32_t width, uint32_t height,
                         VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                         std::shared_ptr<void> textureOwner = nullptr);

    /// Is the GPU backend active?
    [[nodiscard]] bool isGpuActive() const noexcept { return m_gpuActive; }

    // ── CPU fallback ────────────────────────────────────────────────────

    /// Display a CPU frame (fallback when GPU is unavailable).
    void displayFrame(std::shared_ptr<CachedFrame> frame);
    void displayFrame(const CachedFrame& frame);

    /// Clear to black.
    void clearFrame();

    /// Request a redraw.
    void refresh();

    /// Current frame dimensions.
    [[nodiscard]] uint32_t frameWidth() const noexcept { return m_frameWidth; }
    [[nodiscard]] uint32_t frameHeight() const noexcept { return m_frameHeight; }

    /// Current view zoom level (1.0 = 100%).
    [[nodiscard]] float viewZoom() const noexcept { return m_viewZoom; }

    /// Current view pan offset (widget pixels).
    [[nodiscard]] float viewPanX() const noexcept { return m_viewPanX; }
    [[nodiscard]] float viewPanY() const noexcept { return m_viewPanY; }

    /// Set pan offset programmatically (e.g. from overlay widget).
    void setViewPan(float px, float py);

    /// Set zoom level programmatically (1.0 = 100%).
    void setViewZoom(float zoom);

    /// Zoom to fill the entire widget (no letterbox/pillarbox).
    void zoomToFill();

    /// Source image dimensions shown by GPU.
    [[nodiscard]] uint32_t srcWidth()  const noexcept { return m_srcW; }
    [[nodiscard]] uint32_t srcHeight() const noexcept { return m_srcH; }

    /// GPU viewport rect in widget-local logical pixels.
    /// Updated every present; returns empty if no frame has been presented.
    [[nodiscard]] QRectF gpuFrameRect() const noexcept { return m_gpuFrameRect; }

    /// Reset zoom to 1.0 and pan to (0,0).
    void resetZoomPan();

    /// Access the underlying QWindow (for event filter installation).
    [[nodiscard]] QWindow* nativeWindow() const noexcept { return m_nativeWindow; }

    /// The native viewport owns left-button (inline text routing) and
    /// middle-button (reset view) double-clicks. Handling both in one native
    /// event filter avoids platform-dependent filter-chain fall-through.
    [[nodiscard]] static bool ownsNativeDoubleClick(
        Qt::MouseButton button) noexcept {
        return button == Qt::LeftButton || button == Qt::MiddleButton;
    }

    /// Apply a cursor to the embedded Vulkan surface.  Setting it only on
    /// the wrapped QWindow is unreliable for a createWindowContainer'd
    /// native surface, so this also sets it on the container widget and
    /// this widget — whichever the platform actually honors wins.
    void setViewportCursor(const QCursor& cursor);

    /// Accessors used by the Win32 WM_SETCURSOR subclass procedure.
    [[nodiscard]] void* origWndProc() const noexcept { return m_origWndProc; }
    [[nodiscard]] void* winCursor()   const noexcept { return m_winCursor; }

    /// Actual native surface size (HWND client rect).  May differ from
    /// QWidget::size() due to createWindowContainer / DPI quirks.
    /// Falls back to QWidget::size() on non-Windows.
    [[nodiscard]] QSize nativeSurfaceSize() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void frameDisplayed();

    /// Emitted when the embedded native surface receives a left double-click.
    /// The transform overlay maps the global point into sequence space and
    /// starts Premiere-style inline title editing.
    void nativeLeftDoubleClicked(QPointF globalPosition,
                                 Qt::KeyboardModifiers modifiers);

    /// Emitted when the viewport zoom level changes.
    void viewZoomChanged(float zoom);

    /// Emitted when the viewport is resized (so the monitor can re-composite).
    void resized();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool initGpu();
    void shutdownGpu();
    bool createSwapchainResources();
    void destroySwapchainResources();
    bool createRenderPass();
    bool createPipeline();
    bool createDescriptorResources();
    bool createSyncObjects();
    bool createPresentSemaphores();
    bool createFramebuffers();
    void handleResize();
    void presentFrame(VkSemaphore waitSemaphore = VK_NULL_HANDLE);
    void presentClearFrame();

    // ── Semaphore lifecycle ──────────────────────────────────────────────
    /// Collect semaphores consumed by the previous present and keep them
    /// alive (no-op recycle).  Semaphores are destroyed only during
    /// shutdownGpu() to avoid the handle-reuse race with the compositor
    /// thread that caused nvoglv64.dll NULL-deref crashes.
    void recycleSemaphores();

    // ── Native window + Vulkan surface ──────────────────────────────────
    QWindow*          m_nativeWindow{nullptr};
    QWidget*          m_windowContainer{nullptr};
    VkSurfaceKHR      m_surface{VK_NULL_HANDLE};

    // ── Cursor subclassing (Windows) ────────────────────────────────────
    // Qt's QWindow::setCursor is not honored for a createWindowContainer'd
    // raw Vulkan surface, so we intercept WM_SETCURSOR on the HWND and
    // force the cursor via the Win32 API.  m_desiredCursor is kept alive
    // so its HCURSOR handle stays valid.
    QCursor           m_desiredCursor;
    void*             m_origWndProc{nullptr};   // original WNDPROC (WNDPROC)
    void*             m_winCursor{nullptr};     // HCURSOR shown via WM_SETCURSOR
    bool              m_winCursorOwned{false};  // true ⇒ DestroyIcon on replace
    void              installCursorSubclass();
    void              removeCursorSubclass();

    // ── Swapchain ───────────────────────────────────────────────────────
    std::unique_ptr<Swapchain> m_swapchain;
    bool m_swapchainDirty{false};

    // ── Render pass + framebuffers ──────────────────────────────────────
    VkRenderPass      m_renderPass{VK_NULL_HANDLE};
    std::vector<VkFramebuffer> m_framebuffers;

    // ── Pipeline ────────────────────────────────────────────────────────
    VkPipeline        m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout  m_pipelineLayout{VK_NULL_HANDLE};
    VkShaderModule    m_vertShader{VK_NULL_HANDLE};
    VkShaderModule    m_fragShader{VK_NULL_HANDLE};

    // ── Descriptors ─────────────────────────────────────────────────────
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};
    VkSampler             m_fallbackSampler{VK_NULL_HANDLE};

    // ── Sync ────────────────────────────────────────────────────────────
    VkSemaphore       m_imageAvailable{VK_NULL_HANDLE};
    // Present-wait semaphores are associated with acquired swapchain images.
    // Reusing one for another image while presentation still owns it violates
    // VUID-vkQueueSubmit-pSignalSemaphores-00067 and can reset the driver.
    std::vector<VkSemaphore> m_renderFinishedByImage;
    VkFence           m_inFlightFence{VK_NULL_HANDLE};
    VkCommandPool     m_commandPool{VK_NULL_HANDLE};
    VkCommandBuffer   m_commandBuffer{VK_NULL_HANDLE};

    // ── Source image (compositor output) ─────────────────────────────────
    VkImageView       m_sourceView{VK_NULL_HANDLE};
    VkSampler         m_sourceSampler{VK_NULL_HANDLE};
    uint32_t          m_srcW{0};
    uint32_t          m_srcH{0};
    bool              m_imageDirty{false};
    // Holds a reference to the CURRENTLY sampled texture until the
    // GPU-signaled fence indicates the next frame's work is complete.
    std::shared_ptr<void> m_sourceTextureOwner;

    // ── Pending source image (deferred until after fence wait) ─────────
    // displayGpuImage stores handles here; presentFrame applies them to
    // m_sourceView/m_sourceSampler AFTER the previous frame's fence has
    // signaled and m_presentMtx is held.  This closes the race where the
    // compositor resizes and destroys the output texture between the
    // handle store (displayGpuImage) and the fence wait (presentFrame).
    VkImageView       m_pendingView{VK_NULL_HANDLE};
    VkSampler         m_pendingSampler{VK_NULL_HANDLE};
    uint32_t          m_pendingW{0};
    uint32_t          m_pendingH{0};
    bool              m_pendingValid{false};
    // Holds a reference to the source texture (via gpuTextureOwner)
    // until presentFrame's fence signals the GPU is done reading it.
    std::shared_ptr<void> m_pendingTextureOwner;

    // ── CPU fallback ────────────────────────────────────────────────────
    QImage m_cpuImage;
    std::shared_ptr<CachedFrame> m_cpuFrameRef;

    // ── CPU → GPU upload slots (for async composite display) ───────────
    // Reuse a small ring of upload textures + command buffers + fences so
    // CPU-backed preview frames avoid per-frame Vulkan alloc/destroy and do
    // not overwrite the texture currently being sampled for presentation.
    struct UploadSlot {
        std::unique_ptr<Texture> texture;
        VkCommandBuffer          commandBuffer{VK_NULL_HANDLE};
        VkFence                  fence{VK_NULL_HANDLE};
        // Deferred staging cleanup — kept alive until the slot's fence
        // is signaled so the GPU can finish reading the staging buffer.
        VkBuffer      pendingStagingBuffer{VK_NULL_HANDLE};
        void*         pendingStagingAlloc{nullptr};     // VmaAllocation
        void*         pendingStagingAllocator{nullptr};  // VmaAllocator
    };
    static constexpr uint32_t kUploadSlotCount = 3;
    std::vector<UploadSlot> m_uploadSlots;
    size_t m_nextUploadSlot{0};

    bool     m_gpuActive{false};
    bool     m_gpuInitialized{false};
    uint32_t m_frameWidth{0};
    uint32_t m_frameHeight{0};

    // ── Present serialization ────────────────────────────────────────────
    // Protects VkCommandBuffer recording/submission in presentFrame() and
    // presentClearFrame() from concurrent access by the FramePresenter
    // thread (displayGpuImage path) and the UI thread (zoom/pan/resize).
    std::mutex m_presentMtx;

    // ── Deferred semaphore recycling ─────────────────────────────────────
    // Per-frame inter-queue semaphores are created by CompositeEngine and
    // passed to presentFrame() via displayGpuImage().  We used to destroy
    // them after use, but that caused a handle-reuse race: the compositor
    // thread could create a new semaphore with the same handle the GUI
    // thread was about to destroy, destroying the NEW semaphore and causing
    // a NULL-deref in the NVIDIA driver.
    //
    // Instead, we RECYCLE them: used semaphores are stored in this pool and
    // the cycle (signal → wait → signal → wait → ... ) continues without
    // ever destroying and recreating handles.  The pool is drained (freed)
    // only during shutdownGpu().
    std::vector<VkSemaphore> m_recycledSemaphores;

    // ── Viewport zoom & pan ─────────────────────────────────────────────
    float m_viewZoom{1.0f};
    float m_viewPanX{0.0f};
    float m_viewPanY{0.0f};

    // ── Cached GPU viewport rect (logical pixels) ───────────────────────
    QRectF m_gpuFrameRect;

    // ── Cached widget dimensions for thread-safe resize handling ────────
    // Updated by resizeEvent (UI thread), read by handleResize (render thread).
    std::atomic<uint32_t> m_cachedWidth{1};
    std::atomic<uint32_t> m_cachedHeight{1};

    // ── Resize debounce ──────────────────────────────────────────────────
    // QWidget::resizeEvent fires per-pixel during dock animations / drags.
    // Each fire used to trigger a full swapchain recreate + vkDeviceWaitIdle,
    // which thrashes the NVIDIA driver and caused VK_ERROR_DEVICE_LOST during
    // playback.  Now we cache the latest dimensions and start a 100ms
    // single-shot timer; the swapchain is rebuilt once, after the user stops
    // resizing.  resizeEvent also drives an inline rebuild on every WM_SIZE
    // during the drag (the modal pump doesn't drain Qt's queued events),
    // so this timer is primarily a safety net for the post-drag settle.
    QTimer* m_resizeDebounceTimer{nullptr};
    static constexpr int kResizeDebounceMs = 100;

    // ── Deferred-redraw coalescing ───────────────────────────────────────
    // Zoom/pan/wheel handlers schedule a queued-connection redraw rather
    // than calling presentFrame() directly.  Without this, the UI thread
    // would block on m_presentMtx (held by the FramePresenter thread doing
    // a 60fps composite-present cycle), which manifested as ~100ms
    // pollTimer gaps in the captured perf log.  This flag coalesces
    // multiple back-to-back UI requests into a single deferred present.
    std::atomic<bool> m_redrawQueued{false};
    void scheduleDeferredRedraw();
};

} // namespace rt
