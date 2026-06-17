/*
 * TimelinePanel — Main timeline container widget.
 *
 * Step 12: Composes ruler, track area, track headers, scrollbars,
 * and playhead into the NLE timeline UI.
 * Step 13: Extended with editing tools, selection, snapping, shortcuts.
 */

#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <QSplitter>
#include <QRubberBand>
#include <QPointer>
#include <QPushButton>
#include <QPixmap>

#include "timeline/TimelineLayoutEngine.h"
#include "timeline/EditOperations.h"
#include "timeline/Transition.h"  // for DragClipState::originalTransitions
#include "widgets/TimelineTrackWidget.h"  // for EditPointSide enum used below

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QTimer;
class QImage;

namespace rt {

class Timeline;
class TimelineRuler;
class TimelineTrackWidget;
class AnimationVideoCache;
class NLEScrollBar;
class Track;
class CommandStack;
class ShortcutManager;
class GhostTrackOverlay;
class MediaPool;
class CompositeService;

/// Track header widget (name, V/A icon, lock/mute/solo, height handle).
class TrackHeader : public QWidget
{
    Q_OBJECT

public:
    explicit TrackHeader(QWidget* parent = nullptr);
    ~TrackHeader() override;

    void setTrack(const Track* track, size_t index);
    void setHeight(float h);

    static constexpr int kHeaderWidth = 160;

    QSize sizeHint() const override;

signals:
    void lockToggled(size_t trackIndex, bool locked);
    void muteToggled(size_t trackIndex, bool muted);
    void soloToggled(size_t trackIndex, bool soloed);
    void targetToggled(size_t trackIndex, bool targeted);
    void collapseToggled(size_t trackIndex, bool collapsed);
    void syncLockToggled(size_t trackIndex, bool syncLocked);
    void heightChanged(size_t trackIndex, float newHeight);
    void trackRenamed(size_t trackIndex, const QString& newName);
    void addTrackRequested(bool video, bool above, size_t nearIndex);
    void addDividerRequested(bool above, size_t nearIndex);
    void deleteTrackRequested(size_t trackIndex);
    void trackSizePresetRequested(size_t trackIndex, float height);
    // Drag-to-reorder signals
    void reorderDragStarted(size_t trackIndex);
    void reorderDragMoved(size_t trackIndex, const QPoint& globalPos);
    void reorderDragFinished(size_t trackIndex, const QPoint& globalPos, bool commit);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    const Track* m_track{nullptr};
    size_t       m_trackIndex{0};
    float        m_height{80.0f};

    // Track height resize drag state
    bool  m_resizeDragging{false};
    int   m_resizeDragStartY{0};
    float m_resizeDragStartHeight{0.0f};
    static constexpr int kResizeGripHeight = 5; // pixels from bottom edge
    static constexpr float kMinTrackHeight = 30.0f;
    static constexpr float kMaxTrackHeight = 300.0f;

    // Reorder drag state
    bool  m_reorderPressed{false};
    bool  m_reorderActive{false};
    QPoint m_reorderPressGlobal;

    QRect targetButtonRect() const;
    QRect buttonRect(int index) const;
    int   contentYOffset() const;
    QRect lockButtonRect() const;
    QRect muteButtonRect() const;
    QRect soloButtonRect() const;
    QRect syncLockButtonRect() const;
};

/// Main timeline panel — the NLE editing area.
class TimelinePanel : public QWidget
{
    Q_OBJECT

public:
    /// Source-monitor drag-out mode threaded through the drop signals.
    /// 0 = video+audio (default), 1 = video only, 2 = audio only.
    enum StreamDragMode { DragBoth = 0, DragVideoOnly = 1, DragAudioOnly = 2 };

    /// Represents a selected gap between clips on a track.
    struct GapSelection {
        size_t  trackIndex{SIZE_MAX};
        int64_t startTick{0};
        int64_t endTick{0};
        bool    active{false};
    };

    explicit TimelinePanel(QWidget* parent = nullptr);
    ~TimelinePanel() override;

    /// Set the timeline data model.
    void setTimeline(Timeline* timeline);

    /// Update playhead position and repaint.
    void setPlayheadPosition(int64_t tick);

    /// Get the layout engine (for external queries).
    const TimelineLayoutEngine& layoutEngine() const { return m_layoutEngine; }
    TimelineLayoutEngine& layoutEngine() { return m_layoutEngine; }

