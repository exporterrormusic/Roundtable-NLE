/*
 * ProjectPanelSidePanel.cpp — side panel management (show/hide/toggle),
 * slot handlers, and the "Open" list populator, extracted from
 * ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSet>
#include <QPixmap>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace rt {

// External free-function helpers (defined in ProjectPanelHelpers.cpp)
QString formatFileSize(qint64 bytes);
QString formatDate(const QDateTime& dt);

// =============================================================================
// Side Panel management
// =============================================================================

void ProjectPanel::toggleSidePanel(SidePanelMode mode)
{
    if (m_sidePanelMode == mode) {
        hideSidePanel();
    } else {
        showSidePanel(mode);
    }
}

void ProjectPanel::showSidePanel(SidePanelMode mode)
{
    // 460 (not 400): the NEW / OPEN / SETTINGS forms — e.g. the "1920×1080 FHD"
    // resolution button row — overflow a 400px panel and get clipped on the
    // right.  (Re-applied after the ProjectPanel redesign revert dropped it.)
    static constexpr int PANEL_WIDTH = 460;

    m_sidePanelMode = mode;

    m_newBtn->setChecked(mode == SidePanelMode::New);
    m_openFileBtn->setChecked(mode == SidePanelMode::Open);
    m_settingsBtn->setChecked(mode == SidePanelMode::Settings);

    if (mode == SidePanelMode::New) {
        m_sidePanelStack->setCurrentWidget(m_newPageScroll ? m_newPageScroll : m_newPage);
        m_nameInput->setFocus();
        m_nameInput->selectAll();
    } else if (mode == SidePanelMode::Open) {
        m_sidePanelStack->setCurrentWidget(m_openPage);
        populateOpenList();
        m_openList->setFocus();
    } else if (mode == SidePanelMode::Settings) {
        m_projDirInput->setText(m_projectsDir);
        m_sidePanelStack->setCurrentWidget(m_settingsPage);
    }

    if (m_sidePanel->isVisible() && m_sidePanel->width() > 10)
        return;

    m_sidePanel->setMinimumWidth(0);
    m_sidePanel->setMaximumWidth(0);
    m_sidePanel->setVisible(true);

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_sidePanel, "minimumWidth");
    animMin->setDuration(150);
    animMin->setStartValue(0);
    animMin->setEndValue(PANEL_WIDTH);
    animMin->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_sidePanel, "maximumWidth");
    animMax->setDuration(150);
    animMax->setStartValue(0);
    animMax->setEndValue(PANEL_WIDTH);
    animMax->setEasingCurve(QEasingCurve::OutCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        if (m_sidePanelMode != SidePanelMode::None) {
            m_sidePanel->setMinimumWidth(PANEL_WIDTH);
            m_sidePanel->setMaximumWidth(PANEL_WIDTH);
        }
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void ProjectPanel::hideSidePanel()
{
    if (m_sidePanelMode == SidePanelMode::None) return;
    m_sidePanelMode = SidePanelMode::None;

    m_newBtn->setChecked(false);
    m_openFileBtn->setChecked(false);
    m_settingsBtn->setChecked(false);

    auto* group = new QParallelAnimationGroup(this);

    auto* animMin = new QPropertyAnimation(m_sidePanel, "minimumWidth");
    animMin->setDuration(120);
    animMin->setStartValue(m_sidePanel->minimumWidth());
    animMin->setEndValue(0);
    animMin->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMin);

    auto* animMax = new QPropertyAnimation(m_sidePanel, "maximumWidth");
    animMax->setDuration(120);
    animMax->setStartValue(m_sidePanel->maximumWidth());
    animMax->setEndValue(0);
    animMax->setEasingCurve(QEasingCurve::InCubic);
    group->addAnimation(animMax);

    connect(group, &QParallelAnimationGroup::finished, this, [this]() {
        m_sidePanel->setVisible(false);
    });

    group->start(QAbstractAnimation::DeleteWhenStopped);
}

// =============================================================================
// Slots
// =============================================================================

void ProjectPanel::onCreateClicked()
{
    QString name = m_nameInput->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, "Cannot Create Project",
            "Please enter a project name.");
        m_nameInput->setFocus();
        return;
    }

    static const QString invalidChars = R"(/\:*?"<>|)";
    for (const QChar& ch : name) {
        if (invalidChars.contains(ch)) {
            QMessageBox::warning(this, "Invalid Project Name",
                QString("Project name cannot contain: %1\n\n"
                        "Please remove the invalid character '%2'.")
                    .arg(invalidChars, ch));
            return;
        }
    }

    QString saveDir = m_locationInput ? m_locationInput->text().trimmed() : QString();
    if (!saveDir.isEmpty()) {
        QDir dir(saveDir);
        if (!dir.exists()) {
            auto reply = QMessageBox::question(this, "Create Directory",
                QString("The save location does not exist:\n%1\n\n"
                        "Create this folder?").arg(saveDir),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (reply == QMessageBox::No)
                return;
            if (!dir.mkpath(".")) {
                QMessageBox::warning(this, "Cannot Create Directory",
                    "Failed to create the save location folder.\n"
                    "Please choose a different location.");
                return;
            }
        }
    }

    uint32_t resW = customResWidth();
    uint32_t resH = customResHeight();
    double fps = customFps();

    if (!saveDir.isEmpty())
        addRecentSaveLocation(saveDir);

    emit createProject(name, resW, resH, fps, saveDir);
    m_nameInput->clear();
    hideSidePanel();
}

void ProjectPanel::onSearchTextChanged(const QString& text)
{
    m_searchFilter = text.trimmed();
    rebuildTable();
}

void ProjectPanel::onSortChanged(int index)
{
    m_sortMode = index;
    rebuildTable();
}

void ProjectPanel::onFileTreeDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid()) return;
    QString path = m_fileModel->filePath(index);
    QFileInfo fi(path);
    if (fi.isFile()) {
        hideSidePanel();
        emit openFilePath(path);
    }
}

void ProjectPanel::onOpenListItemDoubleClicked(QListWidgetItem* item)
{
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty() && QFileInfo(path).isFile()) {
        hideSidePanel();
        emit openFilePath(path);
    }
}

// =============================================================================
// Open list populator
// =============================================================================

void ProjectPanel::populateOpenList()
{
    if (!m_openList) return;
    m_openList->clear();

    const auto& c = Theme::colors();
    constexpr int kThumbW = 160;
    constexpr int kThumbH = 100;

    QDir dir(m_projectsDir);
    QStringList filters;
    filters << "*.rtp";

    QFileInfoList files;
    for (const auto& subDir : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sub(subDir.absoluteFilePath());
        files.append(sub.entryInfoList(filters, QDir::Files, QDir::Time));
    }

    QSet<QString> seen;
    for (const auto& fi : files)
        seen.insert(QFileInfo(fi.absoluteFilePath()).absoluteFilePath().toLower());

    const QString projDirPrefix = QDir::toNativeSeparators(m_projectsDir).toLower();
    for (const auto& rp : m_recentPaths) {
        QFileInfo rfi(rp);
        QString normPath = rfi.absoluteFilePath().toLower();
        if (seen.contains(normPath)) continue;
        if (rfi.exists() && rfi.fileName().endsWith(".rtp", Qt::CaseInsensitive)) {
            if (normPath.startsWith(projDirPrefix))
                continue;
            files.append(rfi);
            seen.insert(normPath);
        }
    }

    std::sort(files.begin(), files.end(),
              [](const QFileInfo& a, const QFileInfo& b) {
                  return a.lastModified() > b.lastModified();
              });

    for (const QFileInfo& fi : files) {
        if (fi.fileName().endsWith(".autosave", Qt::CaseInsensitive))
            continue;

        auto* itemWidget = new QWidget;
        auto* hlay = new QHBoxLayout(itemWidget);
        hlay->setContentsMargins(4, 4, 4, 4);
        hlay->setSpacing(12);

        auto* thumbLabel = new QLabel;
        thumbLabel->setFixedSize(kThumbW, kThumbH);
        thumbLabel->setAlignment(Qt::AlignCenter);

        QString thumbPng = fi.absolutePath() + "/" + fi.baseName() + ".png";
        QString thumbJpg = fi.absolutePath() + "/" + fi.baseName() + ".jpg";
        QPixmap pix(thumbPng);
        if (pix.isNull()) pix.load(thumbJpg);

        if (!pix.isNull()) {
            thumbLabel->setPixmap(pix.scaled(kThumbW, kThumbH,
                                              Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
            thumbLabel->setStyleSheet(QStringLiteral(
                "background: %1; border-radius: 4px; border: 1px solid %2;")
                .arg(Theme::rgb(c.surface2))
                .arg(Theme::rgb(c.border)));
        } else {
            thumbLabel->setText("\U0001F3AC");
            thumbLabel->setStyleSheet(QStringLiteral(
                "background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                "stop:0 %1, stop:1 %2);"
                "border-radius: 4px; border: 1px solid %3;"
                "font-size: 32px; color: %4;")
                .arg(Theme::rgb(c.surface3))
                .arg(Theme::rgb(c.surface1))
                .arg(Theme::rgb(c.border))
                .arg(Theme::rgb(c.textTertiary)));
        }
        hlay->addWidget(thumbLabel);

        auto* infoLayout = new QVBoxLayout;
        infoLayout->setSpacing(4);
        infoLayout->setContentsMargins(0, 4, 0, 4);

        auto* nameLabel = new QLabel(fi.baseName());
        nameLabel->setStyleSheet(QStringLiteral(
            "font-size: 15px; font-weight: 600; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textPrimary)));
        nameLabel->setWordWrap(true);
        infoLayout->addWidget(nameLabel);

        auto* dateLabel = new QLabel(formatDate(fi.lastModified()));
        dateLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textSecondary)));
        infoLayout->addWidget(dateLabel);

        auto* sizeLabel = new QLabel(formatFileSize(fi.size()));
        sizeLabel->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1; background: transparent;")
            .arg(Theme::rgb(c.textTertiary)));
        infoLayout->addWidget(sizeLabel);

        infoLayout->addStretch();
        hlay->addLayout(infoLayout, 1);

        auto* listItem = new QListWidgetItem;
        listItem->setData(Qt::UserRole, fi.absoluteFilePath());
        listItem->setSizeHint(QSize(0, kThumbH + 12));
        m_openList->addItem(listItem);
        m_openList->setItemWidget(listItem, itemWidget);
    }
}

} // namespace rt
