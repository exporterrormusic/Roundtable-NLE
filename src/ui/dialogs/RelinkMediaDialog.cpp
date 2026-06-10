/*
 * RelinkMediaDialog.cpp — Premiere Pro-style media relink UI.
 */

#include "dialogs/RelinkMediaDialog.h"
#include "PathUtils.h"
#include "project/AssetDatabase.h"
#include "media/MediaPool.h"
#include "Theme.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QVBoxLayout>

namespace rt {

RelinkMediaDialog::RelinkMediaDialog(AssetDatabase* assetDb,
                                     MediaPool* pool,
                                     QWidget* parent)
    : QDialog(parent)
    , m_assetDb(assetDb)
    , m_pool(pool)
{
    setWindowTitle(tr("Link Media"));
    setMinimumSize(600, 400);

    auto* layout = new QVBoxLayout(this);

    auto* infoLabel = new QLabel(
        tr("The following media files are offline. Select an item and click "
           "\"Locate\" to find the file, or \"Relink All\" to search a folder."),
        this);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("Name"), tr("Original Path"), tr("Status")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    layout->addWidget(m_tree, 1);

    auto* buttonRow = new QHBoxLayout;
    m_relinkBtn = new QPushButton(tr("Locate..."), this);
    m_relinkBtn->setToolTip(tr("Browse for the selected file"));
    m_relinkAllBtn = new QPushButton(tr("Relink All..."), this);
    m_relinkAllBtn->setToolTip(tr("Search a folder for all offline files by name"));
    buttonRow->addWidget(m_relinkBtn);
    buttonRow->addWidget(m_relinkAllBtn);
    buttonRow->addStretch();

    m_statusLabel = new QLabel(this);
    buttonRow->addWidget(m_statusLabel);
    buttonRow->addStretch();

    m_closeBtn = new QPushButton(tr("Close"), this);
    buttonRow->addWidget(m_closeBtn);
    layout->addLayout(buttonRow);

    connect(m_relinkBtn, &QPushButton::clicked, this, &RelinkMediaDialog::relinkSelected);
    connect(m_relinkAllBtn, &QPushButton::clicked, this, &RelinkMediaDialog::relinkAll);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &RelinkMediaDialog::locateFile);

    populateList();
}

void RelinkMediaDialog::populateList()
{
    m_tree->clear();
    if (!m_assetDb) return;

    auto offlineIds = m_assetDb->findOfflineAssets();
    for (uint64_t id : offlineIds) {
        const auto* entry = m_assetDb->findById(id);
        if (!entry) continue;

        auto* item = new QTreeWidgetItem;
        item->setText(0, QString::fromStdString(entry->name));
        item->setText(1, QString::fromStdString(pathToUtf8(entry->absolutePath)));
        item->setText(2, tr("Offline"));
        item->setForeground(2, QColor(220, 60, 60));
        item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<quint64>(id)));
        m_tree->addTopLevelItem(item);
    }

    int count = m_tree->topLevelItemCount();
    m_statusLabel->setText(count == 0
        ? tr("All media online")
        : tr("%1 offline file(s)").arg(count));

    m_relinkBtn->setEnabled(count > 0);
    m_relinkAllBtn->setEnabled(count > 0);
}

void RelinkMediaDialog::relinkSelected()
{
    auto selected = m_tree->selectedItems();
    if (selected.isEmpty()) {
        // If nothing selected, treat as locate for the first item
        if (m_tree->topLevelItemCount() > 0)
            locateFile(m_tree->topLevelItem(0));
        return;
    }

    for (auto* item : selected)
        locateFile(item);
}

void RelinkMediaDialog::relinkAll()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select folder to search for offline media"));
    if (dir.isEmpty()) return;

    std::filesystem::path searchDir = dir.toStdString();
    int found = 0;

    for (int i = m_tree->topLevelItemCount() - 1; i >= 0; --i) {
        auto* item = m_tree->topLevelItem(i);
        uint64_t id = item->data(0, Qt::UserRole).toULongLong();
        const auto* entry = m_assetDb->findById(id);
        if (!entry) continue;

        // Search recursively by filename
        std::string filename = pathToUtf8(entry->absolutePath.filename());
        for (auto it = std::filesystem::recursive_directory_iterator(
                 searchDir, std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            if (pathToUtf8(it->path().filename()) == filename) {
                if (m_assetDb->relinkAsset(id, it->path())) {
                    m_relinked.push_back(id);
                    item->setText(2, tr("Relinked"));
                    item->setForeground(2, QColor(80, 200, 80));
                    item->setText(1, QString::fromStdString(pathToUtf8(it->path())));
                    ++found;
                }
                break;
            }
        }
    }

    m_statusLabel->setText(tr("Relinked %1 of %2 files")
        .arg(found).arg(m_tree->topLevelItemCount()));
}

void RelinkMediaDialog::locateFile(QTreeWidgetItem* item)
{
    if (!item) return;
    uint64_t id = item->data(0, Qt::UserRole).toULongLong();
    const auto* entry = m_assetDb->findById(id);
    if (!entry) return;

    QString filter = tr("Media Files (*.mov *.mp4 *.avi *.mkv *.webm *.wav *.flac *.mp3 *.png *.jpg *.jpeg *.tiff *.bmp);;All Files (*)");
    QString file = QFileDialog::getOpenFileName(
        this,
        tr("Locate \"%1\"").arg(QString::fromStdString(entry->name)),
        QString::fromStdString(pathToUtf8(entry->absolutePath.parent_path())),
        filter);
    if (file.isEmpty()) return;

    if (m_assetDb->relinkAsset(id, file.toStdString())) {
        m_relinked.push_back(id);
        item->setText(1, file);
        item->setText(2, tr("Relinked"));
        item->setForeground(2, QColor(80, 200, 80));

        int remaining = 0;
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            if (m_tree->topLevelItem(i)->text(2) == tr("Offline"))
                ++remaining;
        }
        m_statusLabel->setText(remaining == 0
            ? tr("All media relinked!")
            : tr("%1 offline file(s) remaining").arg(remaining));
    }
}

} // namespace rt
