/*
 * ProjectBinImport.cpp — File import and item management for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: importFiles, addFiles, addFilesToBin, addFilesToNamedBin,
 * removeFile, clearAll, refreshAllViews, selectAllItems, ensureDefaultBins,
 * itemCount
 */

#include "panels/project/ProjectBin.h"
#include "PathUtils.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"

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
#include <cctype>
#include <filesystem>
#include <set>

namespace rt {

namespace {

std::string automaticImportPathKey(const std::filesystem::path& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        normalized = path.lexically_normal();
    std::string key = pathToUtf8(normalized);
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
#endif
    return key;
}

} // namespace

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
        "Video (*.mp4 *.m4v *.mkv *.avi *.mov *.webm *.ts *.mts *.m2ts "
        "*.mpg *.mpeg *.wmv *.flv *.mxf *.gif);;"
        "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp *.tif *.tiff "
        "*.avif *.jxl);;"
        "Audio (*.wav *.mp3 *.flac *.ogg *.aac *.m4a *.opus *.wma *.aif "
        "*.aiff);;"
        "Spine (*.skel *.json)");

    if (files.isEmpty()) return;

    // Persist the last import directory
    QString dir = QFileInfo(files.first()).absolutePath();
    settings.setValue("Import/lastDir", dir);
    settings.sync();

    std::vector<std::filesystem::path> paths;
    paths.reserve(files.size());
    for (const auto& f : files)
        paths.emplace_back(utf8ToPath(f.toStdString()));

    // Decide the destination bin as a breadcrumb PATH (root→leaf bin names).
    //  1. An explicitly selected bin in the tree wins (most specific intent).
    //     Uses selectedItems() (not currentItem()) to avoid a stale currentItem
    //     from a previous click causing silent nesting.
    //  2. Otherwise, the open sub-bin folder (m_iconBinPath) is the target.
    //     The user navigates INTO a bin — in icon view, or via a sub-bin tab in
    //     list view — without that bin being "selected" in the tree. This was
    //     previously ignored, so importing while browsing a bin dumped files
    //     into root; the open-folder view then resynced and appeared to "show
    //     everything". m_iconBinPath is empty only at the root tab, so this is
    //     a no-op there.
    QStringList targetBinPath;
    if (m_listWidget) {
        const auto& sel = m_listWidget->selectedItems();
        if (sel.size() == 1 && sel.first()->data(0, Qt::UserRole + 2).toBool()) {
            for (auto* node = sel.first(); node; node = node->parent())
                if (node->data(0, Qt::UserRole + 2).toBool())
                    targetBinPath.prepend(node->text(0));
        }
    }
    if (targetBinPath.isEmpty() && !m_iconBinPath.isEmpty())
        targetBinPath = m_iconBinPath;

    // Snapshot before/after so Ctrl+Z can undo the import, exactly like
    // the drag-and-drop path in handleDropEvent().
    auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());

    if (!targetBinPath.isEmpty())
        addFilesToBinPath(paths, targetBinPath);
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
    const bool restoreFocusedBin = m_listView && m_listViewFocused
        && !m_iconBinPath.isEmpty();
    const auto savedFolders = restoreFocusedBin
        ? m_rootFolderState : binFolderState();

    for (const auto& f : files) {
        // Skip empty paths (synthetic items saved inadvertently)
        if (f.empty())
            continue;
        // Explicit imports create independent master items. Stable item IDs,
        // not source paths, distinguish duplicate references in the bin.
        // Synthetic adjustment-layer items: no source media to open, no
        // thumbnail to load. Recreate them with their existing name.
        if (projectBinIsAdjustmentPath(f)) {
            QString name = QString::fromStdString(pathToUtf8(f.stem()));
            m_grid->addRestoredItem(f, MediaType::Unknown, /*handle*/ 0,
                                    /*itemId*/ 0, name,
                                    /*labelColor*/ 0xFFFFAA44);
            continue;
        }
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
    }
    m_grid->loadVisibleThumbnails();
    // Always sync the list view (source of truth for bins)
    syncListView(savedFolders.empty() ? nullptr : &savedFolders);
    if (restoreFocusedBin)
        focusListViewOnBin();
    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToBin(const std::vector<std::filesystem::path>& files,
                               QTreeWidgetItem* targetBin)
{
    // Resolve the target bin to its FULL breadcrumb PATH (root→leaf bin names)
    // BEFORE delegating: addFilesToBinPath's syncListView clears the tree, so
    // the raw QTreeWidgetItem* would dangle. Capturing the path here keeps the
    // bin findable again (and handles nested sub-bins, which a top-level name
    // lookup would miss).
    QStringList targetBinPath;
    for (auto* node = targetBin; node; node = node->parent()) {
        if (node->data(0, Qt::UserRole + 2).toBool())
            targetBinPath.prepend(node->text(0));
    }
    if (targetBinPath.isEmpty() && !m_iconBinPath.isEmpty())
        targetBinPath = m_iconBinPath;
    addFilesToBinPath(files, targetBinPath);
}

