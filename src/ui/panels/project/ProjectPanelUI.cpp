/*
 * ProjectPanelUI.cpp — setupUI() extracted from ProjectPanel.cpp.
 *
 * Builds the entire ProjectPanel layout: icon rail, side panels
 * (NEW / OPEN / SETTINGS), content area with project table, and
 * bottom action bar.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QPainter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "Settings.h"

namespace rt {

// ── Helper: draw a simple folder icon pixmap ──────────────────────────────
QIcon createFolderIcon()
{
    // Use warm/neutral tones that work regardless of theme
    static const QColor kTab(190, 160, 80);
    static const QColor kBody(210, 190, 110);

    QPixmap pix(24, 20);
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Folder tab (small rectangle at top-left)
        p.setPen(Qt::NoPen);
        p.setBrush(kTab);
        p.drawRect(4, 3, 7, 4);

        // Folder body (main rectangle)
        p.setBrush(kBody);
        p.drawRect(1, 6, 22, 13);

        // Subtle highlight line on top edge of body
        p.setPen(QColor(230, 215, 150));
        p.drawLine(1, 6, 22, 6);
    }
    return QIcon(pix);
}

// =============================================================================
// UI Setup  (assembler; sections built by build* helpers)
// =============================================================================

void ProjectPanel::setupUI()
{
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    buildIconRail();
    rootLayout->addWidget(m_iconRail);

    buildSidePanel();
    rootLayout->addWidget(m_sidePanel);

    buildContentArea();
    rootLayout->addWidget(m_contentArea, 1);
}

void ProjectPanel::buildIconRail()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    m_iconRail = new QWidget;
    m_iconRail->setObjectName("IconRail");
    m_iconRail->setFixedWidth(150);
    m_iconRail->setStyleSheet(QStringLiteral(
        "#IconRail { background: %1;"
        "  border-right: 1px solid %2; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.border)));

    auto* railLayout = new QVBoxLayout(m_iconRail);
    railLayout->setContentsMargins(8, m.spacingXl, 8, m.spacingXl);
    railLayout->setSpacing(0);

    // Shared style for rail buttons - MASSIVE icons (matches main nav rail)
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

    // Divider between rail entries — single container widget wrapping
    // a 1px line to prevent DPI sub-pixel drift.
    auto addRailDivider = [&]() {
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
        railLayout->addWidget(div);
    };

    struct RailEntry { const char* icon; const char* label; const char* tip; bool checkable; };
    RailEntry entries[] = {
        {"\U0001F195", "NEW",      "Create a new project (Ctrl+N)", true},
        {"\U0001F4C2", "OPEN",     "Open project from file",        true},
        {"\U0001F4BE", "SAVE",     "Save current project (Ctrl+S)", false},
        {"\U0001F4E5", "IMPORT",   "Import a project file",         false},
        {"\u2699",     "SETTINGS", "Project settings",              true},
    };

    QPushButton* railBtns[5]{};
    for (int i = 0; i < 5; ++i) {
        auto* btn = new QPushButton(QString::fromUtf8(entries[i].icon));
        btn->setToolTip(QString::fromUtf8(entries[i].tip));
        btn->setFixedSize(128, 84);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(entries[i].checkable);
        btn->setStyleSheet(railBtnStyle);
        railLayout->addWidget(btn, 0, Qt::AlignHCenter);

        railLayout->addSpacing(4);

        auto* lbl = new QLabel(QString::fromUtf8(entries[i].label));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFixedHeight(20);
        lbl->setStyleSheet(QStringLiteral(
            "font-size: 15px; color: %1; font-weight: 800;")
            .arg(Theme::rgb(c.textPrimary)));
        railLayout->addWidget(lbl, 0, Qt::AlignHCenter);

        if (i < 4)
            addRailDivider();
        railBtns[i] = btn;
    }

    m_newBtn      = railBtns[0];
    m_openFileBtn = railBtns[1];
    m_saveBtn     = railBtns[2];
    m_importBtn   = railBtns[3];
    m_settingsBtn = railBtns[4];

    connect(m_newBtn, &QPushButton::clicked,
            this, [this]() { toggleSidePanel(SidePanelMode::New); });
    connect(m_openFileBtn, &QPushButton::clicked,
            this, [this]() { toggleSidePanel(SidePanelMode::Open); });
    connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
        hideSidePanel();
        emit saveRequested();
    });
    connect(m_importBtn, &QPushButton::clicked, this, [this]() {
        hideSidePanel();
        auto settings = rt::appSettings();
        QString lastDir = settings.value("Import/lastDir", QString()).toString();
        if (lastDir.isEmpty())
            lastDir = QDir::homePath();
        QString path = QFileDialog::getOpenFileName(
            this, "Import Project", lastDir,
            "ROUNDTABLE Projects (*.rtp);;All Files (*)");
        if (!path.isEmpty()) {
            QString dir = QFileInfo(path).absolutePath();
            settings.setValue("Import/lastDir", dir);
            settings.sync();
            emit importProject(path);
        }
    });
    connect(m_settingsBtn, &QPushButton::clicked,
            this, [this]() { toggleSidePanel(SidePanelMode::Settings); });

    railLayout->addStretch();
}

void ProjectPanel::buildContentArea()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();
    m_contentArea = new QWidget;
    m_contentArea->setObjectName("ContentArea");
    m_contentArea->setStyleSheet(QStringLiteral(
        "#ContentArea { background: %1; }")
        .arg(Theme::rgb(c.surface1)));
    auto* contentLayout = new QVBoxLayout(m_contentArea);
    contentLayout->setContentsMargins(m.spacingMd, m.spacingMd,
                                      m.spacingMd, m.spacingMd);
    contentLayout->setSpacing(m.spacingMd);

    // -- Search + Sort bar ------------------------------------------------
    auto* searchBar = new QHBoxLayout;
    searchBar->setSpacing(m.spacingMd);

    m_searchInput = new QLineEdit;
    m_searchInput->setPlaceholderText(
        QStringLiteral("\U0001F50D  Search projects..."));
    m_searchInput->setClearButtonEnabled(true);
    m_searchInput->setMinimumWidth(400);
    m_searchInput->setMinimumHeight(56);
    m_searchInput->setStyleSheet(QStringLiteral(
        "QLineEdit { font-size: 22px; padding: 12px 18px; }"));
    connect(m_searchInput, &QLineEdit::textChanged,
            this, &ProjectPanel::onSearchTextChanged);
    searchBar->addWidget(m_searchInput, 1);

    m_sortCombo = new QComboBox;
    m_sortCombo->addItem("Newest First");
    m_sortCombo->addItem("Oldest First");
    m_sortCombo->addItem("Name A\u2013Z");
    m_sortCombo->addItem("Name Z\u2013A");
    m_sortCombo->setFixedWidth(280);
    m_sortCombo->setMinimumHeight(56);
    m_sortCombo->setStyleSheet(QStringLiteral(
        "QComboBox { font-size: 20px; padding: 6px 14px; }"));
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ProjectPanel::onSortChanged);
    searchBar->addWidget(m_sortCombo);

    m_refreshBtn = new QPushButton(QStringLiteral("\U0001F504"));
    m_refreshBtn->setToolTip("Refresh project list (F5)");
    m_refreshBtn->setObjectName("GhostBtn");
    m_refreshBtn->setFixedSize(56, 56);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setStyleSheet(QStringLiteral(
        "QPushButton { font-size: 28px; }"));
    searchBar->addWidget(m_refreshBtn);

    contentLayout->addLayout(searchBar);

    // -- Project Table ----------------------------------------------------
    m_projectTable = new QTableWidget(0, 6);
    m_projectTable->setObjectName("ProjectTable");
    m_projectTable->setHorizontalHeaderLabels(
        {"", "NAME", "RESOLUTION", "FPS", "SIZE", "MODIFIED"});
    m_projectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_projectTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_projectTable->setAlternatingRowColors(true);
    m_projectTable->setShowGrid(true);
    m_projectTable->setWordWrap(false);
    m_projectTable->verticalHeader()->setVisible(false);
    m_projectTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_projectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_projectTable->setFocusPolicy(Qt::StrongFocus);

    // Column sizing - all interactive (user-resizable)
    auto* hh = m_projectTable->horizontalHeader();
    hh->setSectionsMovable(true);
    hh->setSectionsClickable(true);
    hh->setSectionResizeMode(QHeaderView::Interactive);     // all columns
    m_projectTable->setColumnWidth(0, 290);
    m_projectTable->setColumnWidth(1, 400);                 // name - wide default
    m_projectTable->setColumnWidth(2, 220);
    m_projectTable->setColumnWidth(3, 140);
    m_projectTable->setColumnWidth(4, 180);
    m_projectTable->setColumnWidth(5, 300);
    hh->setDefaultAlignment(Qt::AlignCenter | Qt::AlignVCenter);
    // NAME header stays left-aligned with padding
    auto* nameHeaderItem = new QTableWidgetItem("NAME");
    nameHeaderItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_projectTable->setHorizontalHeaderItem(1, nameHeaderItem);
    hh->setHighlightSections(false);
    hh->setStretchLastSection(true);                        // last col fills remainder
    m_projectTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont headerFont(t.fontFamily, 18, t.weightBold);
    hh->setFont(headerFont);
    hh->setFixedHeight(52);

    // Set minimum column widths based on widest header label
    int minSectionW;
    int minLastColW;
    {
        QFontMetrics fm(headerFont);
        QStringList labels = {"THUMB", "NAME", "RESOLUTION", "FPS", "SIZE", "MODIFIED"};
        int padding = 28;  // header padding + border
        int maxLabelW = 50;
        for (const auto& label : labels) {
            int w = fm.horizontalAdvance(label) + padding;
            if (w > maxLabelW) maxLabelW = w;
        }
        hh->setMinimumSectionSize(maxLabelW);
        minSectionW = maxLabelW;
        // Last column (MODIFIED) needs extra space for date content like "Yesterday 2:30 PM"
        QFont dataFont(t.fontFamily, 22);
        QFontMetrics dfm(dataFont);
        minLastColW = dfm.horizontalAdvance("Yesterday 12:30 PM") + 40;
        minLastColW = qMax(minLastColW, minSectionW);
    }

    // Clamp column resizes so the last column never gets pushed off-screen
    connect(hh, &QHeaderView::sectionResized,
            this, [this, minLastColW, minSectionW](int logicalIndex, int /*oldSize*/, int newSize) {
        auto* hdr = m_projectTable->horizontalHeader();
        int colCount = m_projectTable->columnCount();
        int lastCol = colCount - 1;
        // Don't clamp the stretch (last) column itself
        if (logicalIndex == lastCol) return;

        int viewportW = m_projectTable->viewport()->width();
        // Compute max width this column can be: viewport minus all other non-last columns minus last col minimum
        int otherColumnsW = 0;
        for (int i = 0; i < colCount - 1; ++i) {
            if (i == logicalIndex) continue;
            if (!hdr->isSectionHidden(i))
                otherColumnsW += hdr->sectionSize(i);
        }
        int maxAllowed = viewportW - otherColumnsW - minLastColW;
        maxAllowed = qMax(maxAllowed, minSectionW);  // never below header label width

        if (newSize > maxAllowed) {
            hdr->blockSignals(true);
            m_projectTable->setColumnWidth(logicalIndex, maxAllowed);
            hdr->blockSignals(false);
        }
    });

    // Restore saved header layout from previous session
    {
        auto settings = rt::appSettings();
        QByteArray savedState = settings.value("ProjectPanel/HeaderState").toByteArray();
        if (!savedState.isEmpty())
            hh->restoreState(savedState);
    }

    // Header context menu -> toggle column visibility
    hh->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(hh, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        auto* hdr = m_projectTable->horizontalHeader();
        QMenu menu(this);
        // Columns: 0=thumb, 1=name, 2=resolution, 3=fps, 4=size, 5=modified
        QStringList names = {"", "NAME", "RESOLUTION", "FPS", "SIZE", "MODIFIED"};
        for (int col = 0; col < m_projectTable->columnCount(); ++col) {
            if (col == 1) continue;  // NAME column always visible (it stretches)
            QString label = (col == 0) ? "THUMBNAIL" : names[col];
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(!hdr->isSectionHidden(col));
            connect(action, &QAction::toggled, this, [this, col](bool visible) {
                m_projectTable->horizontalHeader()->setSectionHidden(col, !visible);
            });
        }
        menu.exec(hdr->mapToGlobal(pos));
    });

    // Double-click -> open project
    connect(m_projectTable, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int /*col*/) {
        auto* item = m_projectTable->item(row, 1);
        if (item)
            emit openProject(item->data(Qt::UserRole).toString());
    });

    // Selection changed -> update action buttons
    connect(m_projectTable, &QTableWidget::itemSelectionChanged,
            this, &ProjectPanel::updateActionButtons);

    // Right-click context menu
    connect(m_projectTable, &QWidget::customContextMenuRequested,
            this, &ProjectPanel::showContextMenu);

    // Click on empty area -> deselect; click content area -> close side panel
    m_projectTable->viewport()->installEventFilter(this);

    contentLayout->addWidget(m_projectTable, 1);

    // -- Empty state ------------------------------------------------------
    m_emptyStateWidget = new QWidget;
    auto* emptyLay = new QVBoxLayout(m_emptyStateWidget);
    emptyLay->setAlignment(Qt::AlignCenter);
    emptyLay->setSpacing(m.spacingXxl);

    auto* emptyIcon = new QLabel("\xF0\x9F\x8E\xAC");  // film clapper
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyIcon->setStyleSheet("font-size: 80px;");
    emptyLay->addWidget(emptyIcon);

    m_emptyLabel = new QLabel(
        "No projects yet\nClick \u2795 NEW to create your first project");
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "font-size: 20px; color: %1; line-height: 1.6;")
        .arg(Theme::rgb(c.textSecondary)));
    emptyLay->addWidget(m_emptyLabel);

    m_emptyStateWidget->setVisible(false);
    contentLayout->addWidget(m_emptyStateWidget, 1);

    // -- Bottom Action Bar ------------------------------------------------
    m_actionBar = new QWidget;
    auto* actionBarLayout = new QHBoxLayout(m_actionBar);
    actionBarLayout->setContentsMargins(0, m.spacingMd, 0, 0);
    actionBarLayout->setSpacing(m.spacingMd);

    auto makeActionBtn = [&](const QString& text, const QColor& bg,
                             const QColor& hoverBg, const QColor& fg)
        -> QPushButton*
    {
        auto* btn = new QPushButton(text);
        btn->setMinimumHeight(48);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setEnabled(false);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: %2; border: none;"
            "  border-radius: %3px; font-size: 16px; font-weight: 700;"
            "  padding: 10px 24px; }"
            "QPushButton:hover { background: %4; }"
            "QPushButton:disabled { background: %5; color: %6; opacity: 0.5; }")
            .arg(Theme::rgb(bg))
            .arg(Theme::rgb(fg))
            .arg(m.radiusMd)
            .arg(Theme::rgb(hoverBg))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.textTertiary)));
        return btn;
    };

    actionBarLayout->addStretch();

    m_openActionBtn = makeActionBtn(
        "\u25B6  Open Project", c.primaryBtnBg, c.primaryBtnHover, Qt::white);
    connect(m_openActionBtn, &QPushButton::clicked, this, [this]() {
        QString name = selectedProjectName();
        if (!name.isEmpty()) emit openProject(name);
    });
    actionBarLayout->addWidget(m_openActionBtn);

    m_dupeActionBtn = makeActionBtn(
        "\U0001F4CB  Duplicate", c.surface3, c.surface2, c.textPrimary);
    connect(m_dupeActionBtn, &QPushButton::clicked, this, [this]() {
        QString name = selectedProjectName();
        if (!name.isEmpty()) emit duplicateProject(name);
    });
    actionBarLayout->addWidget(m_dupeActionBtn);

    m_renameActionBtn = makeActionBtn(
        "\u270F\uFE0F  Rename", c.surface3, c.surface2, c.textPrimary);
    connect(m_renameActionBtn, &QPushButton::clicked, this, [this]() {
        QString name = selectedProjectName();
        if (!name.isEmpty()) {
            bool ok = false;
            QString newName = QInputDialog::getText(
                this, "Rename Project",
                "New name:", QLineEdit::Normal, name, &ok);
            newName = newName.trimmed();
            if (ok && !newName.isEmpty() && newName != name)
                emit renameProject(name, newName);
        }
    });
    actionBarLayout->addWidget(m_renameActionBtn);

    m_deleteActionBtn = makeActionBtn(
        "\U0001F5D1  Delete", c.dangerBg, c.dangerText, c.dangerText);
    connect(m_deleteActionBtn, &QPushButton::clicked, this, [this]() {
        QString name = selectedProjectName();
        if (!name.isEmpty()) {
            // Look up the file path from the project info
            QString fpath;
            for (const auto& p : m_allProjects) {
                if (p.name == name) { fpath = p.filePath; break; }
            }
            emit deleteProject(name, fpath);
        }
    });
    actionBarLayout->addWidget(m_deleteActionBtn);

    contentLayout->addWidget(m_actionBar);

    // Install event filter on content area to close side panel on click
    m_contentArea->installEventFilter(this);
}

} // namespace rt