    /// Notify that zoom/scroll changed — repaints ruler, scrollbar, and tracks.
    void notifyZoomChanged();

    /// Refresh all track widgets from the data model.
    void rebuildTracks();

    /// Number of track widgets currently laid out (used to detect when the
    /// timeline's track count changed and a full rebuild is required).
    [[nodiscard]] size_t laidOutTrackCount() const { return m_trackWidgets.size(); }

    /// Audio anchor the last media dragMove resolved — the drop handler reads
    /// this so its placement matches the ghost preview exactly (first audio
    /// track index the streams occupy; past the last existing audio track means
    /// the user dragged into new-track space).  SIZE_MAX when no audio.
    [[nodiscard]] size_t ghostDropAudioAnchor() const { return m_ghostDropAudioAnchor; }
    /// Number of discrete audio streams in the source of the last media drag.
    [[nodiscard]] int ghostDropAudioStreams() const { return m_ghostDropAudioStreams; }

    /// Incremental track insertion — creates a header + widget for one new
    /// track without destroying existing widgets.  Avoids the blank flash
    /// that full rebuildTracks() causes.
    void insertTrackWidgetIncremental(size_t trackIndex);

    /// Lightweight refresh — updates waveform/thumbnail caches and repaints
    /// track widgets WITHOUT destroying and recreating them.  Use this for
    /// edits that don't change the number or order of tracks (splits,
    /// deletes, cuts, pastes within existing tracks, razor).
    void refreshTrackContents();

    /// Recompute the Premiere-style render bar from the timeline's static
    /// compositing complexity (§4.6 slice 1) and push it to the ruler.
    /// Cheap; called from the track rebuild/refresh paths.
    void refreshRenderBar();

    /// Drop one clip's cached waveform peaks so loadWaveforms() re-decodes the
    /// (media path, audio-stream ordinal) it now points at. Call when a clip's
    /// chosen audio stream changes so the drawn waveform follows the audible
    /// stream.
    void invalidateClipWaveform(uint64_t clipId);

    /// Ensure a single divider track sits between the video and audio
    /// sections (Premiere-style). Mutates the timeline model; call before
    /// (re)building track widgets.
    void ensureSectionDivider();

    /// Insert a user-placed divider at the given index, pushing an undo
    /// command if a command stack is wired up. Triggers rebuildTracks().
    void addDividerUndoable(size_t insertAt);

    /// Zoom to fit the entire timeline.
    void zoomToFit();

    /// Zoom in centered on playhead (keyboard shortcut Ctrl+=).
    /// Doubles zoom level while keeping playhead in the same screen position.
    void zoomIn();

    /// Zoom out centered on playhead (keyboard shortcut Ctrl+-).
    /// Halves zoom level while keeping playhead in the same screen position.
    void zoomOut();

    /// Set the framerate for timecode display.
    void setFrameRate(double fps);

    /// Current width of the track header column (dynamic, resizable via splitter).
    int headerWidth() const;

    // ── Step 13: Editing ────────────────────────────────────────────────

    /// Set the command stack for undo/redo.
    void setCommandStack(CommandStack* stack);

    /// Set the shortcut manager.
    void setShortcutManager(ShortcutManager* shortcuts);

    /// Set the active editing tool.
    void setActiveTool(EditTool tool);

    /// Get the active editing tool.
    [[nodiscard]] EditTool activeTool() const noexcept { return m_activeTool; }

    /// Get the selection set.
    [[nodiscard]] const SelectionSet& selection() const { return m_selection; }
    [[nodiscard]] SelectionSet& selection() { return m_selection; }

    /// Get the gap selection state.
    [[nodiscard]] const GapSelection& gapSelection() const { return m_gapSelection; }
    [[nodiscard]] GapSelection& gapSelection() { return m_gapSelection; }

    /// Clear the gap selection and update track widgets.
    void clearGapSelection();

    /// Get the selected transition track/index (SIZE_MAX = none selected).
    [[nodiscard]] size_t selectedTransitionTrack() const noexcept { return m_selectedTransitionTrack; }
    [[nodiscard]] size_t selectedTransitionIndex() const noexcept { return m_selectedTransitionIndex; }

    /// Clear the transition selection and update track widgets.
    void clearTransitionSelection();