void ProjectBin::addFilesToBinPath(const std::vector<std::filesystem::path>& files,
                                   const QStringList& targetBinPath)
{
    const bool restoreFocusedBin = m_listView && m_listViewFocused
        && !m_iconBinPath.isEmpty();
    const auto savedFolders = restoreFocusedBin
        ? m_rootFolderState : binFolderState();

    std::vector<uint64_t> addedIds;
    addedIds.reserve(files.size());
    for (const auto& f : files) {
        if (f.empty()) continue;
        if (projectBinIsAdjustmentPath(f)) {
            QString name = QString::fromStdString(pathToUtf8(f.stem()));
            const int index = m_grid->addRestoredItem(
                f, MediaType::Unknown, /*handle*/ 0, /*itemId*/ 0, name,
                /*labelColor*/ 0xFFFFAA44);
            if (index >= 0)
                addedIds.push_back(
                    m_grid->items()[static_cast<size_t>(index)].itemId);
            continue;
        }
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
        addedIds.push_back(m_grid->items().back().itemId);
    }
    m_grid->loadVisibleThumbnails();

    // Rebuild the list view (creates tree items at top-level)
    syncListView(savedFolders.empty() ? nullptr : &savedFolders);

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
            for (uint64_t addedId : addedIds) {
                for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
                    auto* it = m_listWidget->topLevelItem(i);
                    if (it->data(0, kBinItemIdRole).toULongLong() == addedId) {
                        auto* taken = m_listWidget->takeTopLevelItem(i);
                        if (taken) bin->addChild(taken);
                        break;
                    }
                }
            }
            bin->setExpanded(true);
        }
    }

    if (restoreFocusedBin)
        focusListViewOnBin();
    if (!m_listView)
        syncIconView();
}

void ProjectBin::addFilesToNamedBin(const std::vector<std::filesystem::path>& files,
                                    const QString& binName,
                                    const QString& parentBinName)
{
    if (files.empty() || binName.isEmpty()) return;

    const bool restoreFocusedBin = m_listView && m_listViewFocused
        && !m_iconBinPath.isEmpty();
    const auto savedFolders = restoreFocusedBin
        ? m_rootFolderState : binFolderState();

    // Explicit imports are independent master items, even when two items
    // reference the same source path. Track their stable IDs so only these
    // new instances are moved into the requested bin after rebuilding.
    std::vector<uint64_t> addedIds;
    addedIds.reserve(files.size());
    for (const auto& f : files) {
        if (f.empty()) continue;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto result = m_mediaSources->openSource({f, RenderRequestType::Still, false});
            handle = result.handle;
        }
        m_grid->addItem(f, MediaType::Unknown, handle);
        addedIds.push_back(m_grid->items().back().itemId);
    }
    m_grid->loadVisibleThumbnails();
    syncListView(savedFolders.empty() ? nullptr : &savedFolders);

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
        targetBin = new NaturalTreeWidgetItem();
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

    // Move the exact newly-added instances into the target bin. Matching by
    // path would grab an older duplicate instead.
    for (uint64_t addedId : addedIds) {
        for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, kBinItemIdRole).toULongLong() == addedId) {
                auto* taken = m_listWidget->takeTopLevelItem(i);
                if (taken) targetBin->addChild(taken);
                break;
            }
        }
    }

    targetBin->setExpanded(true);
    if (parentBin) parentBin->setExpanded(true);

    if (restoreFocusedBin)
        focusListViewOnBin();
    if (!m_listView) syncIconView();
}

void ProjectBin::addMissingFilesToNamedBin(
    const std::vector<std::filesystem::path>& files,
    const QString& binName,
    const QString& parentBinName)
{
    if (files.empty() || binName.isEmpty()) return;

    // Automatic workflows (sync/export, generated VO approval) should be
    // idempotent. Manual import still deliberately uses addFiles* directly,
    // where duplicate master items remain supported.
    std::set<std::string> represented;
    if (m_grid) {
        for (const auto& item : m_grid->items()) {
            if (!item.isFolder && !item.filePath.empty())
                represented.insert(automaticImportPathKey(item.filePath));
        }
    }

    std::vector<std::filesystem::path> missing;
    missing.reserve(files.size());
    for (const auto& file : files) {
        const std::string key = automaticImportPathKey(file);
        if (!key.empty() && represented.insert(key).second)
            missing.push_back(file);
    }

    if (!missing.empty())
        addFilesToNamedBin(missing, binName, parentBinName);
}

