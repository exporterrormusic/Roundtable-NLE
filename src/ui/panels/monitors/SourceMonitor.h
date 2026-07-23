/*
 * SourceMonitor — Source clip preview panel.
 *
 * Step 15: Source & Program Monitors
 *
 * The Source Monitor loads a single media clip for preview:
 *   - Viewport displays the current frame
 *   - MiniTimeline scrub bar below shows clip duration + playhead
 *   - I/O point buttons to mark in/out on the source clip
 *   - Drag from source to timeline (insert/overwrite edit)
 *   - Internal PlaybackController manages independent transport (play/pause/scrub)
 *
 * Layout:
 *   ┌─────────────────────────────────┐
 *   │  Clip name label               │
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │         Viewport                │
 *   │       (frame display)           │
 *   │                                 │
 *   ├─────────────────────────────────┤
 *   │  [MiniTimeline scrub bar]       │
 *   ├─────────────────────────────────┤
 *   │  ◀I  ▶O  ⏮ ◀ ▶⏸ ▶ ⏭  TC      │
 *   └─────────────────────────────────┘
 */

#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QStackedLayout;

namespace rt {

class AudioEngine;
class AudioPlaybackService;
class AVSyncClock;
class TransportButton;
class Viewport;
class MiniTimeline;
class PlaybackController;
class MediaPool;
class MediaSourceService;
class Project;
class Timeline;
class WaveformDisplayWidget;
class SpineClip;
class CompositeService;
struct CachedFrame;
enum class ResolutionTier : uint8_t;

/// Source monitor panel — loads and previews a single media clip.
class SourceMonitor : public QWidget
{
    Q_OBJECT

public:
    explicit SourceMonitor(QWidget* parent = nullptr);
    ~SourceMonitor() override;

    // ── Media loading ───────────────────────────────────────────────────

    /// Set the shared AudioEngine for source clip playback.
    void setAudioEngine(AudioEngine* engine);

    /// Callback type for rendering a nested sequence frame.
    using SequenceFrameProvider = std::function<
        std::shared_ptr<CachedFrame>(int64_t tick, uint32_t w, uint32_t h,
                                     bool scrub, ResolutionTier tier,
                                     bool still)>;

    /// Resolves the inner sequence's Timeline lazily so the source monitor
    /// never holds a stale pointer when the project changes.
    using SequenceTimelineGetter = std::function<Timeline*()>;

    /// Load a sequence for preview (renders via compositeFrame callback).
    /// @param timelineGetter  Optional — when set, the source monitor uses it
    /// to resolve the inner sequence's Timeline for audio playback. When
    /// null, sequence preview plays without audio (legacy behaviour).
    void loadSequence(size_t sequenceIndex, const QString& name,
                      int64_t durationTicks, double fps,
                      uint32_t sequenceWidth, uint32_t sequenceHeight,
                      SequenceFrameProvider frameProvider,
                      SequenceTimelineGetter timelineGetter = nullptr);

    /// Set playback resolution by index (0=Full, 1=1/2, 2=1/4, 3=1/8).
    void setPlaybackResolutionIndex(int index)
    {
        if (m_playbackResCombo && index >= 0 && index < 4)
            m_playbackResCombo->setCurrentIndex(index);
    }

    /// Load a SpineClip for live preview (renders with SpineRenderer).
    /// @param spineClip  The SpineClip to preview (non-owning).
    /// @param compositeService  For renderSpineClip() access.
    void loadSpineClip(SpineClip* spineClip, CompositeService* compositeService);

    /// Is the currently loaded source a sequence (not a media clip)?
    [[nodiscard]] bool isSequenceLoaded() const noexcept { return m_isSequence; }

    /// Get the loaded sequence index (valid only when isSequenceLoaded()).
    [[nodiscard]] size_t sequenceIndex() const noexcept { return m_sequenceIndex; }

    /// Load a clip by media handle + pool reference.
    /// Sets up the viewport, scrub bar, and internal transport for the clip.
    void loadClip(uint64_t mediaHandle, MediaPool* pool);

    /// Clear the current clip from the monitor.
    void clearClip();

    /// Is a clip currently loaded?
    [[nodiscard]] bool hasClip() const noexcept { return m_hasClip; }

    /// Get the currently loaded media handle.
    [[nodiscard]] uint64_t mediaHandle() const noexcept { return m_mediaHandle; }

    // ── In / Out points ─────────────────────────────────────────────────

    /// Mark the in-point at the current playhead position.
    void markIn();

    /// Mark the out-point at the current playhead position.
    void markOut();

    /// Clear both in and out points.
    void clearInOut();

