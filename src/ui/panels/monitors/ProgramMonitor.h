/*
 * ProgramMonitor — Timeline composite preview panel.
 *
 * Step 15: Source & Program Monitors
 *
 * The Program Monitor shows the live composite of the timeline at the
 * current playhead position:
 *   - Viewport displays the composited frame
 *   - Connects to the main PlaybackController (shared with TransportBar)
 *   - MiniTimeline scrub bar for quick timeline navigation
 *   - Overlay displays timecode and safe areas
 *   - Click-to-select elements in the frame (future: transform gizmo)
 *
 * Layout:
 *   ┌─────────────────────────────────┐
 *   │  "Program" label                │
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │         Viewport                │
 *   │    (composited frame)           │
 *   │                                 │
 *   ├─────────────────────────────────┤
 *   │  [MiniTimeline scrub bar]       │
 *   ├─────────────────────────────────┤
 *   │  Fit ▼ | SafeArea | TC display │
 *   └─────────────────────────────────┘
 */

#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QStackedLayout;

namespace rt {

class TransportButton;
class Viewport;
class VulkanViewport;
class TransformOverlayWidget;
class MiniTimeline;
class PlaybackController;
class PlaybackScheduler;
class Timeline;
class MediaPool;
struct CachedFrame;

/// Program monitor panel — shows the composited timeline output.
class ProgramMonitor : public QWidget
{
    Q_OBJECT

public:
    explicit ProgramMonitor(QWidget* parent = nullptr);
    ~ProgramMonitor() override;

    // ── Dependencies ────────────────────────────────────────────────────

    /// Attach the main playback controller (shared with TransportBar).
    void setController(PlaybackController* controller);

    /// Attach the timeline data model.
    void setTimeline(Timeline* timeline);

    /// Attach the media pool for frame decoding.
    void setMediaPool(MediaPool* pool);

    /// Get the attached controller.
    [[nodiscard]] PlaybackController* controller() const noexcept { return m_controller; }

    /// Get the attached timeline.
    [[nodiscard]] Timeline* timeline() const noexcept { return m_timeline; }

    /// Set GPU display mode — when true, compositing skips CPU readback
    /// and the compositor output is displayed directly via VulkanViewport.
    void setGpuDisplayEnabled(bool enabled);
    [[nodiscard]] bool isGpuDisplayEnabled() const noexcept { return m_gpuDisplay; }

    // ── Display control ─────────────────────────────────────────────────

    /// Start polling for playback position updates (~60fps).
    void startPolling(int intervalMs = 16);

    /// Stop polling.
    void stopPolling();

    /// A10: explicitly stop the PlaybackScheduler (clock + producer +
    /// presenter threads).  Called from App::~App Phase 1 before the
    /// MainWindow widget tree is destroyed in Phase 3.  Without this,
    /// the presenter thread could still call into a partially-destroyed
    /// CompositeService when MainWindow tears down.
    void stopPlaybackPipeline();

    /// Force a display refresh at the current playhead position.
    void refresh();

    /// Mark the display as dirty so the next poll cycle re-composites.
    /// Unlike refresh(), this does NOT block the calling thread with an
    /// immediate compositeFrame — the render happens on the next ~16ms
    /// poll timer tick.  Use this after edits that modify timeline data
    /// (split, trim, delete, property changes) to keep the UI responsive.
    void requestRefresh();

    /// Fully reset the Program Monitor's rendering state.
    /// Call this after workspace/dock layout changes to recover from a
    /// corrupted composite preview (black screen, stale overlays, wrong
    /// viewport index, broken native HWND clipping, etc.).
    /// Resets: view stack index, last-rendered tick, pipeline cache,
    /// transform overlay, wall-clock state, and native window clipping.
    void resetViewState();

    /// Queue a non-blocking pre-roll composite request for playback start.
    /// This warms decode/prefetch/GPU caches on the render thread without
    /// synchronously compositing on the UI thread.
    void requestPlaybackPreroll(int64_t tick);

    /// Set playback resolution by index (0=Full, 1=1/2, 2=1/4, 3=1/8).
    void setPlaybackResolutionIndex(int index)
    {
        if (m_playbackResCombo && index >= 0 && index < 4)
            m_playbackResCombo->setCurrentIndex(index);
    }

    /// Set the output resolution for compositing.
    void setOutputResolution(uint32_t width, uint32_t height);
    [[nodiscard]] uint32_t outputWidth() const noexcept { return m_outputWidth; }
    [[nodiscard]] uint32_t outputHeight() const noexcept { return m_outputHeight; }

    /// Actual composite resolution (outputWidth / playback quality divisor).
    [[nodiscard]] uint32_t compositeWidth() const noexcept {
        uint32_t w = m_outputWidth / static_cast<uint32_t>(m_playbackResDivisor);
        return std::clamp(w, 64u, 3840u);
    }
    [[nodiscard]] uint32_t compositeHeight() const noexcept {
        uint32_t h = m_outputHeight / static_cast<uint32_t>(m_playbackResDivisor);
        return std::clamp(h, 36u, 2160u);
    }

