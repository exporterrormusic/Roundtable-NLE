// CharacterBrowserUI.cpp - UI construction (extracted from CharacterBrowser.cpp).

#include "panels/characters/CharacterBrowser.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QAbstractItemView>

class UpwardComboBox : public QComboBox
{
public:
    using QComboBox::QComboBox;
    void showPopup() override
    {
        QComboBox::showPopup();
        QWidget* popup = view()->parentWidget();
        if (popup) {
            QPoint above = mapToGlobal(QPoint(0, 0));
            above.setY(above.y() - popup->height());
            if (above.y() < 0) above.setY(0);  // don't go above the screen top
            popup->move(above);
        }
    }
};


namespace rt {

void CharacterBrowser::setupUI()
{
    const auto& m = Theme::metrics();
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(m.spacingLg, m.spacingLg, m.spacingLg, m.spacingLg);
    layout->setSpacing(10);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(3);

    auto* leftPanel = createLeftPanel();
    leftPanel->setMinimumWidth(280);
    m_splitter->addWidget(leftPanel);
    m_splitter->addWidget(createRightPanel());

    m_splitter->setStretchFactor(0, 1);  // List
    m_splitter->setStretchFactor(1, 3);  // Preview
    m_splitter->setSizes({320, 800});

    layout->addWidget(m_splitter);
}

QWidget* CharacterBrowser::createLeftPanel()
{
    const auto& m = Theme::metrics();
    const auto& c = Theme::colors();
    auto* panel = new QWidget;
    panel->setObjectName("LeftCard");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(m.spacingMd);

    // â”€â”€ Title with character count â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_titleLabel = new QLabel(QStringLiteral("\xF0\x9F\x91\xA5  Characters"));
    m_titleLabel->setObjectName("PanelTitle");
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: bold; color: %1; padding: 4px 0;")
        .arg(Theme::rgb(c.accent)));
    layout->addWidget(m_titleLabel);

    // â”€â”€ Search â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_searchField = new QLineEdit;
    m_searchField->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D  Search by name..."));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setObjectName("SearchField");
    m_searchField->setMinimumWidth(180);
    m_searchField->setMinimumHeight(44);
    m_searchField->setStyleSheet(QStringLiteral(
        "QLineEdit { font-size: %6px; padding: 10px 14px; border-radius: %1px;"
        "  background: %2; color: %3; border: 1px solid %4; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface1), Theme::rgb(c.textPrimary),
             Theme::rgb(c.border), Theme::rgb(c.accent))
        .arg(Theme::typography().sizeSmall));
    connect(m_searchField, &QLineEdit::textChanged,
            this, &CharacterBrowser::onSearchChanged);
    layout->addWidget(m_searchField);

    // â”€â”€ Category filter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(m.spacingMd);
    m_categoryLabel = new QLabel("Category:");
    m_categoryLabel->setObjectName("FieldLabel");
    m_categoryLabel->setStyleSheet(QStringLiteral(
        "font-size: %1px; color: %2;")
        .arg(Theme::typography().sizeSmall)
        .arg(Theme::rgb(c.textSecondary)));
    filterRow->addWidget(m_categoryLabel);
    m_categoryFilter = new QComboBox;
    m_categoryFilter->setObjectName("FilterCombo");
    m_categoryFilter->addItems({"All", "Character", "NPC", "Background", "Other"});
    m_categoryFilter->setToolTip("Filter characters by category type");
    m_categoryFilter->setMinimumHeight(36);
    m_categoryFilter->setStyleSheet(QStringLiteral(
        "QComboBox { font-size: %1px; padding: 6px 12px; }")
        .arg(Theme::typography().sizeSmall));
    connect(m_categoryFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CharacterBrowser::onCategoryChanged);
    filterRow->addWidget(m_categoryFilter);
    layout->addLayout(filterRow);

    // â”€â”€ Downloaded Only checkbox â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_downloadedOnly = new QCheckBox(QStringLiteral("\xE2\x9C\x94  Downloaded Only"));
    m_downloadedOnly->setObjectName("FilterCheck");
    m_downloadedOnly->setChecked(false);
    m_downloadedOnly->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    m_downloadedOnly->setToolTip("Show only characters that are downloaded locally");
    connect(m_downloadedOnly, &QCheckBox::toggled,
            this, &CharacterBrowser::onDownloadedOnlyToggled);
    layout->addWidget(m_downloadedOnly);

