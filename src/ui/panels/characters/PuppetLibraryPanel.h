/*
 * PuppetLibraryPanel — manage PNG puppet characters.
 *
 * Two modes:
 *   • readOnly  — a drag-only tree used by the timeline Library dock; each
 *                 variant leaf drops a PngPuppetClip via the "puppet:" URI.
 *   • editable  — the CHARACTERS→PUPPETS page: a fully-integrated, SHOTS-style
 *                 cascading editor with Character → Outfit → Action thumbnail
 *                 columns, a Faces column (4 PNG drop slots), and a live
 *                 animated preview.  No pop-up creation dialogs.
 */

#pragma once

#include <QChar>
#include <QWidget>

#include <array>

namespace rt { class MediaDragTreeWidget; }

class QToolButton;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QCheckBox;

namespace rt {

class PuppetLibraryPanel : public QWidget
{
    Q_OBJECT

public:
    /// `readOnly` = drag-only library (no editor); used by the timeline Library
    /// dock.  The CHARACTERS→Puppets page uses the editable cascade form.
    explicit PuppetLibraryPanel(QWidget* parent = nullptr, bool readOnly = false);

    /// Rebuild from assets/png_characters/.
    void refresh();

    [[nodiscard]] MediaDragTreeWidget* treeWidget() const noexcept { return m_tree; }

    /// Filter the editable character column to entries whose name starts with
    /// `letter` ('#' = first non-alphabetic name).  A null QChar() clears the
    /// filter (shows all).  Used by the CHARACTERS-page A-Z letter side panel.
    void filterByLetter(QChar letter);

signals:
    /// Emitted after a puppet is created/edited/deleted so other views (e.g. the
    /// COMPOSE Puppets library tab) can refresh.
    void puppetsChanged();

private:
    // ── Read-only drag tree (timeline Library dock) ─────────────────────────
    void buildReadOnlyUI();
    void refreshTree();
    [[nodiscard]] QString selectedFolder() const;

    // ── Editable cascade (CHARACTERS page) ──────────────────────────────────
    void buildEditableUI();
    void refreshCharacters();
    void refreshOutfits();
    void refreshActions();
    void refreshFaces();
    void applyLetterFilter();   ///< (re)apply m_letterFilter to m_charList rows

    void onCharRowChanged();
    void onOutfitRowChanged();
    void onActionRowChanged();

    void createCharacter(const QString& name);
    void createOutfit(const QString& name);
    void createAction(const QString& name);

    void deleteCharacter(const QString& folder);
    void deleteOutfit(const QString& outfit);
    void deleteAction(const QString& action);
    void renameCharacter(const QString& folder, const QString& newName);
    void renameOutfit(const QString& oldName, const QString& newName);
    void renameAction(const QString& oldName, const QString& newName);

    void setFaceImage(int faceIndex, const QString& sourcePngPath);
    void reloadPreviewFaces();

    [[nodiscard]] QString currentVariantKey() const;

    bool                 m_readOnly{false};

    // read-only mode
    MediaDragTreeWidget* m_tree{nullptr};

    // editable mode — cascade columns
    QListWidget*         m_charList{nullptr};
    QListWidget*         m_outfitList{nullptr};
    QListWidget*         m_actionList{nullptr};
    QLineEdit*           m_newCharEdit{nullptr};
    QLineEdit*           m_newOutfitEdit{nullptr};
    QLineEdit*           m_newActionEdit{nullptr};

    // editable mode — faces column + preview
    std::array<QWidget*, 4> m_faceSlots{};
    QWidget*             m_preview{nullptr};   ///< file-local PuppetPreview*
    QCheckBox*           m_talkCheck{nullptr};

    // editable mode — current selection
    QString              m_curFolder;
    QString              m_curOutfit;
    QString              m_curAction;
    QChar                m_letterFilter{};   ///< null = no A-Z filter active

    bool                 m_updating{false};
};

} // namespace rt