    // ── Viewport access ─────────────────────────────────────────────────

    /// Direct access to the viewport widget.
    [[nodiscard]] Viewport* viewport() const noexcept { return m_viewport; }

    /// Direct access to the GPU viewport widget.
    [[nodiscard]] VulkanViewport* vulkanViewport() const noexcept { return m_vulkanViewport; }

    /// Direct access to the transform overlay (sits on top of GPU viewport).
    [[nodiscard]] TransformOverlayWidget* transformOverlay() const noexcept { return m_transformOverlay; }

    /// Direct access to the mini-timeline.
    [[nodiscard]] MiniTimeline* miniTimeline() const noexcept { return m_miniTimeline; }

    /// Access the most recently displayed frame (for scopes, export, etc.).
    /// Thread-safe: pipeline present thread writes, UI thread reads.
    [[nodiscard]] std::shared_ptr<CachedFrame> lastDisplayedFrame() const;

    /// Direct access to the playback scheduler.
    [[nodiscard]] PlaybackScheduler* pipeline() const noexcept;

    // ── Compositing callback ────────────────────────────────────────────

    /// Set a callback that composites the timeline at a given tick and returns
    /// a frame. This allows the compositor (from Step 10) to be plugged in.
    /// Signature: shared_ptr<CachedFrame> composite(tick, w, h, scrubMode, stillMode)
    using CompositeCallback = std::function<std::shared_ptr<CachedFrame>(
        int64_t tick, uint32_t w, uint32_t h, bool scrubMode, bool stillMode)>;
    void setCompositeCallback(CompositeCallback cb);

    /// Set a callback invoked whenever the playback-resolution dropdown
    /// changes.  Divisor: 1=Full, 2=1/2, 4=1/4, 8=1/8.  The compositor
    /// uses this to pick the matching ResolutionTier so decode/cache
    /// sizes scale with preview resolution — otherwise the dropdown
    /// only shrinks composite output while the decoder still produces
    /// full-resolution frames (the bug this fixes).
    using PlaybackTierCallback = std::function<void(int divisor)>;
    void setPlaybackTierCallback(PlaybackTierCallback cb);

    /// Set a callback to query VRAM usage (returns percentage 0-100).
    /// Used to show a VRAM pressure warning overlay.
    using VramQueryCallback = std::function<int()>;
    void setVramQueryCallback(VramQueryCallback cb) { m_vramQuery = std::move(cb); }

    /// Notify the monitor that an external seek/scrub occurred so it
    /// retries compositing even if the poll-timer tick hasn't changed yet.
    void notifyScrub();

    /// Signal that a new composited frame is available for display.
    /// Called from the composite callback or pipeline.
    void onNewFrame(std::shared_ptr<CachedFrame> frame);

    QSize sizeHint() const override;

signals:
    /// Emitted when the user scrubs the program mini-timeline.
    void scrubbed(int64_t tick);

    /// Emitted when the display refreshes.
    void frameDisplayed(int64_t tick);

    /// Emitted when export frame button is clicked.
    void exportFrameRequested();

    /// Emitted when user presses I to set in-point at current playhead.
    void inPointRequested();

    /// Emitted when user presses O to set out-point at current playhead.
    void outPointRequested();

private slots:
    void onPollTimer();
    void onScrub(int64_t tick);
    void onFitModeChanged(int index);

private:
    void setupUI();
    void updateDisplay();
    void updateTimecodeDisplay();
    void syncOverlayGeometry();
    [[nodiscard]] bool usesAsyncPipelinePath() const noexcept;
    void ensurePipelineStarted();
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

    // Widgets
    QStackedLayout*      m_viewStack{nullptr};
    Viewport*              m_viewport{nullptr};
    VulkanViewport*        m_vulkanViewport{nullptr};
    TransformOverlayWidget* m_transformOverlay{nullptr};
    MiniTimeline*          m_miniTimeline{nullptr};
    QLabel*           m_timecodeLabel{nullptr};
    QLabel*           m_durationLabel{nullptr};
    QLabel*           m_zoomLabel{nullptr};
    QComboBox*        m_fitModeCombo{nullptr};
    QPushButton*      m_btnSafeArea{nullptr};
    QPushButton*      m_btnGrid{nullptr};
    QComboBox*        m_playbackResCombo{nullptr};
    QPushButton*      m_btnExportFrame{nullptr};
    QPushButton*      m_btnSettings{nullptr};
    TransportButton*  m_btnScreenshot{nullptr};
    QLabel*           m_resOverlayLabel{nullptr};
    QLabel*           m_droppedFrameLabel{nullptr};  ///< Dropped frame counter display

