/*
 * CharacterShotPanel.cpp — Combined CHARACTERS page with icon sub-rail.
 */

#include "CharacterShotPanel.h"
#include "CharacterBrowser.h"
#include "panels/project/ConversionPanel.h"
#include "panels/characters/PuppetLibraryPanel.h"
#include "ShotComposer.h"
#include "Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// Construction / destruction
// ═════════════════════════════════════════════════════════════════════════════

CharacterShotPanel::CharacterShotPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

CharacterShotPanel::~CharacterShotPanel() = default;

// ═════════════════════════════════════════════════════════════════════════════
// Dependency injection
// ═════════════════════════════════════════════════════════════════════════════

void CharacterShotPanel::setModelManager(ModelManager* mgr)
{
    if (m_characterBrowser) m_characterBrowser->setModelManager(mgr);
    if (m_shotComposer)     m_shotComposer->setModelManager(mgr);
    if (m_conversionPanel)  m_conversionPanel->setModelManager(mgr);
}

void CharacterShotPanel::setPresetsDirectory(const std::filesystem::path& dir)
{
    if (m_shotComposer) m_shotComposer->setPresetsDirectory(dir);
}

void CharacterShotPanel::setAnimVideoCache(AnimationVideoCache* cache)
{
    if (m_characterBrowser) m_characterBrowser->setAnimVideoCache(cache);
    if (m_conversionPanel) m_conversionPanel->setAnimVideoCache(cache);
}

// ═════════════════════════════════════════════════════════════════════════════
// Mode switching
// ═════════════════════════════════════════════════════════════════════════════

void CharacterShotPanel::setMode(Mode mode)
{
    int idx = static_cast<int>(mode);
    if (idx >= 0 && idx < 5 && m_railButtons[idx]) {
        m_railButtons[idx]->setChecked(true);
        m_contentStack->setCurrentIndex(idx);
    }
}

CharacterShotPanel::Mode CharacterShotPanel::currentMode() const noexcept
{
    int id = m_railGroup ? m_railGroup->checkedId() : 0;
    return static_cast<Mode>(id);
}

// ═════════════════════════════════════════════════════════════════════════════
// Rail divider helper
// ═════════════════════════════════════════════════════════════════════════════

void CharacterShotPanel::addRailDivider(QVBoxLayout* layout)
{
    const auto& c = Theme::colors();

    auto* div = new QWidget;
    div->setFixedHeight(17);
    auto* lay = new QVBoxLayout(div);
    lay->setContentsMargins(16, 0, 16, 0);
    lay->setSpacing(0);
    lay->addStretch();
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background: %1;").arg(Theme::rgb(c.border)));
    lay->addWidget(line);
    lay->addStretch();
    layout->addWidget(div);
}

