/*
 * ShotComposer — character shot composition panel.
 *
 * Step 18: Shot Composer
 *
 * Ported from the Python ShotPanel (~2000 lines) into a Qt6 panel.
 *
 * Layout (three-column):
 * ┌──────────────────┬───────────────────────┬──────────────────┐
 * │  LIBRARY         │   PREVIEW  (16:9)     │  SHOT NAME       │
 * │                  │                       │  [____________]  │
 * │  [Shots]         │   ┌─────────────────┐ │                  │
 * │  - My Shot 1     │   │                 │ │  LAYERS          │
 * │  - My Shot 2     │   │   (Viewport)    │ │  ┌────────────┐  │
 * │                  │   │   letterboxed   │ │  │ Layer List │  │
 * │  [Characters]    │   └─────────────────┘ │  └────────────┘  │
 * │  - Modernia      │                       │  [+CH] [+BG]     │
 * │  - Rapi          │                       │                  │
 * │  - Dorothy       │                       │  Layer Props     │
 * │                  │                       │  ┌─────────────┐ │
 * │  [Backgrounds]   │                       │  │Transform│CH│C││
 * │  - bg_01.png     │                       │  │ X/Y/Scale   │ │
 * │  - bg_02.png     │                       │  └─────────────┘ │
 * └──────────────────┴───────────────────────┴──────────────────┘
 */

#pragma once

#include "spine/ShotPreset.h"
#include "widgets/ScrubbySpinBox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QStackedWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <deque>
#include <memory>
#include <optional>
#include <map>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QImage>

namespace rt {

class ModelManager;
class SpineEngine;
class SpinePreviewWidget;
class VideoDecoder;
class AnimationVideoCache;

/// Shot Composer — three-panel editor for character shot compositions.
class ShotComposer : public QWidget
{
    Q_OBJECT

public:
    explicit ShotComposer(QWidget* parent = nullptr);
    ~ShotComposer() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the model manager for character library browsing.
    void setModelManager(ModelManager* mgr);

    /// Set the animation video cache (for cache-status indicators).
    void setAnimVideoCache(const AnimationVideoCache* cache);

    /// Set the presets directory and scan for saved presets.
    void setPresetsDirectory(const std::filesystem::path& dir);

    /// Ensure default shots exist for the given character names.
    /// Creates a default shot preset for each character that doesn't already have one.
    void ensureDefaultShotsForCharacters(const QStringList& characters);

    // ── Current shot ────────────────────────────────────────────────────

    /// Get the current shot preset being edited.
    [[nodiscard]] const ShotPreset& currentShot() const noexcept { return m_currentShot; }
    [[nodiscard]] ShotPreset& currentShot() noexcept { return m_currentShot; }

    /// Create a new empty shot (shows name dialog).
    void newShot();

    /// Create a new empty shot with the given name (no dialog).
    void newShot(const QString& name);

    /// Set the current shot for editing.
    void setCurrentShot(const ShotPreset& preset);

    /// Save the current shot to the preset manager.
    bool saveCurrentShot();

    /// Duplicate the current shot with a new name.
    void duplicateCurrentShot();

    /// Clear the character thumbnail cache (call when characters are deleted/updated).
    void clearCharacterThumbCache();

    /// Refresh the character library list widget.
    void refreshCharacterLibrary();

    /// Refresh the shot list and character filter sidebar.
    void refreshShotList();

    // ── Character management ────────────────────────────────────────────

    /// Add a character to the current shot by name.
    /// For video characters, pass the mute/talk video paths.
    int addCharacter(const std::string& characterName,
                     const std::string& videoMutePath = {},
                     const std::string& videoTalkPath = {});

    /// Remove a character from the current shot by index.
    bool removeCharacter(int index);

    /// Get the currently selected layer index (-1 if none).
    [[nodiscard]] int selectedLayerIndex() const noexcept { return m_selectedLayer; }

    /// Select a layer by its index in the layer order.
    void selectLayer(int index);

    // ── Background management ───────────────────────────────────────────

    /// Add a background to the current shot.
    int addBackground(const std::string& path);

    /// Remove a background by index.
    bool removeBackground(int index);

    /// Refresh the background library list from disk (subfolders become groups).
    void refreshBackgroundLibrary();

    // ── Layer ordering ──────────────────────────────────────────────────

    void moveSelectedLayerUp();
    void moveSelectedLayerDown();
    void moveSelectedLayerToFront();
    void moveSelectedLayerToBack();

    // ── Group operations ────────────────────────────────────────────────

    /// Group the selected layers into a new folder.
    void groupSelectedLayers();

