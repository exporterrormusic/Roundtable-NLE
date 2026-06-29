/*
 * ProjectBinSlots.cpp — Slot implementations for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: onFilterChanged, onSearchChanged, onZoomChanged,
 * onItemDoubleClicked, onListItemDoubleClicked, setListView,
 * openBinTab, onBinTabChanged, onBinTabCloseRequested, revealByPath
 */

#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"

#include <QFileInfo>
#include <QLineEdit>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QHeaderView>

#include <cmath>
#include <functional>

namespace rt {

void ProjectBin::onFilterChanged(int index)
{
    switch (index)
    {
    case 0: m_activeType = MediaType::Unknown; break; // All
    case 1: m_activeType = MediaType::Video;   break;
    case 2: m_activeType = MediaType::Image;   break;
    case 3: m_activeType = MediaType::Audio;   break;
    case 4: m_activeType = MediaType::Spine;   break;
    default: m_activeType = MediaType::Unknown; break;
    }

    m_grid->setTypeFilter(m_activeType);
    m_grid->loadVisibleThumbnails();
    syncListView();
}

void ProjectBin::onSearchChanged(const QString& text)
{
    m_grid->setFilter(text);
    if (m_listView)
        syncListView();
}

void ProjectBin::onZoomChanged(int value)
{
    float zoom = static_cast<float>(value) / 100.0f;
    m_grid->setZoom(zoom);
    m_grid->loadVisibleThumbnails();

    // In list view, scale the row height
    if (m_listView) {
        int rowH = std::max(16, static_cast<int>(16 * zoom));
        int iconSz = std::max(12, rowH - 4);
        m_listWidget->setIconSize(QSize(iconSz, iconSz));
        m_listWidget->setStyleSheet(
            m_listWidget->styleSheet()
            + QStringLiteral(" QTreeView::item { height: %1px; }").arg(rowH));
    }
}

void ProjectBin::onItemDoubleClicked(int index, const std::filesystem::path& filePath)
{
    // Check if it's a folder item — navigate into it (only if it's a real bin)
    if (index >= 0 && index < static_cast<int>(m_grid->items().size())) {
        const auto& gridItem = m_grid->items()[index];
        if (gridItem.isFolder) {
            // Verify it's an actual bin (not a sequence shown as folder)
            QTreeWidgetItem* container = nullptr;
            for (const auto& seg : m_iconBinPath) {
                int cnt = container ? container->childCount()
                                    : m_listWidget->topLevelItemCount();
                for (int i = 0; i < cnt; ++i) {
                    auto* c = container ? container->child(i)
                                        : m_listWidget->topLevelItem(i);
                    if (c->data(0, Qt::UserRole + 2).toBool() && c->text(0) == seg) {
                        container = c;
                        break;
                    }
                }
            }
            int cnt = container ? container->childCount()
                                : m_listWidget->topLevelItemCount();
            bool isBin = false;
            for (int i = 0; i < cnt; ++i) {
                auto* c = container ? container->child(i)
                                    : m_listWidget->topLevelItem(i);
                if (c->data(0, Qt::UserRole + 2).toBool() &&
                    c->text(0) == gridItem.folderName) {
                    isBin = true;
                    break;
                }
            }
            if (isBin) {
                QStringList binPath = m_iconBinPath;
                binPath.append(gridItem.folderName);
                openBinTab(binPath, gridItem.folderName);
                return;
            }
            // Sequence — try to open it. Defer via QTimer::singleShot so the
            // double-click event finishes before the page switch + timeline
            // rebuild run; otherwise the open appears to stall until the user
            // clicks elsewhere (same fix as onListItemDoubleClicked below).
            if (m_project) {
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    if (QString::fromStdString(m_project->sequence(si)->name()) == gridItem.folderName) {
                        QTimer::singleShot(0, this, [this, si]() {
                            emit sequenceOpened(si);
                        });
                        return;
                    }
                }
            }
            return;
        }
    }

    // Color matte → reopen the colour picker to recolour it (Premiere).
    if (isColorMatte(filePath)) {
        editColorMatte(filePath);
        return;
    }

    // Media item — load in source monitor
    uint64_t handle = 0;
    const auto* item = m_grid->selectedItem();
    if (item)
        handle = item->mediaHandle;

    emit loadInSourceMonitor(filePath, handle);
}

