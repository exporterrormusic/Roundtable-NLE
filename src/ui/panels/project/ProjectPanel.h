/*
 * ProjectPanel — V5 "Inline Stage" project manager (PROJECTS tab).
 *
 * Widescreen-optimised layout with icon-rail sidebar, full-width project
 * table (with screenshot thumbnails), and inline expanding side panels
 * for NEW and OPEN actions (no floating windows).
 *
 * ┌──────┬─────────────┬────────────────────────────────────────────────────┐
 * │ ICON │  SIDE PANEL │  [🔍 Search...]                  [Sort ▼]  [↻]   │
 * │ RAIL │  (NEW or    ├────────────────────────────────────────────────────┤
 * │      │   OPEN)     │  ██ │ NAME            │ RES   │ FPS│ SIZE │ DATE  │
 * │  ＋   │             │  ██ │ Roundtable Feb  │1920×… │ 30 │ 2.1M │ Today │
 * │  📂   │             │  ██ │ Other Project   │1080×… │ 60 │ 1.8M │ Feb 20│
 * │  💾   │             │     │                 │       │    │      │       │
 * │  📥   │             │     │                 │       │    │      │       │
 * └──────┴─────────────┴────────────────────────────────────────────────────┘
 *
 * Click "＋ NEW" → inline side panel slides in with name field, template
 * radio list (Blank / Roundtable / Short Form / Film 4K / Custom), and
 * Create button.  Selecting "Custom" inline-expands width/height/fps fields.
 *
 * Click "📂 OPEN" → inline side panel slides in with file tree browser
 * rooted at the projects directory, filtered for .rtp files.
 */

#pragma once

#include <QWidget>
#include <QDateTime>
#include <QVector>
#include <QStringList>
#include <QIcon>

#include <atomic>

class QLabel;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTableWidget;
class QButtonGroup;
class QSpinBox;
class QDoubleSpinBox;
class QKeyEvent;
class QResizeEvent;
class QTreeView;
class QFileSystemModel;
class QStackedWidget;
class QListWidget;
class QListWidgetItem;
class QGridLayout;
class QScrollArea;

namespace rt {

/// Paint a small folder icon pixmap (22×18) matching the current theme.
QIcon createFolderIcon();

/// Project template presets (resolution + fps) — kept for backward compat.
enum class ProjectTemplate : int
{
    Blank = 0,
};

/// Which side panel is currently showing (if any).
enum class SidePanelMode : int
{
    None     = 0,
    New      = 1,
    Open     = 2,
    Settings = 3,
};

/// Lightweight metadata for a project file (read from header only).
struct ProjectInfo
{
    QString   name;
    QString   filePath;
    qint64    fileSize{0};
    QDateTime lastModified;
    uint32_t  resW{1920};
    uint32_t  resH{1080};
    double    fps{30.0};
    bool      isCurrent{false};
};

/// Project manager panel — create, open, and manage projects.
class ProjectPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectPanel(QWidget* parent = nullptr);
    ~ProjectPanel() override;

    // ── Public API (called by MainWindow) ───────────────────────────────

    void setProjects(const QVector<ProjectInfo>& projects);
    void setCurrentProjectName(const QString& name);
    void setRecentProjects(const QStringList& paths);
    void setProjectsDirectory(const QString& dir);

    /// Look up the full file path for a project by name.
    /// Returns an empty string if the project is not in the current list.
    [[nodiscard]] QString projectFilePath(const QString& name) const;

    // ── Custom-template accessors (read when tmpl == Custom) ────────────

    [[nodiscard]] uint32_t customResWidth()  const;
    [[nodiscard]] uint32_t customResHeight() const;
    [[nodiscard]] double   customFps()       const;

    // ── Widget accessors (MainWindow / tests) ───────────────────────────

    [[nodiscard]] QPushButton* refreshButton()  const noexcept { return m_refreshBtn; }
    [[nodiscard]] QPushButton* createButton()   const noexcept { return m_createBtn; }
    [[nodiscard]] QLineEdit*   nameInput()      const noexcept { return m_nameInput; }
    [[nodiscard]] QPushButton* openFileButton() const noexcept { return m_openFileBtn; }
    [[nodiscard]] QPushButton* saveButton()     const noexcept { return m_saveBtn; }