    /// Ungroup the currently selected group layer.
    void ungroupSelectedGroup();

    /// Add an empty group at the end of the layer list.
    void addEmptyGroup();

    /// UI-only layer group for organizing layers in the compose panel.
    struct LayerGroupInfo {
        std::string name;
        bool expanded = true;
        int  firstChild = -1;  ///< First layer index in m_layerOrder
        int  lastChild  = -1;  ///< Last layer index in m_layerOrder
    };

    /// Push current state onto the undo stack (clears redo stack).
    void pushUndoState();

    /// Undo to the previous state.
    void undo();

    /// Redo to the next state.
    void redo();

    // ── Size hints ──────────────────────────────────────────────────────
    QSize sizeHint() const override;
    bool  eventFilter(QObject* obj, QEvent* event) override;

    // ── Accessors for testing ───────────────────────────────────────────

    // Library panel
    [[nodiscard]] QTabWidget*   libraryTabs()        const noexcept { return m_libraryTabs; }
    [[nodiscard]] QListWidget*   shotList()            const noexcept { return m_shotList; }
    [[nodiscard]] QComboBox*     shotCombo()           const noexcept { return m_shotSortCombo; }
    [[nodiscard]] QListWidget*  characterLibrary()    const noexcept { return m_characterLibrary; }
    [[nodiscard]] QListWidget*  backgroundLibrary()   const noexcept { return m_backgroundLibrary; }
    [[nodiscard]] QListWidget*  videoLibrary()        const noexcept { return m_videoLibrary; }
    [[nodiscard]] QPushButton*  newShotButton()       const noexcept { return m_newShotBtn; }
    [[nodiscard]] QPushButton*  saveShotButton()      const noexcept { return m_saveShotBtn; }
    [[nodiscard]] QPushButton*  deleteShotButton()    const noexcept { return m_deleteShotBtn; }

    // Preview
    [[nodiscard]] QWidget*      previewArea()         const noexcept { return m_previewArea; }

    // Right panel — shot info
    [[nodiscard]] QLineEdit*    shotNameEdit()        const noexcept { return m_shotNameEdit; }
    [[nodiscard]] QListWidget*  layerList()           const noexcept { return m_layerList; }
    [[nodiscard]] QPushButton*  addCharBtn()          const noexcept { return m_addCharBtn; }
    [[nodiscard]] QPushButton*  addBgBtn()            const noexcept { return m_addBgBtn; }
    [[nodiscard]] QPushButton*  removeLayerBtn()      const noexcept { return m_removeLayerBtn; }
    [[nodiscard]] QPushButton*  addGroupBtn()         const noexcept { return m_addGroupBtn; }
    [[nodiscard]] QPushButton*  layerUpBtn()          const noexcept { return m_layerUpBtn; }
    [[nodiscard]] QPushButton*  layerDownBtn()        const noexcept { return m_layerDownBtn; }

    // Layer properties
    [[nodiscard]] QGroupBox*    charPropsGroup()      const noexcept { return m_charPropsGroup; }
    [[nodiscard]] QGroupBox*    bgPropsGroup()        const noexcept { return m_bgPropsGroup; }
    [[nodiscard]] ScrubbySpinBox* posXSpin()          const noexcept { return m_posXSpin; }
    [[nodiscard]] ScrubbySpinBox* posYSpin()          const noexcept { return m_posYSpin; }
    [[nodiscard]] ScrubbySpinBox* scaleSpin()         const noexcept { return m_scaleSpin; }
    [[nodiscard]] ScrubbySpinBox* rotationSpin()      const noexcept { return m_rotationSpin; }
    [[nodiscard]] ScrubbySpinBox* opacitySpin()       const noexcept { return m_opacitySpin; }
    [[nodiscard]] ScrubbySpinBox* blurSpin()          const noexcept { return m_blurSpin; }
    [[nodiscard]] QComboBox*      outfitCombo()       const noexcept { return m_outfitCombo; }
    [[nodiscard]] QComboBox*      stanceCombo()       const noexcept { return m_stanceCombo; }
    [[nodiscard]] QComboBox*      animCombo()       const noexcept { return m_animCombo; }
    [[nodiscard]] QCheckBox*      talkingCheck()      const noexcept { return m_talkingCheck; }
    [[nodiscard]] QCheckBox*      flipXCheck()        const noexcept { return m_flipXCheck; }
    [[nodiscard]] QCheckBox*      flipYCheck()        const noexcept { return m_flipYCheck; }
    [[nodiscard]] QCheckBox*      visibleCheck()      const noexcept { return m_visibleCheck; }

