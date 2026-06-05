/*
 * ProjectPanelNewPanel.cpp — NEW "Create Project" panel responsive layout,
 * summary labels, and resolution grid building, extracted from
 * ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace rt {

// Forward declarations from ProjectPanelHelpers.cpp
uint32_t customResWidthExternal(const ProjectPanel* panel);
uint32_t customResHeightExternal(const ProjectPanel* panel);
double customFpsExternal(const ProjectPanel* panel);

// =============================================================================
// Resolution grid builder
// =============================================================================

void ProjectPanel::rebuildResGrid(uint32_t arW, uint32_t arH)
{
    auto* outerLay = m_resGridWidget->layout();
    if (!outerLay) return;

    QLayoutItem* item;
    while ((item = outerLay->takeAt(0)) != nullptr) {
        if (auto* w = item->widget()) {
            const auto btns = w->findChildren<QAbstractButton*>();
            for (auto* btn : btns)
                m_resGroup->removeButton(btn);
            w->deleteLater();
        }
        delete item;
    }

    auto* container = new QWidget(m_resGridWidget);
    auto* grid = new QGridLayout(container);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(m_newSizes.gridSpacing);

    struct ResPreset { uint32_t w; uint32_t h; const char* tag; };
    QVector<ResPreset> presets;

    if (arW == 21 && arH == 9) {
        presets = {{3440,1440,"UW"},{3840,1600,"UW"},{5120,2160,"5K"}};
    } else if (arW >= arH) {
        uint32_t widths[] = {1280, 1920, 3840};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            uint32_t bh = static_cast<uint32_t>(std::round(static_cast<double>(widths[i]) * arH / arW));
            presets.append({widths[i], bh, tags[i]});
        }
    } else {
        uint32_t heights[] = {720, 1080, 2160};
        const char* tags[] = {"HD", "FHD", "4K"};
        for (int i = 0; i < 3; ++i) {
            uint32_t bw = static_cast<uint32_t>(std::round(static_cast<double>(heights[i]) * arW / arH));
            presets.append({bw, heights[i], tags[i]});
        }
    }

    const auto& c = Theme::colors();
    auto chipStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 700; padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto dashStyle = [&]() {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 700; padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7; color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };

    for (int i = 0; i < presets.size(); ++i) {
        auto* btn = new QPushButton(
            QString("%1\u00D7%2  %3").arg(presets[i].w).arg(presets[i].h).arg(presets[i].tag));
        btn->setCheckable(true);
        btn->setChecked(i == 0);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(chipStyle());
        btn->setProperty("resW", presets[i].w);
        btn->setProperty("resH", presets[i].h);
        m_resGroup->addButton(btn, i);
        grid->addWidget(btn, i / 2, i % 2);
    }

    auto* customBtn = new QPushButton("Custom");
    customBtn->setCheckable(true);
    customBtn->setCursor(Qt::PointingHandCursor);
    customBtn->setStyleSheet(dashStyle());
    int customId = presets.size();
    m_resGroup->addButton(customBtn, customId);
    grid->addWidget(customBtn, customId / 2, customId % 2);

    outerLay->addWidget(container);

    connect(m_resGroup, &QButtonGroup::idClicked, this, [this](int id) {
        auto* btn = m_resGroup->button(id);
        m_customResRow->setVisible(btn && btn->text().trimmed() == "Custom");
        updateSummaryLabels();
    });

    updateSummaryLabels();
}

// =============================================================================
// Summary labels
// =============================================================================

void ProjectPanel::updateSummaryLabels()
{
    if (m_summaryNameLabel) {
        QString name = m_nameInput ? m_nameInput->text().trimmed() : QString();
        QString display = name.isEmpty() ? QStringLiteral("New Project") : name;
        m_summaryNameLabel->setText(QStringLiteral("\U0001F4C4  ") + display);
    }

    uint32_t rw = customResWidth();
    uint32_t rh = customResHeight();
    m_summaryResLabel->setText(QString("%1\u00D7%2").arg(rw).arg(rh));

    double fps = customFps();
    m_summaryFpsLabel->setText(QString("%1 fps").arg(fps, 0, 'f', (fps == std::floor(fps)) ? 0 : 2));
}

// =============================================================================
// Height-aware responsive layout for the NEW "Create Project" panel
// =============================================================================

void ProjectPanel::applyNewPanelResponsiveLayout()
{
    const int h = height();

    if (h >= 800) {
        m_newSizes = NewPanelSizes();
    } else if (h >= 600) {
        m_newSizes = NewPanelSizes{
            /*cardMarginTB*/ 10,  /*cardMarginLR*/ 12,  /*cardSpacing*/ 6,
            /*stepFontSize*/ 10,
            /*btnFontSize*/ 15,   /*btnPadV*/ 6,        /*btnPadH*/ 10,
            /*inputFontSize*/ 12, /*inputPadV*/ 6,      /*inputPadH*/ 8,
            /*siFontSize*/ 12,    /*siPadV*/ 5,         /*siPadH*/ 6,
            /*siMinW*/ 45,        /*siMaxW*/ 50,
            /*sumPadTB*/ 10,      /*sumPadLR*/ 10,      /*sumSpacing*/ 8,
            /*sumNameFontSize*/ 11, /*sumSpecFontSize*/ 11, /*sumIconFontSize*/ 10,
            /*createBtnFontSize*/ 12, /*createBtnPadV*/ 6, /*createBtnPadH*/ 14,
            /*createBtnMinH*/ 32,
            /*pageSpacing*/ 6,
            /*gridSpacing*/ 2,
            /*dividerSpacing*/ 8,
            /*headerFontSize*/ 14,
            /*closeBtnSize*/ 24,
            /*browseBtnSize*/ 26,
            /*recentLblFontSize*/ 8,
            /*recentSampleFontSize*/ 9, /*recentSamplePadV*/ 2, /*recentSamplePadH*/ 5,
            /*customRowFontSize*/ 9
        };
    } else {
        m_newSizes = NewPanelSizes{
            /*cardMarginTB*/ 6,   /*cardMarginLR*/ 8,   /*cardSpacing*/ 4,
            /*stepFontSize*/ 9,
            /*btnFontSize*/ 13,   /*btnPadV*/ 4,        /*btnPadH*/ 8,
            /*inputFontSize*/ 11, /*inputPadV*/ 4,      /*inputPadH*/ 6,
            /*siFontSize*/ 11,    /*siPadV*/ 3,         /*siPadH*/ 5,
            /*siMinW*/ 40,        /*siMaxW*/ 45,
            /*sumPadTB*/ 6,       /*sumPadLR*/ 6,       /*sumSpacing*/ 5,
            /*sumNameFontSize*/ 10, /*sumSpecFontSize*/ 10, /*sumIconFontSize*/ 9,
            /*createBtnFontSize*/ 11, /*createBtnPadV*/ 5, /*createBtnPadH*/ 12,
            /*createBtnMinH*/ 28,
            /*pageSpacing*/ 4,
            /*gridSpacing*/ 2,
            /*dividerSpacing*/ 5,
            /*headerFontSize*/ 13,
            /*closeBtnSize*/ 20,
            /*browseBtnSize*/ 22,
            /*recentLblFontSize*/ 8,
            /*recentSampleFontSize*/ 8, /*recentSamplePadV*/ 1, /*recentSamplePadH*/ 4,
            /*customRowFontSize*/ 8
        };
    }

    const auto& c = Theme::colors();

    auto chipSS = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %9px; font-weight: 700;"
            "  padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto dashSS = [&]() -> QString {
        return QStringLiteral(
            "QPushButton { background: %1; border: 1px dashed %2;"
            "  color: %3; font-size: %9px; font-weight: 700;"
            "  padding: %10px %11px; }"
            "QPushButton:hover { background: %4; border-color: %5; }"
            "QPushButton:checked { background: %6; border-color: %7;"
            "  color: %8; }")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.borderLight))
            .arg(Theme::rgb(c.accentDim)).arg(Theme::rgb(c.accent))
            .arg(Theme::rgb(c.accent))
            .arg(m_newSizes.btnFontSize).arg(m_newSizes.btnPadV).arg(m_newSizes.btnPadH);
    };
    auto stepSS = [&]() -> QString {
        return QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;"
            " letter-spacing: 0.6px; text-transform: uppercase;")
            .arg(m_newSizes.stepFontSize).arg(Theme::rgb(c.textPrimary));
    };
    auto inputSS = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: %4px; padding: %5px %6px;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(m_newSizes.inputFontSize).arg(m_newSizes.inputPadV).arg(m_newSizes.inputPadH);
    };
    auto smallInputSS = [&]() -> QString {
        return QStringLiteral(
            "background: %1; border: 1px solid %2;"
            " color: %3; font-size: %4px; padding: %5px %6px;"
            " min-width: %7px; max-width: %8px; text-align: center;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textPrimary))
            .arg(m_newSizes.siFontSize).arg(m_newSizes.siPadV).arg(m_newSizes.siPadH)
            .arg(m_newSizes.siMinW).arg(m_newSizes.siMaxW);
    };

    if (auto* pageLayout = qobject_cast<QVBoxLayout*>(m_newPage->layout())) {
        pageLayout->setSpacing(m_newSizes.pageSpacing);
    }

    for (int i = 1; i <= 5; ++i) {
        auto* card = m_newPage->findChild<QWidget*>(QString("NewCard%1").arg(i));
        if (!card || !card->layout()) continue;
        card->layout()->setContentsMargins(
            m_newSizes.cardMarginLR, m_newSizes.cardMarginTB,
            m_newSizes.cardMarginLR, m_newSizes.cardMarginTB);
        card->layout()->setSpacing(m_newSizes.cardSpacing);

        if (auto* stepLbl = card->findChild<QLabel*>(QString("NewStepLbl%1").arg(i)))
            stepLbl->setStyleSheet(stepSS());
    }

    if (auto* title = m_newPage->findChild<QLabel*>("NewHeaderTitle")) {
        title->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: %2; color: %3;")
            .arg(m_newSizes.headerFontSize)
            .arg(Theme::typography().weightBold)
            .arg(Theme::rgb(c.textPrimary)));
    }
    if (auto* closeBtn = m_newPage->findChild<QPushButton*>("NewCloseBtn")) {
        closeBtn->setFixedSize(m_newSizes.closeBtnSize, m_newSizes.closeBtnSize);
    }

    if (auto* arGrid = m_newPage->findChild<QWidget*>("NewArGrid")) {
        if (auto* arLay = qobject_cast<QGridLayout*>(arGrid->layout()))
            arLay->setSpacing(m_newSizes.gridSpacing);
    }

    if (auto* fpsGrid = m_newPage->findChild<QWidget*>("NewFpsGrid")) {
        if (auto* fpsLay = qobject_cast<QGridLayout*>(fpsGrid->layout()))
            fpsLay->setSpacing(m_newSizes.gridSpacing);
    }

    QString chip = chipSS();
    QString dash = dashSS();
    for (auto* btn : {m_ar16_9, m_ar9_16, m_ar21_9}) {
        if (btn) btn->setStyleSheet(chip);
    }
    if (m_arCustom) m_arCustom->setStyleSheet(dash);

    for (auto* btn : {m_fps24, m_fps30, m_fps60}) {
        if (btn) btn->setStyleSheet(chip);
    }
    if (m_fpsCustom) m_fpsCustom->setStyleSheet(dash);

    {
        int arId = m_arGroup ? m_arGroup->checkedId() : 0;
        if (arId == 3) {
            rebuildResGrid(static_cast<uint32_t>(m_customArW ? m_customArW->value() : 16),
                           static_cast<uint32_t>(m_customArH ? m_customArH->value() : 9));
        } else if (arId == 1) {
            rebuildResGrid(9, 16);
        } else if (arId == 2) {
            rebuildResGrid(21, 9);
        } else {
            rebuildResGrid(16, 9);
        }
    }

    QString inSS = inputSS();
    if (m_nameInput) m_nameInput->setStyleSheet(inSS);
    if (m_locationInput) m_locationInput->setStyleSheet(inSS);

    QString siSS = smallInputSS();
    for (auto* sb : {m_customArW, m_customArH, m_customResW, m_customResH}) {
        if (sb) sb->setStyleSheet(siSS);
    }
    if (m_customFps) m_customFps->setStyleSheet(siSS);

    QString rowLabelSS = QStringLiteral(
        "font-size: %1px; color: %2; font-weight: 600;")
        .arg(m_newSizes.customRowFontSize).arg(Theme::rgb(c.textPrimary));
    for (auto* row : {m_customArRow, m_customResRow, m_customFpsRow}) {
        if (!row) continue;
        const auto labels = row->findChildren<QLabel*>();
        for (auto* lbl : labels)
            lbl->setStyleSheet(rowLabelSS);
    }

    if (m_locationBrowseBtn)
        // Keep the labeled "Browse…" button height responsive but let its width
        // grow to fit the text (don't force it into a tiny icon-only square).
        m_locationBrowseBtn->setFixedHeight(m_newSizes.browseBtnSize);

    if (auto* recentLbl = m_newPage->findChild<QLabel*>("NewRecentLbl")) {
        recentLbl->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 600; color: %2; letter-spacing: 0.4px;")
            .arg(m_newSizes.recentLblFontSize).arg(Theme::rgb(c.textPrimary)));
    }
    if (auto* recentSample = m_newPage->findChild<QPushButton*>("NewRecentSample")) {
        recentSample->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: %4px; padding: %5px %6px; }"
            "QPushButton:hover { background: %7; color: %8; }")
            .arg(Theme::rgb(c.surface1)).arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textTertiary))
            .arg(m_newSizes.recentSampleFontSize)
            .arg(m_newSizes.recentSamplePadV).arg(m_newSizes.recentSamplePadH)
            .arg(Theme::rgb(c.surface2)).arg(Theme::rgb(c.textPrimary)));
    }

    if (m_summaryBar) {
        if (auto* sbLay = m_summaryBar->layout()) {
            sbLay->setContentsMargins(
                m_newSizes.sumPadLR, m_newSizes.sumPadTB,
                m_newSizes.sumPadLR, m_newSizes.sumPadTB);
            sbLay->setSpacing(m_newSizes.sumSpacing);
        }
        if (auto* nameLbl = m_newPage->findChild<QLabel*>("NewSummaryName")) {
            nameLbl->setStyleSheet(QStringLiteral(
                "font-size: %1px; font-weight: 700; color: %2;")
                .arg(m_newSizes.sumNameFontSize).arg(Theme::rgb(c.textPrimary)));
        }
    }
    if (m_summaryResLabel) {
        m_summaryResLabel->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;")
            .arg(m_newSizes.sumSpecFontSize).arg(Theme::rgb(c.textPrimary)));
    }
    if (m_summaryFpsLabel) {
        m_summaryFpsLabel->setStyleSheet(QStringLiteral(
            "font-size: %1px; font-weight: 700; color: %2;")
            .arg(m_newSizes.sumSpecFontSize).arg(Theme::rgb(c.textPrimary)));
    }

    if (m_summaryBar) {
        const auto icons = m_summaryBar->findChildren<QLabel*>();
        for (auto* iconLbl : icons) {
            const auto t = iconLbl->text();
            if (t == QStringLiteral("\U0001F4D0") ||
                t == QStringLiteral("\U0001F39E\uFE0F")) {
                iconLbl->setStyleSheet(QStringLiteral(
                    "font-size: %1px; color: %2;")
                    .arg(m_newSizes.sumIconFontSize).arg(Theme::rgb(c.textTertiary)));
            }
        }
    }

    if (m_createBtn) {
        m_createBtn->setMinimumHeight(m_newSizes.createBtnMinH);
        m_createBtn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: white; border: none;"
            "  font-size: %2px; font-weight: 700; padding: %3px %4px; }"
            "QPushButton:pressed { background: %6; }")
            .arg(Theme::rgb(c.primaryBtnBg))
            .arg(m_newSizes.createBtnFontSize)
            .arg(m_newSizes.createBtnPadV).arg(m_newSizes.createBtnPadH)
            .arg(Theme::rgb(c.accent)));
    }

    if (m_summaryBar) {
        m_summaryBar->setStyleSheet(QStringLiteral(
            "background: %1; border: 1px solid %2; padding: %3px %4px;")
            .arg(Theme::rgb(c.surface0)).arg(Theme::rgb(c.border))
            .arg(m_newSizes.sumPadTB).arg(m_newSizes.sumPadLR));
    }
}

} // namespace rt