    /// Clear the Premiere-style edit-point bracket from every track widget.
    /// Public so external actions (e.g. Ctrl+T add-transition) can wipe the
    /// bracket the instant the edit completes instead of waiting for the
    /// next mouse press.
    void clearEditPointSelection();

    /// Get the snap engine.
    [[nodiscard]] const SnapEngine& snapEngine() const { return m_snapEngine; }
    [[nodiscard]] SnapEngine& snapEngine() { return m_snapEngine; }

    /// Get the clipboard (const).
    [[nodiscard]] const ClipboardContents& clipboard() const { return m_clipboard; }

    /// Get the clipboard (mutable — for populating from workspace shortcuts).
    [[nodiscard]] ClipboardContents& mutableClipboard() { return m_clipboard; }

    /// Toggle snapping on/off.
    void setSnappingEnabled(bool enabled);

    /// Toggle linked selection (move A/V clips together).
    void setLinkedSelectionEnabled(bool enabled) { m_linkedSelectionEnabled = enabled; }
    [[nodiscard]] bool linkedSelectionEnabled() const noexcept { return m_linkedSelectionEnabled; }

    /// Toggle nest-sequences mode (insert sequences as nested vs individual clips).
    void setNestSequencesEnabled(bool enabled) { m_nestSequencesEnabled = enabled; }
    [[nodiscard]] bool nestSequencesEnabled() const noexcept { return m_nestSequencesEnabled; }

    /// Open the Paste Attributes dialog for the current selection.
    /// Remembers which checkboxes were checked last time (Premiere-style).
    void showPasteAttributesDialog();

    /// Copy attributes from the first selected clip into the attributes clipboard.
    /// Called automatically by Ctrl+C so Paste Attributes is always ready.
    void copyAttributesFromSelection();

    /// Show or hide the caption (Subtitles) track. "Hidden" mutes the track
    /// (so the compositor skips the burned-in overlay) and collapses both its
    /// header and content row in the timeline.
    void setCaptionTrackVisible(bool visible);

    /// Re-apply the caption track's hidden/shown state (caption track muted ⇒
    /// header+widget collapsed). Call after rebuildTracks so it survives.
    void applyCaptionTrackVisibility();

    /// Sync ruler in/out markers from the timeline data model.
    void updateInOutRange();

    /// Set pointer to animation video cache (for cached-clip color override).
    void setAnimVideoCache(const AnimationVideoCache* cache);

    /// Set pointer to media pool (for drag preview duration lookup).
    void setMediaPool(MediaPool* pool) noexcept { m_mediaPool = pool; }

    /// Set pointer to the composite service so the render bar can show which
    /// segments are pre-rendered (green) via the segment cache (§4.6 2d).
    void setCompositeService(CompositeService* svc) noexcept { m_compositeService = svc; }

    /// Last clicked clip edge (for Ctrl+T default transition).
    /// Returns { clipRef, edge, valid } — valid is false if no edge was clicked.
    struct ClickedEdge {
        ClipRef  clipRef;
        ClipEdge edge{ClipEdge::Head};
        bool     valid{false};
    };
    [[nodiscard]] ClickedEdge lastClickedEdge() const { return m_lastClickedEdge; }

signals:
    /// Emitted when user scrubs the playhead.
    void playheadMoved(int64_t tick);

    /// Emitted when a clip is selected.
    void clipSelected(size_t trackIndex, size_t clipIndex);

    /// Emitted when the active tool changes.
    void toolChanged(EditTool tool);

    /// Emitted when selection changes.
    void selectionChanged();

    /// Emitted when timeline content changes (clips moved/added/removed/trimmed).
    void contentChanged();

    /// Emitted when a new clip is created via a tool (e.g., Text tool).
    void clipCreated();

    /// Emitted when a clip context menu action is requested.
    void clipContextAction(const QString& action, size_t trackIndex, uint64_t clipId);

    /// Emitted when a track should be added.
    void addTrackAbove(size_t nearIndex, bool video);
    void addTrackBelow(size_t nearIndex, bool video);
    void deleteTrack(size_t trackIndex);

    /// Emitted when media is dropped from the project bin onto the timeline.
    void mediaDropped(const QString& filePath, uint64_t mediaHandle, int64_t atTick,
                      size_t trackIndex, int dragMode = 0);

