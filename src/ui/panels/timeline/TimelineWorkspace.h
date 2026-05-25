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
class QFileSystemWatcher;
class QLabel;
class QSettings;
class QTabBar;
class QToolButton;

namespace rt {

// Forward declarations — panels
class AudioMixer;
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
class DockLayoutManager;
class PlaybackController;
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
    void wireMediaDropSignals();
    void wireNestSignals();
    void wireEffectDropSignals();
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
    [[nodiscard]] ProjectBin*       projectBin()       const noexcept { return m_projectBin; }
    [[nodiscard]] Project*          project()          const noexcept { return m_project; }
    [[nodiscard]] PropertiesPanel*  propertiesPanel()  const noexcept { return m_propertiesPanel; }
    [[nodiscard]] EffectControlsPanel* effectControlsPanel() const noexcept { return m_effectControlsPanel; }
    [[nodiscard]] EffectsPanel*     effectsPanel()     const noexcept { return m_effectsPanel; }
    [[nodiscard]] AudioMixer*       audioMixer()       const noexcept { return m_audioMixer; }
    [[nodiscard]] KeyframeEditor*   keyframeEditor()   const noexcept { return m_keyframeEditor; }
    [[nodiscard]] HistoryPanel*     historyPanel()     const noexcept { return m_historyPanel; }
    [[nodiscard]] ScopesPanel*      scopesPanel()      const noexcept { return m_scopesPanel; }
    [[nodiscard]] CharactersPanel*   charactersPanel()   const noexcept { return m_charactersPanel; }
    [[nodiscard]] LibraryPanel*      libraryPanel()      const noexcept { return m_libraryPanel; }

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

    /// Rescan the timeline for referenced media files and (re)arm a
    /// QFileSystemWatcher on each. When a watched file is overwritten in
    /// Windows Explorer (e.g. replacing a .png with a new version of the
    /// same name) this live-swaps it into the project/program monitor,
    /// just like Premiere Pro. Cheap and idempotent — safe to call after
    /// any timeline mutation, project load, or media import.
    void rescanMediaWatch();

    /// Backward compat — returns the QDockWidget wrapping the named panel.
    [[nodiscard]] QDockWidget* dockForPanel(const QString& panelName) const;

    /// Number of dock widgets.
    [[nodiscard]] int dockCount() const noexcept { return m_dockWidgets.size(); }

    /// Access all dock widgets (for building Window menu).
    [[nodiscard]] const QMap<QString, QDockWidget*>& dockWidgets() const noexcept { return m_dockWidgets; }

    /// Composite a single frame at the given tick (used by ProgramMonitor and ExportPanel preview).
    std::shared_ptr<struct CachedFrame> compositeFrame(int64_t tick, uint32_t outW, uint32_t outH,
                                                       bool scrubMode = false);

    /// Force Full resolution for ExportPanel preview/export frames (wraps CompositeService).
    void setForceFullResolution(bool force);

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
    // in Windows Explorer (replace a .png with a same-named new version)
    // hot-reloads it into the project, Premiere-style. fileChanged is
    // debounced because editors/Explorer often write in several bursts.
    QFileSystemWatcher* m_mediaWatcher{nullptr};
    QTimer*             m_mediaWatchDebounce{nullptr};
    std::set<std::string> m_mediaWatchPending;   ///< paths awaiting debounced reload
    std::atomic<bool>   m_mediaWatchRescanQueued{false}; ///< coalesces MediaPool open notifications
    /// Single-shot debounce for the MediaPool onMediaOpened → rescan path.
    /// MediaPool opens files constantly (prewarm, lookahead, still-image
    /// live-replace re-opens); rescanning the watcher on every open turned
    /// into a GUI-thread storm (stat ×N on a cloud drive + watcher churn).
    /// Coalesce a burst of opens into ONE rescan ~1.5 s later.
    QTimer*             m_mediaWatchRescanTimer{nullptr};
    /// Last media-path set rescanMediaWatch() actually applied — lets a
    /// (debounced) rescan early-out as a cheap no-op when nothing changed.
    std::set<std::string> m_lastMediaWatchWant;
    /// Per-path (size, mtime) signature so a fileChanged whose content
    /// didn't actually change (Windows delivers spurious/coalesced events
    /// on window restore, attribute touches, etc.) does NOT trigger a
    /// reload+composite-invalidate — that left the Program Monitor blank
    /// on minimize/restore.
    std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> m_mediaWatchSig;