    // New UI widgets
    QPushButton*      m_btnLoop{nullptr};           ///< Loop playback toggle
    QLineEdit*        m_timecodeEdit{nullptr};       ///< Editable timecode field (hidden until click)
    QLabel*           m_shuttleSpeedLabel{nullptr};  ///< Shuttle speed display (e.g. "2x")
    QLabel*           m_dropIndicator{nullptr};     ///< Frame drop indicator (green/yellow/red dot)

    // Transport buttons (Premiere Pro style)
    TransportButton*  m_btnGoStart{nullptr};
    TransportButton*  m_btnStepBack{nullptr};
    TransportButton*  m_btnPlayPause{nullptr};
    TransportButton*  m_btnStop{nullptr};
    TransportButton*  m_btnStepForward{nullptr};
    TransportButton*  m_btnGoEnd{nullptr};

    QTimer*           m_pollTimer{nullptr};

    // Dependencies
    PlaybackController* m_controller{nullptr};
    Timeline*           m_timeline{nullptr};
    MediaPool*          m_pool{nullptr};

    // Composite
    CompositeCallback   m_compositeCallback;
    PlaybackTierCallback m_playbackTierCallback;

    // Output resolution
    uint32_t m_outputWidth{1920};
    uint32_t m_outputHeight{1080};

    // Last rendered tick (to avoid redundant renders)
    int64_t  m_lastRenderedTick{-1};
    int64_t  m_lastRenderedFrame{-1}; ///< Last composited project frame number

    // Scrub debounce — store pending scrub tick, render on next poll
    bool     m_scrubPending{false};
    int64_t  m_pendingScrubTick{0};
    bool     m_isScrubbing{false};
    int      m_scrubSettleCounter{0};  ///< Post-scrub re-render countdown
    int      m_editSettleCounter{0};  ///< Post-edit re-render countdown (full res)

    // Scrub composite throttle: skip every other frame during scrubbing
    // to reduce GPU driver load (prevents NV driver stack overflow from
    // rapid Vulkan resource churn when spine state is recreated).
    int      m_scrubSkipCounter{0};

    // Frame-budget tracking
    QElapsedTimer m_compositeStopwatch;
    int64_t  m_lastCompositeMs{0};   ///< How long the last composite took (ms)

    // Playback resolution divisor (1 = Full, 2 = 1/2, 4 = 1/4, 8 = 1/8).
    // When m_playbackResAdaptive is true, the FrameProducer's adaptive
    // controller owns the effective divisor and this field only tracks
    // the seed value.
    int      m_playbackResDivisor{2};
    bool     m_playbackResAdaptive{false};

    // GPU display
    bool     m_gpuDisplay{false};    ///< True when VulkanViewport is active
    bool     m_firstShowDone{false}; ///< True after the first showEvent

    // VRAM pressure monitoring
    VramQueryCallback m_vramQuery;

    /// Atomic flag set in the destructor BEFORE stopping pipeline threads.
    /// Checked by the present callback and presentFrame() to prevent
    /// use-after-free when the presenter thread calls into a partially-
    /// destroyed ProgramMonitor (the callback lambda captures `this`).
    std::atomic<bool> m_destroying{false};

    // ── PlaybackScheduler ────────────────────────────────────────────
    // Decoupled composite/present engine.  Two threads:
    //   Composite thread: produces frames at project FPS (accumulator-timed)
    //   Present thread:   displays via VulkanViewport (MAILBOX swapchain)
    // Replaces the old monolithic renderThreadLoop.
    std::unique_ptr<PlaybackScheduler> m_pipeline;
    void initPipeline();

    // ── Coalesced cross-thread present ────────────────────────────────
    // The presenter thread (background) queues presentFrame calls on the
    // GUI thread via QMetaObject::invokeMethod.  Without coalescing, if
    // the GUI thread is busy (scrubbing), every presenter frame piles up
    // in the Qt event queue — each holding a shared_ptr<CachedFrame> with
    // pixel data — causing unbounded memory growth and eventual OOM crash.
    //
    // Solution: store the latest frame from the presenter thread and
    // queue at most ONE invokeMethod at a time.  When processed, it picks
    // up the latest frame and clears the flag.
    std::mutex                              m_queuedPresentMtx;
    std::shared_ptr<CachedFrame>            m_queuedPresentFrame;
    std::atomic<bool>                       m_queuedPresentPending{false};
    void flushQueuedPresent();

    /// Most recently presented frame, regardless of whether it came from
    /// the direct UI path or the async producer/presenter pipeline.
    std::shared_ptr<CachedFrame> m_lastDirectFrame;

    // ── Dirty flag optimization (Phase 5.C) ───────────────────────────
    // Set true whenever a new frame is pushed from the compositor.
    // The pollTimer checks this and skips work if false, saving CPU
    // cycles when the view is static.
    std::atomic<bool> m_newFrameAvailable{false};
    std::shared_ptr<CachedFrame> m_pendingFrame;

    /// Present callback wired into the pipeline — called from present thread.
    bool presentFrame(const std::shared_ptr<CachedFrame>& frame);
};

} // namespace rt