    /// Emitted when media is dropped from the Source Monitor with in/out points.
    void mediaDroppedWithRegion(const QString& filePath, uint64_t mediaHandle,
                                int64_t atTick, size_t trackIndex,
                                int64_t sourceIn, int64_t sourceOut,
                                int dragMode = 0);

    /// Emitted when an external file (from Windows Explorer) is dropped onto the timeline.
    void externalFileDropped(const QString& filePath, int64_t atTick, size_t trackIndex);

    /// Emitted when an effect is dropped onto a timeline clip.
    void effectDroppedOnClip(size_t trackIndex, uint64_t clipId, int effectType);

    /// Emitted when a glitch preset (multi-effect macro) is dropped onto a
    /// timeline clip. `presetId` is a GlitchPreset value.
    void glitchPresetDroppedOnClip(size_t trackIndex, uint64_t clipId, int presetId);

    /// Emitted when an audio DSP processor (EQ/Dynamics) is dropped onto a
    /// timeline clip. `kind` is an audiofx::ProcessorKind value.
    void audioFxDroppedOnClip(size_t trackIndex, uint64_t clipId, int kind);

    /// Emitted when a transition is dropped at a clip edge.
    void transitionDroppedAtEdge(size_t trackIndex, uint64_t leftClipId,
                                 uint64_t rightClipId, int64_t editPointTick,
                                 int transitionType);

    /// Emitted when the user clicks on a transition body (not just its edge).
    void transitionSelected(size_t trackIndex, size_t transitionIndex);

    /// Emitted when the user double-clicks a clip (load in Source Monitor).
    void clipDoubleClicked(size_t trackIndex, size_t clipIndex);

    /// Emitted when user chooses "Nest" from the timeline context menu.
    void nestSelectedClips(const std::vector<ClipRef>& clips, const QString& nestName);

    /// Emitted when a sequence is dropped from the project bin onto the timeline.
    /// dragMode: 0 = video+audio, 1 = video only, 2 = audio only.
    void sequenceDropped(size_t sequenceIndex, int64_t atTick, size_t trackIndex,
                         int64_t sourceIn = -1, int64_t sourceOut = -1,
                         int dragMode = 0);

    /// Emitted when an Adjustment Layer is dropped from the project bin.
    /// Always lands on a video track; default 5-second duration.
    void adjustmentDropped(const QString& name, int64_t atTick, size_t trackIndex);

    /// Emitted when user wants to open a nested sequence (double-click or right-click).
    void openNestedSequence(size_t sequenceIndex);

    /// Emitted when user chooses "Reveal in Project" from clip context menu.
    void revealInProjectBin(const QString& filePath);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onRulerScrub(int64_t tick);
    void onScrollChanged();

    /// Recompute and apply the minimum header width based on track names.
    void updateMinHeaderWidth();

private:
    Timeline*             m_timeline{nullptr};
    TimelineLayoutEngine  m_layoutEngine;
    CommandStack*         m_commandStack{nullptr};
    ShortcutManager*      m_shortcuts{nullptr};

    // Use-after-free guard
    std::atomic<bool> m_destroying{false};

    // Child widgets
    TimelineRuler*   m_ruler{nullptr};
    QWidget*         m_trackHeaderArea{nullptr};
    QWidget*         m_trackContentArea{nullptr};
    NLEScrollBar*    m_scrollBar{nullptr};
    QScrollArea*     m_verticalScroll{nullptr};
    QScrollArea*     m_headerScroll{nullptr};
    QSplitter*       m_headerSplitter{nullptr};
    bool             m_splitterInitialized{false};
    QWidget*         m_headerSpacer{nullptr};
    QWidget*         m_scrollSpacer{nullptr};

    std::vector<QPointer<TrackHeader>>          m_trackHeaders;
    std::vector<QPointer<TimelineTrackWidget>>  m_trackWidgets;

    // Playhead overlay — lightweight transparent widget that draws only the
    // playhead line, avoiding full track widget repaints on every tick.
    QWidget* m_playheadOverlay{nullptr};
    int64_t m_playheadTick{0};

    // Snap indicator line (white line shown when a snap target is hit during drag)
    int64_t m_snapIndicatorTick{-1};   // -1 = hidden

    // Razor tool hover indicator
    int64_t m_razorHoverTick{-1};
    size_t  m_razorHoverTrack{SIZE_MAX};

