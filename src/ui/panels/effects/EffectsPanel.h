/*
 * EffectsPanel — UI for browsing and managing clip effects.
 *
 * Step 22: Effects System
 *
 * Features:
 *  - Effect type browser (list of available effects)
 *  - Clip effect stack display (ordered list with enable/disable)
 *  - Add / remove / reorder effects
 *  - Per-effect parameter controls (spinboxes with keyframe support)
 *  - Drag-and-drop effects onto clips
 */

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QGroupBox>
#include <QCheckBox>
#include <QComboBox>

#include <filesystem>
#include <memory>

namespace rt {

class Clip;
class Track;
class EffectStack;
class CommandStack;
class Effect;

class EffectsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget* parent = nullptr);
    ~EffectsPanel() override;

    // ── Clip binding ────────────────────────────────────────────────────
    void setClip(Clip* clip, Track* track);
    [[nodiscard]] Clip* clip() const noexcept { return m_clip; }

    // ── Command stack ───────────────────────────────────────────────────
    void setCommandStack(CommandStack* stack) noexcept { m_commandStack = stack; }
    [[nodiscard]] CommandStack* commandStack() const noexcept { return m_commandStack; }

    // ── Refresh ─────────────────────────────────────────────────────────
    void refresh();

    // ── Test accessors ──────────────────────────────────────────────────
    [[nodiscard]] QListWidget* browserList()   const noexcept { return m_browserList; }
    [[nodiscard]] QTreeWidget* browserTree()   const noexcept { return m_browserTree; }
    [[nodiscard]] QListWidget* stackList()     const noexcept { return m_stackList; }
    [[nodiscard]] QPushButton* addButton()     const noexcept { return m_addButton; }
    [[nodiscard]] QPushButton* removeButton()  const noexcept { return m_removeButton; }
    [[nodiscard]] QPushButton* moveUpButton()  const noexcept { return m_moveUpButton; }
    [[nodiscard]] QPushButton* moveDownButton()const noexcept { return m_moveDownButton; }

signals:
    void effectAdded();
    void effectRemoved();
    void effectMoved();
    void effectChanged();

private:
    void setupUI();
    void populateBrowser();
    void filterBrowser(const QString& text);
    void refreshStack();

    void onAddClicked();
    void onRemoveClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onStackSelectionChanged();
    void onBrowserContextMenu(const QPoint& pos);
    void loadPresetsFromDisk();
    void saveEffectPreset(const Effect& fx);
    std::unique_ptr<Effect> loadEffectPreset(const std::filesystem::path& path);
    [[nodiscard]] bool canMutateClip() const noexcept;

    Clip*         m_clip{nullptr};
    Track*        m_track{nullptr};
    CommandStack* m_commandStack{nullptr};

    // Browser
    QGroupBox*    m_browserGroup{nullptr};
    QListWidget*  m_browserList{nullptr};
    QTreeWidget*  m_browserTree{nullptr};
    QLineEdit*    m_searchField{nullptr};

    // Stack
    QGroupBox*    m_stackGroup{nullptr};
    QListWidget*  m_stackList{nullptr};
    QPushButton*  m_addButton{nullptr};
    QPushButton*  m_removeButton{nullptr};
    QPushButton*  m_moveUpButton{nullptr};
    QPushButton*  m_moveDownButton{nullptr};

    // Status
    QLabel*       m_statusLabel{nullptr};

    bool m_updating{false};

    std::filesystem::path m_presetsDir;
};

} // namespace rt