void ProjectBin::onListItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;

    // Bins: open in a new tab
    if (item->data(0, Qt::UserRole + 2).toBool()) {
        // Build the full bin path by walking up the tree
        QStringList binPath;
        for (auto* p = item; p; p = p->parent()) {
            if (p->data(0, Qt::UserRole + 2).toBool())
                binPath.prepend(p->text(0));
        }
        openBinTab(binPath, item->text(0));
        return;
    }

    // Sequences: open in timeline.
    // Defer via QTimer::singleShot so the double-click event finishes
    // processing before the page switch (setCurrentPage → Timeline) and
    // heavy timeline rebuild run. Without this, the QStackedWidget page
    // change stalls until the next event-loop cycle, making it look like
    // nothing happened until the user clicks somewhere else.
    if (item->data(0, Qt::UserRole + 3).toBool()) {
        size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
        QTimer::singleShot(0, this, [this, seqIdx]() {
            emit sequenceOpened(seqIdx);
        });
        return;
    }

    // Media items
    std::filesystem::path filePath(item->data(0, Qt::UserRole).toString().toStdString());

    // Color matte → reopen the colour picker to recolour it (Premiere).
    if (isColorMatte(filePath)) {
        editColorMatte(filePath);
        return;
    }

    // Other media → load in source monitor
    uint64_t handle = item->data(0, Qt::UserRole + 1).toULongLong();
    emit loadInSourceMonitor(filePath, handle);
}

void ProjectBin::setListView(bool listMode)
{
    m_listView = listMode;

    // Persist the view preference for the current tab so each tab
    // remembers its own list/icon choice independently, and capture the
    // tab's bin path so we can re-scope after rebuilding the tree.
    QStringList tabPath;
    if (m_binTabBar) {
        int idx = m_binTabBar->currentIndex();
        if (idx >= 0 && idx < m_binTabViewModes.size())
            m_binTabViewModes[idx] = listMode;
        if (idx >= 0 && idx < m_binTabPaths.size())
            tabPath = m_binTabPaths[idx];
    }

    m_btnListView->setChecked(listMode);
    m_btnIconView->setChecked(!listMode);
    m_listWidget->setVisible(listMode);
    m_scrollArea->setVisible(!listMode);
    m_iconNavBar->setVisible(!listMode);

    // Apply current zoom to whichever view is active
    onZoomChanged(m_zoomSlider->value());

    // Rebuild a FULL (unflattened) tree before per-view scoping. Without this,
    // focusListViewOnBin() leaves m_listWidget flattened (the bin's children
    // become top-level and the bin container is destroyed), so a later
    // syncIconView() walk of m_iconBinPath would fail and silently reset scope
    // to root — the "icon view shows everything" bug. rebuildFullTree() prefers
    // LIVE data so edits made in this tab survive the toggle, only un-flattening
    // from the snapshot when the tree is actually flattened.
    m_iconBinPath = tabPath;            // empty for the root tab
    rebuildFullTree();

    if (listMode) {
        if (!tabPath.isEmpty())
            focusListViewOnBin();       // reads m_iconBinPath
    } else {
        syncIconView();
    }
}

void ProjectBin::rebuildFullTree()
{
    // If the list view is currently focused, m_listWidget is flattened, so the
    // live binFolderState() would capture that flattened structure — un-flatten
    // from the snapshot captured at focus time instead. Otherwise the live tree
    // is full and up to date with any imports/drops/new-bins made in this tab,
    // so rebuild from it (nullptr → binFolderState of the current tree). This is
    // the key to not reverting post-snapshot edits.
    if (m_listViewFocused)
        syncListView(m_rootFolderState.empty() ? nullptr : &m_rootFolderState);
    else
        syncListView();
}

void ProjectBin::focusListViewOnBin()
{
    if (m_iconBinPath.isEmpty() || !m_listWidget)
        return;

    // Walk the tree to find the target bin container.
    // (The tree was just rebuilt by syncListView(), so items are fresh.)
    QTreeWidgetItem* container = nullptr;
    for (const auto& seg : m_iconBinPath) {
        int count = container ? container->childCount()
                              : m_listWidget->topLevelItemCount();
        bool found = false;
        for (int i = 0; i < count; ++i) {
            auto* child = container ? container->child(i)
                                    : m_listWidget->topLevelItem(i);
            if (child->data(0, Qt::UserRole + 2).toBool() && child->text(0) == seg) {
                container = child;
                found = true;
                break;
            }
        }
        if (!found) {
            // Bin path segment missing — reset to root.
            m_iconBinPath.clear();
            return;
        }
    }

    if (!container)
        return;

    // Snapshot the FULL tree right before we flatten it (the tree is full here —
    // the caller rebuilt it). This keeps m_rootFolderState fresh so a later
    // rebuildFullTree() can un-flatten with the current structure.
    m_rootFolderState = binFolderState();

    // Take ownership of the target bin's children before clear() deletes them.
    QList<QTreeWidgetItem*> savedChildren;
    while (container->childCount() > 0)
        savedChildren.append(container->takeChild(0));

    // Clear the entire tree (this deletes container and all other items).
    m_listWidget->clear();
    m_dropHighlightItem = nullptr;

    // Reparent the bin's children as the new top-level items.
    for (auto* child : savedChildren)
        m_listWidget->addTopLevelItem(child);

    // The tree is now flattened — rebuildFullTree() must un-flatten from the
    // snapshot rather than read this (partial) live tree.
    m_listViewFocused = true;

    // Update the status label to reflect the focused count.
    int count = m_listWidget->topLevelItemCount();
    m_statusLabel->setText(QString("%1 items").arg(count));
}