    // Linked selection: when true, clips on V+A tracks move together
    bool m_linkedSelectionEnabled{true};
    bool m_nestSequencesEnabled{false};

    // Waveform peak cache: clipId → vector of peak amplitudes (0..1)
    std::unordered_map<uint64_t, std::vector<float>> m_waveformPeaks;
    // Path-based index — avoids re-decoding when a clip is split (new ID,
    // same source file).  Waveform peaks cover the entire source file so
    // they are identical for every clip that shares the same mediaPath.
    std::unordered_map<std::string, std::vector<float>> m_waveformByPath;
    std::unordered_set<std::string> m_pendingWaveformPaths;
    std::unordered_set<std::string> m_failedWaveformPaths;
    uint64_t m_waveformLoadGeneration{0};

    // Video thumbnail cache: clipId → first-frame thumbnail (QPixmap)
    std::unordered_map<uint64_t, QPixmap> m_thumbnailCache;
    // Same idea: path-based index for thumbnails.
    std::unordered_map<std::string, QPixmap> m_thumbnailByPath;
    // Negative cache: paths that failed to open or produced unusable format
    // (e.g. YUV-only decode, missing file).  Prevents hundreds of repeated
    // VideoDecoder::open() calls (~150ms each for HEVC/NVDEC) on every
    // loadThumbnails() invocation.
    std::unordered_set<std::string> m_failedThumbnailPaths;
    // Thumbnails decode on background threads (mirrors the waveform path) so
    // opening a project with many video clips doesn't freeze the UI thread.
    std::unordered_set<std::string> m_pendingThumbnailPaths;
    uint64_t m_thumbnailLoadGeneration{0};

    // Animation video cache (for Spine clip cached-vs-live color override)
    const AnimationVideoCache* m_animVideoCache{nullptr};
    MediaPool* m_mediaPool{nullptr};
    CompositeService* m_compositeService{nullptr};

    // Step 13: Editing state
    EditTool          m_activeTool{EditTool::Selection};
    SelectionSet      m_selection;
    size_t            m_selectedTransitionTrack{SIZE_MAX};
    size_t            m_selectedTransitionIndex{SIZE_MAX};

    // Gap selection state
    GapSelection      m_gapSelection;

    SnapEngine        m_snapEngine;
    ClipboardContents m_clipboard;

    // Effect clipboard (for Copy Effects / Paste Effects)
    std::unique_ptr<EffectStack> m_effectClipboard;

    // Transform attributes clipboard (for Copy Attributes / Paste Attributes)
    struct AttributesClipboard {
        KeyframeTrack<float> opacity{1.0f};
        KeyframeTrack<float> posX{0.0f};
        KeyframeTrack<float> posY{0.0f};
        KeyframeTrack<float> scaleX{1.0f};
        KeyframeTrack<float> scaleY{1.0f};
        KeyframeTrack<float> rotation{0.0f};
        double speed{1.0};
        KeyframeTrack<float> speedRamp{1.0f};
    };
    std::optional<AttributesClipboard> m_attrClipboard;

    // Persistent paste-attribute checkbox state (Premiere-style: remembers last selection)
    // Bit flags: 0=opacity, 1=posX, 2=posY, 3=scaleX, 4=scaleY, 5=rotation, 6=speed, 7=speedRamp
    uint8_t m_pasteAttrMask{0x3F}; // Transform bits default on; speed/speedramp default off

    // Drag state
    enum class DragMode { None, ClipMove, ClipTrimHead, ClipTrimTail,
                          PendingMarquee, MarqueeSelect, RollingEdit,
                          SlipTool, SlideTool, TransitionTrim,
                          PendingClipClick };
    DragMode  m_dragMode{DragMode::None};
    QPointF   m_dragStart;
    QPointF   m_marqueeEnd;   // Current endpoint during marquee drag
    ClipRef   m_dragClipRef;
    int64_t   m_dragOriginalIn{0};
    int64_t   m_dragOriginalSourceIn{0};
    int64_t   m_dragOriginalDuration{0};
    size_t    m_dragOriginalTrack{0};