    // Composite service (GPU compositing + spine rendering)
    std::unique_ptr<CompositeService> m_compositeService;

    // ── JKL key state ──────────────────────────────────────────────────
    // Tracks held-key state for Premiere-style K+J / K+L slow shuttle.
    // When K is held and J/L is pressed, we engage a 0.5× scrub that
    // stops the moment J/L is released.
    bool              m_kHeld{false};
    bool              m_kSlowShuttleActive{false};

    // Panels (owned by splitter hierarchy)
    TimelinePanel*    m_timelinePanel{nullptr};
    SourceMonitor*    m_sourceMonitor{nullptr};
    ProgramMonitor*   m_programMonitor{nullptr};
    ProjectBin*       m_projectBin{nullptr};
    PropertiesPanel*  m_propertiesPanel{nullptr};
    EffectControlsPanel* m_effectControlsPanel{nullptr};
    GraphicsEditorPanel* m_GraphicsEditorPanel{nullptr};
    ColorGradingPanel*     m_ColorGradingPanel{nullptr};
    EffectsPanel*     m_effectsPanel{nullptr};
    AudioMixer*       m_audioMixer{nullptr};
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
    QToolButton*      m_toolButtons[8]{};  // Selection, Ripple, Rolling, Razor, Slip, Slide, Text, Zoom

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
    // When active, a single panel is reparented out of the dock system to
    // fill the entire workspace.  Pressing tilde again restores the layout.
    bool m_panelMaximized{false};
    QWidget* m_maximizedWidget{nullptr};       // the panel widget being shown fullscreen
    QDockWidget* m_maximizedDock{nullptr};      // its owning dock (nullptr = central widget)

    /// Snapshot of the inner QMainWindow dock state taken just before the
    /// panel was maximized.  Used by saveDockLayout() so that saving while
    /// maximized writes the pre-maximize arrangement instead of the broken
    /// state where the maximized dock is reparented out and all others hidden.
    QByteArray m_dockStateBeforeMaximize;

    /// Exact visibility snapshot captured at maximize time so restore can
    /// revert to PRECISELY the prior layout (panels the user had closed
    /// stay closed).  No widget is ever reparented — the maximized panel
    /// just gets all the space because its siblings are hidden — so the
    /// program monitor's native Vulkan surface is never destroyed.
    std::unordered_map<QDockWidget*, bool> m_dockVisBeforeMax;
    std::vector<std::pair<QMainWindow*, bool>> m_edgeVisBeforeMax;
    /// saveState() for EVERY dock-hosting window (inner + each edge
    /// column).  Each edge column is its own QMainWindow with its own
    /// internal dock heights; restoring only the inner one left the edge
    /// columns' stacked panels relaying to default proportions.
    std::vector<std::pair<QMainWindow*, QByteArray>> m_dockStatesBeforeMax;
    QByteArray m_edgeSplitterStateBeforeMax;
    QWidget*   m_centralBeforeMax{nullptr};
    bool       m_centralVisBeforeMax{true};
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
    void warmAudioCacheAsync();
    void logTimelineAudioPerfSnapshot(const char* reason);
    bool m_audioWindowRefreshScheduled{false};

    // Deferred post-edit work — avoids blocking the main thread with audio /
    // spine warm-up during split / delete / paste operations.
    void schedulePostEditWork();
    bool m_postEditScheduled{false};

    // Video compositing

    /// Pre-open all video/image media handles so first compositeFrame is fast.
    void preOpenVideoMedia();

