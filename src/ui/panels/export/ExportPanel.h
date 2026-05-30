/*
 * ExportPanel — Export settings and render queue UI.
 *
 * Step 24: Export Pipeline
 *
 * Provides controls for configuring export settings (resolution, codec,
 * quality, output path) and monitoring render queue progress.
 *
 * Layout:
 * ┌──────────────────────────────────────────────────────────┐
 * │  PRESET   [YouTube 1080p ▼]                              │
 * │                                                          │
 * │  Resolution  [1920] x [1080]     FPS  [30 ▼]            │
 * │  Codec       [H.264 ▼]          Accel [NVENC ▼]         │
 * │  Quality     CRF [18───────]                             │
 * │                                                          │
 * │  Audio       [✓ Include]  Codec [PCM ▼]                  │
 * │  Container   [MP4 ▼]                                     │
 * │                                                          │
 * │  Output      [path/to/output.mp4        ] [Browse]       │
 * │                                                          │
 * │  [ Start Export ]  [ Cancel ]                             │
 * │                                                          │
 * │  ┌────────────────────────────────────────────────────┐  │
 * │  │  Progress: ████████░░░░░  62%  12.3 fps  ETA 45s  │  │
 * │  └────────────────────────────────────────────────────┘  │
 * │                                                          │
 * │  Queue:                                                  │
 * │  ✓ Job 1 — output_01.mp4 — Complete                     │
 * │  ► Job 2 — output_02.mp4 — Running 62%                  │
 * │  ○ Job 3 — output_03.mp4 — Queued                       │
 * └──────────────────────────────────────────────────────────┘
 */

#pragma once

#include "Encoder.h"
#include "AudioMixdown.h"
#include "RenderQueue.h"
#include "timeline/TimelineObserver.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QImage>

#include <atomic>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace rt {

class Project;
class Timeline;
class Compositor;
class ExportMiniTimeline;
class PlaybackController;
class AudioEngine;
class CommandStack;

/// Export panel — configure and launch video/audio exports.
class ExportPanel : public QWidget, public TimelineObserver
{
    Q_OBJECT

public:
    explicit ExportPanel(QWidget* parent = nullptr);
    ~ExportPanel() override;

    /// True while the render queue worker thread is processing a job.
    /// Used by MainWindow::closeEvent to prompt before tearing down the
    /// app and silently cancelling the user's export.
    [[nodiscard]] bool isExporting() const noexcept;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the timeline and compositor used for rendering.
    void setTimeline(Timeline* timeline);
    void setCompositor(Compositor* compositor);
    void setProject(Project* project);

    /// Build and return the current export job config from UI state.
    [[nodiscard]] ExportJobConfig buildJobConfig() const;

    /// Callback that renders a composited frame at a given tick.
    using PreviewCallback = std::function<std::shared_ptr<struct CachedFrame>(int64_t tick, uint32_t w, uint32_t h, bool scrub)>;
    void setPreviewCallback(PreviewCallback cb);

    /// Update the preview thumbnail (called when panel becomes visible).
    void refreshPreview();

private:
    /// Snapshot current in/out points + range-combo selection, apply new
    /// values, refresh the preview, and push a LambdaCommand so Ctrl+Z
    /// reverses the edit.  No-op if the new state equals the current state.
    void applyInOutPointEdit(const std::string& description,
                             int64_t newInPoint,
                             int64_t newOutPoint,
                             int     newRangeComboIdx);

public:

    /// Set the global playback controller (for audio + AV-sync during preview play).
    void setPlaybackController(PlaybackController* controller);

    /// Set the audio engine (for scrub audio and playback).
    void setAudioEngine(AudioEngine* engine);

    /// Set the command stack for undo/redo integration (in/out point edits).
    void setCommandStack(CommandStack* stack) { m_commandStack = stack; }

    // ── Accessors (for testing) ─────────────────────────────────────────

    [[nodiscard]] QComboBox*   presetCombo()    const { return m_presetCombo; }
    [[nodiscard]] QSpinBox*    widthSpin()      const { return m_widthSpin; }
    [[nodiscard]] QSpinBox*    heightSpin()     const { return m_heightSpin; }
    [[nodiscard]] QComboBox*   fpsCombo()       const { return m_fpsCombo; }
    [[nodiscard]] QComboBox*   codecCombo()     const { return m_codecCombo; }
    [[nodiscard]] QComboBox*   accelCombo()     const { return m_accelCombo; }
    [[nodiscard]] QSlider*     crfSlider()      const { return m_crfSlider; }
    [[nodiscard]] QLabel*      crfLabel()       const { return m_crfLabel; }
    [[nodiscard]] QComboBox*   containerCombo() const { return m_containerCombo; }
    [[nodiscard]] QCheckBox*   audioCheck()     const { return m_audioCheck; }
    [[nodiscard]] QLineEdit*   outputPath()     const { return m_outputPath; }
    [[nodiscard]] QPushButton* startButton()    const { return m_startButton; }
    [[nodiscard]] QPushButton* cancelButton()   const { return m_cancelButton; }
    [[nodiscard]] QProgressBar* progressBar()   const { return m_progressBar; }
    [[nodiscard]] QLabel*      statusLabel()    const { return m_statusLabel; }
    [[nodiscard]] QListWidget* jobList()        const { return m_jobList; }

signals:
    /// Emitted when an export job starts.
    void exportStarted(uint32_t jobId);

    /// Emitted with progress updates.
    void exportProgress(uint32_t jobId, float percent);

    /// Emitted when a job completes or fails.
    void exportFinished(uint32_t jobId, bool success, const QString& message);

