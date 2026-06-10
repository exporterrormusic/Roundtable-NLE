/*
 * ProjectBinQueries.cpp — Query methods for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: allFiles, filesOfType, binFolderState, restoreBinFolders
 */

#include "panels/project/ProjectBin.h"
#include "PathUtils.h"
#include "panels/project/ProjectBinInternal.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "media/MediaSourceService.h"

#include <QTreeWidgetItem>

#include <algorithm>
#include <functional>
#include <map>

namespace rt {

std::vector<Project::BinItem> ProjectBin::exportBinItems() const
{
    std::vector<Project::BinItem> out;
    if (!m_grid) return out;
    const auto& items = m_grid->items();
    out.reserve(items.size());
    for (const auto& it : items) {
        if (it.isFolder || it.filePath.empty())
            continue;
        Project::BinItem bi;
        bi.id          = it.itemId;
        bi.path        = it.filePath;
        bi.displayName = (it.displayName.isEmpty()
            ? QString::fromStdString(pathToUtf8(it.filePath.filename()))
            : it.displayName).toStdString();
        bi.labelColor  = it.labelColor;
        out.push_back(std::move(bi));
    }
    return out;
}

void ProjectBin::restoreBinModel(const std::vector<Project::BinItem>& items,
                                 const std::vector<BinFolderState>& folders)
{
    if (!m_grid) return;
    clearAll();

    for (const auto& bi : items) {
        if (bi.path.empty()) continue;
        // Synthetic adjustment-layer items: no source to open, force orange
        // label even if the saved color drifted.
        if (projectBinIsAdjustmentPath(bi.path)) {
            uint32_t color = (bi.labelColor != 0xFF888888)
                ? bi.labelColor : 0xFFFFAA44;
            m_grid->addRestoredItem(
                bi.path, MediaType::Unknown, /*handle*/ 0, bi.id,
                QString::fromStdString(bi.displayName), color);
            continue;
        }
        // Do NOT open the media source synchronously here.  Opening every
        // bin item on the UI thread (m_mediaSources->openSource → MediaPool
        // probe + NVDEC init, ~25-150ms each) froze project-open for seconds
        // on bins with dozens of items.  The handle isn't needed at restore:
        //   • thumbnails are generated from the file PATH (async
        //     ThumbnailGenerator, via loadVisibleThumbnails below),
        //   • the "media offline" overlay keys off file existence, not the
        //     handle, and
        //   • timeline drops / source-monitor opens re-open on demand.
        // Timeline media is additionally warmed in parallel by
        // preOpenVideoMedia().  Pass handle 0 and let it open lazily.
        m_grid->addRestoredItem(
            bi.path, MediaType::Unknown, /*handle*/ 0, bi.id,
            QString::fromStdString(bi.displayName), bi.labelColor);
    }
    m_grid->loadVisibleThumbnails();
    syncListView();                 // builds tree rows w/ kBinItemIdRole
    if (!folders.empty())
        restoreBinFolders(folders); // matches "@<id>" keys
    if (!m_listView)
        syncIconView();
}

std::vector<std::filesystem::path> ProjectBin::allFiles() const
{
    std::vector<std::filesystem::path> result;
    const auto& items = m_grid->items();
    result.reserve(items.size());
    for (const auto& item : items) {
        // Skip synthetic items (sequences, bin folders) introduced by
        // syncIconView() — they have empty file paths and saving them
        // would create spurious "Unknown"-type entries on reload.
        if (item.isFolder || item.filePath.empty())
            continue;
        result.push_back(item.filePath);
    }
    return result;
}

std::vector<std::filesystem::path> ProjectBin::filesOfType(MediaType type) const
{
    std::vector<std::filesystem::path> result;
    for (const auto& item : m_grid->items())
    {
        if (item.type == type)
            result.push_back(item.filePath);
    }
    return result;
}

std::vector<BinFolderState> ProjectBin::binFolderState() const
{
    std::vector<BinFolderState> folders;
    std::function<void(QTreeWidgetItem*, const QString&)> visitBin =
        [&](QTreeWidgetItem* item, const QString& path) {
            BinFolderState folder;
            folder.name = path.toStdString();
            folder.expanded = item->isExpanded();

            for (int c = 0; c < item->childCount(); ++c) {
                auto* child = item->child(c);
                if (child->data(0, Qt::UserRole + 2).toBool()) {
                    QString childPath = path;
                    if (!childPath.isEmpty()) childPath += '/';
                    childPath += child->text(0);
                    visitBin(child, childPath);
                } else {
                    // Prefer the file path over the per-instance "@<id>" key
                    // here: applyBinSnapshot() does clearAll()+addFiles(),
                    // which assigns FRESH ids to rebuilt items, so an id-key
                    // captured pre-rebuild can never match anything after.
                    // The path round-trips through addFiles → syncListView
                    // (which writes the path back to Qt::UserRole), so
                    // reparentByKey()'s pathKey-fallback matches it cleanly.
                    // Without this, undo of an Explorer import (and any
                    // applyBinSnapshot-based undo) flattens every bin to root.
                    QString k = child->data(0, Qt::UserRole).toString();
                    if (k.isEmpty()) k = projectBinItemKey(child);
                    folder.childKeys.push_back(k.toStdString());
                }
            }

            folders.push_back(std::move(folder));
        };

    for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (!item->data(0, Qt::UserRole + 2).toBool()) continue;
        visitBin(item, item->text(0));
    }