bool ProjectBin::removeFile(const std::filesystem::path& filePath)
{
    return m_grid->removeItem(filePath);
}

bool ProjectBin::removeItemById(uint64_t itemId)
{
    return itemId != 0 && m_grid && m_grid->removeItemById(itemId);
}

void ProjectBin::replaceMedia(QTreeWidgetItem* selected)
{
    if (!selected) return;

    const QString oldPath = selected->data(0, Qt::UserRole).toString();
    const QString displayName = selected->text(0);
    if (oldPath.isEmpty()) return;

    // The workspace owns the actual mutation so the bin, every sequence,
    // decode caches, and undo history change as one operation.
    promptRelinkMedia(utf8ToPath(oldPath.toStdString()), displayName);
}

void ProjectBin::promptRelinkMedia(const std::filesystem::path& oldPath,
                                   const QString& displayName)
{
    if (oldPath.empty()) return;

    auto settings = rt::appSettings();
    QString startDir = settings.value("Import/lastDir", QString()).toString();
    if (startDir.isEmpty() || !QDir(startDir).exists()) {
        startDir = QFileInfo(QString::fromStdString(pathToUtf8(oldPath)))
                       .absolutePath();
    }
    if (startDir.isEmpty() || !QDir(startDir).exists())
        startDir = QDir::homePath();

    const QString newFile = QFileDialog::getOpenFileName(
        this, tr("Re-link Media - %1").arg(displayName), startDir,
        QStringLiteral("Media Files (*.mp4 *.m4v *.mkv *.avi *.mov *.webm "
                       "*.ts *.mts *.m2ts *.mpg *.mpeg *.wmv *.flv *.mxf "
                       "*.gif *.png *.jpg *.jpeg *.bmp *.tga *.webp *.tif "
                       "*.tiff *.avif *.jxl *.wav *.mp3 *.flac *.ogg *.aac "
                       "*.m4a *.opus *.wma *.aif *.aiff);;All Files (*.*)"));
    if (newFile.isEmpty()) return;

    settings.setValue("Import/lastDir", QFileInfo(newFile).absolutePath());
    settings.sync();
    const auto newPath = utf8ToPath(newFile.toStdString());
    if (newPath == oldPath) return;

    emit mediaRelinkRequested(
        QString::fromStdString(pathToUtf8(oldPath)),
        QString::fromStdString(pathToUtf8(newPath)));
}

int ProjectBin::replacePathReferences(const std::filesystem::path& oldPath,
                                      const std::filesystem::path& newPath)
{
    if (!m_grid || oldPath.empty() || newPath.empty() || oldPath == newPath)
        return 0;

    auto savedFolders = binFolderState();
    const std::string oldUtf8 = pathToUtf8(oldPath);
    const std::string newUtf8 = pathToUtf8(newPath);
    auto rewriteFolderKeys = [&](std::vector<BinFolderState>& folders) {
        for (auto& folder : folders)
            for (auto& key : folder.childKeys)
                if (key == oldUtf8) key = newUtf8;
    };
    rewriteFolderKeys(savedFolders);
    rewriteFolderKeys(m_rootFolderState);

    uint64_t newHandle = 0;
    if (m_mediaSources) {
        auto result = m_mediaSources->openSource(
            {newPath, RenderRequestType::Still, false});
        newHandle = result.handle;
    }

    int updated = 0;
    for (auto& item : m_grid->mutableItems()) {
        if (item.isFolder || item.filePath != oldPath) continue;
        item.filePath = newPath;
        item.type = ThumbnailGenerator::detectMediaType(newPath);
        item.mediaHandle = newHandle;
        item.thumbnail.reset();
        ++updated;
    }
    if (updated == 0) return 0;

    syncListView(&savedFolders);
    if (!m_listView) syncIconView();
    m_grid->loadVisibleThumbnails();

    spdlog::info("ProjectBin: relinked {} item(s) '{}' -> '{}'",
                 updated, oldUtf8, newUtf8);
    return updated;
}

void ProjectBin::clearAll()
{
    m_grid->clearItems();
    // Also clear the tree/list widget so stale items from a previous
    // project don't linger even if refreshAllViews() isn't called.
    if (m_listWidget) {
        m_listWidget->cancelPendingRename();  // close inline editor before freeing items
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
        auto* item = new NaturalTreeWidgetItem();
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