    // Rolling edit drag state
    uint64_t  m_rollLeftClipId{0};
    uint64_t  m_rollRightClipId{0};
    size_t    m_rollTrackIndex{0};
    int64_t   m_rollOriginalEditPoint{0};
    // Original clip states for direct-manipulation rolling edit
    int64_t   m_rollLeftOrigIn{0};
    int64_t   m_rollLeftOrigDur{0};
    int64_t   m_rollLeftOrigSrcIn{0};
    int64_t   m_rollRightOrigIn{0};
    int64_t   m_rollRightOrigDur{0};
    int64_t   m_rollRightOrigSrcIn{0};
    // Precomputed live-drag bounds for the seam — mirrors the commit
    // clamp in EditOperations::rollingEdit so the seam stops at the
    // same tick during drag as it lands on at release. Without this,
    // the live preview can slide past the source-media or right-clip-
    // head limits and then snap back on release.
    int64_t   m_rollMinEditPoint{0};
    int64_t   m_rollMaxEditPoint{0};

    // Transition trim drag state
    size_t  m_transTrimTrackIndex{0};
    size_t  m_transTrimIndex{0};       // index in track->transitions()
    bool    m_transTrimIsStart{false}; // dragging the start or end of the transition?
    int64_t m_transTrimOrigDuration{0};
    int64_t m_transTrimOrigEditPoint{0};

    // Last clicked edge (for Ctrl+T transitions) — uses public ClickedEdge struct
    ClickedEdge m_lastClickedEdge;

    // Multi-clip drag state: stores each selected clip's original position
    // and the transitions on its source track that reference it. The
    // live-drag preview moves clips by removeClip()+addClip(), which drops
    // transitions referencing the moved clip on the source track. We
    // capture them at drag-start so the mouse-release restore pass can
    // put them back on the original track before the actual move command
    // runs (otherwise moveClipToTrack has no transitions to carry over).
    struct DragClipState {
        ClipRef ref;
        int64_t originalIn{0};
        int64_t originalDuration{0};
        int64_t originalSourceIn{0};
        size_t  originalTrack{0};
        std::vector<Transition> originalTransitions; // refs originalTrack, ref.clipId
    };
    std::vector<DragClipState> m_dragSelectedClips;
    size_t m_dragTargetTrack{SIZE_MAX};  // track under cursor during drag

    // ── Ghost track (drag-to-create new track, Premiere Pro style) ──
    bool   m_ghostTrackVisible{false};
    bool   m_ghostTrackIsAbove{false}; // true = new video track above topmost, false = new audio below
    bool   m_ghostTrackOnExisting{false}; // true = ghost overlay positioned on an existing track (multi-clip drag)
    int    m_ghostTrackY{0};           // Y position of ghost track in panel coords
    int    m_ghostTrackHeight{60};     // height of the ghost track
    // Audio anchor resolved by the LAST dragMove for the current media drag.
    // The drop reuses this exact decision instead of re-deriving routing from
    // coarse above/below booleans — eliminates ghost/drop divergence (the
    // multi-stream "always new tracks" and single-stream "below won't create"
    // bugs).  m_ghostDropAudioAnchor is the first audio track index the streams
    // occupy; if it points PAST the last existing audio track the user dragged
    // into new-track space (so every stream is brand-new).  SIZE_MAX = no audio.
    size_t m_ghostDropAudioAnchor{SIZE_MAX};
    int    m_ghostDropAudioStreams{1}; // discrete audio streams in the dragged source
    GhostTrackOverlay* m_ghostOverlay{nullptr};
    // Second overlay for the companion side of a video+audio drag — so
    // both the video destination AND the audio destination can show
    // individual clip outlines simultaneously during multi-clip drags.
    GhostTrackOverlay* m_ghostOverlayAudio{nullptr};

    // Drag-to-reorder state
    size_t m_reorderSrcIndex{SIZE_MAX};
    void   updateReorderOverlay(const QPoint& globalMousePos);
    size_t computeReorderInsertionIndex(const QPoint& globalMousePos) const;

    // Marquee rubber-band overlay (draws ON TOP of children)
    QRubberBand* m_rubberBand{nullptr};

    // Marquee state: prior selection captured at marquee start (for
    // Shift+marquee additive selection — Premiere-style "add to set").
    std::vector<ClipRef> m_marqueeBaseSelection;
    // Auto-scroll while marquee drag pegs against left/right edge of
    // the track area.  Repaints + scrolls every tick for as long as
    // the cursor stays in the edge zone, like Premiere Pro.
    QTimer*  m_marqueeScrollTimer{nullptr};
    QPointF  m_marqueeLastMovePos;