    /// Set a thumbnail for a project by copying a user-selected image.
    void setThumbnailForProject(const QString& projectName);

    /// Remove the thumbnail for a project, resetting it to the placeholder.
    void removeThumbnailForProject(const QString& projectName);

    /// Set a thumbnail from raw BGRA pixel data (used by auto-capture on save).
    void setThumbnailFromPixels(const QString& projectName, const uint8_t* bgra,
                                uint32_t width, uint32_t height);

    /// Set a thumbnail from a QImage (used by viewport grab).
    void setThumbnailFromImage(const QString& projectName, const QImage& image);

signals:
    void createProject(const QString& name, uint32_t resW, uint32_t resH,
                       double fps, const QString& saveDir);
    void openProject(const QString& name);
    void deleteProject(const QString& name, const QString& filePath);
    void renameProject(const QString& oldName, const QString& newName);
    void duplicateProject(const QString& name);
    void openFromFile();
    void openFilePath(const QString& filePath);          // from inline file browser
    void saveRequested();
    void openRecentProject(const QString& filePath);
    void revealInExplorer(const QString& name);
    void importProject(const QString& srcPath);
    void exportProject(const QString& name, const QString& dstPath);
    void projectsDirChanged(const QString& newDir);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onCreateClicked();
    void onSearchTextChanged(const QString& text);
    void onSortChanged(int index);
    void onFileTreeDoubleClicked(const QModelIndex& index);
    void onOpenListItemDoubleClicked(QListWidgetItem* item);

private:
    void setupUI();
    // setupUI() section builders (each builds a member widget):
    void buildIconRail();      // ProjectPanelUI.cpp
    void buildSidePanel();     // ProjectPanelSidePanelBuild.cpp
    void buildContentArea();   // ProjectPanelUI.cpp
    void rebuildTable();
    void showSidePanel(SidePanelMode mode);
    void hideSidePanel();
    void toggleSidePanel(SidePanelMode mode);
    void rebuildResGrid(uint32_t arW, uint32_t arH);
    void updateSummaryLabels();
    void syncARfromResolution();
    QString selectedProjectName() const;
    void showContextMenu(const QPoint& pos);
    void showOpenListContextMenu(const QPoint& pos);
    void updateActionButtons();
    void applyResponsiveLayout();
    void applyNewPanelResponsiveLayout();
    void populateOpenList();
    QString thumbnailPathForProject(const QString& projectName) const;
    void rebuildRecentPathButtons();
    void addRecentSaveLocation(const QString& path);

    // ── Use-after-free guard ────────────────────────────────────────────
    std::atomic<bool> m_destroying{false};

    // ── Data ────────────────────────────────────────────────────────────
    QVector<ProjectInfo> m_allProjects;
    QStringList          m_recentPaths;
    QStringList          m_recentSaveLocations;
    QString              m_currentProjectName;
    QString              m_searchFilter;
    QString              m_projectsDir;
    int                  m_sortMode{0};
    SidePanelMode        m_sidePanelMode{SidePanelMode::None};

    // ── Icon Rail ───────────────────────────────────────────────────────
    QWidget*     m_iconRail{nullptr};
    QPushButton* m_newBtn{nullptr};
    QPushButton* m_openFileBtn{nullptr};
    QPushButton* m_saveBtn{nullptr};
    QPushButton* m_importBtn{nullptr};
    QPushButton* m_settingsBtn{nullptr};

    // ── Side Panel (inline expanding column) ────────────────────────────
    QWidget*         m_sidePanel{nullptr};
    QStackedWidget*  m_sidePanelStack{nullptr};

