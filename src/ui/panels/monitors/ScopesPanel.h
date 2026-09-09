/*
 * ScopesPanel — Professional color monitoring: Waveform, Vectorscope, Histogram.
 *
 * Architecture (modeled after MLT/Shotcut consumer-frame-show pattern):
 *   - feedFrame() copies pixel data and posts it to a dedicated worker thread
 *   - Worker thread runs O(w×h) analysis + renders scope visualization to QImage
 *   - Finished QImage is delivered to the UI thread via QueuedConnection
 *   - paintEvent() simply blits the pre-rendered QImage (~0.3ms)
 *   - Throttled to ~15fps to avoid unnecessary CPU work
 *
 * This ensures ZERO heavy computation runs on the Qt UI thread, matching
 * how professional NLEs (Premiere Pro, DaVinci Resolve, Shotcut) handle
 * scopes without impacting playback smoothness.
 */

#pragma once

#include <QWidget>
#include <QComboBox>
#include <QImage>
#include <QLabel>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class QResizeEvent;

namespace rt {

class ScopesPanel : public QWidget
{
    Q_OBJECT

public:
    enum ScopeMode : int {
        Waveform = 0,
        Vectorscope,
        Histogram,
        ModeCount
    };

    explicit ScopesPanel(QWidget* parent = nullptr);
    ~ScopesPanel() override;

    /// Feed a frame for analysis. Pixels are BGRA8, row-major.
    /// This is safe to call from any thread — data is copied to the worker.
    /// Throttled internally: calls arriving faster than ~15fps are skipped.
    void feedFrame(const uint8_t* pixels, int width, int height);

    /// Set the scope display mode.
    void setScopeMode(ScopeMode mode);
    [[nodiscard]] ScopeMode scopeMode() const noexcept { return m_mode; }

    QSize sizeHint() const override;

signals:
    void scopeModeChanged(int mode);

    /// Internal signal: worker thread delivers a rendered scope image.
    /// Connected via QueuedConnection so the UI thread just blits it.
    void scopeImageReady(QImage image, quint64 revision);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // ── Worker thread (all heavy work runs here, never on UI thread) ────
    void startWorker();
    void stopWorker();
    void workerLoop();

    // Analysis (runs on worker thread)
    void analyzeWaveform(const uint8_t* pixels, int w, int h);
    void analyzeVectorscope(const uint8_t* pixels, int w, int h);
    void analyzeHistogram(const uint8_t* pixels, int w, int h);

    // Rendering to QImage (runs on worker thread)
    QImage renderWaveform(int imgW, int imgH);
    QImage renderVectorscope(int imgW, int imgH);
    QImage renderHistogram(int imgW, int imgH);

    // UI thread slot
    void onScopeImageReady(QImage image, quint64 revision);
    void updateWorkerConfiguration(ScopeMode mode, const QSize& renderSize);

    ScopeMode m_mode{Waveform};
    QComboBox* m_modeCombo{nullptr};
    QLabel*    m_statusLabel{nullptr};

    // ── Pre-rendered scope image (set by worker, blitted by paintEvent) ─
    QImage m_renderedScope;

    // ── Worker thread state ─────────────────────────────────────────────
    std::thread             m_worker;
    std::atomic<bool>       m_workerRunning{false};
    std::mutex              m_workerMtx;
    std::condition_variable m_workerCv;

    // Pending frame buffer (double-buffered: UI writes, worker reads)
    std::vector<uint8_t>    m_pendingPixels;
    int                     m_pendingWidth{0};
    int                     m_pendingHeight{0};
    ScopeMode               m_pendingMode{Waveform};
    int                     m_pendingRenderWidth{200};
    int                     m_pendingRenderHeight{160};
    quint64                 m_pendingRevision{0};
    bool                    m_hasPending{false};

    // Written by the UI thread and captured with each pending frame. The
    // worker reads these fields only while holding m_workerMtx.
    ScopeMode               m_requestedMode{Waveform};
    int                     m_requestedRenderWidth{200};
    int                     m_requestedRenderHeight{160};
    quint64                 m_configRevision{0};

    // ── Analysis data (owned by worker thread) ──────────────────────────
    static constexpr int kWaveformCols = 256;
    static constexpr int kWaveformLevels = 256;
    std::array<std::array<uint16_t, kWaveformLevels>, kWaveformCols> m_waveformData{};
    uint16_t m_waveformMax{1};

    static constexpr int kVectorscopeSize = 256;
    std::array<std::array<uint16_t, kVectorscopeSize>, kVectorscopeSize> m_vectorscopeData{};
    uint16_t m_vectorscopeMax{1};

    std::array<uint32_t, 256> m_histR{};
    std::array<uint32_t, 256> m_histG{};
    std::array<uint32_t, 256> m_histB{};
    std::array<uint32_t, 256> m_histLuma{};
    uint32_t m_histMax{1};

    // ── Throttle ────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point m_lastFeedTime{};
    static constexpr int kMinFeedIntervalMs = 66; // ~15fps max
};

} // namespace rt
