/*
 * ProjectPanelTable.cpp — table rebuild, selection, action button state,
 * and responsive layout extracted from ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"

#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QWidget>

#include <algorithm>

namespace rt {

// External helpers defined in ProjectPanelHelpers.cpp
QString formatFileSize(qint64 bytes);
QString formatDate(const QDateTime& dt);
QWidget* createProjectThumb(const ProjectInfo& info, QWidget* parent,
                             ProjectPanel* panel);

// =============================================================================
// Table rebuild (filter + sort + populate)
// =============================================================================

void ProjectPanel::rebuildTable()
{
    QString prevSelected = selectedProjectName();
    int prevScroll = m_projectTable->verticalScrollBar()
                         ? m_projectTable->verticalScrollBar()->value()
                         : 0;

    m_projectTable->setRowCount(0);

    QVector<ProjectInfo> filtered;
    for (const auto& p : m_allProjects) {
        if (m_searchFilter.isEmpty()
            || p.name.contains(m_searchFilter, Qt::CaseInsensitive))
            filtered.append(p);
    }

    switch (m_sortMode) {
    case 0: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.lastModified > b.lastModified; }); break;
    case 1: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.lastModified < b.lastModified; }); break;
    case 2: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; }); break;
    case 3: std::sort(filtered.begin(), filtered.end(),
                [](const ProjectInfo& a, const ProjectInfo& b)
                { return a.name.compare(b.name, Qt::CaseInsensitive) > 0; }); break;
    }

    bool empty = filtered.isEmpty() && m_searchFilter.isEmpty()
                 && m_allProjects.isEmpty();
    m_projectTable->setVisible(!empty);
    m_emptyStateWidget->setVisible(empty);

    if (filtered.isEmpty()) return;

    const auto& c = Theme::colors();
    const auto& t = Theme::typography();

    m_projectTable->setRowCount(filtered.size());

    for (int row = 0; row < filtered.size(); ++row) {
        const auto& info = filtered[row];

        m_projectTable->setRowHeight(row, 190);

        m_projectTable->setCellWidget(row, 0,
            createProjectThumb(info, m_projectTable, this));

        QString display = info.name;
        if (info.isCurrent)
            display += QStringLiteral("  \u25CF CURRENT");
        auto* nameItem = new QTableWidgetItem(QStringLiteral("   ") + display);
        nameItem->setData(Qt::UserRole, info.name);
        nameItem->setData(Qt::UserRole + 1, info.filePath);
        nameItem->setData(Qt::UserRole + 2, info.show);
        QFont nameFont(t.fontFamily, 26,
                       info.isCurrent ? t.weightBold : t.weightSemiBold);
        nameItem->setFont(nameFont);
        if (info.isCurrent)
            nameItem->setForeground(c.accent);
        m_projectTable->setItem(row, 1, nameItem);

        auto* resItem = new QTableWidgetItem(
            QStringLiteral("%1\u00D7%2").arg(info.resW).arg(info.resH));
        resItem->setTextAlignment(Qt::AlignCenter);
        QFont resFont(t.fontFamily, 22);
        resItem->setFont(resFont);
        resItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 2, resItem);

        auto* fpsItem = new QTableWidgetItem(
            QString::number(info.fps, 'g', 4));
        fpsItem->setTextAlignment(Qt::AlignCenter);
        QFont fpsFont(t.fontFamily, 22);
        fpsItem->setFont(fpsFont);
        fpsItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 3, fpsItem);

        auto* sizeItem = new QTableWidgetItem(formatFileSize(info.fileSize));
        sizeItem->setTextAlignment(Qt::AlignCenter);
        QFont sizeFont(t.fontFamily, 22);
        sizeItem->setFont(sizeFont);
        sizeItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 4, sizeItem);

        auto* dateItem = new QTableWidgetItem(formatDate(info.lastModified));
        dateItem->setTextAlignment(Qt::AlignCenter);
        QFont dateFont(t.fontFamily, 22);
        dateItem->setFont(dateFont);
        dateItem->setForeground(c.textSecondary);
        m_projectTable->setItem(row, 5, dateItem);

        if (info.isCurrent) {
            for (int col = 0; col < 6; ++col) {
                if (auto* it = m_projectTable->item(row, col))
                    it->setBackground(c.accentSubtle);
            }
        }

        if (!prevSelected.isEmpty() && info.name == prevSelected)
            m_projectTable->selectRow(row);
    }

    if (auto* sb = m_projectTable->verticalScrollBar())
        sb->setValue(prevScroll);
}

// =============================================================================
// Selection helpers
// =============================================================================

QString ProjectPanel::selectedProjectName() const
{
    auto items = m_projectTable->selectedItems();
    if (items.isEmpty()) return {};
    int row = items.first()->row();
    auto* nameItem = m_projectTable->item(row, 1);
    return nameItem ? nameItem->data(Qt::UserRole).toString() : QString();
}

// =============================================================================
// Action button state
// =============================================================================

void ProjectPanel::updateActionButtons()
{
    bool hasSelection = !selectedProjectName().isEmpty();
    m_openActionBtn->setEnabled(hasSelection);
    m_dupeActionBtn->setEnabled(hasSelection);
    m_renameActionBtn->setEnabled(hasSelection);
    m_deleteActionBtn->setEnabled(hasSelection);

    const qreal opacity = hasSelection ? 1.0 : 0.4;
    for (auto* btn : {m_openActionBtn, m_dupeActionBtn,
                      m_renameActionBtn, m_deleteActionBtn}) {
        auto* effect = new QGraphicsOpacityEffect(btn);
        effect->setOpacity(opacity);
        btn->setGraphicsEffect(effect);
    }
}

// =============================================================================
// Responsive layout adjustments based on width
// =============================================================================

void ProjectPanel::applyResponsiveLayout()
{
    const int w = width();

    int thumbW, thumbH, rowH, railW, railBtnW, railBtnH;
    int namePt, dataPt, searchH, refreshSz, sortW;

    if (w < 1400) {
        thumbW = 160; thumbH = 100; rowH = 120;
        railW = 100; railBtnW = 84; railBtnH = 56;
        namePt = 16; dataPt = 14;
        searchH = 36; refreshSz = 36; sortW = 180;
    } else if (w < 2200) {
        thumbW = 220; thumbH = 140; rowH = 155;
        railW = 128; railBtnW = 106; railBtnH = 70;
        namePt = 21; dataPt = 18;
        searchH = 46; refreshSz = 46; sortW = 230;
    } else {
        thumbW = 280; thumbH = 180; rowH = 190;
        railW = 150; railBtnW = 128; railBtnH = 84;
        namePt = 26; dataPt = 22;
        searchH = 56; refreshSz = 56; sortW = 280;
    }

    if (m_searchInput) m_searchInput->setFixedHeight(searchH);
    if (m_sortCombo) {
        m_sortCombo->setFixedHeight(searchH);
        m_sortCombo->setFixedWidth(sortW);
    }
    if (m_refreshBtn) {
        m_refreshBtn->setFixedSize(refreshSz, refreshSz);
        m_refreshBtn->setIconSize(QSize(refreshSz / 2, refreshSz / 2));
    }

    if (m_projectTable) {
        const auto& c = Theme::colors();
        const auto& m = Theme::metrics();

        m_projectTable->verticalHeader()->setDefaultSectionSize(rowH);

        QString tableStyle = QString(
            "QTableWidget { background: %1; border: none; gridline-color: %2; "
            "font-size: %3pt; }"
            "QTableWidget::item { padding: %4px; }"
            "QTableWidget::item:selected { background: %6; }"
            "QHeaderView::section { background: %7; color: %8; font-size: %9pt; "
            "font-weight: 600; padding: %10px; border: none; "
            "border-bottom: 2px solid %11; border-right: 1px solid %5; }")
            .arg(Theme::rgb(c.surface0))
            .arg(Theme::rgb(c.border))
            .arg(dataPt)
            .arg(m.spacingMd)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.accentSubtle))
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.textSecondary))
            .arg(dataPt - 2)
            .arg(m.spacingMd)
            .arg(Theme::rgb(c.accent));
        m_projectTable->setStyleSheet(tableStyle);
    }
}

} // namespace rt