// ═════════════════════════════════════════════════════════════════════════════
// UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void CharacterShotPanel::setupUI()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // ── Icon Rail (180 px, surface1 background — matches ProjectPanel) ──
    m_rail = new QWidget;
    m_rail->setObjectName("CharShotRail");
    m_rail->setFixedWidth(150);
    m_rail->setStyleSheet(QStringLiteral(
        "#CharShotRail { background: %1;"
        "  border-right: 1px solid %2; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.border)));

    auto* railLayout = new QVBoxLayout(m_rail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);

    // Shared style for rail buttons — large icons
    QString railBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 42px;"
        "  padding: 12px 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:pressed { background: %5; color: white; }"
        "QPushButton:checked { background: %6; color: %7; }")
        .arg(m.radiusXl)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    m_railGroup = new QButtonGroup(this);
    m_railGroup->setExclusive(true);

    struct RailEntry { const char* icon; const char* label; const char* tip; };
    RailEntry entries[] = {
        {"\U0001F4DA", "LIBRARY",  "Browse, search & download characters"},
        {"\U0001F3AD", "CUSTOM",   "Create custom characters from 4 images"},
        {"\U0001F504", "CONVERT",  "Manage Spine animation video conversion"},
        {"\U0001F3AC", "COMPOSE",  "Build shots from characters & backgrounds"},
        {"\u2699",     "SETTINGS", "Character & shot settings"},
    };

    for (int i = 0; i < 5; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(entries[i].icon));
        btn->setToolTip(QString::fromUtf8(entries[i].tip));
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setStyleSheet(railBtnStyle);
        m_railButtons[i] = btn;
        m_railGroup->addButton(btn, i);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(QString::fromUtf8(entries[i].label));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: %1px; color: %2; font-weight: 800;")
            .arg(Theme::typography().sizeCaption)
            .arg(Theme::rgb(c.textPrimary)));
        m_railLabels[i] = lbl;
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        if (i < 4)
            addRailDivider(railLayout);
    }

    railLayout->addStretch();
    rootLayout->addWidget(m_rail);

    // ── Letter side panel (A-Z navigation, pop-out column) ──────────────
    buildLetterSidePanel();
    rootLayout->addWidget(m_letterSidePanel);

    // ── Content stack (fills remaining width) ────────────────────────────
    m_contentStack = new QStackedWidget;
    m_contentStack->setStyleSheet(QStringLiteral(
        "QStackedWidget { background: %1; }")
        .arg(Theme::hex(c.surface1)));
    rootLayout->addWidget(m_contentStack, 1);

    // ── Page 0: LIBRARY — CharacterBrowser ──────────────────────────────
    m_characterBrowser = new CharacterBrowser(this);
    m_contentStack->addWidget(m_characterBrowser);   // index 0

    // ── Page 1: PUPPETS — PNG puppet creator/manager ────────────────────
    m_puppetLibraryPanel = new PuppetLibraryPanel(this);
    m_contentStack->addWidget(m_puppetLibraryPanel); // index 1

    // ── Page 2: CONVERT — ConversionPanel ────────────────────────
    m_conversionPanel = new ConversionPanel(this);
    m_contentStack->addWidget(m_conversionPanel);    // index 2

    // ── Page 3: COMPOSE — ShotComposer ──────────────────────────────────
    m_shotComposer = new ShotComposer(this);
    m_contentStack->addWidget(m_shotComposer);       // index 3

    // ── Shots column (sidebar between rail and content, COMPOSE-only) ──
    m_shotsColumnWidget = m_shotComposer->shotsColumn();
    if (m_shotsColumnWidget) {
        rootLayout->insertWidget(1, m_shotsColumnWidget); // rail(0) → shots(1) → letter(2) → content(3)
        m_shotsColumnWidget->setVisible(false);           // hidden until COMPOSE is selected
    }

    // ── Character filter column (between rail and shots, COMPOSE-only) ──
    m_charFilterWidget = m_shotComposer->charFilterColumn();
    if (m_charFilterWidget) {
        rootLayout->insertWidget(1, m_charFilterWidget);  // rail(0) → charFilter(1) → shots(2) → letter(3) → content(4)
        m_charFilterWidget->setVisible(false);
    }

    // ── Show filter column (between rail and character filter, COMPOSE-only) ──
    m_showFilterWidget = m_shotComposer->showFilterColumn();
    if (m_showFilterWidget) {
        rootLayout->insertWidget(1, m_showFilterWidget); // rail(0) → show(1) → charFilter(2) → shots(3) → letter(4) → content(5)
        m_showFilterWidget->setVisible(false);
    }

    // ── Page 4: SETTINGS ────────────────────────────────────────
    m_contentStack->addWidget(createSettingsPage());  // index 4

    // Wire rail buttons to page switching
    connect(m_railGroup, &QButtonGroup::idClicked,
            this, [this](int id) {
        m_contentStack->setCurrentIndex(id);
        static const char* modeNames[] = {"LIBRARY", "CUSTOM", "CONVERT", "COMPOSE", "SETTINGS"};
        if (id >= 0 && id < 5)
            spdlog::debug("CharacterShotPanel: switched to {}", modeNames[id]);
        // Auto show/hide letter nav when switching to/from Library or Custom
        // (both have an A-Z–navigable character list). Start each visit with a
        // clean A-Z selection so a stale letter from another page doesn't leave
        // the CUSTOM list filtered behind the scenes.
        if (id == Library || id == Puppets) {
            if (!m_letterPanelVisible)
                showLetterPanel();
            if (m_activeLetterBtn) { m_activeLetterBtn->setChecked(false); m_activeLetterBtn = nullptr; }
        } else {
            hideLetterPanel();
        }
        if (id != Puppets && m_puppetLibraryPanel)
            m_puppetLibraryPanel->filterByLetter(QChar());  // drop any CUSTOM filter
        // Show shots column only in COMPOSE mode
        if (m_shotsColumnWidget)
            m_shotsColumnWidget->setVisible(id == Compose);
        if (m_charFilterWidget)
            m_charFilterWidget->setVisible(id == Compose);
        if (m_showFilterWidget)
            m_showFilterWidget->setVisible(id == Compose);

        // Auto-refresh conversion table when switching to Convert page
        if (id == Convert && m_conversionPanel)
            m_conversionPanel->refreshTable();
    });

    // Start on LIBRARY page
    m_railButtons[0]->setChecked(true);
    m_contentStack->setCurrentIndex(0);

    // Auto-show letter panel since Library is the default page
    showLetterPanel();

    // When a character is deleted from LIBRARY, refresh both the COMPOSE
    // character library and the shot list (which also rebuilds the character
    // filter column) so deleted characters disappear from the filter.
    connect(m_characterBrowser, &CharacterBrowser::deleteRequested,
            this, [this](const QString&) {
        if (m_shotComposer) {
            m_shotComposer->clearCharacterThumbCache();
            m_shotComposer->refreshCharacterLibrary();
            m_shotComposer->refreshShotList();
        }
    });

    // When a character is downloaded/added, also refresh COMPOSE characters
    // and rebuild the character filter list so newly downloaded characters
    // appear immediately in the COMPOSE sidebar filter.
    connect(m_characterBrowser, &CharacterBrowser::downloadRequested,
            this, [this](const QString&) {
        if (m_shotComposer) {
            m_shotComposer->clearCharacterThumbCache();
            m_shotComposer->refreshCharacterLibrary();
            m_shotComposer->refreshShotList();
        }
    });

    // When conversions finish, refresh COMPOSE so new thumbnails appear
    connect(m_conversionPanel, &ConversionPanel::conversionsFinished,
            this, [this]() {
        if (m_shotComposer) {
            m_shotComposer->clearCharacterThumbCache();
            m_shotComposer->refreshCharacterLibrary();
            m_shotComposer->refreshShotList();
        }
    });

    // When a puppet is created/edited/deleted in the PUPPETS sub-page, refresh
    // the COMPOSE Puppets library tab so it appears there immediately.
    if (m_puppetLibraryPanel) {
        connect(m_puppetLibraryPanel, &PuppetLibraryPanel::puppetsChanged,
                this, [this]() {
            if (m_shotComposer)
                m_shotComposer->refreshPuppetLibrary();
        });
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Settings page
// ═════════════════════════════════════════════════════════════════════════════

QWidget* CharacterShotPanel::createSettingsPage()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(m.spacingXxl * 2, m.spacingXxl * 2,
                               m.spacingXxl * 2, m.spacingXxl * 2);
    layout->setSpacing(m.spacingXl);
    layout->setAlignment(Qt::AlignTop);

    // Title
    auto* title = new QLabel("\u2699  Character & Shot Settings");
    title->setStyleSheet(QStringLiteral(
        "font-size: 28px; font-weight: bold; color: %1;")
        .arg(Theme::rgb(c.textPrimary)));
    layout->addWidget(title);

    // Separator
    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setFixedHeight(1);
    sep->setStyleSheet(QStringLiteral(
        "background: %1; border: none;")
        .arg(Theme::rgb(c.border)));
    layout->addWidget(sep);

    // Placeholder content
    auto* placeholder = new QLabel(
        "Character model paths, shot presets directory,\n"
        "default animations, and other settings will appear here.");
    placeholder->setAlignment(Qt::AlignCenter);
    placeholder->setStyleSheet(QStringLiteral(
        "font-size: 18px; color: %1; padding: 60px;")
        .arg(Theme::rgb(c.textTertiary)));
    layout->addWidget(placeholder);

    layout->addStretch();

    return page;
}

