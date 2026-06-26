/*
 * ProjectBin — Tabbed media browser panel.
 *
 * Step 16: Project Bin
 *
 * The Project Bin is the central media management panel:
 *   - Tabbed categories: All / Video / Image / Audio / Spine
 *   - Search/filter bar at top
 *   - Zoom slider for thumbnail size
 *   - Import button to add files
 *   - Double-click to load in source monitor
 *   - Drag-to-timeline or source monitor
 *
 * Layout:
 *   ┌─────────────────────────────────────────────┐
 *   │  [All] [Video] [Image] [Audio] [Spine]     │
 *   ├─────────────────────────────────────────────┤
 *   │  [🔍 Search...          ]  [Import] [-][+] │
 *   ├─────────────────────────────────────────────┤
 *   │  ┌──────┐ ┌──────┐ ┌──────┐               │
 *   │  │      │ │      │ │      │  ...           │
 *   │  │thumb │ │thumb │ │thumb │               │
 *   │  ├──────┤ ├──────┤ ├──────┤               │
 *   │  │name  │ │name  │ │name  │               │
 *   │  └──────┘ └──────┘ └──────┘               │
 *   │  ...                                       │
 *   ├─────────────────────────────────────────────┤
 *   │  Items: 42                                  │
 *   └─────────────────────────────────────────────┘
 */

#pragma once

#include "decode/ThumbnailGenerator.h"
#include "project/Project.h"   // Project::BinItem (rich bin model)

#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabBar>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class ThumbnailGrid;
class ThumbnailGenerator;
class MediaDragTreeWidget;
class MediaPool;
class MediaSourceService;
class Project;
class CommandStack;
class Timeline;
struct Resolution;

/// Bin folder info for serialization.
struct BinFolderState {
    std::string              name;
    bool                     expanded = true;
    std::vector<std::string> childKeys;
};

