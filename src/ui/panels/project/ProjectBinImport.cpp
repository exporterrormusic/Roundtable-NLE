/*
 * ProjectBinImport.cpp — File import and item management for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: importFiles, addFiles, addFilesToBin, addFilesToNamedBin,
 * removeFile, clearAll, refreshAllViews, selectAllItems, ensureDefaultBins,
 * itemCount
 */

#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "media/MediaPool.h"
#include "media/MediaSourceService.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QTreeWidgetItem>
#include <QInputDialog>
#include <QLineEdit>

#include "Settings.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace rt {

// -----------------------------------------------------------------------------
//  Import / manage items
// -----------------------------------------------------------------------------

void ProjectBin::importFiles()
{
    auto settings = rt::appSettings();
    QString lastDir = settings.value("Import/lastDir", QString()).toString();
    if (lastDir.isEmpty())
        lastDir = QDir::homePath();

    QStringList files = QFileDialog::getOpenFileNames(
        this,
        "Import Media",
        lastDir,
        "All Files (*.*);;"
        "Video (*.mp4 *.mkv *.avi *.mov *.webm);;"
        "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;"
        "Audio (*.wav *.mp3 *.flac *.ogg *.aac);;"
        "Spine (*.skel *.json)");

    if (files.isEmpty()) return;

    // Persist the last import directory
    QString dir = QFileInfo(files.first()).absolutePath();
    settings.setValue("Import/lastDir", dir);
    settings.sync();

    std::vector<std::filesystem::path> paths;
    paths.reserve(files.size());
    for (const auto& f : files)
        paths.emplace_back(f.toStdString());

    // If the user has a bin selected, import into that bin instead of root.
    // Uses selectedItems() (not currentItem()) to avoid the persistent
    // currentItem from a previous click causing silent nesting.
    QTreeWidgetItem* targetBin = nullptr;
    if (m_listWidget) {
        const auto& sel = m_listWidget->selectedItems();
        if (sel.size() == 1 && sel.first()->data(0, Qt::UserRole + 2).toBool())
            targetBin = sel.first();
    }

    // Snapshot before/after so Ctrl+Z can undo the import, exactly like
    // the drag-and-drop path in handleDropEvent().
    auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());

    if (targetBin)
        addFilesToBin(paths, targetBin);
    else
        addFiles(paths);

    if (m_commandStack) {
        auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Import Media",
            [this, after]()  { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*after); },
            [this, before]() { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*before); }));
    }
}