void ProjectBin::openBinTab(const QStringList& binPath, const QString& name)
{
    // Check if a tab for this bin already exists
    for (int i = 0; i < m_binTabPaths.size(); ++i) {
        if (m_binTabPaths[i] == binPath) {
            m_binTabBar->setCurrentIndex(i);
            return;
        }
    }
    // Create a new tab
    int idx = m_binTabBar->addTab(name);
    m_binTabPaths.append(binPath);
    m_binTabViewModes.append(m_listView);  // new tab inherits current view mode
    m_binTabBar->setCurrentIndex(idx);
}

void ProjectBin::onBinTabChanged(int index)
{
    if (index < 0 || index >= m_binTabPaths.size())
        return;

    m_iconBinPath = m_binTabPaths[index];

    // Restore this tab's independent list/icon view preference.
    if (index < m_binTabViewModes.size()) {
        m_listView = m_binTabViewModes[index];
        m_btnListView->setChecked(m_listView);
        m_btnIconView->setChecked(!m_listView);
        m_listWidget->setVisible(m_listView);
        m_scrollArea->setVisible(!m_listView);
        m_iconNavBar->setVisible(!m_listView);
    }

    // Defer the view rebuild so the tab visibly switches first.
    // syncIconView()/syncListView() walk the tree, rebuild grid items, and
    // kick thumbnail decode — blocking the UI thread. Running on the next
    // event-loop tick lets the tab indicator paint first.
    //
    // Preserve the current view mode — don't force icon view for sub-tabs.
    // The user's list/icon preference should be sticky across tab switches.
    //
    // Always rebuild the full QTreeWidget first (via syncListView) so that
    // both list and icon views read from a correct, untransformed tree.
    QTimer::singleShot(0, this, [this]() {
        // Rebuild a full tree (live when possible, snapshot to un-flatten),
        // then scope for the new tab's view mode + path.
        rebuildFullTree();
        if (m_listView) {
            if (!m_iconBinPath.isEmpty())
                focusListViewOnBin();
        } else {
            syncIconView();
        }
    });
}

void ProjectBin::onBinTabCloseRequested(int index)
{
    // Never close the root "Project" tab
    if (index <= 0)
        return;

    m_binTabBar->removeTab(index);
    m_binTabPaths.removeAt(index);
    m_binTabViewModes.removeAt(index);

    // If we closed the active tab, switch to root
    if (m_binTabBar->currentIndex() < 0)
        m_binTabBar->setCurrentIndex(0);
}

void ProjectBin::revealByPath(const QString& filePath)
{
    if (!m_listWidget) return;

    // Normalize the incoming path for comparison
    QString normalized = QFileInfo(filePath).absoluteFilePath();

    // Recursively search all items (top-level and children of bin folders)
    std::function<QTreeWidgetItem*(QTreeWidgetItem*)> findItem;
    findItem = [&](QTreeWidgetItem* parent) -> QTreeWidgetItem* {
        int count = parent ? parent->childCount()
                          : m_listWidget->topLevelItemCount();
        for (int i = 0; i < count; ++i) {
            auto* item = parent ? parent->child(i)
                                : m_listWidget->topLevelItem(i);
            // Skip bin folders
            if (item->data(0, Qt::UserRole + 2).toBool()) {
                if (auto* found = findItem(item))
                    return found;
                continue;
            }
            QString itemPath = QFileInfo(item->data(0, Qt::UserRole).toString()).absoluteFilePath();
            if (itemPath == normalized)
                return item;
        }
        return nullptr;
    };

    if (auto* item = findItem(nullptr)) {
        // Expand parent bins so the item is visible
        for (auto* p = item->parent(); p; p = p->parent())
            p->setExpanded(true);
        m_listWidget->setCurrentItem(item);
        m_listWidget->scrollToItem(item, QAbstractItemView::PositionAtCenter);
    }
}

} // namespace rt
