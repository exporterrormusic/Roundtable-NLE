/*
 * TimelineWorkspace — Dockable NLE workspace (the TIMELINE tab).
 *
 * Layout modeled after Adobe Premiere Pro 2024 default workspace.
 * Uses a nested QMainWindow with QDockWidgets so every panel is
 * individually dockable, rearrangeable, tabbable, and floatable
 * — exactly like Premiere Pro's panel system.
 *
 * Default layout:
 * ┌─────────────────────────────────────────────────────────────────┐
 * │  ┌──────────┬──────────┬──────────┬──────────────────┐        │
 * │  │ Project  │ Source   │ Program  │ Effect Controls  │  DOCK  │
 * │  │ Bin      │ Monitor  │ Monitor  │ Effects/Keyframes│  AREA  │
 * │  └──────────┴──────────┴──────────┴──────────────────┘        │
 * │  ┌─────┬─────────────────────────────────────────┬─────┐      │
 * │  │Tool │  Sequence 1 toolbar   [▣ Snap] [Zoom]   │     │      │
 * │  │Col  ├─────────────────────────────────────────┤ VU  │ CENT │
 * │  │ ⬆   │  Timeline Panel (ruler+tracks+scrollbar)│Meter│ RAL  │
 * │  │ ✂   │                                         │     │      │
 * │  │ ⇆   │                                         │     │      │
 * │  └─────┴─────────────────────────────────────────┴─────┘      │
 * └─────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <QShortcut>
#include <QTimer>
#include <QWidget>
#include <QSplitter>
#include <QMainWindow>
#include <QKeyEvent>
#include <QMap>
#include <QString>

#include "panels/timeline/SelectionState.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>



class QDockWidget;
class QLabel;
class QSettings;
class QTabBar;
class QToolButton;

namespace rt {

enum class ResolutionTier : uint8_t;

// Forward declarations — panels
class CaptionsPanel;
class TierListPanel;
class CharactersPanel;
class LibraryPanel;
class CommandStack;
class EffectControlsPanel;
class EffectsPanel;
class GraphicsEditorPanel;
class HistoryPanel;
class KeyframeEditor;
class ColorGradingPanel;
class PropertiesPanel;
class ProgramMonitor;
class ProjectBin;
class ScopesPanel;
class ShortcutManager;
class SourceMonitor;
class TimelinePanel;
class AudioSampleProvider;
class AudioEngine;
class MediaPool;
class MediaSourceService;
class ModelManager;
class AudioPlaybackService;
class AnimationVideoCache;
class Clip;
class CompositeService;
struct OpacityMask;
class DockLayoutManager;
class DropController;
class MediaWatchController;
class OverlayController;
class PanelMaximizeController;
class PlaybackController;
class ShortcutController;
class ShotPresetManager;
class SpineClip;
class TitleClip;
class Timeline;
class VUMeter;
class Project;

/// The TIMELINE page — splitter-based NLE workspace matching original layout.
class TimelineWorkspace : public QWidget
{
    Q_OBJECT

    // God-class decomposition (§3.1 friend-narrowing — COMPLETE): the four
    // binder controllers (Project / Drop / Overlay / Shortcut) were all
    // narrowed off friend access; each reaches the workspace only through
    // the public binder-accessor block below.  No friend classes remain.

public:
    explicit TimelineWorkspace(QWidget* parent = nullptr);
    ~TimelineWorkspace() override;

    // ── Dependency injection ────────────────────────────────────────────
    // Trivial pointer assignments — noexcept for better compiler optimisations.
    void setTimeline(Timeline* timeline);
    void setCommandStack(CommandStack* stack);
    void setShortcutManager(ShortcutManager* mgr);
    void setAudioEngine(AudioEngine* engine);
    void setPlaybackController(PlaybackController* ctrl);
    void setMediaPool(MediaPool* pool);
    void setMediaSourceService(MediaSourceService* service);
    void setModelManager(ModelManager* mgr);
    void setShotPresetManager(ShotPresetManager* mgr);
    void setProject(Project* project);

    // ── Build ───────────────────────────────────────────────────────────
    void buildPanels();
    void wirePanelSignals();
    void wireClipSelectionSignals();
    // Sub-groups of wireClipSelectionSignals(), split across sibling .cpp files:
    void wireViewportTransformSignals();   // shim → OverlayController
    void wireTransformOverlaySignals();    // shim → OverlayController
    void wireOverlayToolSignals();         // shim → OverlayController
    void wireTimelineContentSignals();     // TimelineWorkspaceWiringViewport.cpp
    void wirePanelFeedbackSignals();       // TimelineWorkspaceWiringPanels.cpp
    void wireMediaDropSignals();           // shim → DropController
    void wireNestSignals();                // shim → DropController
    void wireEffectDropSignals();          // shim → DropController
    void wireTrackSignals();

    /// Rebuild the sequence tab bar from the current project.
    void refreshSequenceTabs();

    /// Mark a sequence as open in the tab bar (Premiere Pro style).
    /// Does nothing if already open.
    void openSequenceTab(size_t index);

    /// Insert a nested sequence clip at the playhead on the first targeted video track.
    void nestSequence(size_t sequenceIndex, const QString& sequenceName);

    // ── Panel accessors ─────────────────────────────────────────────────
    [[nodiscard]] TimelinePanel*    timelinePanel()    const noexcept { return m_timelinePanel; }
    [[nodiscard]] SourceMonitor*    sourceMonitor()    const noexcept { return m_sourceMonitor; }
    [[nodiscard]] ProgramMonitor*   programMonitor()   const noexcept { return m_programMonitor; }

    // ── Transport target (which monitor Space / JKL drive) ───────────────
    // Sticky selection, set on explicit clicks/loads (NOT hover-focus), so
    // opening a clip in the Source Monitor keeps it the transport target even
    // as the cursor hovers other panels.  See activeController() in
    // TimelineWorkspaceKeys.cpp.
    /// Select which monitor owns transport. Switching contexts pauses the
    /// other controller and restores the selected context's audio sources so
    /// the shared AudioEngine can never be driven by both at once.
    void setSourceTransportActive(bool active);
    [[nodiscard]] bool sourceTransportActive() const noexcept { return m_sourceTransportActive; }
    [[nodiscard]] ProjectBin*       projectBin()       const noexcept { return m_projectBin; }
    [[nodiscard]] Project*          project()          const noexcept { return m_project; }
    [[nodiscard]] PropertiesPanel*  propertiesPanel()  const noexcept { return m_propertiesPanel; }
    [[nodiscard]] EffectControlsPanel* effectControlsPanel() const noexcept { return m_effectControlsPanel; }
    [[nodiscard]] EffectsPanel*     effectsPanel()     const noexcept { return m_effectsPanel; }
    [[nodiscard]] KeyframeEditor*   keyframeEditor()   const noexcept { return m_keyframeEditor; }
    [[nodiscard]] HistoryPanel*     historyPanel()     const noexcept { return m_historyPanel; }
    [[nodiscard]] ScopesPanel*      scopesPanel()      const noexcept { return m_scopesPanel; }
    [[nodiscard]] CharactersPanel*   charactersPanel()   const noexcept { return m_charactersPanel; }
    [[nodiscard]] LibraryPanel*      libraryPanel()      const noexcept { return m_libraryPanel; }

    // ── Binder/controller accessors (god-class decomposition, §3.1) ─────
    // Public surface the extracted binder controllers (DropController, ...)
    // use in place of friend back-pointer access while their dependency
    // surfaces are narrowed off the workspace.
    [[nodiscard]] Timeline*            timeline()            const noexcept { return m_timeline; }
    [[nodiscard]] CommandStack*        commandStack()        const noexcept { return m_commandStack; }
    [[nodiscard]] MediaPool*           mediaPool()           const noexcept { return m_mediaPool; }
    [[nodiscard]] PlaybackController*  playbackController()  const noexcept { return m_playbackController; }
    [[nodiscard]] ShotPresetManager*   shotPresetManager()   const noexcept { return m_shotPresetManager; }
    [[nodiscard]] GraphicsEditorPanel* graphicsEditorPanel() const noexcept { return m_GraphicsEditorPanel; }
    [[nodiscard]] ColorGradingPanel*   colorGradingPanel()   const noexcept { return m_ColorGradingPanel; }
    [[nodiscard]] CaptionsPanel*       captionsPanel()       const noexcept { return m_captionsPanel; }
    [[nodiscard]] TierListPanel*       tierListPanel()       const noexcept { return m_tierListPanel; }
    [[nodiscard]] SelectionState&       selection()       noexcept { return m_selection; }
    [[nodiscard]] const SelectionState& selection() const noexcept { return m_selection; }
    /// The 9 edit-tool buttons (Selection..Pen Mask), for keyboard-shortcut
    /// focus-policy / active-tool sync.  Returns a reference to the array.
    [[nodiscard]] QToolButton* (&toolButtons() noexcept)[9] { return m_toolButtons; }
    /// True once destruction has begun (lambdas/callbacks must bail).
    [[nodiscard]] bool isDestroying() const noexcept { return m_destroying.load(std::memory_order_acquire); }

    /// Flush the composite result LRU cache (call when transforms change).
    void invalidateCompositeCache();
    /// A3: drop only LRU entries whose tick is in [fromTick, toTick].
    /// Edit commands that affect a known time slice (trim, split, ripple
    /// of a single clip) should call this instead of the full-flush form,
    /// to keep cached frames outside the affected range alive across
    /// stepping/scrubbing — a major Premiere-vs-current responsiveness gap.
    void invalidateCompositeCacheRange(int64_t fromTick, int64_t toTick);
    /// Warm the audio decode cache asynchronously (post-edit work).
    void warmAudioCacheAsync();
    /// Sync the ProgramMonitor MiniTimeline in/out from the Timeline.
    void syncProgramMonitorInOut();
    /// Rebuild the Program Monitor transform overlay (shim → OverlayController).
    void updateTransformOverlay();
    /// Deferred overlay re-sync via QTimer (shim → OverlayController).
    void scheduleOverlayRefresh();
    /// Deferred post-edit audio/spine warm-up (split/delete/paste).
    void schedulePostEditWork();

    /// Get the animation video cache (may be nullptr if Spine disabled).
    [[nodiscard]] const AnimationVideoCache* animVideoCache() const noexcept;

    /// Get the animation video cache (non-const, for queueing renders).
    [[nodiscard]] AnimationVideoCache* animVideoCacheMutable() noexcept;

    /// Invalidate cached audio sources so they are reloaded on next play.
    void invalidateAudioSources();

    /// Ensure all audio sources are loaded (blocking decode on cache miss).
    void ensureAudioSourcesLoaded();

    /// Direct access to the audio playback service.
    [[nodiscard]] AudioPlaybackService* audioPlayback() const noexcept { return m_audioPlayback.get(); }

    /// Call after undo/redo to refresh composite cache and transform overlay.
    void refreshAfterUndoRedo();

    /// A media file's bytes changed on disk (e.g. an edited Color Matte).
    /// Forces MediaPool to re-decode it, drops the compositor's cached
    /// handle for that path, flushes the composite cache, and refreshes
    /// the program monitor so every timeline instance updates live.
    void refreshChangedMedia(const std::filesystem::path& path);

    /// Backward compat — returns the QDockWidget wrapping the named panel.
    [[nodiscard]] QDockWidget* dockForPanel(const QString& panelName) const;

    /// Number of dock widgets.
    [[nodiscard]] int dockCount() const noexcept { return m_dockWidgets.size(); }

    /// Access all dock widgets (for building Window menu).
    [[nodiscard]] const QMap<QString, QDockWidget*>& dockWidgets() const noexcept { return m_dockWidgets; }

    /// Composite a single frame at the given tick (used by ProgramMonitor and ExportPanel preview).
    std::shared_ptr<struct CachedFrame> compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                       bool scrubMode = false,
                                                       bool stillMode = false);

    /// Prompt for a destination and export the Program Monitor's current
    /// playhead frame at the active sequence's full resolution.
    void exportCurrentFrame();

    /// Composite using an explicit monitor-specific decode tier.
    std::shared_ptr<struct CachedFrame> compositeFrameAtTier(
        int64_t tick, uint32_t outW, uint32_t outH, bool scrubMode,
        bool stillMode, ResolutionTier tier);

    /// Phase 4.2 — export 16F passthrough (see CompositeService::tryBuild16f-
    /// Passthrough).  Returns a dual-payload (RGBA16F + 8-bit BGRA) frame when
    /// the tick is a single opaque 1:1 >8-bit clip, else nullptr (caller falls
    /// back to compositeFrame).  Used only by the ExportPanel preview callback.
    std::shared_ptr<struct CachedFrame> compositeFrame16f(int64_t tick,
                                                          uint32_t outW, uint32_t outH);

    /// Re-probe the segment cache and repaint the timeline render bar.  Call
    /// after anything that changes cache state behind the timeline's back —
    /// e.g. returning to the Timeline page after an export populated it.
    void refreshRenderBar();

    /// §4.6 slice 3 export write-through: store a finished EXPORT frame (full-
    /// res, pixels present) into the segment cache so a first export populates
    /// it (re-exports reuse).  Forwards to CompositeService::cacheExportFrame.
    void cacheExportFrame(int64_t tick,
                          const std::shared_ptr<struct CachedFrame>& frame);

    /// §4.6 2d: "Render In to Out" — pre-render the marked in/out range into
    /// the segment cache at the current playback composite size, then enable
    /// the cache-read consult so playback replays the green segment.  Stops
    /// playback first (mirrors export).
    /// Returns: >=0 frames newly rendered; -1 no valid in/out range; -2 no
    /// composite service / zero output size.  Caller shows the user feedback.
    int renderInToOut();

    /// Force Full resolution for ExportPanel preview/export frames (wraps CompositeService).
    void setForceFullResolution(bool force);

    /// Alpha export (Phase 4.2): straight-alpha transparent composite output for
    /// ProRes-4444 / PNG export.  Forwards to CompositeService::setExportAlpha.
    void setExportAlpha(bool keep);

    /// Set in/out point at current playhead (called from Timeline menu).
    void setInPoint();
    /// Set out point at current playhead (called from Timeline menu).
    void setOutPoint();
    /// Clear in/out points (called from Timeline menu).
    void clearInOut();

signals:
    /// Emitted when the user clicks a sequence tab.
    void sequenceTabChanged(size_t index);

    /// Emitted when the user closes a sequence tab.
    void sequenceTabClosed(size_t index);

    /// Emitted when the user requests renaming a sequence tab.
    void sequenceTabRenameRequested(size_t index);

    /// Emitted when the user requests duplicating a sequence.
    void sequenceTabDuplicateRequested(size_t index);

    /// Emitted when the user requests sequence settings (resolution, fps, name).
    void sequenceTabSettingsRequested(size_t index);

    /// Emitted when the user drags media onto the timeline but no project
    /// or sequence exists yet.  The receiver should create a project with
    /// a sequence matching the file's properties, then the drop can be retried.
    void requestNewProjectForMedia(const QString& filePath, int64_t atTick,
                                   size_t trackIndex);

    /// Emitted when a default project was auto-created (e.g. user created a
    /// sequence with no project open). The receiver should call setCurrentProject
    /// to take ownership.
    void autoProjectCreated(class Project* project);

    /// Emitted on the UI thread as the background media warmup pass opens each
    /// handle (`done` of `total`).  MainWindow drives the loading progress bar
    /// from this so the user sees real movement during a large open.
    void backgroundWarmupProgress(int done, int total);

    /// Emitted on the UI thread when the background media warmup pass
    /// (preOpenVideoMedia) finishes and no warmup threads remain active.
    /// MainWindow uses this to drop the project-loading input lock once the
    /// timeline is fully populated and safe to interact with.
    void backgroundWarmupFinished();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    // Dependencies (non-owning)
    Timeline*        m_timeline{nullptr};
    CommandStack*    m_commandStack{nullptr};
    ShortcutManager* m_shortcutManager{nullptr};
    AudioEngine*     m_audioEngine{nullptr};
    PlaybackController* m_playbackController{nullptr};
    MediaPool*     m_mediaPool{nullptr};
    MediaSourceService* m_mediaSourceService{nullptr};
    ModelManager*  m_modelManager{nullptr};
    ShotPresetManager* m_shotPresetManager{nullptr};
    Project*       m_project{nullptr};

    // ── Live media file-swap watcher ────────────────────────────────────
    // Watches every media file the timeline references so overwriting one
    // in Windows Explorer hot-reloads it into the project, Premiere-style.
    // The watcher/debounce/poll machinery lives in MediaWatchController;
    // the reload reaction is refreshChangedMedia() above.
    std::unique_ptr<MediaWatchController> m_mediaWatch;

    // Composite service (GPU compositing + spine rendering)
    std::unique_ptr<CompositeService> m_compositeService;
    // Guards re-entrant renderInToOut() (its processEvents pump could redispatch
    // the Ctrl+Shift+R action mid-render).
    bool m_renderingInToOut{false};

    // Panels (owned by splitter hierarchy)
    TimelinePanel*    m_timelinePanel{nullptr};
    SourceMonitor*    m_sourceMonitor{nullptr};
    ProgramMonitor*   m_programMonitor{nullptr};

    // True when the Source Monitor is the transport target (Space/JKL drive
    // it instead of the timeline).  Updated on explicit user activation.
    bool              m_sourceTransportActive{false};
    ProjectBin*       m_projectBin{nullptr};
    PropertiesPanel*  m_propertiesPanel{nullptr};
    EffectControlsPanel* m_effectControlsPanel{nullptr};
    GraphicsEditorPanel* m_GraphicsEditorPanel{nullptr};
    CaptionsPanel*    m_captionsPanel{nullptr};
    TierListPanel*    m_tierListPanel{nullptr};
    ColorGradingPanel*     m_ColorGradingPanel{nullptr};
    EffectsPanel*     m_effectsPanel{nullptr};
    KeyframeEditor*   m_keyframeEditor{nullptr};
    HistoryPanel*     m_historyPanel{nullptr};
    ScopesPanel*      m_scopesPanel{nullptr};
    CharactersPanel*   m_charactersPanel{nullptr};
    LibraryPanel*      m_libraryPanel{nullptr};

    // Audio meter (right side of timeline, Premiere Pro style)
    VUMeter*          m_timelineVUMeter{nullptr};

    // Timecode display in timeline toolbar
    QLabel*           m_timelineTimecode{nullptr};

    // Sequence tab bar (Premiere Pro style — multiple open sequences)
    QTabBar*          m_sequenceTabBar{nullptr};
    bool              m_suppressTabChange{false};

    /// Set of sequence indices currently open as tabs.
    /// When empty, refreshSequenceTabs() seeds it with all sequences.
    std::set<size_t>  m_openSequenceTabs;

    /// Tab-index → sequence-index mapping, rebuilt by refreshSequenceTabs().
    std::vector<size_t> m_tabToSeq;

    // Tool buttons (for sync with keyboard shortcuts)
    QToolButton*      m_toolButtons[9]{};  // Selection, Ripple, Rolling, Razor, Slip, Slide, Text, Zoom, Pen Mask

    // Nested QMainWindow for dock widget support
    QMainWindow*      m_innerMainWindow{nullptr};

    // Outer QSplitter wrapping m_innerMainWindow for full-height edge columns
    QSplitter*        m_edgeSplitter{nullptr};

    // Dock widgets (owned by m_innerMainWindow)
    QMap<QString, QDockWidget*> m_dockWidgets;

    // Legacy splitter pointers (no longer used, kept for compat)
    QSplitter*        m_verticalSplitter{nullptr};
    QSplitter*        m_topSplitter{nullptr};

    bool m_panelsBuilt{false};

    // ── Panel maximize (Premiere Pro tilde key) ─────────────────────────
    // When active, every sibling panel/column is hidden so a single panel
    // fills the workspace; tilde again restores the exact prior layout.
    // All state + logic live in PanelMaximizeController.
    std::unique_ptr<PanelMaximizeController> m_panelMaximize;
public:
    void togglePanelMaximize();
private:

    // VU meter polling timer (feeds AudioEngine::meter() → m_timelineVUMeter)
    QTimer* m_meterTimer{nullptr};

    // ── Audio playback service (decode cache, providers, prefetch) ────
    std::unique_ptr<AudioPlaybackService> m_audioPlayback;

    // Thin wrappers — delegate to m_audioPlayback (keep private so split
    // .cpp files that implement TimelineWorkspace methods can still call).
    void loadAudioSources(bool allowBlockingMisses = true);
    void scheduleAudioPlaybackWindowRefresh();
    void logTimelineAudioPerfSnapshot(const char* reason);
    void relinkMedia(const QString& oldPath, const QString& newPath);
    // warmAudioCacheAsync() moved to the public binder-accessor block above.
    bool m_audioWindowRefreshScheduled{false};

    // Deferred post-edit work — avoids blocking the main thread with audio /
    // spine warm-up during split / delete / paste operations.
    // schedulePostEditWork() is in the public binder-accessor block above
    // (called by ShortcutController).
    bool m_postEditScheduled{false};

    // Video compositing

    /// Pre-open all video/image media handles so first compositeFrame is fast.
    void preOpenVideoMedia();

    /// Upgrade legacy media masks after MediaPool supplies authoritative
    /// dimensions/rotation. Offline sources remain safely in legacy space.
    int migrateDeferredLegacyMediaMasks();

    /// Warm active playback media/GPU resources before the first visible frame.
    void prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

public:
    /// Whether background media warmup (preOpenVideoMedia thread) is still active.
    /// Callers should gate playback start on this being false to avoid
    /// use-after-free crashes during startup (Keyframe<float> iteration in
    /// the compositor can race with timeline population).  MainWindow also
    /// uses it to decide whether to hold the project-open input lock.
    [[nodiscard]] bool isBackgroundWarmupActive() const noexcept { return m_backgroundWarmupActive.load(std::memory_order_acquire); }

private:
    /// Number of background warmup threads still running.
    std::atomic<int> m_backgroundWarmupActive{0};

    /// Destruction guard — checked by lambdas and callbacks.
    std::atomic<bool> m_destroying{false};







    // ── Panel build helpers (extracted from TimelineWorkspacePanels.cpp) ─
    /// Create all dock widgets and panel instances.
    void createPanelWidgets();
    /// Arrange the dock layout (Premiere Pro default arrangement).
    void arrangeDockLayout();
    /// Wire all playback controller signals (scrub, position, state, composite).
    void wirePlaybackSignals();
    /// Register keyboard shortcuts (Home/End, I/O, Ctrl+X/C/V, etc.).
    void registerKeyboardShortcuts();

    // invalidateCompositeCache() / invalidateCompositeCacheRange() moved to
    // the public binder-accessor block above (called by DropController).

#ifdef ROUNDTABLE_HAS_SPINE
    /// Schedule an async spine shared-data load (runs on UI thread via Qt).
    void scheduleSpineSharedLoad(const std::string& charName,
                                 const std::string& outfit,
                                 int stance,
                                 const std::string& assetsDir);
    /// Warm any newly-added SpineClips (called after timeline edits).
    void warmNewSpineClips();
    /// Preload all spine assets visible on the current timeline.
    void preloadSpineAssets();
#endif

    // Current clip/layer selection — drives the Program Monitor transform
    // overlay, properties/effect panels, and edit shortcuts.  See
    // SelectionState.h (extracted so binder controllers can take a
    // SelectionState* instead of the whole workspace).
    SelectionState m_selection;

    size_t m_eyedropperEffectIdx{0};           ///< Effect index pending eyedropper pick
    uint8_t m_savedEditToolBeforeEyedropper{0}; ///< Saved edit tool to restore after pick

    // ── Transform overlay (state + handlers live in OverlayController) ──
    // The drag-session state (group-move snapshots, scale-drag flip
    // capture, inline-text-edit snapshot) and the shared onOverlay*
    // drag/click handlers moved into OverlayController; these shims keep
    // the many intra-workspace call sites unchanged.
    std::unique_ptr<OverlayController> m_overlay;
    // updateTransformOverlay() / scheduleOverlayRefresh() are in the public
    // binder-accessor block above (shims → m_overlay; called by
    // ShortcutController and intra-workspace edit code).

    /// Drag-and-drop binder (media / effect / nest drops); the
    /// wire*DropSignals()/wireNestSignals() methods above are shims to it.
    std::unique_ptr<DropController> m_drop;

    /// Keyboard routing binder (key handling incl. JKL slow-shuttle state,
    /// QShortcut registration).  keyPressEvent/keyReleaseEvent are thin
    /// wrappers over it; registerKeyboardShortcuts() is a shim.
    std::unique_ptr<ShortcutController> m_shortcuts;

    // syncProgramMonitorInOut() is in the public binder-accessor block above
    // (called by ShortcutController).

    /// Apply a shot preset to a clip group, wrapped in an undo command.
    void applyShotSwitch(uint64_t groupId, const std::string& newShotName);






    // ── GPU compositing ─────────────────────────────────────────────────
    // Reusable texture pool for uploading CPU-decoded frames to the GPU.
    // Indexed by layer slot (0..N-1). Textures are reused when dimensions
    // match and recreated when they change.

public:
    void setGpuDisplayMode(bool on);
    [[nodiscard]] bool gpuDisplayMode() const noexcept;

    /// Access the composite service (GPU compositing + spine rendering).
    CompositeService* compositeService() { return m_compositeService.get(); }

    /// Save the dock layout (dock positions, sizes, tab order) into the given QSettings group.
    void saveDockLayout(QSettings& settings);

    /// Restore a previously saved dock layout. Returns true on success.
    bool restoreDockLayout(QSettings& settings);

    /// Reset the dock layout to the built-in default (Premiere Pro style).
    void resetToDefaultDockLayout();

    /// Internal: perform the actual default dock layout reset.
    /// Extracted so resetToDefaultDockLayout() can defer the work when
    /// the widget is not yet visible.
    void doResetToDefaultDockLayout();

    /// Cancel a pending default layout reset.  Called when a saved
    /// workspace is found during project open — the saved layout should
    /// take priority over the USE_AS_DEFAULT preset.
    void cancelPendingDefaultLayoutReset();

    /// Access the dock layout manager.
    [[nodiscard]] DockLayoutManager* dockLayoutManager() const noexcept;

protected:
    void showEvent(QShowEvent* event) override;

private:
    // Dock layout persistence (save / restore / deferred apply)
    std::unique_ptr<DockLayoutManager> m_dockLayoutManager;

    /// The initial programmatic dock state (saved after buildPanels) so
    /// "Reset to Default Layout" can fully recreate the stock arrangement.
    QByteArray m_defaultDockState;

    /// True when resetToDefaultDockLayout() was called while the widget was
    /// hidden — the actual reset is deferred until the next showEvent.
    bool m_pendingDefaultLayoutReset{false};

    /// Install an EdgeColumnGuard on an edge column QMainWindow so that
    /// docks dragged out are reparented to the host and empty columns are
    /// auto-destroyed.  Defined in TimelineWorkspacePanels.cpp where the
    /// guard class lives.
    void installEdgeGuard(QMainWindow* edgeCol);

    /// Apply a callable to each non-null pointer in a variadic pack.
    /// Zero-cost abstraction that eliminates repetitive null-guard boilerplate.
    /// Usage: forEach(m_a, m_b, m_c, [](auto* p){ p->doSomething(); });
    template<typename... Ps, typename F>
    static void forEach(const Ps*... ptrs, F&& fn)
    {
        ((ptrs && fn(*ptrs)), ...);
    }
};

} // namespace rt