void ProjectBin::addFiles(const std::vector<std::filesystem::path>& files)
{
    for (const auto& f : files) {
        // Skip empty paths (synthetic items saved inadvertently)
        if (f.empty())
            continue;
        // Skip duplicates — Premiere Pro silently ignores re-imports
        if (m_grid->hasItem(f))
            continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();
    // Always sync the list view (source of truth for bins)
    syncListView();
    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToBin(const std::vector<std::filesystem::path>& files,
                               QTreeWidgetItem* targetBin)
{
    // Save the target bin's FULL PATH (root→leaf bin-name breadcrumbs)
    // BEFORE syncListView clears the tree. The raw QTreeWidgetItem* pointer
    // becomes dangling after m_listWidget->clear(), and a simple name
    // lookup at top level fails for nested sub-bins.
    QStringList targetBinPath;
    for (auto* node = targetBin; node; node = node->parent()) {
        if (node->data(0, Qt::UserRole + 2).toBool())
            targetBinPath.prepend(node->text(0));
    }

    // Add to the grid (data model), skipping duplicates
    for (const auto& f : files) {
        if (m_grid->hasItem(f))
            continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();

    // Rebuild the list view (creates tree items at top-level)
    syncListView();

    // If a target bin was specified, walk the saved breadcrumb path
    // to locate the same bin again (handles nested sub-bins correctly).
    if (!targetBinPath.isEmpty()) {
        QTreeWidgetItem* bin = nullptr;
        // Find top-level bin matching the first path segment
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == targetBinPath.first()) {
                bin = it;
                break;
            }
        }
        // Descend through remaining path segments
        for (int depth = 1; bin && depth < targetBinPath.size(); ++depth) {
            QTreeWidgetItem* next = nullptr;
            for (int i = 0; i < bin->childCount(); ++i) {
                auto* ch = bin->child(i);
                if (ch->data(0, Qt::UserRole + 2).toBool() && ch->text(0) == targetBinPath[depth]) {
                    next = ch;
                    break;
                }
            }
            bin = next;
        }
        if (bin) {
            for (const auto& f : files) {
                QString key = QString::fromStdString(f.string());
                for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
                    auto* it = m_listWidget->topLevelItem(i);
                    if (it->data(0, Qt::UserRole).toString() == key) {
                        auto* taken = m_listWidget->takeTopLevelItem(i);
                        if (taken) bin->addChild(taken);
                        break;
                    }
                }
            }
            bin->setExpanded(true);
        }
    }

    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToNamedBin(const std::vector<std::filesystem::path>& files,
                                    const QString& binName,
                                    const QString& parentBinName)
{
    if (files.empty() || binName.isEmpty()) return;

    // Add to the grid (data model), skipping duplicates
    for (const auto& f : files) {
        if (m_grid->hasItem(f)) continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();
    syncListView();

    // Find or create the parent bin
    QTreeWidgetItem* parentBin = nullptr;
    if (!parentBinName.isEmpty()) {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == parentBinName) {
                parentBin = it;
                break;
            }
        }
    }

    // Find or create the target bin
    auto findBinUnder = [](QTreeWidgetItem* parent, const QString& name) -> QTreeWidgetItem* {
        if (!parent) return nullptr;
        for (int i = 0; i < parent->childCount(); ++i) {
            auto* child = parent->child(i);
            if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == name)
                return child;
        }
        return nullptr;
    };

    QTreeWidgetItem* targetBin = nullptr;
    if (parentBin) {
        targetBin = findBinUnder(parentBin, binName);
    } else {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == binName) {
                targetBin = it;
                break;
            }
        }
    }

    if (!targetBin) {
        targetBin = new QTreeWidgetItem();
        targetBin->setText(0, binName);
        targetBin->setData(0, Qt::UserRole + 2, true);
        targetBin->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
        targetBin->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
        targetBin->setFlags(targetBin->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
        if (parentBin)
            parentBin->addChild(targetBin);
        else
            m_listWidget->addTopLevelItem(targetBin);
    }

    // Move the newly-added files into the target bin
    for (const auto& f : files) {
        QString key = QString::fromStdString(f.string());
        for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole).toString() == key) {
                auto* taken = m_listWidget->takeTopLevelItem(i);
                if (taken) targetBin->addChild(taken);
                break;
            }
        }
        // Also check children of other bins (in case syncListView placed them elsewhere)
        if (parentBin) {
            for (int bi = 0; bi < m_listWidget->topLevelItemCount(); ++bi) {
                auto* bin = m_listWidget->topLevelItem(bi);
                if (bin == targetBin || !bin->data(0, Qt::UserRole + 2).toBool()) continue;
                for (int ci = bin->childCount() - 1; ci >= 0; --ci) {
                    auto* child = bin->child(ci);
                    if (child->data(0, Qt::UserRole).toString() == key) {
                        auto* taken = bin->takeChild(ci);
                        if (taken) targetBin->addChild(taken);
                    }
                }
            }
        }
    }

    targetBin->setExpanded(true);
    if (parentBin) parentBin->setExpanded(true);

    if (!m_listView) syncIconView();
}

bool ProjectBin::removeFile(const std::filesystem::path& filePath)
{
    return m_grid->removeItem(filePath);
}

