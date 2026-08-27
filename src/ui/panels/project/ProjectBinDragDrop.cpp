/*
 * ProjectBinDragDrop.cpp — Drag-and-drop handling for ProjectBin.
 * Extracted from ProjectBin.cpp (P2.1 of modularization plan).
 */

#include "panels/project/ProjectBin.h"
#include "panels/project/ProjectBinInternal.h"
#include "PathUtils.h"
#include "widgets/MediaDragTreeWidget.h"
#include "Theme.h"
#include "project/Project.h"
#include "playback/MediaPool.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTreeWidgetItem>

#include <spdlog/spdlog.h>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Internal helpers
// ═════════════════════════════════════════════════════════════════════════════

ProjectBin::BinSnapshot ProjectBin::captureBinSnapshot()
{
    BinSnapshot s;
    s.files = allFiles();
    s.folders = binFolderState();
    return s;
}

void ProjectBin::applyBinSnapshot(const BinSnapshot& s)
{
    m_dropHighlightItem = nullptr;  // tree will be cleared
    clearAll();
    m_listWidget->clear();

    if (!s.files.empty()) {
        addFiles(s.files);
    } else {
        syncListView();
        if (!m_listView)
            syncIconView();
    }

    if (!s.folders.empty())
        restoreBinFolders(s.folders);

    syncListView();
    if (!m_listView)
        syncIconView();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag-and-drop event handlers
// ═════════════════════════════════════════════════════════════════════════════

bool ProjectBin::handleDropEvent(QEvent* ev)
{
    auto* de = static_cast<QDropEvent*>(ev);
    // ── Internal reparent: items dragged within the bin tree ──
    const bool hasInternalMime = de->mimeData()->hasFormat("application/x-roundtable-media") ||
                                 de->mimeData()->hasFormat("application/x-roundtable-sequence") ||
                                 de->mimeData()->hasFormat("application/x-roundtable-bin-item") ||
                                 de->mimeData()->hasFormat("application/x-roundtable-adjustment") ||
                                 de->mimeData()->hasFormat("application/x-roundtable-tree-item-ptrs");
    bool isInternal = hasInternalMime;
    if (isInternal) {
        de->setDropAction(Qt::MoveAction);
        auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
        auto* hitItem = m_listWidget->itemAt(de->position().toPoint());
        QTreeWidgetItem* targetBin = nullptr;
        if (hitItem) {
            if (hitItem->data(0, Qt::UserRole + 2).toBool())
                targetBin = hitItem;
            else if (hitItem->parent() &&
                     hitItem->parent()->data(0, Qt::UserRole + 2).toBool())
                targetBin = hitItem->parent();
        }
        if (!targetBin && m_dropHighlightItem &&
            m_dropHighlightItem->data(0, Qt::UserRole + 2).toBool()) {
            targetBin = m_dropHighlightItem;
        }

        auto isDescendantOf = [](QTreeWidgetItem* node, QTreeWidgetItem* possibleAncestor) {
            for (auto* p = node ? node->parent() : nullptr; p; p = p->parent()) {
                if (p == possibleAncestor)
                    return true;
            }
            return false;
        };

        QList<QTreeWidgetItem*> draggedItems;
        QByteArray ptrData = de->mimeData()->data("application/x-roundtable-tree-item-ptrs");
        if (!ptrData.isEmpty()) {
            for (const QByteArray& token : ptrData.split(',')) {
                bool ok = false;
                const qulonglong raw = token.toULongLong(&ok);
                if (!ok || raw == 0) continue;
                auto* item = reinterpret_cast<QTreeWidgetItem*>(static_cast<quintptr>(raw));
                if (!item) continue;
                if (!draggedItems.contains(item))
                    draggedItems.append(item);
            }
        }
        if (draggedItems.isEmpty())
            draggedItems = m_listWidget->selectedItems();

        int movedCount = 0;
        for (auto* item : draggedItems) {
            bool itemIsBin = item->data(0, Qt::UserRole + 2).toBool();
            if (itemIsBin) {
                if (item == targetBin) continue;
                if (targetBin && isDescendantOf(targetBin, item))
                    continue;
            }

            if (auto* parent = item->parent()) {
                parent->removeChild(item);
            } else {
                int idx = m_listWidget->indexOfTopLevelItem(item);
                if (idx >= 0) m_listWidget->takeTopLevelItem(idx);
            }
            if (targetBin)
                targetBin->addChild(item);
            else
                m_listWidget->addTopLevelItem(item);
            ++movedCount;
        }
        // Don't auto-expand on drop — hover-delay expand is handled by
        // setAutoExpandDelay() configured in ProjectBinUI.cpp.

        if (m_commandStack) {
            auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Move Bin Items",
                [this, after]() { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*after); },
                [this, before]() { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*before); }));
        }

        de->acceptProposedAction();
        m_dropHighlightItem = nullptr;
        m_listWidget->viewport()->update();
        return true;
    }

    // ── External drop: URLs from Windows Explorer ──
    if (de->mimeData()->hasUrls()) {
        de->setDropAction(Qt::CopyAction);
        std::vector<std::filesystem::path> paths;
        for (const QUrl& url : de->mimeData()->urls()) {
            if (url.isLocalFile())
                paths.emplace_back(utf8ToPath(
                    url.toLocalFile().toUtf8().toStdString()));
        }
        if (!paths.empty()) {
            auto* hitItem = m_listWidget->itemAt(de->position().toPoint());
            QTreeWidgetItem* targetBin = nullptr;
            if (hitItem) {
                if (hitItem->data(0, Qt::UserRole + 2).toBool()) {
                    targetBin = hitItem;
                } else {
                    for (auto* p = hitItem->parent(); p; p = p->parent()) {
                        if (p->data(0, Qt::UserRole + 2).toBool()) {
                            targetBin = p;
                            break;
                        }
                    }
                }
            }
            if (!targetBin && m_dropHighlightItem &&
                m_dropHighlightItem->data(0, Qt::UserRole + 2).toBool()) {
                targetBin = m_dropHighlightItem;
            }

            // Snapshot before/after so a Windows Explorer drop is undoable
            // with Ctrl+Z, exactly like the internal "Move Bin Items" path.
            // addFilesToBin() is synchronous (it populates m_grid and the
            // tree inline), so the "after" snapshot is complete here.
            auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
            addFilesToBin(paths, targetBin);
            if (m_commandStack) {
                auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                    "Import Media",
                    [this, after]()  { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*after); },
                    [this, before]() { if (m_destroying.load(std::memory_order_acquire)) return; applyBinSnapshot(*before); }));
            }

            de->acceptProposedAction();
            m_dropHighlightItem = nullptr;
            m_listWidget->viewport()->update();
            return true;
        }
    }

    // Note: the delete-on-drop path (handling "x-roundtable-delete" MIME)
    // was previously here but required local lambdas defined inside
    // eventFilter() (findTimelineRefs, extractTimelineClips, etc.).
    // Those lambdas are defined in ProjectBin.cpp's eventFilter and are
    // used by the Key_Delete handler there.  The delete-on-drop code path
    // can be re-enabled if needed by extracting those helpers into proper
    // member functions.

    return false;
}