    /// Get in/out points in ticks.
    [[nodiscard]] int64_t inPoint()  const noexcept;
    [[nodiscard]] int64_t outPoint() const noexcept;
    [[nodiscard]] bool hasInPoint()  const noexcept;
    [[nodiscard]] bool hasOutPoint() const noexcept;

    /// Duration of the selected region for drag-to-timeline.
    [[nodiscard]] int64_t selectedDuration() const noexcept;

    // ── Transport controls ──────────────────────────────────────────────

    /// Get the internal playback controller (for external wiring).
    [[nodiscard]] PlaybackController* controller() const noexcept { return m_controller.get(); }

    /// Current playhead position in ticks.
    [[nodiscard]] int64_t currentTick() const noexcept;

    /// Scrub to a specific tick position.
    void scrubTo(int64_t tick);

    /// Scrub to a specific frame number.
    void scrubToFrame(int64_t frameNumber);

    // ── Frame access ────────────────────────────────────────────────────

    /// Get the total number of frames in the loaded clip.
    [[nodiscard]] int64_t frameCount() const noexcept { return m_frameCount; }

    /// Get the framerate of the loaded clip.
    [[nodiscard]] double frameRate() const noexcept { return m_fps; }

    /// Get the clip duration in ticks.
    [[nodiscard]] int64_t clipDuration() const noexcept { return m_clipDuration; }

    // ── Source info for timeline insertion ───────────────────────────────

    /// Data describing a source region, used when dragging to timeline.
    struct SourceRegion
    {
        uint64_t mediaHandle{0};
        int64_t  sourceIn{0};    ///< Source start tick
        int64_t  sourceOut{0};   ///< Source end tick
        int64_t  duration{0};    ///< Duration in ticks
        double   fps{24.0};
    };

    /// Get the current source region (in/out or full clip).
    [[nodiscard]] SourceRegion sourceRegion() const;

    // ── Display label ───────────────────────────────────────────────────

    void setClipName(const QString& name);

    QSize sizeHint() const override;

signals:
    /// Emitted when the user wants to insert the source region into the timeline.
    void insertRequested(SourceRegion region);

    /// Emitted when the user wants to overwrite the source region into the timeline.
    void overwriteRequested(SourceRegion region);

    /// Emitted when the playhead changes in the source monitor.
    void playheadChanged(int64_t tick);

    /// Emitted when in/out points change.
    void inOutChanged();

    /// Emitted when a clip is dropped but no pool is set (MainWindow should handle).
    void dropReceived(uint64_t mediaHandle);

    /// Emitted when a sequence is dropped and should be loaded for preview.
    void sequenceDropReceived(size_t sequenceIndex);

    /// Emitted when a spine character animation is dropped from the
    /// CharactersPanel. The URI is the raw tree-item payload, e.g.
    /// "spine:CharName|outfit|stanceInt|animName". TimelineWorkspace
    /// parses it, builds a temp SpineClip, and routes to loadSpineClip()
    /// (the source monitor itself doesn't own a CompositeService).
    void spineDropReceived(const QString& spineUri);

    /// Emitted when source audio playback starts (for mutual exclusion with timeline).
    void playbackStarted();

    /// Emitted when the user scrubs the source monitor's mini-timeline,
    /// so the timeline can halt its own playback for mutual exclusion.
    void scrubbed();

public:
    /// Set the MediaPool reference for drag-drop loading.
    void setMediaPool(MediaPool* pool) noexcept { m_pool = pool; }

    /// Set the shared MediaSourceService for frame access.
    void setMediaSourceService(MediaSourceService* service) noexcept { m_mediaSources = service; }

    /// Inject the Project so previewed sequences that themselves contain
    /// nested SequenceClips on audio tracks can be expanded for playback.
    void setSequenceProject(Project* project);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onScrub(int64_t tick);
    void onPollTimer();

private:
    void resizeEvent(QResizeEvent* event) override;
    void setupUI();
    void updateFrameDisplay();
    void updateTimecodeDisplay();
    void exportViewportFrame();
    void loadWaveformAsync();
    bool ensureSourceAudioLoaded();
    void requestSourceAudioLoadAsync();
    void loadSourceAudio();
    void startSourceAudio();
    void stopSourceAudio();
    bool ensureScrubAudioLoaded(int64_t frame, int64_t durationFrames);
    void scrubAudioAt(int64_t tick);
    void updateWaveformSelection();

    // Sequence-preview audio (uses an AudioPlaybackService pointed at the
    // inner sequence's Timeline, mirroring how the main timeline plays).
    void startSequenceAudio();
    void stopSequenceAudio();
    Timeline* resolveSequenceTimeline() const;