    // Background properties
    [[nodiscard]] ScrubbySpinBox* bgPosXSpin()        const noexcept { return m_bgPosXSpin; }
    [[nodiscard]] ScrubbySpinBox* bgPosYSpin()        const noexcept { return m_bgPosYSpin; }
    [[nodiscard]] ScrubbySpinBox* bgScaleSpin()       const noexcept { return m_bgScaleSpin; }
    [[nodiscard]] ScrubbySpinBox* bgOpacitySpin()     const noexcept { return m_bgOpacitySpin; }
    [[nodiscard]] ScrubbySpinBox* bgBlurSpin()        const noexcept { return m_bgBlurSpin; }

    // Camera
    [[nodiscard]] ScrubbySpinBox* cameraZoomSpin()    const noexcept { return m_cameraZoomSpin; }
    [[nodiscard]] ScrubbySpinBox* cameraPanXSpin()    const noexcept { return m_cameraPanXSpin; }
    [[nodiscard]] ScrubbySpinBox* cameraPanYSpin()    const noexcept { return m_cameraPanYSpin; }

    // Splitter
    [[nodiscard]] QSplitter*    mainSplitter()        const noexcept { return m_splitter; }

    /// Get the preset manager (for testing).
    [[nodiscard]] ShotPresetManager& presetManager() noexcept { return m_presetManager; }

    /// Get the standalone shots column widget (to be reparented into CharacterShotPanel).
    [[nodiscard]] QWidget* shotsColumn() const noexcept { return m_shotsColumn; }

    /// Get the character-filter thumbnail column (to be reparented into CharacterShotPanel).
    [[nodiscard]] QWidget* charFilterColumn() const noexcept { return m_charFilterColumn; }

signals:
    /// Emitted when the shot composition changes.
    void shotChanged();

    /// Emitted when the user wants to drag the shot to the timeline.
    void dragToTimeline(const ShotPreset& preset);

    /// Emitted when a layer is selected.
    void layerSelected(int index);

    /// Emitted when the user re-links a media asset on disk.  Listeners
    /// (e.g. TimelineWorkspace) walk timelines/presets and update references.
    void mediaRelinkRequested(const QString& oldPath, const QString& newPath);

private slots:
    void onShotListSelectionChanged();
    void onLayerListSelectionChanged();
    void onShotNameChanged(const QString& name);
    void onCharacterPropertyChanged();
    void onBackgroundPropertyChanged();
    void onCameraPropertyChanged();

private:
    void setupUI();
    QWidget* createLeftPanel();
    QWidget* createPropertiesPanel();
    QWidget* createShotsColumn();
    QWidget* createCharFilterColumn();

    void refreshLayerList();
    void copySelectedLayer();
    void pasteLayer();
    void copyTransform();
    void pasteTransform();

    void populateLayerProperties();
    void clearLayerProperties();
    void showCharacterProperties(const CharacterState& ch);
    void showBackgroundProperties(const BackgroundState& bg);
    void refreshVideoLibrary();
    int  addVideoLayer(const std::string& filename);
    void onVideoTimingChanged();
    QImage extractVideoThumbnail(const std::string& path);
    void updatePreview();
    void setLibraryIconSize(int sz);
    QPixmap makeCharacterThumbnail(const std::string& charName, int sz);
    QPixmap makeShotThumbnail(const ShotPreset& shot, int thumbW, int thumbH);
    void    saveShotThumbnail(const ShotPreset& shot);
    QString shotThumbnailPath(const std::string& shotName) const;

    // ── Video decoder pool (for looping video playback) ───────────────
    struct VideoPlaybackState {
        std::unique_ptr<VideoDecoder> decoder;
        double currentTime = 0.0;
        double duration    = 0.0;
        double fps         = 30.0;
        double frameAccum  = 0.0;  ///< Accumulator for frame-rate limiting
        QImage lastFrame;          ///< Most recent decoded frame as BGRA QImage
        bool   needsChromaKey = false;  ///< File is a GREEN-suffixed chroma-key source

        // Async decode: persistent worker thread produces frames, UI grabs them
        std::mutex frameMutex;
        QImage pendingFrame;       ///< Written by worker, read by UI
        std::atomic<bool> frameReady{false};
        std::atomic<bool> decoding{false};  ///< Worker is busy

        // Persistent worker thread — avoids per-frame thread creation overhead
        std::thread workerThread;
        std::condition_variable wakeWorker;
        std::mutex workerMutex;
        std::atomic<bool> stopWorker{false};

        ~VideoPlaybackState();
    };

