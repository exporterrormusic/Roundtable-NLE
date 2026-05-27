// BackgroundDownloadUI.cpp — UI construction for BackgroundDownloadPanel.
//
// Layout: [ Thumbnail grid                       ]  (stretch)
//         [ Status label                         ]
//         [▼ Instructions  (collapsible)         ]
//         [  - Instructions text                 ]
//         [  - Open MEGA button                  ]

#include "panels/backgrounds/BackgroundDownloadPanel.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
// UI Setup
// ═════════════════════════════════════════════════════════════════════════════

void BackgroundDownloadPanel::setupUI()
{
    const auto& c = Theme::colors();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ── Search bar ──────────────────────────────────────────────────────
    m_searchEdit = new QLineEdit;
    m_searchEdit->setObjectName("NikkeSearch");
    m_searchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search NikkeBKG\u2026"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; "
        "border-radius: %4px; padding: 6px 10px; font-size: 13px; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(Theme::rgb(c.surface0), Theme::rgb(c.text),
             Theme::rgb(c.border), QString::number(Theme::metrics().radiusMd),
             Theme::rgb(c.accent)));
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &BackgroundDownloadPanel::onSearchChanged);
    layout->addWidget(m_searchEdit);

    // ── Thumbnail grid (takes all available space) ──────────────────────
    m_backgroundGrid = new BackgroundGridWidget;
    m_backgroundGrid->setObjectName("BackgroundGrid");
    m_backgroundGrid->setViewMode(QListView::IconMode);
    m_backgroundGrid->setIconSize(QSize(96, 96));
    m_backgroundGrid->setGridSize(QSize(120, 130));
    m_backgroundGrid->setWordWrap(true);
    m_backgroundGrid->setSpacing(4);
    m_backgroundGrid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_backgroundGrid->setDragEnabled(true);
    m_backgroundGrid->setDragDropMode(QAbstractItemView::DragOnly);
    m_backgroundGrid->setResizeMode(QListView::Adjust);
    m_backgroundGrid->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: 1px solid %2; border-radius: %3px; }"
        "QListWidget::item { padding: 4px; border-radius: 4px; }"
        "QListWidget::item:hover { background: %4; }"
        "QListWidget::item:selected { border: 1px solid %5; background: %6; }")
        .arg(Theme::rgb(c.surface0), Theme::rgb(c.border),
             QString::number(Theme::metrics().radiusMd),
             Theme::rgb(c.surface2),
             Theme::rgb(c.accent),
             Theme::rgb(c.accentDim)));
    layout->addWidget(m_backgroundGrid, 1);
    // Forward double-click from the grid widget to the panel signal
    connect(m_backgroundGrid, &BackgroundGridWidget::backgroundActivated,
            this, &BackgroundDownloadPanel::backgroundActivated);

    // ── Status label ─────────────────────────────────────────────────────
    m_statusLabel = new QLabel(QStringLiteral("No backgrounds found yet"));
    m_statusLabel->setObjectName("StatusLabel");
    m_statusLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1; padding: 2px 0;")
        .arg(Theme::rgb(c.textSecondary)));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    // ── Collapsible toggle button (always visible) ───────────────────────
    m_instructionsToggle = new QPushButton(QStringLiteral("\u25B6  Show download options"));
    m_instructionsToggle->setObjectName("GhostBtn");
    m_instructionsToggle->setCheckable(true);
    m_instructionsToggle->setChecked(false);
    m_instructionsToggle->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 12px; font-weight: 500; padding: 4px 8px; "
        "text-align: left; border: none; color: %1; }"
        "QPushButton:hover { color: %2; }")
        .arg(Theme::rgb(c.textSecondary), Theme::rgb(c.textPrimary)));
    connect(m_instructionsToggle, &QPushButton::clicked,
            this, &BackgroundDownloadPanel::toggleInstructions);

    // ── Collapsible container (instructions + buttons) ──────────────────
    m_instructionsContainer = new QWidget(this);
    m_instructionsContainer->setObjectName("InstructionsContainer");
    auto* containerLayout = new QVBoxLayout(m_instructionsContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(8);

    // Instructions text
    m_instructionsLabel = new QLabel(QStringLiteral(
        "1. Click \"Open MEGA\" to open the folder in your browser.\n"
        "2. Click \"Download\" \u2192 \"Standard Download\" on the MEGA page.\n"
        "3. Place the downloaded files into:\n"
        "      assets/NikkeBKG/\n"
        "\n"
        "New files are detected automatically."));
    m_instructionsLabel->setWordWrap(true);
    m_instructionsLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1; line-height: 1.5; padding: 8px; "
        "background: %2; border-radius: %3px;")
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.surface1))
        .arg(QString::number(Theme::metrics().radiusMd)));
    containerLayout->addWidget(m_instructionsLabel);

    // Button row — just the MEGA link
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);

    m_openMegaBtn = new QPushButton(QStringLiteral("\xF0\x9F\x94\x97  Open MEGA Folder"));
    m_openMegaBtn->setObjectName("PrimaryBtn");
    m_openMegaBtn->setMinimumHeight(44);
    m_openMegaBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 14px; font-weight: 600; padding: 8px 16px; }"));
    m_openMegaBtn->setToolTip("Open the MEGA folder in your browser to download backgrounds");
    connect(m_openMegaBtn, &QPushButton::clicked,
            this, &BackgroundDownloadPanel::onOpenMegaClicked);
    btnRow->addWidget(m_openMegaBtn);

    containerLayout->addLayout(btnRow);

    layout->addWidget(m_instructionsToggle);
    layout->addWidget(m_instructionsContainer);
    m_instructionsContainer->setVisible(false);  // start collapsed
}

// ═════════════════════════════════════════════════════════════════════════════
// Toggle instructions + buttons visibility
// ═════════════════════════════════════════════════════════════════════════════

void BackgroundDownloadPanel::toggleInstructions()
{
    bool visible = m_instructionsToggle->isChecked();
    m_instructionsContainer->setVisible(visible);
    m_instructionsToggle->setText(visible
        ? QStringLiteral("\u25BC  Show download options")
        : QStringLiteral("\u25B6  Show download options"));
}

} // namespace rt