    // Premiere-style source-monitor drag-out. Both = video+audio (the
    // viewport drag); VideoOnly / AudioOnly = the dedicated drag buttons.
    enum class SourceDragMode { Both, VideoOnly, AudioOnly };
    void startSourceDrag(SourceDragMode mode);

    /// Enable/disable + fade the drag-video / drag-audio buttons to match
    /// what the loaded source actually has (e.g. no audio button for a
    /// still image; no video button for a .wav).
    void refreshDragButtons(bool hasVideo, bool hasAudio);

    // Widgets
    QLabel*           m_clipLabel{nullptr};
    Viewport*         m_viewport{nullptr};
    WaveformDisplayWidget* m_waveformWidget{nullptr};
    QStackedLayout*   m_viewStack{nullptr};
    MiniTimeline*     m_miniTimeline{nullptr};
    QLabel*           m_timecodeLabel{nullptr};
    QLineEdit*        m_timecodeEdit{nullptr};
    QLabel*           m_durationLabel{nullptr};
    QLabel*           m_zoomLabel{nullptr};
    QLabel*           m_shuttleSpeedLabel{nullptr};
    TransportButton*  m_btnGoStart{nullptr};
    TransportButton*  m_btnStepBack{nullptr};
    TransportButton*  m_btnPlayPause{nullptr};
    TransportButton*  m_btnStop{nullptr};
    TransportButton*  m_btnStepForward{nullptr};
    TransportButton*  m_btnGoEnd{nullptr};
    TransportButton*  m_btnScreenshot{nullptr};
    QPushButton*      m_btnLoop{nullptr};
    QPushButton*      m_btnExportFrame{nullptr};
    QComboBox*        m_playbackResCombo{nullptr};
    QComboBox*        m_fitModeCombo{nullptr};
    QPushButton*      m_btnSafeArea{nullptr};
    QPushButton*      m_btnDragVideo{nullptr};
    QPushButton*      m_btnDragAudio{nullptr};
    QPoint            m_dragBtnStartPos;

    QTimer*           m_pollTimer{nullptr};

    std::atomic<bool> m_destroying{false};

    // Scrub coalescing — store the latest pending scrub tick and only
    // decode it on the next poll tick.  Without this, every mouse-move
    // event during a drag-scrub forces a precise seek + inline decode of
    // a fresh frame; on long-GOP H.264 sources that's 100-300ms each and
    // the UI feels molasses.  Mirrors ProgramMonitor's pattern.
    bool              m_scrubPending{false};
    int64_t           m_pendingScrubTick{0};

    // Internal transport & media
    std::unique_ptr<PlaybackController> m_controller;
    AudioEngine*      m_audioEngine{nullptr};
    MediaPool*        m_pool{nullptr};
    MediaSourceService* m_mediaSources{nullptr};
    uint64_t          m_mediaHandle{0};
    bool              m_hasClip{false};
    bool              m_audioOnly{false};
    int64_t           m_frameCount{0};
    int64_t           m_clipDuration{0};
    double            m_fps{24.0};

    // Decoded audio buffer for playback
    std::shared_ptr<std::vector<float>> m_audioSamples;
    uint32_t          m_audioChannels{0};
    bool              m_audioLoadFailed{false};
    bool              m_audioLoadInFlight{false};
    uint64_t          m_audioLoadGeneration{0};
    uint64_t          m_waveformLoadGeneration{0};

    std::shared_ptr<std::vector<float>> m_scrubAudioSamples;
    uint32_t          m_scrubAudioChannels{0};
    int64_t           m_scrubAudioStartFrame{0};
    int               m_scrubSettleCounter{0}; ///< Post-scrub poll countdown
    bool              m_scrubQualityActive{false}; ///< Use reduced quality while drag-scrubbing

    // Audio engine state management
    AVSyncClock*      m_savedSyncClock{nullptr};
    bool              m_sourceAudioActive{false};

    // Drag-out support (drag from viewport to timeline)
    QPoint            m_dragStartPos;

    // Sequence preview state
    bool              m_isSequence{false};
    size_t            m_sequenceIndex{0};
    uint32_t          m_sequenceWidth{1920};
    uint32_t          m_sequenceHeight{1080};
    bool              m_seqHasAudio{false};
    SequenceFrameProvider m_seqFrameProvider;
    SequenceTimelineGetter m_seqTimelineGetter;
    std::unique_ptr<AudioPlaybackService> m_seqAudioPlayback;
    bool              m_seqAudioWindowRefreshScheduled{false};
};

} // namespace rt
