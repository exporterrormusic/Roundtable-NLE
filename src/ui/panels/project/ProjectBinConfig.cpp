/*
 * ProjectBinConfig.cpp — Configuration setters for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 */

#include "QtHelpers.h"
#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "playback/MediaPool.h"
#include "playback/MediaSourceService.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QInputDialog>
#include <QLineEdit>

#include <memory>

namespace rt {

// -----------------------------------------------------------------------------
//  Configuration
// -----------------------------------------------------------------------------

void ProjectBin::setThumbnailGenerator(ThumbnailGenerator* gen)
{
    m_generator = gen;
    m_grid->setThumbnailGenerator(gen);
}

void ProjectBin::setMediaPool(MediaPool* pool) noexcept
{
    m_pool = pool;

    // Create a ThumbnailGenerator if we don't have one yet
    if (pool && !m_generator) {
        m_ownedGenerator = std::make_unique<ThumbnailGenerator>(
            perfProfile().thumbnailThreads, 160, 120);
        m_ownedGenerator->setMediaPool(pool);
        m_ownedGenerator->setCacheDirectory(
            (rt::userDataDir() + "/cache/thumbnails").toStdString());
        setThumbnailGenerator(m_ownedGenerator.get());
    }

    if (m_grid)
        m_grid->setMediaPool(pool);
}

void ProjectBin::setMediaSourceService(MediaSourceService* service) noexcept
{
    m_mediaSources = service;
}

void ProjectBin::setProject(Project* project) noexcept
{
    m_project = project;
    if (m_listView) syncListView();
}

void ProjectBin::setProjectName(const QString& name)
{
    if (m_binTabBar && m_binTabBar->count() > 0) {
        m_binTabBar->setTabText(0, name.isEmpty() ? QStringLiteral("Project") : name);
    }
}

void ProjectBin::setCommandStack(CommandStack* stack) noexcept
{
    m_commandStack = stack;
}

void ProjectBin::refreshFileThumbnail(const std::filesystem::path& filePath)
{
    if (m_grid)
        m_grid->refreshItem(filePath);
}

void ProjectBin::createNewBin()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Bin", "Bin name:",
                                         QLineEdit::Normal, "Bin", &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty())
        return;

    // Only create as a sub-bin when a bin is *itself* explicitly selected.
    // We use selectedItems() instead of currentItem() because currentItem()
    // can persist from a previous click even after the user clicks empty
    // space — causing new bins to silently nest inside the last-focused bin.
    QTreeWidgetItem* targetParentBin = nullptr;
    if (m_listWidget) {
        const auto& sel = m_listWidget->selectedItems();
        if (sel.size() == 1 && sel.first()->data(0, Qt::UserRole + 2).toBool())
            targetParentBin = sel.first();
    }

    auto* binItem = projectBinCreateBinItem(name);
    auto addToParent = [this, binItem, targetParentBin]() {
        if (targetParentBin) {
            targetParentBin->addChild(binItem);
            targetParentBin->setExpanded(true);
        } else if (m_listWidget) {
            m_listWidget->addTopLevelItem(binItem);
        }
        if (m_listWidget) {
            m_listWidget->setCurrentItem(binItem);
            m_listWidget->scrollToItem(binItem);
        }
    };
    auto removeFromParent = [this, binItem, targetParentBin]() {
        if (targetParentBin) {
            int idx = targetParentBin->indexOfChild(binItem);
            if (idx >= 0) targetParentBin->takeChild(idx);
        } else if (m_listWidget) {
            int idx = m_listWidget->indexOfTopLevelItem(binItem);
            if (idx >= 0) m_listWidget->takeTopLevelItem(idx);
        }
    };

    // Route through the command stack so Ctrl+Z removes the new bin and
    // redo re-adds it to the same parent. The QTreeWidgetItem* is owned
    // either by the tree (when added) or by no one (after take) — the
    // command keeps a reference, deleting it only when the command
    // itself dies while the item is detached.
    if (m_commandStack) {
        auto cmd = std::make_unique<LambdaCommand>(
            std::string("New Bin"),
            addToParent,
            removeFromParent);
        m_commandStack->execute(std::move(cmd));
    } else {
        addToParent();
    }
}

// -----------------------------------------------------------------------------
//  Size
// -----------------------------------------------------------------------------

QSize ProjectBin::sizeHint() const
{
    return QSize(320, 450);
}

} // namespace rt