    /// Emitted when the user presses Escape in the Export panel.
    void navigateBack();

    /// Emitted when the in-panel preview play/pause state changes.
    /// MainWindow uses this to suspend the Program Monitor's playback
    /// pipeline so it doesn't double-composite while the Export panel
    /// runs its own UI-thread render loop.
    void previewPlaybackToggled(bool playing);

private slots:
    void onPresetChanged(int index);
    void onCodecChanged(int index);
    void onCrfChanged(int value);
    void onBrowseOutput();
    void onStartExport();
    void onCancelExport();
    void onPollProgress();
    void onRangeChanged(int index);
    void onPlayPause();
    void onStepForward();
    void onStepBack();
    void onSkipToStart();
    void onSkipToEnd();
    void onPlaybackTick();
    void onAddToQueue();
    void onSetInPoint();
    void onSetOutPoint();
    void onClearInOut();
    void updateFileEstimate();

    // TimelineObserver
    void onInOutChanged() override;
    void onTimelineDestroyed(Timeline* tl) override;

protected:
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    std::atomic<bool> m_destroying{false};

    void setupUI();

    /// Persist the directory of `outputPath` as the last-used export
    /// location so future exports default there.
    void rememberExportDir(const std::string& outputPath);

    /// Check for offline media in the timeline. Returns false if the user
    /// decides to cancel the export when warned about offline clips.
    bool checkOfflineMedia();
    void populatePresets();
    void populateCodecs();
    void populateContainers();
    void populateAccel();
    void updateUIFromPreset(ExportPreset preset);
    void onSavePreset();
    void onDeletePreset();
    void loadCustomPresets();
    QString customPresetsDir() const;
    void syncMatchSequenceSettings();

    // ── Widgets ─────────────────────────────────────────────────────────

    // Preset
    QComboBox*    m_presetCombo{nullptr};
    QCheckBox*    m_matchSequenceCheck{nullptr};
    QPushButton*  m_savePresetBtn{nullptr};
    QPushButton*  m_deletePresetBtn{nullptr};

    // Video
    QSpinBox*     m_widthSpin{nullptr};
    QSpinBox*     m_heightSpin{nullptr};
    QComboBox*    m_fpsCombo{nullptr};
    QComboBox*    m_codecCombo{nullptr};
    QComboBox*    m_accelCombo{nullptr};
    QSlider*      m_crfSlider{nullptr};
    QLabel*       m_crfLabel{nullptr};
    QComboBox*    m_containerCombo{nullptr};

    // Audio
    QCheckBox*    m_audioCheck{nullptr};

    // Range
    QComboBox*    m_rangeCombo{nullptr};

    // Preview
    QLabel*       m_previewImageLabel{nullptr};
    QLabel*       m_previewInfoLabel{nullptr};
    ExportMiniTimeline* m_miniTimeline{nullptr};
    PreviewCallback m_previewCallback;

    // Transport controls
    QPushButton*  m_skipToStartBtn{nullptr};
    QPushButton*  m_stepBackBtn{nullptr};
    QPushButton*  m_playPauseBtn{nullptr};
    QPushButton*  m_stepForwardBtn{nullptr};
    QPushButton*  m_skipToEndBtn{nullptr};
    QPushButton*  m_inPointBtn{nullptr};
    QPushButton*  m_outPointBtn{nullptr};
    QPushButton*  m_clearInOutBtn{nullptr};
    QTimer*       m_playbackTimer{nullptr};
    bool          m_playing{false};

    // Saved main timeline playhead — restored when export playback stops
    // so the main timeline stays where the user left it.
    int64_t       m_savedMainPlayhead{-1};

    // Output
    QLineEdit*    m_outputPath{nullptr};
    QPushButton*  m_browseButton{nullptr};

    // Actions
    QPushButton*  m_startButton{nullptr};
    QPushButton*  m_cancelButton{nullptr};
    QPushButton*  m_addQueueButton{nullptr};

    // Estimate
    QLabel*       m_estimateLabel{nullptr};

    // Progress
    QProgressBar* m_progressBar{nullptr};
    QLabel*       m_statusLabel{nullptr};
    QListWidget*  m_jobList{nullptr};

    // Poll timer for progress
    QTimer*       m_pollTimer{nullptr};

    // Backend
    Project*      m_project{nullptr};
    Timeline*     m_timeline{nullptr};
    Compositor*   m_compositor{nullptr};
    PlaybackController* m_playbackController{nullptr};
    AudioEngine*  m_audioEngine{nullptr};
    CommandStack* m_commandStack{nullptr};
    std::unique_ptr<RenderQueue> m_renderQueue;
    uint32_t      m_activeJobId{0};

    // ── Async composite pipeline ────────────────────────────────────────
    struct CompositeSlot {
        std::shared_future<std::shared_ptr<CachedFrame>> future;
        int64_t tick{-1};
    };
    CompositeSlot m_pipelineSlots[2];
    int m_pipelineCurrentSlot{0};

    /// Called from worker thread: pipeline composite for (tick, nextTick).
    /// Submits nextTick's composite (non-blocking), waits for tick's result.
    std::shared_ptr<CachedFrame> pipelineComposite(
        int64_t tick, int64_t nextTick,
        uint32_t w, uint32_t h, bool scrub);

    /// Guards against recursive re-entry into refreshPreview() which can
    /// cause infinite paint recursion and stack overflow (crash pattern
    /// seen with QDialog::exec during paint events).
    bool m_refreshing{false};
};

} // namespace rt