void ProjectBin::clearAll()
{
    m_grid->clearItems();
    // Also clear the tree/list widget so stale items from a previous
    // project don't linger even if refreshAllViews() isn't called.
    if (m_listWidget) {
        m_listWidget->blockSignals(true);
        m_listWidget->clear();
        m_dropHighlightItem = nullptr;  // tree items destroyed by clear()
        m_listWidget->blockSignals(false);
    }
}

void ProjectBin::refreshAllViews()
{
    // Force-sync both views so that stale tree items from a previous
    // project are purged regardless of which view mode is active.
    syncListView();
    syncIconView();
}

void ProjectBin::selectAllItems()
{
    if (m_listWidget)
        m_listWidget->selectAll();
}

void ProjectBin::ensureDefaultBins()
{
    // Collect existing bin names
    std::set<QString> existingBins;
    for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 2).toBool())
            existingBins.insert(item->text(0));
    }

    // Default bin structure -- only auto-create the VO bin.  Other
    // categories (sequences, video, GFX) are no longer auto-generated;
    // the user wants a clean project bin with just VO for synced audio.
    struct DefaultBin {
        QString name;
        std::vector<QString> subBins;
    };
    std::vector<DefaultBin> defaults = {
        {"VO", {}},
    };

    auto makeBinItem = [this](const QString& name) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, name);
        item->setData(0, Qt::UserRole + 2, true);
        item->setIcon(0, makePremiereBinIcon(kLabelBin, "bin"));
        item->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelBin));
        item->setFlags(item->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable);
        return item;
    };

    for (const auto& def : defaults) {
        QTreeWidgetItem* binItem = nullptr;
        if (existingBins.contains(def.name)) {
            // Parent bin exists — find it so we can ensure sub-bins
            for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
                auto* it = m_listWidget->topLevelItem(i);
                if (it->data(0, Qt::UserRole + 2).toBool() && it->text(0) == def.name) {
                    binItem = it;
                    break;
                }
            }
        } else {
            binItem = makeBinItem(def.name);
            m_listWidget->addTopLevelItem(binItem);
        }
        if (!binItem) continue;

        // Ensure sub-bins exist
        for (const auto& sub : def.subBins) {
            bool found = false;
            for (int ci = 0; ci < binItem->childCount(); ++ci) {
                if (binItem->child(ci)->data(0, Qt::UserRole + 2).toBool() &&
                    binItem->child(ci)->text(0) == sub) {
                    found = true;
                    break;
                }
            }
            if (!found)
                binItem->addChild(makeBinItem(sub));
        }
        binItem->setExpanded(true);
    }

    // Auto-sort existing top-level items into bins
    // Sequences → 1_SEQUENCES, Audio → 2_AUDIO, Video/Image → 3_VIDEO, Spine → 4_GFX
    auto findBin = [this](const QString& name) -> QTreeWidgetItem* {
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* item = m_listWidget->topLevelItem(i);
            if (item->data(0, Qt::UserRole + 2).toBool() && item->text(0) == name)
                return item;
        }
        return nullptr;
    };

    QTreeWidgetItem* voBin = findBin("VO");

    // Gather items to reparent (can't remove while iterating)
    std::vector<std::pair<QTreeWidgetItem*, QTreeWidgetItem*>> moves; // item, target bin
    for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
        auto* item = m_listWidget->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 2).toBool()) continue; // skip bins

        QString typeStr = item->text(1);
        if (typeStr == "Audio" && voBin) {
            moves.emplace_back(item, voBin);
        }
    }

    for (auto& [item, bin] : moves) {
        int idx = m_listWidget->indexOfTopLevelItem(item);
        if (idx >= 0) {
            m_listWidget->takeTopLevelItem(idx);
            bin->addChild(item);
        }
    }

    spdlog::info("ProjectBin: ensureDefaultBins — {} bins created, {} items sorted",
                 defaults.size(), moves.size());
}

int ProjectBin::itemCount() const noexcept
{
    return m_grid->itemCount();
}

} // namespace rt
