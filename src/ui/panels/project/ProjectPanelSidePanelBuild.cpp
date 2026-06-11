/*
 * ProjectPanelSidePanelBuild.cpp -- ProjectPanel::buildSidePanel():
 * the inline side panel with NEW / OPEN / SETTINGS stacked pages.
 * Extracted from ProjectPanelUI.cpp::setupUI() (behavior-preserving).
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

void ProjectPanel::buildSidePanel()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();
    const auto& t = Theme::typography();
    m_sidePanel = new QWidget;
    m_sidePanel->setObjectName("SidePanel");
    m_sidePanel->setMinimumWidth(0);
    m_sidePanel->setMaximumWidth(0);
    m_sidePanel->setStyleSheet(QStringLiteral(
        "#SidePanel {"
        "  background: %1;"
        "  border-right: 1px solid %2;"
        "}")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.borderLight)));

    auto* sidePanelLayout = new QVBoxLayout(m_sidePanel);
    sidePanelLayout->setContentsMargins(0, 0, 0, 0);
    sidePanelLayout->setSpacing(0);

    m_sidePanelStack = new QStackedWidget;
    sidePanelLayout->addWidget(m_sidePanelStack);

    // --- NEW page (step-card layout) -------------------------------------
    m_newPage = new QWidget;
    auto* newPageLayout = new QVBoxLayout(m_newPage);
    newPageLayout->setContentsMargins(m.spacingXl, m.spacingXl,
                                      m.spacingXl, m.spacingXl);
    newPageLayout->setSpacing(8);

    // Helper: shared style for step card widgets
    auto cardStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2; padding: 10px 12px;")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border));
    };
    auto stepHeaderStyle = [&]() -> QString {
        return QStringLiteral(
            "font-size: 12px; font-weight: 700; color: %1;"
            " letter-spacing: 0.6px; text-transform: uppercase;")
            .arg(Theme::rgb(c.textPrimary));
    };
    auto chipBtnStyle = [&](bool selected = false) -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: 18px; font-weight: 700;"
            "  padding: 8px 12px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent));
    };
    auto inputStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: 13px; padding: 8px 10px;")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary));
    };
    auto smallInputStyle = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: 13px; padding: 6px 8px;"
            " min-width: 50px; max-width: 60px; text-align: center;")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary));
    };

    // ── Header ───────────────────────────────────────────────────────────
    auto* headerRow = new QHBoxLayout;
    auto* headerTitle = new QLabel("Create New Project");
    headerTitle->setObjectName("NewHeaderTitle");
    headerTitle->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: %1; color: %2;")
        .arg(t.weightBold).arg(Theme::rgb(c.textPrimary)));
    headerRow->addWidget(headerTitle, 1);

    auto* newCloseBtn = new QPushButton("\u2715");
    newCloseBtn->setObjectName("NewCloseBtn");
    newCloseBtn->setFixedSize(26, 26);
    newCloseBtn->setCursor(Qt::PointingHandCursor);
    newCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  font-size: 14px; color: %1; }"
        "QPushButton:hover { background: %2; color: %3; }")
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textPrimary)));
    connect(newCloseBtn, &QPushButton::clicked,
            this, &ProjectPanel::hideSidePanel);
    headerRow->addWidget(newCloseBtn);
    newPageLayout->addLayout(headerRow);

    m_newPage->setObjectName("NewPage");

    // ── Step 1: Project Name ────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setObjectName("NewCard1");
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* stepRow = new QHBoxLayout;
        auto* sl = new QLabel("PROJECT NAME");
        sl->setObjectName("NewStepLbl1");
        sl->setStyleSheet(stepHeaderStyle());
        stepRow->addWidget(sl);
        stepRow->addStretch();
        lay->addLayout(stepRow);

        m_nameInput = new QLineEdit;
        m_nameInput->setPlaceholderText("e.g. My Tier List");
        m_nameInput->setStyleSheet(inputStyle());
        connect(m_nameInput, &QLineEdit::returnPressed,
                this, &ProjectPanel::onCreateClicked);
        connect(m_nameInput, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (m_summaryNameLabel) {
                QString display = text.trimmed().isEmpty()
                    ? QStringLiteral("New Project")
                    : text.trimmed();
                m_summaryNameLabel->setText(QStringLiteral("\U0001F4C4  ") + display);
            }
        });
        lay->addWidget(m_nameInput);

        newPageLayout->addWidget(card);
    }

    // ── Step 2: Save Location ───────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setObjectName("NewCard2");
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* stepRow = new QHBoxLayout;
        auto* sl = new QLabel("SAVE LOCATION");
        sl->setObjectName("NewStepLbl2");
        sl->setStyleSheet(stepHeaderStyle());
        stepRow->addWidget(sl);
        stepRow->addStretch();
        lay->addLayout(stepRow);

        // Location input on its own full-width row.
        m_locationInput = new QLineEdit;
        m_locationInput->setPlaceholderText("Default projects folder");
        m_locationInput->setStyleSheet(inputStyle());
        lay->addWidget(m_locationInput);

        // Browse button on its own row, LEFT-aligned (a trailing stretch
        // absorbs the slack). Right-aligning pushed it against the panel's
        // right edge, which is exactly where the narrow side panel clips.
        auto* folderRow = new QHBoxLayout;
        folderRow->setSpacing(4);

        m_locationBrowseBtn = new QPushButton(QStringLiteral(" Browse…"));
        m_locationBrowseBtn->setIcon(createFolderIcon());
        m_locationBrowseBtn->setFixedHeight(30);
        m_locationBrowseBtn->setMinimumWidth(90);
        m_locationBrowseBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_locationBrowseBtn->setCursor(Qt::PointingHandCursor);
        m_locationBrowseBtn->setToolTip("Browse for a save location folder");
        m_locationBrowseBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: 11px; font-weight: 600;"
            "  padding: 4px 10px; }"
            "QPushButton:hover { background: %4; border-color: %5; color: %6; }")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textSecondary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.textPrimary)));
        connect(m_locationBrowseBtn, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, "Choose Save Location",
                m_locationInput->text().isEmpty() ? m_projectsDir
                                                  : m_locationInput->text());
            if (!dir.isEmpty()) {
                m_locationInput->setText(dir);
                addRecentSaveLocation(dir);
            }
        });
        folderRow->addWidget(m_locationBrowseBtn);
        folderRow->addStretch();
        lay->addLayout(folderRow);

        // Recent paths row
        m_recentPathsWidget = new QWidget;
        auto* rpLay = new QHBoxLayout(m_recentPathsWidget);
        rpLay->setContentsMargins(0, 0, 0, 0);
        rpLay->setSpacing(4);
        auto* recentLbl = new QLabel("Recent:");
        recentLbl->setObjectName("NewRecentLbl");
        recentLbl->setStyleSheet(QStringLiteral(
            "font-size: 9px; font-weight: 600; color: %1; letter-spacing: 0.4px;")
            .arg(Theme::rgb(c.textPrimary)));
        rpLay->addWidget(recentLbl);
        // Quick-select button for the default projects folder
        auto* rpSample = new QPushButton(" Projects");
        rpSample->setIcon(createFolderIcon());
        rpSample->setObjectName("NewRecentSample");
        rpSample->setCursor(Qt::PointingHandCursor);
        rpSample->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: 10px; padding: 2px 7px; }"
            "QPushButton:hover { background: %4; color: %5; }")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textTertiary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.textPrimary)));
        connect(rpSample, &QPushButton::clicked, this, [this]() {
            // Set the path directly from m_projectsDir — never strip display text
            m_locationInput->setText(m_projectsDir);
        });
        rpLay->addWidget(rpSample);
        rpLay->addStretch();
        lay->addWidget(m_recentPathsWidget);

        newPageLayout->addWidget(card);
    }

    // ── Step 3: Aspect Ratio ────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setObjectName("NewCard3");
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* stepRow = new QHBoxLayout;
        auto* sl = new QLabel("ASPECT RATIO");
        sl->setObjectName("NewStepLbl3");
        sl->setStyleSheet(stepHeaderStyle());
        stepRow->addWidget(sl);
        stepRow->addStretch();
        lay->addLayout(stepRow);

        // 2-column grid
        auto* arGrid = new QWidget;
        arGrid->setObjectName("NewArGrid");
        auto* arLay = new QGridLayout(arGrid);
        arLay->setContentsMargins(0, 0, 0, 0);
        arLay->setSpacing(3);

        m_arGroup = new QButtonGroup(this);
        m_arGroup->setExclusive(true);

        auto makeArBtn = [&](const QString& text, const char* objName,
                             int id, bool checked = false) -> QPushButton* {
            auto* btn = new QPushButton(text);
            btn->setObjectName(objName);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setStyleSheet(chipBtnStyle());
            m_arGroup->addButton(btn, id);
            return btn;
        };

        m_ar16_9 = makeArBtn("16:9", "ArBtn", 0, true);
        m_ar9_16 = makeArBtn("9:16", "ArBtn", 1);
        m_ar21_9 = makeArBtn("21:9", "ArBtn", 2);
        m_arCustom = makeArBtn("Custom", "ArBtnCustom", 3);
        m_arCustom->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: 18px; font-weight: 700;"
            "  padding: 8px 12px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim))
            .arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent)));

        arLay->addWidget(m_ar16_9, 0, 0);
        arLay->addWidget(m_ar9_16, 0, 1);
        arLay->addWidget(m_ar21_9, 1, 0);
        arLay->addWidget(m_arCustom, 1, 1);
        lay->addWidget(arGrid);

        // Custom AR row (hidden by default)
        m_customArRow = new QWidget;
        m_customArRow->setVisible(false);
        auto* caLay = new QHBoxLayout(m_customArRow);
        caLay->setContentsMargins(0, 0, 0, 0);
        caLay->setSpacing(6);
        auto* caWLbl = new QLabel("W");
        caWLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
                              .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caWLbl);
        m_customArW = new QSpinBox;
        m_customArW->setRange(1, 999);
        m_customArW->setValue(16);
        m_customArW->setStyleSheet(smallInputStyle());
        caLay->addWidget(m_customArW);
        auto* caSep = new QLabel(":");
        caSep->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;")
                             .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caSep);
        auto* caHLbl = new QLabel("H");
        caHLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
                              .arg(Theme::rgb(c.textPrimary)));
        caLay->addWidget(caHLbl);
        m_customArH = new QSpinBox;
        m_customArH->setRange(1, 999);
        m_customArH->setValue(9);
        m_customArH->setStyleSheet(smallInputStyle());
        caLay->addWidget(m_customArH);
        caLay->addStretch();
        lay->addWidget(m_customArRow);

        // AR group signal
        connect(m_arGroup, &QButtonGroup::idClicked, this, [this](int id) {
            m_customArRow->setVisible(id == 3); // 3 = Custom
            if (id == 3)
                rebuildResGrid(m_customArW->value(), m_customArH->value());
            else if (id == 0)
                rebuildResGrid(16, 9);
            else if (id == 1)
                rebuildResGrid(9, 16);
            else if (id == 2)
                rebuildResGrid(21, 9);
            updateSummaryLabels();
        });

        // Custom AR spinbox changes rebuild res grid
        connect(m_customArW, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) {
            if (m_arGroup->checkedId() == 3) {
                rebuildResGrid(m_customArW->value(), m_customArH->value());
                updateSummaryLabels();
            }
        });
        connect(m_customArH, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) {
            if (m_arGroup->checkedId() == 3) {
                rebuildResGrid(m_customArW->value(), m_customArH->value());
                updateSummaryLabels();
            }
        });

        newPageLayout->addWidget(card);
    }

    // ── Step 4: Resolution ─────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setObjectName("NewCard4");
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* stepRow = new QHBoxLayout;
        auto* sl = new QLabel("RESOLUTION");
        sl->setObjectName("NewStepLbl4");
        sl->setStyleSheet(stepHeaderStyle());
        stepRow->addWidget(sl);
        stepRow->addStretch();
        lay->addLayout(stepRow);

        // Resolution button grid (dynamic — rebuilt by rebuildResGrid)
        m_resGridWidget = new QWidget;
        m_resGridWidget->setObjectName("ResGrid");
        auto* resLay = new QVBoxLayout(m_resGridWidget);
        resLay->setContentsMargins(0, 0, 0, 0);
        resLay->setSpacing(0);
        // The actual grid layout will be created inside rebuildResGrid
        lay->addWidget(m_resGridWidget);

        m_resGroup = new QButtonGroup(this);
        m_resGroup->setExclusive(true);

        // Custom resolution row (hidden by default)
        m_customResRow = new QWidget;
        m_customResRow->setVisible(false);
        auto* crLay = new QHBoxLayout(m_customResRow);
        crLay->setContentsMargins(0, 0, 0, 0);
        crLay->setSpacing(6);
        auto* crWLbl = new QLabel("W");
        crWLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
                              .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(crWLbl);
        m_customResW = new QSpinBox;
        m_customResW->setRange(1, 99999);
        m_customResW->setValue(1920);
        m_customResW->setStyleSheet(smallInputStyle());
        crLay->addWidget(m_customResW);
        auto* crSep = new QLabel("\u00D7");
        crSep->setStyleSheet(QStringLiteral("color: %1; font-weight: 700;")
                             .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(crSep);
        auto* crHLbl = new QLabel("H");
        crHLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
                              .arg(Theme::rgb(c.textPrimary)));
        crLay->addWidget(crHLbl);
        m_customResH = new QSpinBox;
        m_customResH->setRange(1, 99999);
        m_customResH->setValue(1080);
        m_customResH->setStyleSheet(smallInputStyle());
        crLay->addWidget(m_customResH);
        crLay->addStretch();
        lay->addWidget(m_customResRow);

        // Custom resolution change → sync AR (on editingFinished = Enter/blur)
        auto syncArFromRes = [this]() {
            if (!m_customResRow->isVisible()) return;
            uint32_t rw = static_cast<uint32_t>(m_customResW->value());
            uint32_t rh = static_cast<uint32_t>(m_customResH->value());
            if (rw == 0 || rh == 0) return;

            // Simplify ratio
            uint32_t a = rw, b = rh;
            while (b) { uint32_t t = b; b = a % b; a = t; }
            uint32_t g = a;
            uint32_t sw = rw / g, sh = rh / g;

            // Match to preset
            double ratio = static_cast<double>(rw) / rh;
            auto dist = [&](double v) { return std::abs(ratio - v); };
            int bestId = 3; // default: Custom
            double bestDiff = 999.0;

            struct { int id; double val; } presets[] = {
                {0, 16.0/9.0}, {1, 9.0/16.0}, {2, 21.0/9.0}
            };
            for (auto& p : presets) {
                double d = dist(p.val);
                if (d < bestDiff) { bestDiff = d; bestId = p.id; }
            }
            if (bestDiff >= 0.02) bestId = 3; // Not close enough → Custom

            // Toggle AR selection
            if (bestId == 3) {
                m_arCustom->setChecked(true);
                m_customArRow->setVisible(true);
                m_customArW->setValue(static_cast<int>(sw));
                m_customArH->setValue(static_cast<int>(sh));
                rebuildResGrid(sw, sh);
            } else {
                if (bestId == 0) m_ar16_9->setChecked(true);
                else if (bestId == 1) m_ar9_16->setChecked(true);
                else if (bestId == 2) m_ar21_9->setChecked(true);
                m_customArRow->setVisible(false);
                if (bestId == 0) rebuildResGrid(16, 9);
                else if (bestId == 1) rebuildResGrid(9, 16);
                else rebuildResGrid(21, 9);
            }
            updateSummaryLabels();
        };
        connect(m_customResW, &QSpinBox::editingFinished, this, syncArFromRes);
        connect(m_customResH, &QSpinBox::editingFinished, this, syncArFromRes);

        // Live summary update when typing custom resolution
        connect(m_customResW, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { updateSummaryLabels(); });
        connect(m_customResH, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int) { updateSummaryLabels(); });

        newPageLayout->addWidget(card);
    }

    // ── Step 5: Framerate ──────────────────────────────────────────────
    {
        auto* card = new QWidget;
        card->setObjectName("NewCard5");
        card->setStyleSheet(cardStyle());
        auto* lay = new QVBoxLayout(card);
        lay->setContentsMargins(14, 12, 14, 12);
        lay->setSpacing(8);

        auto* stepRow = new QHBoxLayout;
        auto* sl = new QLabel("FRAME RATE");
        sl->setObjectName("NewStepLbl5");
        sl->setStyleSheet(stepHeaderStyle());
        stepRow->addWidget(sl);
        stepRow->addStretch();
        lay->addLayout(stepRow);

        auto* fpsGrid = new QWidget;
        fpsGrid->setObjectName("NewFpsGrid");
        auto* fpsLay = new QGridLayout(fpsGrid);
        fpsLay->setContentsMargins(0, 0, 0, 0);
        fpsLay->setSpacing(3);

        m_fpsGroup = new QButtonGroup(this);
        m_fpsGroup->setExclusive(true);

        auto makeFpsBtn = [&](const QString& text, int id,
                              bool checked = false, bool dashed = false) -> QPushButton* {
            auto* btn = new QPushButton(text);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setCursor(Qt::PointingHandCursor);
            if (dashed) {
                btn->setStyleSheet(QStringLiteral(
                    "QPushButton { background: %1; border: 1px dashed %2;"
                    "  color: %3; font-size: 18px; font-weight: 700;"
                    "  padding: 8px 12px; }"
                    "QPushButton:hover { background: %4; border-color: %5; }"
                    "QPushButton:checked { background: %6; border-color: %7;"
                    "  color: %8; }")
                    .arg(Theme::rgb(c.surface0))
                    .arg(Theme::rgb(c.border))
                    .arg(Theme::rgb(c.textPrimary))
                    .arg(Theme::rgb(c.surface2))
                    .arg(Theme::rgb(c.borderLight))
                    .arg(Theme::rgb(c.accentDim))
                    .arg(Theme::rgb(c.accent))
                    .arg(Theme::rgb(c.accent)));
            } else {
                btn->setStyleSheet(chipBtnStyle());
            }
            m_fpsGroup->addButton(btn, id);
            return btn;
        };

        m_fps24 = makeFpsBtn("24", 0);
        m_fps30 = makeFpsBtn("30", 1, true);
        m_fps60 = makeFpsBtn("60", 2);
        m_fpsCustom = makeFpsBtn("Custom", 3, false, true);

        fpsLay->addWidget(m_fps24, 0, 0);
        fpsLay->addWidget(m_fps30, 0, 1);
        fpsLay->addWidget(m_fps60, 1, 0);
        fpsLay->addWidget(m_fpsCustom, 1, 1);
        lay->addWidget(fpsGrid);

        // Custom FPS row (hidden by default)
        m_customFpsRow = new QWidget;
        m_customFpsRow->setVisible(false);
        auto* cfLay = new QHBoxLayout(m_customFpsRow);
        cfLay->setContentsMargins(0, 0, 0, 0);
        cfLay->setSpacing(6);
        auto* cfLbl = new QLabel("FPS");
        cfLbl->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; font-weight: 600;")
                             .arg(Theme::rgb(c.textPrimary)));
        cfLay->addWidget(cfLbl);
        m_customFps = new QDoubleSpinBox;
        m_customFps->setRange(1.0, 240.0);
        m_customFps->setValue(30.0);
        m_customFps->setDecimals(2);
        m_customFps->setStyleSheet(smallInputStyle());
        cfLay->addWidget(m_customFps);
        cfLay->addStretch();
        lay->addWidget(m_customFpsRow);

        connect(m_fpsGroup, &QButtonGroup::idClicked, this, [this](int id) {
            m_customFpsRow->setVisible(id == 3);
            updateSummaryLabels();
        });
        connect(m_customFps, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double) { updateSummaryLabels(); });

        newPageLayout->addWidget(card);
    }

    // ── Divider ────────────────────────────────────────────────────────
    {
        newPageLayout->addSpacing(12);
        auto* divider = new QFrame;
        divider->setObjectName("NewDivider");
        divider->setFrameShape(QFrame::HLine);
        divider->setStyleSheet(QStringLiteral(
            "background: %1; max-height: 1px;")
            .arg(Theme::rgb(c.border)));
        newPageLayout->addWidget(divider);
        newPageLayout->addSpacing(12);
    }

    // ── Summary Bar + Create Button (2-row layout) ─────────────────────
    {
        m_summaryBar = new QWidget;
        m_summaryBar->setStyleSheet(QStringLiteral(
            "background: %1; border: 1px solid %2; padding: 10px 12px;")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border)));
        auto* sbLay = new QVBoxLayout(m_summaryBar);
        sbLay->setContentsMargins(14, 12, 14, 12);
        sbLay->setSpacing(10);

        // Row 1: Project name (dynamic, updated via textChanged signal)
        m_summaryNameLabel = new QLabel(QStringLiteral("\U0001F4C4  New Project"));
        m_summaryNameLabel->setObjectName("NewSummaryName");
        m_summaryNameLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; font-weight: 700; color: %1;")
            .arg(Theme::rgb(c.textPrimary)));
        sbLay->addWidget(m_summaryNameLabel);

        // Row 2: Resolution × FPS
        auto* specsRow = new QHBoxLayout;
        specsRow->setSpacing(12);
        auto* iconRes = new QLabel(QStringLiteral("\U0001F4D0"));
        iconRes->setStyleSheet(QStringLiteral("font-size: 11px; color: %1;")
                               .arg(Theme::rgb(c.textTertiary)));
        specsRow->addWidget(iconRes);
        m_summaryResLabel = new QLabel("1920\u00D71080");
        m_summaryResLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; font-weight: 700; color: %1;")
            .arg(Theme::rgb(c.textPrimary)));
        specsRow->addWidget(m_summaryResLabel);
        auto* iconFps = new QLabel(QStringLiteral("\U0001F39E\uFE0F"));
        iconFps->setStyleSheet(QStringLiteral("font-size: 11px; color: %1;")
                               .arg(Theme::rgb(c.textTertiary)));
        specsRow->addWidget(iconFps);
        m_summaryFpsLabel = new QLabel("30 fps");
        m_summaryFpsLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; font-weight: 700; color: %1;")
            .arg(Theme::rgb(c.textPrimary)));
        specsRow->addWidget(m_summaryFpsLabel);
        specsRow->addStretch();
        sbLay->addLayout(specsRow);

        // Row 3: Create Project button (full width)
        m_createBtn = new QPushButton(QStringLiteral("\u2728  Create Project"));
        m_createBtn->setCursor(Qt::PointingHandCursor);
        m_createBtn->setMinimumHeight(36);
        m_createBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: white; border: none;"
            "  font-size: 13px; font-weight: 700; padding: 8px 18px; }"
            "QPushButton:hover { background: %2; }"
            "QPushButton:pressed { background: %3; }")
            .arg(Theme::rgb(c.primaryBtnBg))
            .arg(Theme::rgb(c.primaryBtnHover))
            .arg(Theme::rgb(c.accent)));
        connect(m_createBtn, &QPushButton::clicked,
                this, &ProjectPanel::onCreateClicked);
        sbLay->addWidget(m_createBtn);

        newPageLayout->addWidget(m_summaryBar);
    }

    newPageLayout->addStretch();

    // Wrap the NEW page in a scroll area for low-height windows
    m_newPageScroll = new QScrollArea;
    m_newPageScroll->setWidget(m_newPage);
    m_newPageScroll->setWidgetResizable(true);
    m_newPageScroll->setFrameShape(QFrame::NoFrame);
    m_newPageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_newPageScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_sidePanelStack->addWidget(m_newPageScroll);   // index 0

    // Build initial resolution grid (default: 16:9)
    rebuildResGrid(16, 9);

    // Apply initial NEW panel responsive sizing
    applyNewPanelResponsiveLayout();

    // --- OPEN page (thumbnail project list) ------------------------------
    m_openPage = new QWidget;
    auto* openPageLayout = new QVBoxLayout(m_openPage);
    openPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                       m.spacingXxl, m.spacingXxl);
    openPageLayout->setSpacing(m.spacingXl);

    // Close button + title row
    auto* openTitleRow = new QHBoxLayout;
    auto* openTitle = new QLabel("Open Project File");
    openTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    openTitleRow->addWidget(openTitle, 1);

    auto* openCloseBtn = new QPushButton("\u2715");
    openCloseBtn->setFixedSize(36, 36);
    openCloseBtn->setCursor(Qt::PointingHandCursor);
    openCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(openCloseBtn, &QPushButton::clicked,
            this, &ProjectPanel::hideSidePanel);
    openTitleRow->addWidget(openCloseBtn);
    openPageLayout->addLayout(openTitleRow);

    // Keep QFileSystemModel for setProjectsDirectory() compatibility
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setNameFilters({"*.rtp"});
    m_fileModel->setNameFilterDisables(false);

    // Determine initial root directory
    if (m_projectsDir.isEmpty()) {
        auto settings = rt::appSettings();
        QString customDir = settings.value("ProjectsDirectory").toString();
        if (!customDir.isEmpty() && QDir(customDir).exists()) {
            m_projectsDir = customDir;
        } else {
            QDir dir(QCoreApplication::applicationDirPath());
            for (int i = 0; i < 5; ++i) {
                if (dir.exists("projects")) {
                    m_projectsDir = dir.absoluteFilePath("projects");
                    break;
                }
                dir.cdUp();
            }
            if (m_projectsDir.isEmpty())
                m_projectsDir = QCoreApplication::applicationDirPath();
        }
    }
    m_fileModel->setRootPath(m_projectsDir);

    // Hidden tree view kept for test compatibility
    m_fileTree = new QTreeView;
    m_fileTree->setModel(m_fileModel);
    m_fileTree->setRootIndex(m_fileModel->index(m_projectsDir));
    m_fileTree->setVisible(false);

    // ── Thumbnail project list (replaces the tree view) ─────────────────
    m_openList = new QListWidget;
    m_openList->setIconSize(QSize(160, 100));
    m_openList->setSpacing(4);
    m_openList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_openList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_openList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_openList->setStyleSheet(QStringLiteral(
        "QListWidget { font-size: 13px; border: 1px solid %1;"
        "  border-radius: %2px; background: %3; outline: 0; }"
        "QListWidget::item { padding: 6px; border-bottom: 1px solid %4; }"
        "QListWidget::item:selected { background: %5; }"
        "QListWidget::item:hover { background: %6; }")
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface0))
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.accentDim))
        .arg(Theme::rgb(c.surface2)));
    connect(m_openList, &QListWidget::itemDoubleClicked,
            this, &ProjectPanel::onOpenListItemDoubleClicked);
    m_openList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_openList, &QWidget::customContextMenuRequested,
            this, &ProjectPanel::showOpenListContextMenu);
    openPageLayout->addWidget(m_openList, 1);

    // Browse button (for navigating outside the default directory)
    auto* browseRow = new QHBoxLayout;
    browseRow->setSpacing(m.spacingMd);

    auto* browseBtn = new QPushButton("\U0001F4C1  Browse...");
    browseBtn->setMinimumHeight(44);
    browseBtn->setCursor(Qt::PointingHandCursor);
    browseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 14px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        hideSidePanel();
        emit openFromFile();
    });
    browseRow->addWidget(browseBtn);

    // Delete button
    auto* deleteBtn = new QPushButton("\U0001F5D1  Delete");
    deleteBtn->setMinimumHeight(44);
    deleteBtn->setCursor(Qt::PointingHandCursor);
    deleteBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 14px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.error))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    connect(deleteBtn, &QPushButton::clicked, this, [this]() {
        auto* item = m_openList->currentItem();
        if (!item) return;
        QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty()) return;
        QFileInfo fi(path);
        auto reply = QMessageBox::question(this, "Delete Project",
            QString("Delete \"%1\"?\n\nThis cannot be undone.").arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QFile::remove(path);
            populateOpenList();
        }
    });
    browseRow->addWidget(deleteBtn);

    // Open Selected button
    m_openSelectedBtn = new QPushButton("\u25B6  Open Selected");
    m_openSelectedBtn->setMinimumHeight(48);
    m_openSelectedBtn->setCursor(Qt::PointingHandCursor);
    m_openSelectedBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1; color: white; border: none;"
        "  border-radius: %2px; font-size: 16px;"
        "  font-weight: 700; padding: 12px 24px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }")
        .arg(Theme::rgb(c.primaryBtnBg))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.primaryBtnHover))
        .arg(Theme::rgb(c.accent)));
    connect(m_openSelectedBtn, &QPushButton::clicked, this, [this]() {
        auto* item = m_openList->currentItem();
        if (!item) return;
        QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty() && QFileInfo(path).isFile()) {
            hideSidePanel();
            emit openFilePath(path);
        }
    });
    browseRow->addWidget(m_openSelectedBtn, 1);

    openPageLayout->addLayout(browseRow);

    m_sidePanelStack->addWidget(m_openPage);  // index 1

    // --- SETTINGS page ---------------------------------------------------
    m_settingsPage = new QWidget;
    auto* settingsPageLayout = new QVBoxLayout(m_settingsPage);
    settingsPageLayout->setContentsMargins(m.spacingXxl, m.spacingXxl,
                                            m.spacingXxl, m.spacingXxl);
    settingsPageLayout->setSpacing(m.spacingXl);

    // Close button + title row
    auto* settingsTitleRow = new QHBoxLayout;
    auto* settingsTitle = new QLabel("Project Settings");
    settingsTitle->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: %1; color: %2;")
        .arg(t.weightBold)
        .arg(Theme::rgb(c.textPrimary)));
    settingsTitleRow->addWidget(settingsTitle, 1);

    auto* settingsCloseBtn = new QPushButton("\u2715");
    settingsCloseBtn->setFixedSize(36, 36);
    settingsCloseBtn->setCursor(Qt::PointingHandCursor);
    settingsCloseBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none;"
        "  border-radius: %1px; font-size: 18px; color: %2; }"
        "QPushButton:hover { background: %3; color: %4; }")
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface3))
        .arg(Theme::rgb(c.textPrimary)));
    connect(settingsCloseBtn, &QPushButton::clicked,
            this, &ProjectPanel::hideSidePanel);
    settingsTitleRow->addWidget(settingsCloseBtn);
    settingsPageLayout->addLayout(settingsTitleRow);

    // Projects directory section
    auto* dirSectionLabel = new QLabel("PROJECTS DIRECTORY");
    dirSectionLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: %1;"
        " letter-spacing: 1.5px;")
        .arg(Theme::rgb(c.textTertiary)));
    settingsPageLayout->addWidget(dirSectionLabel);

    auto* dirDesc = new QLabel(
        "Where project files are stored. Changing this will "
        "update the project list.");
    dirDesc->setWordWrap(true);
    dirDesc->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: %1;")
        .arg(Theme::rgb(c.textTertiary)));
    settingsPageLayout->addWidget(dirDesc);

    m_projDirInput = new QLineEdit;
    m_projDirInput->setReadOnly(true);
    m_projDirInput->setText(m_projectsDir);
    m_projDirInput->setMinimumHeight(44);
    m_projDirInput->setStyleSheet(QStringLiteral(
        "QLineEdit { font-size: 13px; padding: 10px 14px;"
        "  background: %1; color: %2; }")
        .arg(Theme::rgb(c.surface0))
        .arg(Theme::rgb(c.textSecondary)));
    settingsPageLayout->addWidget(m_projDirInput);

    m_changeDirBtn = new QPushButton("\U0001F4C1  Change Directory...");
    m_changeDirBtn->setMinimumHeight(44);
    m_changeDirBtn->setCursor(Qt::PointingHandCursor);
    m_changeDirBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 14px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    connect(m_changeDirBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Select Projects Directory", m_projectsDir);
        if (!dir.isEmpty() && dir != m_projectsDir) {
            m_projectsDir = dir;
            m_projDirInput->setText(dir);
            setProjectsDirectory(dir);
            emit projectsDirChanged(dir);
        }
    });
    settingsPageLayout->addWidget(m_changeDirBtn);

    // Open projects folder in Explorer
    auto* revealDirBtn = new QPushButton("\U0001F4C2  Open in Explorer");
    revealDirBtn->setMinimumHeight(44);
    revealDirBtn->setCursor(Qt::PointingHandCursor);
    revealDirBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: %4px; font-size: 14px; font-weight: 600;"
        "  padding: 8px 16px; }"
        "QPushButton:hover { background: %5; }")
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textPrimary))
        .arg(Theme::rgb(c.border))
        .arg(m.radiusMd)
        .arg(Theme::rgb(c.surface3)));
    connect(revealDirBtn, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_projectsDir));
    });
    settingsPageLayout->addWidget(revealDirBtn);

    settingsPageLayout->addStretch();
    m_sidePanelStack->addWidget(m_settingsPage);  // index 2

    // Side panel starts hidden
    m_sidePanel->setVisible(false);
}

} // namespace rt