    // ── Cached responsive sizes for NEW panel ───────────────────────────
    struct NewPanelSizes {
        int cardMarginTB{12}, cardMarginLR{14}, cardSpacing{8};
        int  stepFontSize{12};
        int  btnFontSize{18}, btnPadV{8}, btnPadH{12};
        int  inputFontSize{13}, inputPadV{8}, inputPadH{10};
        int  siFontSize{13}, siPadV{6}, siPadH{8}, siMinW{50}, siMaxW{60};
        int  sumPadTB{12}, sumPadLR{14}, sumSpacing{10};
        int  sumNameFontSize{12}, sumSpecFontSize{12}, sumIconFontSize{11};
        int  createBtnFontSize{13}, createBtnPadV{8}, createBtnPadH{18}, createBtnMinH{36};
        int  pageSpacing{8};
        int  gridSpacing{3};
        int  dividerSpacing{12};
        int  headerFontSize{16};
        int  closeBtnSize{26};
        int  browseBtnSize{30};
        int  recentLblFontSize{9};
        int  recentSampleFontSize{10}, recentSamplePadV{2}, recentSamplePadH{7};
        int  customRowFontSize{10};
    } m_newSizes;

    // ── Side Panel → NEW page (step-card layout) ────────────────────────
    QWidget*       m_newPage{nullptr};
    QScrollArea*   m_newPageScroll{nullptr};
    QLineEdit*     m_nameInput{nullptr};
    QLineEdit*     m_locationInput{nullptr};
    QPushButton*   m_locationBrowseBtn{nullptr};
    QWidget*       m_recentPathsWidget{nullptr};

    // Aspect Ratio
    QButtonGroup*  m_arGroup{nullptr};
    QPushButton*   m_ar16_9{nullptr};
    QPushButton*   m_ar9_16{nullptr};
    QPushButton*   m_ar21_9{nullptr};
    QPushButton*   m_arCustom{nullptr};
    QWidget*       m_customArRow{nullptr};
    QSpinBox*      m_customArW{nullptr};
    QSpinBox*      m_customArH{nullptr};

    // Resolution (dynamic buttons rebuilt on AR change)
    QWidget*       m_resGridWidget{nullptr};
    QButtonGroup*  m_resGroup{nullptr};
    QWidget*       m_customResRow{nullptr};
    QSpinBox*      m_customResW{nullptr};
    QSpinBox*      m_customResH{nullptr};

    // Framerate
    QButtonGroup*  m_fpsGroup{nullptr};
    QPushButton*   m_fps24{nullptr};
    QPushButton*   m_fps30{nullptr};
    QPushButton*   m_fps60{nullptr};
    QPushButton*   m_fpsCustom{nullptr};
    QWidget*       m_customFpsRow{nullptr};
    QDoubleSpinBox* m_customFps{nullptr};

    // Summary bar
    QWidget*       m_summaryBar{nullptr};
    QLabel*        m_summaryNameLabel{nullptr};
    QLabel*        m_summaryResLabel{nullptr};
    QLabel*        m_summaryFpsLabel{nullptr};
    QPushButton*   m_createBtn{nullptr};

    // ── Side Panel → OPEN page ──────────────────────────────────────────
    QWidget*          m_openPage{nullptr};
    QFileSystemModel* m_fileModel{nullptr};
    QTreeView*        m_fileTree{nullptr};
    QListWidget*      m_openList{nullptr};
    QPushButton*      m_openSelectedBtn{nullptr};


    // ── Side Panel → SETTINGS page ──────────────────────────────────────
    QWidget*     m_settingsPage{nullptr};
    QLineEdit*   m_projDirInput{nullptr};
    QPushButton* m_changeDirBtn{nullptr};

    // ── Content area ────────────────────────────────────────────────────
    QWidget*      m_contentArea{nullptr};
    QLineEdit*    m_searchInput{nullptr};
    QComboBox*    m_sortCombo{nullptr};
    QPushButton*  m_refreshBtn{nullptr};
    QTableWidget* m_projectTable{nullptr};
    QWidget*      m_emptyStateWidget{nullptr};
    QLabel*       m_emptyLabel{nullptr};
    // ── Bottom action bar ────────────────────────────────────────────
    QWidget*     m_actionBar{nullptr};
    QPushButton* m_openActionBtn{nullptr};
    QPushButton* m_dupeActionBtn{nullptr};
    QPushButton* m_renameActionBtn{nullptr};
    QPushButton* m_deleteActionBtn{nullptr};};

} // namespace rt