/// Project Bin — Premiere Pro-style media browser for the project.
class ProjectBin : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectBin(QWidget* parent = nullptr);
    ~ProjectBin() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the ThumbnailGenerator used for thumbnail loading.
    void setThumbnailGenerator(ThumbnailGenerator* gen);

    /// Set the MediaPool for handle management.
    void setMediaPool(MediaPool* pool) noexcept;

    /// Set the shared MediaSourceService for source access.
    void setMediaSourceService(MediaSourceService* service) noexcept;

    /// Set the Project for sequence management.
    void setProject(Project* project) noexcept;

    /// Update the root tab label to the project name.
    void setProjectName(const QString& name);

    /// Set the CommandStack for undo/redo support.
    void setCommandStack(CommandStack* stack) noexcept;

    // ── Import / manage items ───────────────────────────────────────────

    /// Import files via a file dialog (opens QFileDialog).
    void importFiles();

    /// Add files to the bin programmatically.
    void addFiles(const std::vector<std::filesystem::path>& files);

    /// Add files into a specific bin (or root if targetBin is nullptr).
    void addFilesToBin(const std::vector<std::filesystem::path>& files,
                       QTreeWidgetItem* targetBin);

    /// Add files into the bin at the given breadcrumb path (root→leaf bin
    /// names; empty = root). Path-based so the icon view's open folder
    /// (m_iconBinPath) can be targeted without a tree selection/item pointer.
    void addFilesToBinPath(const std::vector<std::filesystem::path>& files,
                           const QStringList& targetBinPath);

    /// Add files into a named bin (creates the bin if it doesn't exist).
    /// If parentBinName is non-empty, the new bin is created under that parent.
    void addFilesToNamedBin(const std::vector<std::filesystem::path>& files,
                            const QString& binName,
                            const QString& parentBinName = {});

    /// Remove a specific media file from the bin.
    bool removeFile(const std::filesystem::path& filePath);

    /// Replace the media file backing a selected bin item with a new file.
    /// Preserves label color and display name. The selected tree item
    /// is updated in-place; both list and icon views are refreshed.
    void replaceMedia(QTreeWidgetItem* selected);

    /// Clear all items from the bin.
    void clearAll();

    /// Force a full refresh of both list and icon views from current state.
    /// Call after project switch to ensure stale tree items are purged.
    void refreshAllViews();

    /// Create default bin folders (1_SEQUENCES, 2_AUDIO, 3_VIDEO, 4_GFX)
    /// and auto-sort existing items into them.
    void ensureDefaultBins();

    /// Refresh the sequence items in the list from the current project.
    void refreshSequences();

    /// Select all items in the bin (for Ctrl+A).
    void selectAllItems();

    /// Copy the current bin selection (sequence, media item, color matte,
    /// or bin folder) into the bin clipboard (Ctrl+C). Returns true if a
    /// copyable item was selected.
    bool copySelection();

    /// Paste the bin clipboard as a new, independent item (Ctrl+V).
    /// Sequences and generated assets (color mattes) are deep-copied;
    /// footage is duplicated as an independent reference to the same
    /// media. Returns true if something was pasted.
    bool pasteClipboard();

    /// Copy the current selection and immediately paste it — one-step
    /// "Duplicate", Premiere-style. Returns true if something was done.
    bool duplicateSelection();

    /// Whether the bin clipboard currently holds something pasteable.
    [[nodiscard]] bool hasClipboard() const noexcept;

    /// Number of items in the bin.
    [[nodiscard]] int itemCount() const noexcept;

    // ── Query ───────────────────────────────────────────────────────────

    /// Get all file paths in the bin.
    [[nodiscard]] std::vector<std::filesystem::path> allFiles() const;

    /// Get files of a specific type.
    [[nodiscard]] std::vector<std::filesystem::path> filesOfType(MediaType type) const;

    /// Get the current bin folder structure (for serialization).
    [[nodiscard]] std::vector<BinFolderState> binFolderState() const;

    /// Restore bin folder structure from saved state (after adding files).
    void restoreBinFolders(const std::vector<BinFolderState>& folders);

    /// Export every bin entry as a rich per-instance record (id, path,
    /// display name, label colour) — duplicates included. For v14+ save.
    [[nodiscard]] std::vector<Project::BinItem> exportBinItems() const;

    /// Rebuild the bin from rich per-instance records (v14+ load),
    /// preserving each entry's identity/name/label so footage
    /// reference-duplicates survive a save/reload, then restore folders.
    void restoreBinModel(const std::vector<Project::BinItem>& items,
                         const std::vector<BinFolderState>& folders);

    // ── Accessors ───────────────────────────────────────────────────────

    /// Get the internal thumbnail grid widget.
    [[nodiscard]] ThumbnailGrid* grid() const noexcept { return m_grid; }

    /// Get the search/filter line edit.
    [[nodiscard]] QLineEdit* searchField() const noexcept { return m_searchField; }

    /// Get the zoom slider.
    [[nodiscard]] QSlider* zoomSlider() const noexcept { return m_zoomSlider; }

    /// Current active tab type.
    [[nodiscard]] MediaType activeTabType() const noexcept { return m_activeType; }

    // ── View mode ───────────────────────────────────────────────────────

    /// Whether the bin is currently showing list view or icon view.
    [[nodiscard]] bool isListView() const noexcept { return m_listView; }

    /// Create a new bin folder (shows input dialog, adds to tree).
    void createNewBin();

    /// Select and scroll to the item matching the given file path.
    void revealByPath(const QString& filePath);

    /// Refresh the thumbnail for a specific file path, re-generating from
    /// the current on-disk content.  Used when a file is overwritten in
    /// Windows Explorer while the bin is open, so the icon view updates
    /// live without requiring an app restart.
    void refreshFileThumbnail(const std::filesystem::path& filePath);

    /// Open the "New Sequence" dialog and create a sequence with chosen settings.
    void createNewSequence();

    /// Create a sequence from a dropped media file, matching its resolution/fps.
    void createSequenceFromMedia(const std::filesystem::path& filePath);

    /// Create a solid-color matte image (like Premiere Pro's Color Matte).
    /// Prompts for color and name, generates a PNG, and imports it.
    void createColorMatte();

    /// Whether a bin item path is a generated Color Matte asset
    /// (lives in a "Mattes" directory).
    [[nodiscard]] static bool isColorMatte(const std::filesystem::path& path);

    /// Whether a bin item path is a synthetic Adjustment Layer entry
    /// (sentinel "<adjustments>/<name>.adj" — never an on-disk file).
    [[nodiscard]] static bool isAdjustmentLayer(const std::filesystem::path& path);

    /// Create a new Adjustment Layer bin item (Premiere-style — prompts
    /// for a name and adds it as a draggable synthetic entry).
    void createAdjustmentLayer();

    /// Re-open the color picker for an existing Color Matte and rewrite
    /// the PNG in place, refreshing the bin and every timeline instance.
    void editColorMatte(const std::filesystem::path& mattePath);

    /// Scale all clips in a sequence proportionally when resolution changes.
    static void scaleClipsToResolution(class Timeline* seq,
                                       const Resolution& from,
                                       const Resolution& to);

    // ── Size hints ──────────────────────────────────────────────────────

    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    /// Emitted when the user double-clicks an item to load it in the source monitor.
    void loadInSourceMonitor(const std::filesystem::path& filePath, uint64_t mediaHandle);

    /// Emitted when items are imported or removed.
    void itemCountChanged(int count);

    /// Emitted when the user double-clicks a sequence to open it.
    void sequenceOpened(size_t sequenceIndex);

    /// Emitted when a sequence is added, removed, or duplicated.
    void sequencesChanged();

    /// Emitted when sequence settings (resolution, frame rate, name) are changed.
    void sequenceSettingsChanged();

    /// Emitted when a bin media file's bytes change on disk (e.g. a
    /// Color Matte recoloured via double-click). Listeners must drop
    /// any cached decode/texture for this path and refresh.
    void mediaContentChanged(const std::filesystem::path& path);

    /// Emitted when clips have been removed from (or restored to) the
    /// timeline as a side-effect of deleting/undeleting bin media.
    /// Listeners should rebuild timeline track views and flush caches.
    void timelineClipsMutated();

    /// Emitted when user wants to nest a sequence on the active timeline.
    void nestSequenceRequested(size_t sequenceIndex, const QString& sequenceName);

    /// Emitted when a project needs to be created (e.g. creating a sequence
    /// with no project open). The receiver takes ownership of the Project*.
    void projectCreated(class Project* project);