    /// Warm active playback media/GPU resources before the first visible frame.
    void prewarmPlaybackResources(int64_t tick, uint32_t outW, uint32_t outH);

    /// Whether background media warmup (preOpenVideoMedia thread) is still active.
    /// Callers should gate playback start on this being false to avoid
    /// use-after-free crashes during startup (Keyframe<float> iteration in
    /// the compositor can race with timeline population).
    [[nodiscard]] bool isBackgroundWarmupActive() const noexcept { return m_backgroundWarmupActive.load(std::memory_order_acquire); }

    /// Number of background warmup threads still running.
    std::atomic<int> m_backgroundWarmupActive{0};

    /// Destruction guard — checked by lambdas and callbacks.
    std::atomic<bool> m_destroying{false};

    /// Re-entrancy guard for the Text tool empty-area click handler.
    /// Prevents duplicate clip creation when the overlay click signal
    /// fires twice on the first interaction after project open.
    std::atomic<bool> m_textToolBusy{false};







    // ── Panel build helpers (extracted from TimelineWorkspacePanels.cpp) ─
    /// Create all dock widgets and panel instances.
    void createPanelWidgets();
    /// Arrange the dock layout (Premiere Pro default arrangement).
    void arrangeDockLayout();
    /// Wire all playback controller signals (scrub, position, state, composite).
    void wirePlaybackSignals();
    /// Register keyboard shortcuts (Home/End, I/O, Ctrl+X/C/V, etc.).
    void registerKeyboardShortcuts();

    /// Flush the composite result LRU cache (call when transforms change).
    void invalidateCompositeCache();

    /// A3: drop only LRU entries whose tick is in [fromTick, toTick].
    /// Edit commands that affect a known time slice (trim, split, ripple
    /// of a single clip) should call this instead of the full-flush form,
    /// to keep cached frames outside the affected range alive across
    /// stepping/scrubbing — a major Premiere-vs-current responsiveness gap.
    void invalidateCompositeCacheRange(int64_t fromTick, int64_t toTick);

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

    // Transform overlay state — tracks the selected clip for Program Monitor
    Clip* m_selectedClip{nullptr};
    size_t m_selectedTrackIdx{0};
    size_t m_selectedClipIdx{0};
    int    m_selectedGraphicLayerIdx{-1};  ///< Selected layer within GraphicClip (-1 = whole clip)

    /// Text snapshot taken when in-place text editing begins. The layer's
    /// text is temporarily cleared during editing so the rendered text
    /// doesn't show through behind the editor box (Premiere Pro behavior).
    /// Restored on cancel, replaced with new text on commit.
    std::string m_preEditOriginalText;
    bool        m_inlineTextEditActive{false};
    uint32_t m_overlayRefreshGen{0};          ///< Generation counter for deferred overlay updates
    size_t m_eyedropperEffectIdx{0};           ///< Effect index pending eyedropper pick
    uint8_t m_savedEditToolBeforeEyedropper{0}; ///< Saved edit tool to restore after pick
    bool   m_scaleDragActive{false};              ///< True while a program-monitor scale drag is in progress
    bool   m_scaleXWasStaticAtDragStart{true};    ///< Saved scaleX static state at drag start
    bool   m_scaleYWasStaticAtDragStart{true};    ///< Saved scaleY static state at drag start
    void updateTransformOverlay();
    void scheduleOverlayRefresh();  ///< Deferred overlay re-sync via QTimer

    /// Reference canvas resolution for GraphicClip layout (text/shape
    /// layer posX/posY space). This is the project/sequence resolution
    /// that renderGraphicClip() composites at — NOT the ProgramMonitor
    /// preview resolution, which can be lower (e.g. a 1920 preview of a
    /// 4K project). The transform overlay, the text-tool click→posX
    /// conversion, and layer hit-testing must all use this so they agree
    /// with where the compositor actually draws the layer.
    void graphicCanvasRes(uint32_t& w, uint32_t& h) const;

    /// Sync ProgramMonitor MiniTimeline in/out points from the Timeline.
    void syncProgramMonitorInOut();

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