    return folders;
}

void ProjectBin::restoreBinFolders(const std::vector<BinFolderState>& folders)
{
    if (folders.empty()) return;

    // Helper to find and reparent an item by key from top-level items.
    // Matches the saved key against the candidate's "@<id>" key (new
    // per-instance format) OR its legacy path/text key, so both new and
    // pre-v14 projects reparent correctly.
    auto reparentByKey = [this](QTreeWidgetItem* parent, const QString& qKey) {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* candidate = m_listWidget->topLevelItem(i);
            if (candidate == parent) continue;
            if (candidate->data(0, Qt::UserRole + 2).toBool()) continue;
            const quint64 id = candidate->data(0, kBinItemIdRole).toULongLong();
            QString idKey = id ? (QStringLiteral("@") + QString::number(id))
                               : QString();
            QString pathKey = candidate->data(0, Qt::UserRole).toString();
            QString textKey = candidate->text(0);
            if ((!idKey.isEmpty()   && qKey == idKey)   ||
                (!pathKey.isEmpty() && qKey == pathKey)  ||
                (pathKey.isEmpty()  && qKey == textKey)) {
                m_listWidget->takeTopLevelItem(i);
                parent->addChild(candidate);
                return;
            }
        }
    };

    std::map<QString, QTreeWidgetItem*> binMap;
    std::vector<const BinFolderState*> sortedFolders;
    sortedFolders.reserve(folders.size());
    for (const auto& bf : folders) {
        sortedFolders.push_back(&bf);
    }
    std::sort(sortedFolders.begin(), sortedFolders.end(),
              [](const BinFolderState* a, const BinFolderState* b) {
                  auto depthOf = [](const std::string& name) {
                      return static_cast<int>(std::count(name.begin(), name.end(), '/'));
                  };
                  return depthOf(a->name) < depthOf(b->name);
              });

    for (const BinFolderState* folder : sortedFolders) {
        const QString path = QString::fromStdString(folder->name);
        const QStringList parts = path.split('/', Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;

        QTreeWidgetItem* parentBin = nullptr;
        QString currentPath;
        for (const QString& part : parts) {
            if (!currentPath.isEmpty()) currentPath += '/';
            currentPath += part;

            auto it = binMap.find(currentPath);
            if (it != binMap.end()) {
                parentBin = it->second;
                continue;
            }

            QTreeWidgetItem* binItem = nullptr;
            if (parentBin) {
                binItem = projectBinFindChildBin(parentBin, part);
                if (!binItem) {
                    binItem = projectBinCreateBinItem(part);
                    parentBin->addChild(binItem);
                }
            } else {
                for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
                    auto* candidate = m_listWidget->topLevelItem(i);
                    if (candidate->data(0, Qt::UserRole + 2).toBool() && candidate->text(0) == part) {
                        binItem = candidate;
                        break;
                    }
                }
                if (!binItem) {
                    binItem = projectBinCreateBinItem(part);
                    m_listWidget->addTopLevelItem(binItem);
                }
            }

            binMap[currentPath] = binItem;
            parentBin = binItem;
        }
    }

    for (const BinFolderState* folder : sortedFolders) {
        const QString path = QString::fromStdString(folder->name);
        auto it = binMap.find(path);
        if (it == binMap.end()) continue;
        auto* binItem = it->second;
        binItem->setExpanded(folder->expanded);
        for (const auto& key : folder->childKeys)
            reparentByKey(binItem, QString::fromStdString(key));
    }
}

} // namespace rt