    // Video playback helpers
    std::shared_ptr<VideoPlaybackState> getOrCreateVideoPlayer(const std::string& path);
    static QImage advanceVideoPlayer(const std::shared_ptr<VideoPlaybackState>& player, float dt);

    // ── Default-shot persistence ────────────────────────────────────────
    void saveDefaults() const;
    void loadDefaults();

    /// Return the active character filter value from the filter list.
    /// Returns empty string for "ALL", "__UNASSIGNED__" for unassigned,
    /// or the character display name for a specific character filter.
    [[nodiscard]] QString activeCharFilter() const;

    // ── State ───────────────────────────────────────────────────────────
    ShotPreset         m_currentShot;
    ShotPresetManager  m_presetManager;
    std::map<std::string, std::string> m_characterDefaults; ///< character name → default shot name
    ModelManager*      m_modelManager = nullptr;

    /// Tracks the shot name from the most recent successful save.
    /// Used to detect renames and clean up orphaned preset files on disk.
    std::string        m_lastSavedName;
    const AnimationVideoCache* m_animVideoCache = nullptr;
    int                m_selectedLayer = -1;
    bool               m_updating = false;  ///< Prevent recursive signals

    // ── Layer clipboard (copy/paste between shots) ──────────────────────
    struct LayerClipboardEntry {
        LayerType       type;
        CharacterState  character;
        BackgroundState background;
    };
    std::vector<LayerClipboardEntry> m_layerClipboard;

    // ── Transform clipboard (copy/paste transform between layers) ───────
    struct TransformClipboard {
        float posX      = 0.5f;
        float posY      = 0.5f;
        float scale     = 1.0f;
        float rotation  = 0.0f;
        float opacity   = 1.0f;
        bool  flipX     = false;
        bool  flipY     = false;
        bool  visible   = true;
        float cropLeft  = 0.0f;
        float cropRight = 0.0f;
        float cropTop   = 0.0f;
        float cropBottom= 0.0f;
        float blur      = 0.0f;
    };
    std::optional<TransformClipboard> m_transformClipboard;

    // ── Shot clipboard (copy/paste shots between characters) ────────────
    std::optional<ShotPreset> m_shotClipboard;

    // ── Undo / Redo stacks ──────────────────────────────────────────────
    /// Bundles a shot preset snapshot with its layer groups for undo/redo.
    struct UndoState {
        ShotPreset preset;
        std::vector<LayerGroupInfo> groups;
    };
    std::deque<UndoState> m_undoStack;
    std::deque<UndoState> m_redoStack;
    static constexpr int MAX_UNDO = 50;    bool               m_undoPropertyPushed = false;  ///< Coalesced undo for property edits
    std::atomic<bool>  m_destroying{false};
    QTimer*            m_undoCoalesceTimer  = nullptr; ///< Resets m_undoPropertyPushed
    // ── Library panel ───────────────────────────────────────────────────
    QWidget*      m_shotsColumn        = nullptr;   ///< Standalone shots sidebar column
    QWidget*      m_charFilterColumn   = nullptr;   ///< Character thumbnail filter column
    QListWidget*  m_charFilterList     = nullptr;   ///< Character thumbnail filter chip list
    QTabWidget*   m_libraryTabs       = nullptr;
    QListWidget*  m_shotList           = nullptr;   ///< Shot picker thumbnail strip
    QComboBox*    m_shotSortCombo      = nullptr;   ///< Sort dropdown (A-Z, Recent, Character, Favorites)
    QLineEdit*    m_shotSearchEdit     = nullptr;   ///< Filter shots by name
    QLineEdit*    m_filterSearchEdit   = nullptr;   ///< Character filter search bar
    QListWidget*  m_characterLibrary  = nullptr;
    QListWidget*  m_backgroundLibrary = nullptr;
    QListWidget*  m_videoLibrary      = nullptr;
    QLineEdit*    m_charSearchEdit    = nullptr;
    QCheckBox*    m_namedOnlyCheck    = nullptr;
    QSlider*      m_iconZoomSlider    = nullptr;
    int           m_iconSize          = 120;  ///< Current thumbnail size in px
    QPushButton*  m_newShotBtn        = nullptr;
    QPushButton*  m_saveShotBtn       = nullptr;
    QPushButton*  m_deleteShotBtn     = nullptr;

    // ── Layer groups (UI-only) ──────────────────────────────────────────
    std::vector<LayerGroupInfo> m_layerGroups;