    // Auto-scroll while a clip-move/trim drag pegs against the viewport
    // edge.  Mirrors the marquee timer so the timeline follows the
    // cursor when you're dragging a clip off-screen.
    QTimer*  m_clipDragScrollTimer{nullptr};
    QPointF  m_clipDragLastMovePos;
    void     updateClipDragAutoScroll(const QPointF& pos);

    // Effect drag-drop highlight: clip under cursor during effect drag
    std::optional<ClipRef> m_effectDropTarget;

    // Transition drag-drop state: edge being targeted
    struct TransitionDropTarget {
        size_t  trackIndex{SIZE_MAX};
        int64_t editPointTick{-1};
        uint64_t leftClipId{0};
        uint64_t rightClipId{0};
    };
    std::optional<TransitionDropTarget> m_transitionDropTarget;

    void setupLayout();
    void updateTrackGeometries();
    void paintPlayhead(class QPainter& painter);
    void updatePlayheadOverlay();

    /// Show context menu for a clip.
    void showClipContextMenu(const QPointF& globalPos, const ClipRef& ref);

    /// Show context menu for empty timeline area.
    void showEmptyAreaContextMenu(const QPointF& globalPos, size_t trackIndex);

    /// Ensure at least 1 video and 1 audio track exist (Premiere Pro default).
    void ensureDefaultTracks();

    /// Load waveform peaks for all audio clips.
    void loadWaveforms();
    void queueWaveformLoad(const std::string& path, int audioStreamOrdinal);
    void applyWaveformPeaks(uint64_t generation,
                            const std::string& key,
                            std::vector<float> peaks);

    /// Load video thumbnails for all video clips (decode runs on a background
    /// thread; the resulting image is applied on the UI thread).
    void loadThumbnails();
    void queueThumbnailLoad(const std::string& path);
    void applyThumbnail(uint64_t generation, const std::string& path,
                        const QImage& image);

    // Step 13: Editing helpers
    void executeCommand(std::unique_ptr<Command> cmd);
    std::optional<ClipRef> hitTestClip(const QPointF& pos) const;
    size_t hitTestTrack(double y) const;
    ClipEdge hitTestClipEdge(const QPointF& pos, const ClipRef& ref) const;

    /// Premiere-style adaptive trim-handle grab width, in pixels.
    /// Bigger than the old fixed 6px so edges are easy to grab, but
    /// clamped to a fraction of the clip's on-screen width so the two
    /// edge zones never swallow a short/zoomed-out clip's move region.
    /// Tightened from 11/35% to 8/28% so a body-click doesn't slip into
    /// trim mode near the edges — accidental trims were the single
    /// biggest "feel" complaint vs Premiere.
    /// @param clipPixelWidth  on-screen width of the clip in pixels.
    [[nodiscard]] static double edgeGrabPx(double clipPixelWidth) noexcept
    {
        constexpr double kBase = 8.0;    // comfortable default handle
        constexpr double kMin  = 4.0;    // floor for very thin clips
        const double cap = clipPixelWidth * 0.28;  // keep a central move zone
        double v = kBase < cap ? kBase : cap;
        return v < kMin ? kMin : v;
    }
    void updateCursorForTool();
    void wireShortcuts();

    /// Premiere-style edit-point selection. Setting on a specific track
    /// paints brackets at the given tick. The `side` parameter controls
    /// whether both brackets (touching seam) or a single bracket (outer
    /// edge of an isolated clip) is drawn. Clearing wipes the visual
    /// from every track. Cheap: just updates per-track ticks.
    void setEditPointSelection(size_t trackIndex, int64_t tick,
                               EditPointSide side = EditPointSide::Both);

    /// Update snap indicator line across all track widgets and ruler.
    void setSnapIndicator(int64_t tick);

    /// Apply Premiere-style A/V link selection: clips sharing a non-zero
    /// linkId with `seed` are added to (or removed from) the selection
    /// alongside it. No-op when the seed has no link. Used so clicking a
    /// linked video also picks its companion audio (and vice versa), and
    /// shift-toggling carries the partner along with the toggled clip.
    void setLinkPartnersSelected(const ClipRef& seed, bool selected);

    /// Recompute marquee selection from current m_dragStart/m_marqueeLastMovePos
    /// (additively merging in m_marqueeBaseSelection if Shift+marquee).
    void refreshMarqueeSelection();
};

} // namespace rt
