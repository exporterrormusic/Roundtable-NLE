/*
 * CharacterShotPanel — Combined CHARACTERS page with icon sub-rail.
 *
 * Merges the CharacterBrowser (asset acquisition) and ShotComposer
 * (shot composition) into a single page with a sub-rail, matching
 * the architecture of ProjectPanel and AudioSync.
 *
 * Sub-rail layout (180 px, surface1 background):
 * ┌─────────────┐
 * │    📚       │
 * │  LIBRARY    │  Browse, search, download characters
 * ├─────────────┤
 * │    🎬       │
 * │  COMPOSE    │  Build shots from characters + backgrounds
 * ├─────────────┤
 * │    ⚙️       │
 * │  SETTINGS   │  Model paths, presets directory, defaults
 * └─────────────┘
 */

#pragma once

#include <QButtonGroup>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <filesystem>
#include <vector>

class QLabel;
class QPropertyAnimation;
class QParallelAnimationGroup;
class QSplitter;

namespace rt {

class AnimationVideoCache;
class CharacterBrowser;
class ConversionPanel;
class ModelManager;
class ShotComposer;
class PuppetLibraryPanel;

class CharacterShotPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CharacterShotPanel(QWidget* parent = nullptr);
    ~CharacterShotPanel() override;

    // ── Dependency injection ────────────────────────────────────────────

    void setModelManager(ModelManager* mgr);
    void setPresetsDirectory(const std::filesystem::path& dir);

    /// Set the animation video cache for the conversion panel.
    void setAnimVideoCache(AnimationVideoCache* cache);

    // ── Sub-panel accessors ─────────────────────────────────────────────

    [[nodiscard]] CharacterBrowser*  characterBrowser()  const noexcept { return m_characterBrowser; }
    [[nodiscard]] ShotComposer*      shotComposer()      const noexcept { return m_shotComposer; }
    [[nodiscard]] ConversionPanel*   conversionPanel()   const noexcept { return m_conversionPanel; }

    // ── Sub-rail mode ───────────────────────────────────────────────────

    enum Mode { Library = 0, Puppets = 1, Convert = 2, Compose = 3, Settings = 4 };

    void setMode(Mode mode);
    [[nodiscard]] Mode currentMode() const noexcept;

private:
    void setupUI();
    QWidget* createSettingsPage();
    void buildLetterSidePanel();

    // ── Rail helper ─────────────────────────────────────────────────────
    void addRailDivider(QVBoxLayout* layout);

    // ── Letter side panel ───────────────────────────────────────────────
    void showLetterPanel();
    void hideLetterPanel();

    // ── Rail ────────────────────────────────────────────────────────────
    QWidget*       m_rail{nullptr};
    QButtonGroup*  m_railGroup{nullptr};
    QPushButton*   m_railButtons[5]{};
    QLabel*        m_railLabels[5]{};


    // ── Letter side panel ───────────────────────────────────────────────
    QWidget*                  m_letterSidePanel{nullptr};
    QScrollArea*              m_letterScrollArea{nullptr};
    std::vector<QPushButton*> m_letterButtons;
    QPushButton*              m_activeLetterBtn{nullptr};
    bool                      m_letterPanelVisible{false};

    // ── Content ─────────────────────────────────────────────────────────
    QStackedWidget* m_contentStack{nullptr};
    QSplitter*      m_workspaceSplitter{nullptr};

    // ── Sub-panels ──────────────────────────────────────────────────────
    CharacterBrowser*   m_characterBrowser{nullptr};
    ShotComposer*       m_shotComposer{nullptr};
    ConversionPanel*    m_conversionPanel{nullptr};
    PuppetLibraryPanel* m_puppetLibraryPanel{nullptr};
    QWidget*           m_shotsColumnWidget{nullptr};
    QWidget*           m_charFilterWidget{nullptr};
    QWidget*           m_showFilterWidget{nullptr};
};

} // namespace rt