bool ProjectBin::handleDragEnterEvent(QEvent* ev)
{
    auto* de = static_cast<QDragEnterEvent*>(ev);
    const bool hasUrls = de->mimeData()->hasUrls();
    const bool hasMedia = de->mimeData()->hasFormat("application/x-roundtable-media");
    const bool hasSeq = de->mimeData()->hasFormat("application/x-roundtable-sequence");
    const bool hasBin = de->mimeData()->hasFormat("application/x-roundtable-bin-item");
    const bool hasAdj = de->mimeData()->hasFormat("application/x-roundtable-adjustment");
    const bool hasTreePtrs = de->mimeData()->hasFormat("application/x-roundtable-tree-item-ptrs");
    if (hasUrls || hasMedia || hasSeq || hasBin || hasAdj || hasTreePtrs) {
        if (hasMedia || hasSeq || hasBin || hasAdj || hasTreePtrs)
            de->setDropAction(Qt::MoveAction);
        else
            de->setDropAction(Qt::CopyAction);
        de->accept();
        return true;
    }
    return false;
}

bool ProjectBin::handleDragMoveEvent(QEvent* ev)
{
    auto* de = static_cast<QDragMoveEvent*>(ev);
    const bool hasUrls = de->mimeData()->hasUrls();
    const bool hasMedia = de->mimeData()->hasFormat("application/x-roundtable-media");
    const bool hasSeq = de->mimeData()->hasFormat("application/x-roundtable-sequence");
    const bool hasBin = de->mimeData()->hasFormat("application/x-roundtable-bin-item");
    const bool hasAdj = de->mimeData()->hasFormat("application/x-roundtable-adjustment");
    const bool hasTreePtrs = de->mimeData()->hasFormat("application/x-roundtable-tree-item-ptrs");
    if (hasUrls || hasMedia || hasSeq || hasBin || hasAdj || hasTreePtrs) {
        if (hasMedia || hasSeq || hasBin || hasAdj || hasTreePtrs)
            de->setDropAction(Qt::MoveAction);
        else
            de->setDropAction(Qt::CopyAction);

        auto* hitItem = m_listWidget->itemAt(de->position().toPoint());
        QTreeWidgetItem* newHighlight = nullptr;
        if (hitItem) {
            if (hitItem->data(0, Qt::UserRole + 2).toBool())
                newHighlight = hitItem;
            else if (hitItem->parent() &&
                     hitItem->parent()->data(0, Qt::UserRole + 2).toBool())
                newHighlight = hitItem->parent();
        }
        if (newHighlight != m_dropHighlightItem) {
            m_dropHighlightItem = newHighlight;
            m_listWidget->viewport()->update();
        }
        de->accept();
        return true;
    }
    return false;
}

void ProjectBin::handleDragLeave()
{
    if (m_dropHighlightItem) {
        m_dropHighlightItem = nullptr;
        m_listWidget->viewport()->update();
    }
}

} // namespace rt