    // â”€â”€ Show Hidden checkbox â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_showHidden = new QCheckBox(QStringLiteral("\xF0\x9F\x91\x81  Show Hidden"));
    m_showHidden->setObjectName("FilterCheck");
    m_showHidden->setChecked(false);
    m_showHidden->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    m_showHidden->setToolTip("Show permanently hidden characters");
    connect(m_showHidden, &QCheckBox::toggled,
            this, &CharacterBrowser::onShowHiddenToggled);
    layout->addWidget(m_showHidden);

    // â”€â”€ Character list â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_characterList = new QListWidget;
    m_characterList->setObjectName("CharacterList");
    m_characterList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_characterList->setDragEnabled(false);
    m_characterList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_characterList->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: 1px solid %2; border-radius: %3px;"
        "  font-size: %7px; outline: none; }"
        "QListWidget::item { padding: 10px 12px; border-bottom: 1px solid %4;"
        "  min-height: 24px; }"
        "QListWidget::item:last { border-bottom: none; }"
        "QListWidget::item:selected { background: %5; }"
        "QListWidget::item:hover { background: %6; }")
        .arg(Theme::rgb(c.surface0), Theme::rgb(c.border),
             QString::number(m.radiusMd), Theme::rgb(c.borderLight),
             Theme::rgb(c.accentDim), Theme::rgb(c.surface2))
        .arg(Theme::typography().sizeSmall));
    connect(m_characterList, &QListWidget::itemSelectionChanged,
            this, &CharacterBrowser::onCharacterSelectionChanged);
    connect(m_characterList, &QWidget::customContextMenuRequested,
            this, &CharacterBrowser::onContextMenu);
    connect(m_characterList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { populateControls(); });
    layout->addWidget(m_characterList, 1);

    // â”€â”€ Action buttons â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(m.spacingSm);
    m_refreshBtn = new QPushButton(QStringLiteral("\xF0\x9F\x94\x84  Refresh"));
    m_refreshBtn->setObjectName("GhostBtn");
    m_refreshBtn->setMinimumHeight(44);
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %1px; font-weight: 600; padding: 8px 16px; }")
        .arg(Theme::typography().sizeSmall));
    m_refreshBtn->setToolTip("Rescan local files and fetch remote character list (F5)");
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &CharacterBrowser::onRefreshClicked);
    btnRow->addWidget(m_refreshBtn);

    m_downloadBtn = new QPushButton(QStringLiteral("\xE2\xAC\x87\xEF\xB8\x8F  Download"));
    m_downloadBtn->setObjectName("PrimaryBtn");
    m_downloadBtn->setEnabled(false);
    m_downloadBtn->setMinimumHeight(44);
    m_downloadBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %1px; font-weight: 600; padding: 8px 16px; }")
        .arg(Theme::typography().sizeSmall));
    m_downloadBtn->setToolTip("Download the selected character(s) from Nikke DB (Ctrl+D)");
    connect(m_downloadBtn, &QPushButton::clicked,
            this, &CharacterBrowser::onDownloadClicked);
    btnRow->addWidget(m_downloadBtn);

    m_deleteBtn = new QPushButton(QStringLiteral("\xF0\x9F\x97\x91\xEF\xB8\x8F  Delete"));
    m_deleteBtn->setObjectName("DangerBtn");
    m_deleteBtn->setEnabled(false);
    m_deleteBtn->setMinimumHeight(44);
    m_deleteBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: %1px; font-weight: 600; padding: 8px 16px; }")
        .arg(Theme::typography().sizeSmall));
    m_deleteBtn->setToolTip("Delete local files for the selected character(s) (Del)");
    connect(m_deleteBtn, &QPushButton::clicked,
            this, &CharacterBrowser::onDeleteClicked);
    btnRow->addWidget(m_deleteBtn);

    layout->addLayout(btnRow);

    // â”€â”€ Download progress â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_downloadProgress = new QProgressBar;
    m_downloadProgress->setObjectName("DownloadProgress");
    m_downloadProgress->setRange(0, 100);
    m_downloadProgress->setValue(0);
    m_downloadProgress->setVisible(false);
    m_downloadProgress->setMaximumHeight(18);
    layout->addWidget(m_downloadProgress);

    return panel;
}