private slots:
    void onFilterChanged(int index);
    void onSearchChanged(const QString& text);
    void onZoomChanged(int value);
    void onItemDoubleClicked(int index, const std::filesystem::path& filePath);
    void onListItemDoubleClicked(QTreeWidgetItem* item, int column);
    void setListView(bool listMode);
    void onBinTabChanged(int index);
    void onBinTabCloseRequested(int index);

private:
    // ── Drag-and-drop (extracted to ProjectBinDragDrop.cpp) ─────────────

    struct BinSnapshot {
        std::vector<std::filesystem::path> files;
        std::vector<BinFolderState> folders;
    };

    /// One entry in the bin clipboard. Recursive so a copied bin folder
    /// carries its whole subtree.
    struct ClipboardEntry {
        bool                  isBin = false;
        // ── Media item fields ──
        std::filesystem::path filePath;
        QString               displayName;
        uint32_t              labelColor = 0xFF888888;
        MediaType             type = MediaType::Unknown;
        uint64_t              mediaHandle = 0;
        /// Generated, self-contained asset (e.g. a Color Matte PNG). On
        /// paste the file is physically copied so the duplicate can be
        /// changed independently; other footage is duplicated by reference.
        bool                  generatedAsset = false;
        // ── Bin folder fields ──
        QString               binName;
        std::vector<ClipboardEntry> children;
    };
    BinSnapshot captureBinSnapshot();
    void applyBinSnapshot(const BinSnapshot& s);

    // ── Bin clipboard helpers (ProjectBinEvents.cpp) ────────────────────

    /// Snapshot a tree item (media / bin folder) into a ClipboardEntry,
    /// recursing into bin children. Returns isBin=false with empty
    /// filePath if the item can't be captured.
    ClipboardEntry captureClipboardEntry(QTreeWidgetItem* item) const;

    /// Materialize one clipboard entry as a new independent bin item.
    /// `parentBinPath` is the destination folder ("" = bin root); copied
    /// bin folders recurse with their own path. Appends every created
    /// grid itemId, any physically copied file, and folder records (so
    /// duplicated bins reuse the persistence machinery) for undo.
    void applyClipboardEntry(const ClipboardEntry& e,
                             const QString& parentBinPath,
                             std::vector<uint64_t>& createdItemIds,
                             std::vector<std::filesystem::path>& createdFiles,
                             std::vector<BinFolderState>& createdFolders);

    bool handleDropEvent(QEvent* de);
    bool handleDragEnterEvent(QEvent* de);
    bool handleDragMoveEvent(QEvent* de);
    void handleDragLeave();

    // ── UI setup (extracted to ProjectBinUI.cpp) ────────────────────────

    void setupUI();
    void syncListView(const std::vector<BinFolderState>* savedFoldersOverride = nullptr);
    void syncIconView();

    /// After syncListView() builds the full tree, reparent the list widget
    /// so it shows only the children of the bin at m_iconBinPath (used when
    /// a sub-bin tab is active in list-view mode).
    void focusListViewOnBin();

    /// Open (or switch to) a tab for the given bin path.
    void openBinTab(const QStringList& binPath, const QString& name);

    // Widgets
    QComboBox*     m_filterCombo{nullptr};
    QLineEdit*     m_searchField{nullptr};
    QToolButton*   m_importBtn{nullptr};
    QToolButton*   m_btnCreateSequence{nullptr};
    QToolButton*   m_btnListView{nullptr};
    QToolButton*   m_btnIconView{nullptr};
    QSlider*       m_zoomSlider{nullptr};
    QScrollArea*   m_scrollArea{nullptr};
    ThumbnailGrid* m_grid{nullptr};
    class MediaDragTreeWidget* m_listWidget{nullptr};
    QLabel*        m_emptyLabel{nullptr};
    QLabel*        m_statusLabel{nullptr};
    QWidget*       m_iconNavBar{nullptr};
    QLabel*        m_breadcrumbLabel{nullptr};
    QTabBar*       m_binTabBar{nullptr};

    // State
    bool        m_listView{true};  // Default to list view (Premiere style)
    MediaType   m_activeType{MediaType::Unknown}; // Unknown = All
    QStringList m_iconBinPath;     // Current folder path in icon view
    MediaPool*  m_pool{nullptr};
    MediaSourceService* m_mediaSources{nullptr};
    ThumbnailGenerator* m_generator{nullptr};
    std::unique_ptr<ThumbnailGenerator> m_ownedGenerator;  // owned if created internally
    Project*    m_project{nullptr};
    CommandStack* m_commandStack{nullptr};
    std::unique_ptr<Timeline> m_clipboardSequence; // deep copy of copied sequence for paste
    std::vector<ClipboardEntry> m_clipboardItems;  // copied media/matte/bin (non-sequence)
    QTreeWidgetItem* m_dropHighlightItem{nullptr}; // bin highlighted during drag
    QVector<QStringList> m_binTabPaths;  // bin path for each tab (empty = root)
    QVector<bool> m_binTabViewModes;     // per-tab list (true) / icon (false) preference
    std::vector<BinFolderState> m_rootFolderState; // full tree snapshot before sub-bin focus
    std::atomic<bool> m_destroying{false};
};

} // namespace rt