// ═════════════════════════════════════════════════════════════════════════════
// Letter side panel  (A-Z pop-out column, mirrors AudioSync side panel)
// ═════════════════════════════════════════════════════════════════════════════

void CharacterShotPanel::buildLetterSidePanel()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    m_letterSidePanel = new QWidget;
    m_letterSidePanel->setObjectName("LetterSidePanel");
    m_letterSidePanel->setMinimumWidth(0);
    m_letterSidePanel->setMaximumWidth(0);
    m_letterSidePanel->setVisible(false);
    m_letterSidePanel->setStyleSheet(QStringLiteral(
        "#LetterSidePanel {"
        "  background: %1;"
        "  border-right: 1px solid %2;"
        "}")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight)));

    auto* panelLayout = new QVBoxLayout(m_letterSidePanel);
    panelLayout->setContentsMargins(2, 4, 2, 4);
    panelLayout->setSpacing(0);

    // Button style — tall buttons that stretch to fill the bar vertically.
    // No fixed height so they distribute evenly across the panel height.
    QString btnStyle = QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; color: %2; font-size: 17px;"
        "  font-weight: bold; padding: 0; margin: 0; }"
        "QPushButton:hover { background: %3; color: %4; }"
        "QPushButton:checked { background: %5; color: %6; }")
        .arg(m.radiusSm)
        .arg(Theme::rgb(c.textSecondary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.accent));

    // # plus A-Z
    const char* letters[] = {
        "#", "A","B","C","D","E","F","G","H","I","J","K","L","M",
        "N","O","P","Q","R","S","T","U","V","W","X","Y","Z"
    };

    m_letterButtons.reserve(27);
    for (int i = 0; i < 27; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(letters[i]));
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(btnStyle);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        QChar letter = (i == 0) ? QChar('#') : QChar('A' + (i - 1));
        connect(btn, &QPushButton::clicked, this, [this, btn, letter]() {
            const int page = m_contentStack ? m_contentStack->currentIndex() : Library;

            // Re-clicking the active letter clears it. A checkable button has
            // already toggled itself off by the time clicked() fires, so an
            // unchecked active button means "deselect" → clear the CUSTOM filter.
            if (m_activeLetterBtn == btn && !btn->isChecked()) {
                m_activeLetterBtn = nullptr;
                if (m_puppetLibraryPanel)
                    m_puppetLibraryPanel->filterByLetter(QChar());  // show all
                return;
            }

            // Uncheck previous active
            if (m_activeLetterBtn && m_activeLetterBtn != btn)
                m_activeLetterBtn->setChecked(false);
            m_activeLetterBtn = btn;
            btn->setChecked(true);

            // Route to whichever character list is currently showing.
            if (page == Puppets) {
                if (m_puppetLibraryPanel)
                    m_puppetLibraryPanel->filterByLetter(letter);
            } else if (m_characterBrowser) {
                m_characterBrowser->scrollToLetter(letter);
            }
        });

        // Each button gets equal stretch so they fill the bar vertically
        panelLayout->addWidget(btn, 1);
        m_letterButtons.push_back(btn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Animated show / hide / toggle  (follows AudioSync pattern)
// ─────────────────────────────────────────────────────────────────────────────

void CharacterShotPanel::showLetterPanel()
{
    if (!m_letterSidePanel) return;

    constexpr int kTargetWidth = 56;

    m_letterPanelVisible = true;

    m_letterSidePanel->setMinimumWidth(0);
    m_letterSidePanel->setMaximumWidth(0);
    m_letterSidePanel->setVisible(true);

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_letterSidePanel, "minimumWidth");
    animMin->setDuration(150);
    animMin->setStartValue(0);
    animMin->setEndValue(kTargetWidth);
    animMin->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_letterSidePanel, "maximumWidth");
    animMax->setDuration(150);
    animMax->setStartValue(0);
    animMax->setEndValue(kTargetWidth);
    animMax->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_letterSidePanel->setMinimumWidth(56);
        m_letterSidePanel->setMaximumWidth(56);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void CharacterShotPanel::hideLetterPanel()
{
    if (!m_letterSidePanel) return;
    if (!m_letterPanelVisible) return;

    m_letterPanelVisible = false;

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_letterSidePanel, "minimumWidth");
    animMin->setDuration(120);
    animMin->setStartValue(m_letterSidePanel->minimumWidth());
    animMin->setEndValue(0);
    animMin->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_letterSidePanel, "maximumWidth");
    animMax->setDuration(120);
    animMax->setStartValue(m_letterSidePanel->maximumWidth());
    animMax->setEndValue(0);
    animMax->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_letterSidePanel->setVisible(false);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace rt