QWidget* CharacterBrowser::createRightPanel()
{
    const auto& m = Theme::metrics();
    auto* panel = new QWidget;
    panel->setObjectName("RightPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // â”€â”€ Preview area (stretch=1, fills most of right panel) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_previewArea = new QWidget;
    m_previewArea->setObjectName("PreviewArea");
    m_previewArea->setMinimumSize(400, 300);

    auto* previewLayout = new QVBoxLayout(m_previewArea);
    previewLayout->setContentsMargins(0, 0, 0, 0);

#ifdef ROUNDTABLE_HAS_SPINE
    m_spinePreview = new SpinePreviewWidget(m_previewArea);
    m_spinePreview->setBackgroundColor(Theme::colors().surface0);
    previewLayout->addWidget(m_spinePreview);
#else
    auto* previewLabel = new QLabel("Character Preview\n(Spine support not available)");
    previewLabel->setAlignment(Qt::AlignCenter);
    previewLabel->setObjectName("PreviewPlaceholder");
    previewLayout->addWidget(previewLabel);
#endif

    layout->addWidget(m_previewArea, 1);

    // â”€â”€ Controls bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* controls = new QFrame;
    controls->setObjectName("ControlsBar");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(m.spacingXl, m.spacingLg, m.spacingXl, m.spacingLg);
    controlsLayout->setSpacing(m.spacingMd);

    auto* outfitLabel = new QLabel("Outfit:");
    outfitLabel->setObjectName("ControlLabel");
    outfitLabel->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    controlsLayout->addWidget(outfitLabel);
    m_outfitCombo = new UpwardComboBox;
    m_outfitCombo->setObjectName("OutfitCombo");
    m_outfitCombo->setMinimumWidth(140);
    m_outfitCombo->setMinimumHeight(36);
    m_outfitCombo->setStyleSheet(QStringLiteral("font-size: %1px; padding: 4px 8px;")
        .arg(Theme::typography().sizeSmall));
    m_outfitCombo->setToolTip("Select character outfit variant");
    m_outfitCombo->addItem("default");
    connect(m_outfitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CharacterBrowser::onOutfitChanged);
    controlsLayout->addWidget(m_outfitCombo);

    controlsLayout->addSpacing(m.spacingLg);

    auto* stanceLabel = new QLabel("Stance:");
    stanceLabel->setObjectName("ControlLabel");
    stanceLabel->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    controlsLayout->addWidget(stanceLabel);
    m_stanceCombo = new UpwardComboBox;
    m_stanceCombo->setObjectName("StanceCombo");
    m_stanceCombo->setMinimumWidth(100);
    m_stanceCombo->setMinimumHeight(36);
    m_stanceCombo->setStyleSheet(QStringLiteral("font-size: %1px; padding: 4px 8px;")
        .arg(Theme::typography().sizeSmall));
    m_stanceCombo->setToolTip("Select character stance (Default, Aim, Cover)");
    m_stanceCombo->addItems({"Default", "Aim", "Cover"});
    connect(m_stanceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CharacterBrowser::onStanceChanged);
    controlsLayout->addWidget(m_stanceCombo);

    controlsLayout->addSpacing(m.spacingLg);

    auto* animLabel = new QLabel("Animation:");
    animLabel->setObjectName("ControlLabel");
    animLabel->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    controlsLayout->addWidget(animLabel);
    m_animationCombo = new UpwardComboBox;
    m_animationCombo->setObjectName("AnimCombo");
    m_animationCombo->setMinimumWidth(140);
    m_animationCombo->setMinimumHeight(36);
    m_animationCombo->setStyleSheet(QStringLiteral("font-size: %1px; padding: 4px 8px;")
        .arg(Theme::typography().sizeSmall));
    m_animationCombo->setToolTip("Select body animation to play");
    m_animationCombo->addItem("idle");
    connect(m_animationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CharacterBrowser::onAnimationChanged);
    controlsLayout->addWidget(m_animationCombo);

    controlsLayout->addSpacing(m.spacingLg);

    m_talkingCheck = new QCheckBox(QStringLiteral("\xF0\x9F\x92\xAC  Talking"));
    m_talkingCheck->setObjectName("TalkingCheck");
    m_talkingCheck->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    m_talkingCheck->setToolTip("Toggle mouth/talking animation");
    connect(m_talkingCheck, &QCheckBox::toggled,
            this, &CharacterBrowser::onTalkingChanged);
    controlsLayout->addWidget(m_talkingCheck);

    controlsLayout->addStretch();

    // â”€â”€ Status â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setObjectName("StatusLabel");
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: %1px;")
        .arg(Theme::typography().sizeSmall));
    controlsLayout->addWidget(m_statusLabel);

    layout->addWidget(controls);

    return panel;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Population helpers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

} // namespace rt