    // ── Preview ─────────────────────────────────────────────────────────
    QWidget*              m_previewArea   = nullptr;
    SpinePreviewWidget*   m_spinePreview  = nullptr;
    std::vector<std::unique_ptr<SpineEngine>> m_layerEngines;
    std::vector<std::vector<QImage>>          m_layerTextures; ///< One texture set per engine
    std::unordered_map<std::string, QImage>   m_videoFrameCache; ///< Cached first-frame thumbnails
    std::unordered_map<std::string, QImage>   m_bgImageCache;    ///< Cached static background images
    std::unordered_map<std::string, QPixmap>  m_charThumbCache;  ///< Cached character thumbnails (keyed by "name:size")
    std::unordered_map<std::string, std::shared_ptr<VideoPlaybackState>> m_videoPlayers;

    // ── Properties panel ────────────────────────────────────────────────
    QLineEdit*    m_shotNameEdit      = nullptr;
    QCheckBox*    m_defaultShotCheck  = nullptr;
    QComboBox*    m_defaultCharCombo  = nullptr;   ///< Character to set as default for
    QPushButton*  m_setDefaultBtn     = nullptr;   ///< "Set as Default" button
    QListWidget*  m_layerList         = nullptr;
    QPushButton*  m_addCharBtn        = nullptr;
    QPushButton*  m_addBgBtn          = nullptr;
    QPushButton*  m_removeLayerBtn    = nullptr;
    QPushButton*  m_addGroupBtn       = nullptr;
    QPushButton*  m_layerUpBtn        = nullptr;
    QPushButton*  m_layerDownBtn      = nullptr;

    // Character properties
    QStackedWidget* m_propsStack      = nullptr;
    QTabWidget*     m_layerPropsTabs  = nullptr; ///< Tabs: Transform | Character/BG | Video/Crop
    QLabel*         m_emptyPropsLabel = nullptr;
    QGroupBox*      m_charPropsGroup  = nullptr;
    ScrubbySpinBox* m_posXSpin        = nullptr;
    ScrubbySpinBox* m_posYSpin        = nullptr;
    ScrubbySpinBox* m_scaleSpin       = nullptr;
    ScrubbySpinBox* m_rotationSpin    = nullptr;
    ScrubbySpinBox* m_opacitySpin     = nullptr;
    ScrubbySpinBox* m_blurSpin        = nullptr;
    QComboBox*      m_outfitCombo     = nullptr;
    QComboBox*      m_stanceCombo     = nullptr;
    QComboBox*      m_animCombo       = nullptr;
    QCheckBox*      m_talkingCheck    = nullptr;
    QCheckBox*      m_flipXCheck      = nullptr;
    QCheckBox*      m_flipYCheck      = nullptr;
    QCheckBox*      m_visibleCheck    = nullptr;

    // Crop
    QGroupBox*      m_cropGroup       = nullptr;
    ScrubbySpinBox* m_cropLeftSpin    = nullptr;
    ScrubbySpinBox* m_cropRightSpin   = nullptr;
    ScrubbySpinBox* m_cropTopSpin     = nullptr;
    ScrubbySpinBox* m_cropBottomSpin  = nullptr;

    // Background properties
    QGroupBox*      m_bgPropsGroup    = nullptr;
    ScrubbySpinBox* m_bgPosXSpin      = nullptr;
    ScrubbySpinBox* m_bgPosYSpin      = nullptr;
    ScrubbySpinBox* m_bgScaleSpin     = nullptr;
    ScrubbySpinBox* m_bgOpacitySpin   = nullptr;
    ScrubbySpinBox* m_bgBlurSpin      = nullptr;

    // Video timing
    QGroupBox*      m_videoTimingGroup = nullptr;
    ScrubbySpinBox* m_videoInSpin      = nullptr;
    ScrubbySpinBox* m_videoOutSpin     = nullptr;

    // Camera
    ScrubbySpinBox* m_cameraZoomSpin  = nullptr;
    ScrubbySpinBox* m_cameraPanXSpin  = nullptr;
    ScrubbySpinBox* m_cameraPanYSpin  = nullptr;

    // ── Drag-and-drop visual feedback ───────────────────────────────────
    int     m_dropIndicatorIndex = -1;  ///< Layer list: insertion line position (-1 = hidden)
    QWidget* m_dropIndicatorLine = nullptr; ///< Thin colored line overlay widget
    QPixmap m_dragThumb;                ///< Compose preview: semi-transparent thumbnail
    QPoint  m_dragPreviewPos;           ///< Compose preview: cursor position (widget coords)
    bool    m_dragOverPreview = false;  ///< True while dragging over compose preview
    void    updateDropIndicatorLine();  ///< Repositions the indicator line widget

    // Layout
    QSplitter*    m_splitter          = nullptr;
};

} // namespace rt
