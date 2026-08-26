/*
 * CharacterBrowser — Character download and preview panel.
 *
 * Ported from the original Python CharacterPanel (~576 lines).
 * Two-pane splitter: left = character list with search/filter/download,
 * right = preview area with outfit/stance/animation controls.
 *
 * Layout:
 * ┌───────────────────────┬──────────────────────────────────────────┐
 * │  🔍 [Search...]       │                                          │
 * │  Category: [All ▼]    │                                          │
 * │  ☐ Downloaded Only    │         Preview Area                     │
 * │                       │      (Spine renderer placeholder)        │
 * │  ┌─────────────────┐  │                                          │
 * │  │ Modernia         │  │                                          │
 * │  │ Rapi             │  ├──────────────────────────────────────────┤
 * │  │ Dorothy          │  │  Outfit: [default ▼]  Stance: [Default] │
 * │  │ Crown            │  │  Animation: [idle ▼]   ☐ Talking        │
 * │  │ ...              │  │                                          │
 * │  └─────────────────┘  │  Status: 3 characters downloaded         │
 * │  [Refresh] [Download] │                                          │
 * │  [Delete]             │                                          │
 * └───────────────────────┴──────────────────────────────────────────┘
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QWidget>
#include <QJsonObject>
#include <QMap>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class QNetworkAccessManager;

namespace rt {

class AnimationVideoCache;
class ModelManager;
class SpineEngine;
class SpinePreviewWidget;

/// Character browser panel — search, download, and preview Spine characters.
class CharacterBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit CharacterBrowser(QWidget* parent = nullptr);
    ~CharacterBrowser() override;

    // ── Configuration ───────────────────────────────────────────────────

    /// Set the model manager (scans assets/characters/).
    void setModelManager(ModelManager* mgr);

    /// Set the animation video cache for showing conversion status.
    void setAnimVideoCache(AnimationVideoCache* cache);

    // ── Actions ─────────────────────────────────────────────────────────

    /// Refresh the character list from disk.
    void refreshList();

    /// Get the currently selected character name (first selected).
    [[nodiscard]] QString selectedCharacter() const;

    /// Get all selected character names.
    [[nodiscard]] QStringList selectedCharacters() const;

    /// Scroll the character list to the first entry starting with the given letter.
    void scrollToLetter(QChar letter);

    /// User-defined Library display names, keyed by the on-disk folder name.
    [[nodiscard]] const QMap<QString, QString>& customDisplayNames() const noexcept
    {
        return m_customDisplayNames;
    }

    // ── Accessors (testing) ─────────────────────────────────────────────

    [[nodiscard]] QLineEdit*    searchField()       const noexcept { return m_searchField; }
    [[nodiscard]] QComboBox*    categoryFilter()    const noexcept { return m_categoryFilter; }
    [[nodiscard]] QCheckBox*    downloadedOnly()    const noexcept { return m_downloadedOnly; }
    [[nodiscard]] QListWidget*  characterList()     const noexcept { return m_characterList; }
    [[nodiscard]] QPushButton*  refreshButton()     const noexcept { return m_refreshBtn; }
    [[nodiscard]] QPushButton*  downloadButton()    const noexcept { return m_downloadBtn; }
    [[nodiscard]] QPushButton*  deleteButton()      const noexcept { return m_deleteBtn; }
    [[nodiscard]] QWidget*      previewArea()       const noexcept { return m_previewArea; }
    [[nodiscard]] QComboBox*    outfitCombo()       const noexcept { return m_outfitCombo; }
    [[nodiscard]] QComboBox*    stanceCombo()       const noexcept { return m_stanceCombo; }
    [[nodiscard]] QComboBox*    animationCombo()    const noexcept { return m_animationCombo; }
    [[nodiscard]] QCheckBox*    talkingCheck()      const noexcept { return m_talkingCheck; }
    [[nodiscard]] QSplitter*    splitter()          const noexcept { return m_splitter; }

signals:
    void characterSelected(const QString& name);
    void downloadRequested(const QString& name);
    void deleteRequested(const QString& name);
    void characterDisplayNameChanged(const QString& folderName,
                                     const QString& displayName);

private slots:
    void onSearchChanged(const QString& text);
    void onCategoryChanged(int index);
    void onDownloadedOnlyToggled(bool checked);
    void onShowHiddenToggled(bool checked);
    void onCharacterSelectionChanged();
    void onRefreshClicked();
    void onDownloadClicked();
    void onDeleteClicked();
    void onOutfitChanged(int index);
    void onStanceChanged(int index);
    void onAnimationChanged(int index);
    void onTalkingChanged(bool checked);
    void onContextMenu(const QPoint& pos);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void setupUI();
    QWidget* createLeftPanel();
    QWidget* createRightPanel();
    void populateCharacterList();
    void loadMetadata();
    void discoverVideoCharacters();
    void populateControls();
    void loadPreviewModel();
    void fetchRemoteCharacterList();
    void fetchRemoteL2dNames();
    void downloadCharacterModel(const QString& repoPath,
                                const QString& charName = {},
                                const QString& outfitName = "default",
                                std::function<void(bool)> onOutfitComplete = nullptr);
    void downloadFile(const QString& remotePath, const QString& localPath,
                      std::function<void(bool)> callback);
    void resolveCharacterName(const QString& charId) const;
    void saveHiddenChars();
    void saveRenamedDisplayNames();
    [[nodiscard]] QString displayNameFor(const QString& folderName,
                                         const QString& fallback = {}) const;
    void promptRenameCharacter(const QString& folderName);

    // State
    ModelManager* m_modelManager{nullptr};
    AnimationVideoCache* m_animVideoCache{nullptr};
    QNetworkAccessManager* m_networkManager{nullptr};
    QString       m_currentFilter;
    int           m_currentCategory{0};  // 0=All
    std::set<QString> m_remoteCharIds;      // Available from Nikke DB
    std::set<QString> m_localCharNames;     // Downloaded locally
    std::set<QString> m_videoCharNames;     // Video-only characters (from shot presets)
    std::set<QString> m_hiddenCharNames;    // Permanently hidden characters
    QMap<QString, QString> m_defaultDisplayNames;
    QMap<QString, QString> m_customDisplayNames;
    QProgressBar* m_downloadProgress{nullptr};
    std::unique_ptr<SpineEngine> m_spineEngine; // For loading skeleton data
    QJsonObject   m_cachedCharacters;           // P1: Cached metadata
    bool          m_metadataLoaded{false};

    // Left panel
    QLabel*       m_titleLabel{nullptr};
    QLabel*       m_categoryLabel{nullptr};
    QLineEdit*    m_searchField{nullptr};
    QComboBox*    m_categoryFilter{nullptr};
    QCheckBox*    m_downloadedOnly{nullptr};
    QCheckBox*    m_showHidden{nullptr};
    QListWidget*  m_characterList{nullptr};
    QPushButton*  m_refreshBtn{nullptr};
    QPushButton*  m_downloadBtn{nullptr};
    QPushButton*  m_deleteBtn{nullptr};

    // Right panel
    QWidget*              m_previewArea{nullptr};
    SpinePreviewWidget*   m_spinePreview{nullptr};
    QComboBox*            m_outfitCombo{nullptr};
    QComboBox*    m_stanceCombo{nullptr};
    QComboBox*    m_animationCombo{nullptr};
    QCheckBox*    m_talkingCheck{nullptr};
    QLabel*       m_statusLabel{nullptr};

    // Layout
    QSplitter*    m_splitter{nullptr};
};

} // namespace rt
