/*
 * ProjectBinEvents.cpp — Event filter and key handling for ProjectBin.
 * Extracted from ProjectBin.cpp (modularization phase).
 *
 * Contains: eventFilter() — the main event handler that processes
 * key events, drag-and-drop routing, search bar focus, and all
 * item deletion with timeline clip propagation.
 */

#include "panels/project/ProjectBin.h"
#include "PathUtils.h"
#include "panels/project/ProjectBinInternal.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "media/MediaSourceService.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QByteArray>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QRegularExpression>
#include <QTreeWidgetItem>
#include <QUrl>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rt {

// ═════════════════════════════════════════════════════════════════════════════
//  Bin clipboard — copy / paste / duplicate (Premiere-style, all item types)
//
//  These are public so the TimelineWorkspace-level Ctrl+C / Ctrl+V shortcuts
//  (Qt::WidgetWithChildrenShortcut) can delegate to the bin when it has focus,
//  the same way Ctrl+A / Ctrl+B already do. Without that delegation the
//  workspace shortcut swallows the key before it ever reaches this widget's
//  event filter, so in-bin copy/paste never fired.
//
//  Behaviour by item type:
//    • Sequence            → deep-cloned into a new sequence.
//    • Color matte / asset → underlying file is physically copied so the
//                            duplicate can be recoloured independently.
//    • Footage/image/audio → independent bin entry referencing the same
//                            source media (a second "master clip"), with
//                            its own name and label colour.
//  Duplicates are named with a numeric suffix ("Clip" → "Clip 2" → …).
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// "Color Matte" → {"Color Matte", 1};  "Clip 3" → {"Clip", 3}
std::pair<QString, int> splitTrailingNumber(const QString& name)
{
    static const QRegularExpression re(QStringLiteral(R"(^(.*\S)\s+(\d+)\s*$)"));
    auto m = re.match(name);
    if (m.hasMatch())
        return { m.captured(1), m.captured(2).toInt() };
    return { name.trimmed(), 1 };
}

// First "<stem> N" not rejected by `taken` (N starts after any number
// already on `base`), matching the chosen "Clip → Clip 2 → Clip 3" scheme.
QString nextNumberedName(const QString& base,
                         const std::function<bool(const QString&)>& taken)
{
    auto [stem, n] = splitTrailingNumber(base);
    for (int i = n + 1; i < n + 100000; ++i) {
        QString cand = stem + QStringLiteral(" ") + QString::number(i);
        if (!taken(cand)) return cand;
    }
    return base + QStringLiteral(" copy");
}

// Captured bytes of a Color Matte PNG so deleting it (which also removes
// the file from disk) can be undone byte-for-byte.
struct MatteFileBackup {
    std::filesystem::path path;
    QByteArray            bytes;
    bool                  active = false;
};

} // namespace

ProjectBin::ClipboardEntry
ProjectBin::captureClipboardEntry(QTreeWidgetItem* item) const
{
    ClipboardEntry e;
    if (!item) return e;

    const bool isBin = item->data(0, Qt::UserRole + 2).toBool();
    const bool isSeq = item->data(0, Qt::UserRole + 3).toBool();

    if (isBin) {
        e.isBin   = true;
        e.binName = item->text(0);
        for (int i = 0; i < item->childCount(); ++i) {
            auto* ch = item->child(i);
            if (!ch || ch->data(0, Qt::UserRole + 3).toBool())
                continue;  // sequences inside bins are handled separately
            ClipboardEntry ce = captureClipboardEntry(ch);
            if (ce.isBin || !ce.filePath.empty())
                e.children.push_back(std::move(ce));
        }
        return e;
    }

    if (isSeq) return e;  // sequence path uses m_clipboardSequence

    const QString fp = item->data(0, Qt::UserRole).toString();
    if (fp.isEmpty()) return e;
    e.filePath = std::filesystem::path(fp.toStdString());

    const uint64_t id = item->data(0, kBinItemIdRole).toULongLong();
    const ThumbnailItem* src = nullptr;
    if (m_grid) {
        const auto& gitems = m_grid->items();
        if (id) {
            for (const auto& gi : gitems)
                if (gi.itemId == id) { src = &gi; break; }
        }
        if (!src) {
            for (const auto& gi : gitems)
                if (!gi.isFolder && gi.filePath == e.filePath) { src = &gi; break; }
        }
    }
    if (src) {
        e.displayName = src->displayName.isEmpty()
            ? QString::fromStdString(pathToUtf8(src->filePath.filename()))
            : src->displayName;
        e.labelColor  = src->labelColor;
        e.type        = src->type;
        e.mediaHandle = src->mediaHandle;
    } else {
        e.displayName = item->text(0);
    }
    e.generatedAsset = isColorMatte(e.filePath);
    return e;
}

void ProjectBin::applyClipboardEntry(
    const ClipboardEntry& e,
    const QString& parentBinPath,
    std::vector<uint64_t>& createdItemIds,
    std::vector<std::filesystem::path>& createdFiles,
    std::vector<BinFolderState>& createdFolders)
{
    if (!m_grid) return;

    // Record a freshly-created media item into its destination folder
    // (by "@<itemId>" so it round-trips just like a saved project).
    auto recordInFolder = [&](uint64_t newId) {
        if (parentBinPath.isEmpty() || newId == 0) return;
        const std::string key =
            "@" + std::to_string(static_cast<unsigned long long>(newId));
        for (auto& f : createdFolders)
            if (QString::fromStdString(f.name) == parentBinPath) {
                f.childKeys.push_back(key);
                return;
            }
        BinFolderState bf;
        bf.name = parentBinPath.toStdString();
        bf.expanded = true;
        bf.childKeys.push_back(key);
        createdFolders.push_back(std::move(bf));
    };

    if (e.isBin) {
        // Build a uniquely-named destination folder, then recurse so
        // children become independent duplicates inside it.
        QString binPath;
        if (parentBinPath.isEmpty()) {
            auto binNameTaken = [&](const QString& cand) {
                for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
                    auto* it = m_listWidget->topLevelItem(i);
                    if (it->data(0, Qt::UserRole + 2).toBool() &&
                        it->text(0) == cand)
                        return true;
                }
                for (const auto& f : createdFolders) {
                    QString n = QString::fromStdString(f.name);
                    if (!n.contains('/') && n == cand) return true;
                }
                return false;
            };
            binPath = nextNumberedName(
                e.binName.isEmpty() ? QStringLiteral("Bin") : e.binName,
                binNameTaken);
        } else {
            binPath = parentBinPath + QStringLiteral("/") + e.binName;
        }
        // Ensure the folder exists even if it ends up empty.
        bool have = false;
        for (const auto& f : createdFolders)
            if (QString::fromStdString(f.name) == binPath) { have = true; break; }
        if (!have) {
            BinFolderState bf;
            bf.name = binPath.toStdString();
            bf.expanded = true;
            createdFolders.push_back(std::move(bf));
        }
        for (const auto& ch : e.children)
            applyClipboardEntry(ch, binPath, createdItemIds,
                                createdFiles, createdFolders);
        return;
    }
    if (e.filePath.empty()) return;

    // Pick a unique display name among existing grid items.
    auto nameTaken = [this](const QString& cand) {
        for (const auto& gi : m_grid->items()) {
            if (gi.isFolder) continue;
            QString n = gi.displayName.isEmpty()
                ? QString::fromStdString(pathToUtf8(gi.filePath.filename()))
                : gi.displayName;
            if (n == cand) return true;
        }
        return false;
    };
    const QString newName = nextNumberedName(e.displayName, nameTaken);

    if (e.generatedAsset) {
        // Physically copy the file so the duplicate is independently
        // editable (e.g. recolour a Color Matte).
        std::error_code ec;
        std::filesystem::path dir = e.filePath.parent_path();
        std::string stem = pathToUtf8(e.filePath.stem());
        std::string ext  = pathToUtf8(e.filePath.extension());
        std::filesystem::path dst;
        for (int i = 2; i < 100000; ++i) {
            dst = dir / (stem + " " + std::to_string(i) + ext);
            if (!std::filesystem::exists(dst)) break;
        }
        std::filesystem::copy_file(e.filePath, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("ProjectBin: failed to copy asset '{}' → '{}': {}",
                          pathToUtf8(e.filePath), pathToUtf8(dst), ec.message());
            return;
        }
        if (m_grid->hasItem(dst)) return;
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto r = m_mediaSources->openSource(
                {dst, RenderRequestType::Still, false});
            handle = r.handle;
        }
        m_grid->addItem(dst, e.type, handle);
        auto& items = m_grid->mutableItems();
        if (!items.empty()) {
            items.back().displayName = newName;
            items.back().labelColor  = e.labelColor;
            createdItemIds.push_back(items.back().itemId);
            recordInFolder(items.back().itemId);
        }
        createdFiles.push_back(dst);
        spdlog::info("ProjectBin: duplicated asset '{}' → '{}'",
                     pathToUtf8(e.filePath), pathToUtf8(dst));
        return;
    }

    // Reference duplicate: a second independent bin entry pointing at the
    // same source media (Premiere "master clip" semantics).
    int srcIdx = -1;
    const auto& items = m_grid->items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].isFolder && items[i].filePath == e.filePath) {
            srcIdx = static_cast<int>(i);
            break;
        }
    }
    int newIdx = -1;
    if (srcIdx >= 0) {
        newIdx = m_grid->duplicateItem(srcIdx);
    } else {
        // Source no longer in the bin — re-add it as a fresh item.
        uint64_t handle = 0;
        if (m_mediaSources) {
            auto r = m_mediaSources->openSource(
                {e.filePath, RenderRequestType::Still, false});
            handle = r.handle;
        }
        m_grid->addItem(e.filePath, e.type, handle);
        newIdx = m_grid->itemCount() - 1;
    }
    if (newIdx < 0) return;
    auto& mitems = m_grid->mutableItems();
    if (newIdx < static_cast<int>(mitems.size())) {
        mitems[static_cast<size_t>(newIdx)].displayName = newName;
        mitems[static_cast<size_t>(newIdx)].labelColor  = e.labelColor;
        createdItemIds.push_back(mitems[static_cast<size_t>(newIdx)].itemId);
        recordInFolder(mitems[static_cast<size_t>(newIdx)].itemId);
    }
    spdlog::info("ProjectBin: duplicated media '{}' as '{}'",
                 pathToUtf8(e.filePath), newName.toStdString());
}

bool ProjectBin::hasClipboard() const noexcept
{
    return static_cast<bool>(m_clipboardSequence) || !m_clipboardItems.empty();
}

bool ProjectBin::copySelection()
{
    if (!m_listWidget) return false;
    auto* selected = m_listWidget->currentItem();
    if (!selected) return false;

    // Sequence → deep clone into the sequence clipboard.
    if (m_project && selected->data(0, Qt::UserRole + 3).toBool()) {
        size_t seqIdx = selected->data(0, Qt::UserRole + 4).toULongLong();
        auto* seq = m_project->sequence(seqIdx);
        if (!seq) return false;
        m_clipboardSequence = seq->clone();
        m_clipboardItems.clear();
        spdlog::info("ProjectBin: copied sequence '{}' to clipboard",
                     seq->name());
        return true;
    }

    // Media item or bin folder → snapshot into the item clipboard.
    const auto chosen = m_listWidget->selectedItems();
    std::vector<ClipboardEntry> entries;
    if (chosen.size() > 1) {
        for (auto* it : chosen) {
            if (it->data(0, Qt::UserRole + 3).toBool()) continue; // skip seqs
            ClipboardEntry e = captureClipboardEntry(it);
            if (e.isBin || !e.filePath.empty())
                entries.push_back(std::move(e));
        }
    } else {
        ClipboardEntry e = captureClipboardEntry(selected);
        if (e.isBin || !e.filePath.empty())
            entries.push_back(std::move(e));
    }
    if (entries.empty()) return false;
    m_clipboardItems = std::move(entries);
    m_clipboardSequence.reset();
    spdlog::info("ProjectBin: copied {} item(s) to clipboard",
                 m_clipboardItems.size());
    return true;
}

bool ProjectBin::pasteClipboard()
{
    // ── Sequence paste ──
    if (m_clipboardSequence && m_project) {
        auto dup = m_clipboardSequence->clone();
        auto nameTaken = [this](const QString& cand) {
            for (size_t i = 0; i < m_project->sequenceCount(); ++i) {
                auto* s = m_project->sequence(i);
                if (s && QString::fromStdString(s->name()) == cand)
                    return true;
            }
            return false;
        };
        dup->setName(nextNumberedName(
            QString::fromStdString(dup->name()), nameTaken).toStdString());
        if (m_commandStack) {
            size_t newIdx = m_project->sequenceCount();
            auto shared = std::make_shared<decltype(dup)>(std::move(dup));
            m_commandStack->execute(std::make_unique<LambdaCommand>(
                "Paste Sequence",
                [this, shared]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    if (!*shared) return;
                    m_project->addSequence(std::move(*shared));
                    syncListView();
                    emit sequencesChanged();
                },
                [this, newIdx]() {
                    if (m_destroying.load(std::memory_order_acquire)) return;
                    m_project->removeSequence(newIdx);
                    syncListView();
                    emit sequencesChanged();
                }));
        } else {
            m_project->addSequence(std::move(dup));
            syncListView();
            emit sequencesChanged();
        }
        return true;
    }

    // ── Media / asset paste ──
    if (m_clipboardItems.empty()) return false;

    // Premiere: paste INTO a bin only when a bin folder is the current
    // selection. Any other selection (or none) pastes at the bin root.
    QString targetBinPath;
    if (m_listWidget) {
        QTreeWidgetItem* sel = m_listWidget->currentItem();
        if (sel && sel->data(0, Qt::UserRole + 2).toBool()) {
            QStringList segs;
            for (QTreeWidgetItem* p = sel; p; p = p->parent())
                if (p->data(0, Qt::UserRole + 2).toBool())
                    segs.prepend(p->text(0));
            targetBinPath = segs.join('/');
        }
    }

    auto entries =
        std::make_shared<std::vector<ClipboardEntry>>(m_clipboardItems);
    auto createdIds   = std::make_shared<std::vector<uint64_t>>();
    auto createdFiles = std::make_shared<std::vector<std::filesystem::path>>();
    auto createdFolders = std::make_shared<std::vector<BinFolderState>>();
    // Top-level bins that already existed — never deleted on undo (only
    // bins we newly create are removed).
    auto preexistingBins = std::make_shared<std::set<QString>>();

    auto doPaste = [this, entries, createdIds, createdFiles,
                    createdFolders, preexistingBins, targetBinPath]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        createdIds->clear();
        createdFiles->clear();
        createdFolders->clear();
        preexistingBins->clear();
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i) {
            auto* it = m_listWidget->topLevelItem(i);
            if (it->data(0, Qt::UserRole + 2).toBool())
                preexistingBins->insert(it->text(0));
        }
        for (const auto& e : *entries)
            applyClipboardEntry(e, targetBinPath, *createdIds,
                                *createdFiles, *createdFolders);
        if (m_grid) m_grid->loadVisibleThumbnails();
        syncListView();
        // Materialize duplicated/target bin folders + move children in
        // (reuses the same path the project loader uses).
        if (!createdFolders->empty())
            restoreBinFolders(*createdFolders);
        if (!m_listView) syncIconView();
        emit itemCountChanged(itemCount());
    };
    auto undoPaste = [this, createdIds, createdFiles, createdFolders,
                      preexistingBins]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_grid)
            for (uint64_t id : *createdIds)
                m_grid->removeItemById(id);
        for (const auto& f : *createdFiles) {
            std::error_code ec;
            std::filesystem::remove(f, ec);
        }
        // Remove the now-empty bins WE created. Never delete a bin the
        // user already had (e.g. the bin we pasted into).
        for (const auto& bf : *createdFolders) {
            QString name = QString::fromStdString(bf.name);
            if (name.contains('/')) continue;          // sub-bin: goes with parent
            if (preexistingBins->count(name)) continue; // pre-existing target bin
            for (int i = m_listWidget->topLevelItemCount() - 1; i >= 0; --i) {
                auto* it = m_listWidget->topLevelItem(i);
                if (it->data(0, Qt::UserRole + 2).toBool() &&
                    it->text(0) == name)
                    delete m_listWidget->takeTopLevelItem(i);
            }
        }
        syncListView();
        if (!m_listView) syncIconView();
        emit itemCountChanged(itemCount());
    };

    if (m_commandStack) {
        m_commandStack->execute(std::make_unique<LambdaCommand>(
            "Paste Bin Item", doPaste, undoPaste));
    } else {
        doPaste();
    }
    return true;
}

bool ProjectBin::duplicateSelection()
{
    if (!copySelection()) return false;
    return pasteClipboard();
}

// ═════════════════════════════════════════════════════════════════════════════
//  Event filter — key handling, search bar, drag-drop routing
// ═════════════════════════════════════════════════════════════════════════════

bool ProjectBin::eventFilter(QObject* obj, QEvent* event)
{
    // ── Search field: Escape clears focus ──
    if (obj == m_searchField && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_searchField->clear();
            // Return focus to whichever ancestor wants it (TimelineWorkspace)
            if (auto* p = parentWidget()) {
                // Walk up to the top-level workspace
                while (p->parentWidget() && p->parentWidget()->parentWidget())
                    p = p->parentWidget();
                p->setFocus();
            }
            return true;
        }
    }

    // ── Ctrl+F or '/' focuses the search bar (Premiere Pro behavior) ──
    if ((obj == m_listWidget || obj == m_listWidget->viewport()) && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Slash ||
            (ke->key() == Qt::Key_F && (ke->modifiers() & Qt::ControlModifier))) {
            m_searchField->setFocus();
            m_searchField->selectAll();
            return true;
        }
    }

    // ── List widget / viewport: intercept drag-drop ──
    // delegated to ProjectBinDragDrop.cpp.
    if (obj == m_listWidget || obj == m_listWidget->viewport()) {
        if (event->type() == QEvent::Drop) {
            if (handleDropEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragEnter) {
            if (handleDragEnterEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragMove) {
            if (handleDragMoveEvent(event))
                return true;
        }
        if (event->type() == QEvent::DragLeave) {
            handleDragLeave();
        }
        // Double-click on empty space in list view → Import Media
        if (obj == m_listWidget->viewport() && event->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                QTreeWidgetItem* item = m_listWidget->itemAt(me->pos());
                if (!item) {
                    importFiles();
                    return true;
                }
            }
        }
    }

    // ── Create Sequence button: accept drag-and-drop of media files ──
    if (obj == m_btnCreateSequence) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                m_btnCreateSequence->setStyleSheet(QStringLiteral(
                    "QToolButton { background: %1; border: none; color: %2; "
                    "font-size: 13px; border-radius: 3px; }")
                    .arg(Theme::hex(Theme::colors().accentDim))
                    .arg(Theme::hex(Theme::colors().accent)));
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasUrls() ||
                de->mimeData()->hasFormat("application/x-roundtable-media")) {
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragLeave) {
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            // Reset button style
            m_btnCreateSequence->setStyleSheet(QStringLiteral(
                "QToolButton { background: transparent; border: none; color: %1; "
                "font-size: 13px; border-radius: 3px; }"
                "QToolButton:hover { background: %2; color: %3; }"
                "QToolButton:pressed { background: %4; color: %5; }")
                .arg(Theme::hex(Theme::colors().textTertiary))
                .arg(Theme::hex(Theme::colors().controlBgHover))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().accentDim))
                .arg(Theme::hex(Theme::colors().accent)));

            // External file drop (from Windows Explorer)
            if (de->mimeData()->hasUrls()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    if (url.isLocalFile()) {
                        std::filesystem::path p(url.toLocalFile().toStdString());
                        createSequenceFromMedia(p);
                        break;
                    }
                }
                de->acceptProposedAction();
                return true;
            }

            // Internal media drop (from the bin itself)
            if (de->mimeData()->hasFormat("application/x-roundtable-media")) {
                QByteArray mediaData = de->mimeData()->data("application/x-roundtable-media");
                QStringList paths = QString::fromUtf8(mediaData).split('\n', Qt::SkipEmptyParts);
                if (!paths.isEmpty()) {
                    std::filesystem::path p(paths.first().trimmed().toStdString());
                    createSequenceFromMedia(p);
                }
                de->acceptProposedAction();
                return true;
            }
        }
    }

    // ── List widget: Copy / Paste / Delete for sequences ──
    if ((obj == m_listWidget || obj == m_listWidget->viewport()) && event->type() == QEvent::KeyPress && m_project) {
        auto* ke = static_cast<QKeyEvent*>(event);
        auto* selected = m_listWidget->currentItem();
        bool isSeq = selected && selected->data(0, Qt::UserRole + 3).toBool();
        size_t seqIdx = isSeq
            ? selected->data(0, Qt::UserRole + 4).toULongLong()
            : size_t(-1);

        // Enter / Return — rename the selected item inline, exactly like
        // the context-menu "Rename" and Premiere Pro's behavior. Only when
        // the item is editable and not already being edited.
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
            !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            if (selected && (selected->flags() & Qt::ItemIsEditable)) {
                m_listWidget->editItem(selected, 0);
                return true;
            }
        }

        // Ctrl+A — select all items
        if (ke->matches(QKeySequence::SelectAll)) {
            m_listWidget->selectAll();
            return true;
        }

        // Ctrl+C — copy selection (sequence, media, color matte)
        if (ke->matches(QKeySequence::Copy)) {
            if (copySelection())
                return true;
        }

        // Ctrl+V — paste the clipboard as an independent duplicate
        if (ke->matches(QKeySequence::Paste) && hasClipboard()) {
            pasteClipboard();
            return true;
        }

        // Delete / Backspace — handle deletion
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            // ── Helpers for "propagate delete into timeline" (like Premiere Pro) ──
            struct TimelineRef { size_t seqIdx; size_t trackIdx; uint64_t clipId; };
            auto findTimelineRefs =
                [this](const std::vector<std::filesystem::path>& paths)
                -> std::vector<TimelineRef>
            {
                std::vector<TimelineRef> refs;
                if (!m_project || paths.empty()) return refs;
                auto normalize = [](const std::filesystem::path& p) {
                    std::error_code ec;
                    auto c = std::filesystem::weakly_canonical(p, ec);
                    auto s = (ec ? p : c).generic_string();
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char ch){ return std::tolower(ch); });
                    return s;
                };
                std::set<std::string> needles;
                for (const auto& p : paths)
                    needles.insert(normalize(p));
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    auto* tl = m_project->sequence(si);
                    if (!tl) continue;
                    for (size_t ti = 0; ti < tl->trackCount(); ++ti) {
                        auto* tr = tl->track(ti);
                        if (!tr) continue;
                        for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                            Clip* c = tr->clip(ci);
                            if (!c) continue;
                            std::string clipPath;
                            if (auto* vc = dynamic_cast<VideoClip*>(c))
                                clipPath = vc->mediaPath();
                            else if (auto* ac = dynamic_cast<AudioClip*>(c))
                                clipPath = ac->mediaPath();
                            else if (auto* ic = dynamic_cast<ImageClip*>(c))
                                clipPath = ic->mediaPath();
                            if (clipPath.empty()) continue;
                            if (needles.count(normalize(clipPath)))
                                refs.push_back({si, ti, c->id()});
                        }
                    }
                }
                return refs;
            };

            auto confirmTimelinePropagation =
                [this](const std::vector<TimelineRef>& foundRefs) -> bool
            {
                if (foundRefs.empty()) return true;
                QMessageBox box(this);
                box.setWindowTitle(tr("Clip is in use"));
                box.setIcon(QMessageBox::Warning);
                box.setText(tr("%1 clip(s) on the timeline reference this media.")
                                .arg(foundRefs.size()));
                box.setInformativeText(
                    tr("Deleting from the bin will also remove these clip(s) "
                       "from the timeline.\n\nProceed?"));
                box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
                box.setDefaultButton(QMessageBox::Cancel);
                return box.exec() == QMessageBox::Yes;
            };

            struct ExtractedClip {
                size_t seqIdx;
                size_t trackIdx;
                std::unique_ptr<Clip> clip;
            };
            auto extractTimelineClips =
                [this](const std::vector<TimelineRef>& refs)
                -> std::vector<ExtractedClip>
            {
                std::vector<ExtractedClip> out;
                out.reserve(refs.size());
                for (const auto& r : refs) {
                    auto* tl = m_project ? m_project->sequence(r.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(r.trackIdx);
                    if (!tr) continue;
                    auto removed = tr->removeClipById(r.clipId);
                    if (removed)
                        out.push_back({r.seqIdx, r.trackIdx, std::move(removed)});
                }
                return out;
            };
            auto restoreTimelineClips =
                [this](std::vector<ExtractedClip>& extracted)
            {
                for (auto& e : extracted) {
                    auto* tl = m_project ? m_project->sequence(e.seqIdx) : nullptr;
                    if (!tl) continue;
                    auto* tr = tl->track(e.trackIdx);
                    if (!tr) continue;
                    tr->addClip(std::move(e.clip));
                }
                extracted.clear();
            };

            std::function<void(QTreeWidgetItem*, std::vector<std::filesystem::path>&)>
                collectDescendantFiles =
                [&](QTreeWidgetItem* node, std::vector<std::filesystem::path>& out) {
                    if (!node) return;
                    for (int i = 0; i < node->childCount(); ++i) {
                        QTreeWidgetItem* ch = node->child(i);
                        if (!ch) continue;
                        bool chIsBin = ch->data(0, Qt::UserRole + 2).toBool();
                        bool chIsSeq = ch->data(0, Qt::UserRole + 3).toBool();
                        if (chIsBin) {
                            collectDescendantFiles(ch, out);
                        } else if (!chIsSeq) {
                            QString fp = ch->data(0, Qt::UserRole).toString();
                            if (!fp.isEmpty())
                                out.emplace_back(fp.toStdString());
                        }
                    }
                };

            auto allSelected = m_listWidget->selectedItems();

            // Multi-select: mass-delete selected items
            if (allSelected.size() > 1) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                std::vector<std::filesystem::path> toRemove;       // bin contents (by path)
                std::vector<uint64_t> mediaIdsToRemove;            // media rows (by id)
                std::vector<std::filesystem::path> mediaPaths;     // matching paths
                std::vector<QTreeWidgetItem*> binsToDelete;
                for (auto* item : allSelected) {
                    bool itemIsSeq = item->data(0, Qt::UserRole + 3).toBool();
                    bool itemIsBin = item->data(0, Qt::UserRole + 2).toBool();
                    if (itemIsSeq) continue;
                    if (itemIsBin) {
                        collectDescendantFiles(item, toRemove);
                        binsToDelete.push_back(item);
                        continue;
                    }
                    QString fp = item->data(0, Qt::UserRole).toString();
                    if (fp.isEmpty()) continue;
                    uint64_t id = item->data(0, kBinItemIdRole).toULongLong();
                    if (id) {
                        mediaIdsToRemove.push_back(id);
                        mediaPaths.emplace_back(fp.toStdString());
                    } else {
                        toRemove.emplace_back(fp.toStdString());
                    }
                }
                // Paths whose timeline clips should be pulled: every bin
                // content path, plus a duplicated media path only when no
                // other (unselected) bin entry still references it.
                std::vector<std::filesystem::path> propagationPaths = toRemove;
                {
                    std::map<std::filesystem::path, int> gridRefs, selRefs;
                    if (m_grid)
                        for (const auto& gi : m_grid->items())
                            if (!gi.isFolder) ++gridRefs[gi.filePath];
                    for (const auto& p : mediaPaths) ++selRefs[p];
                    for (const auto& [p, sel] : selRefs)
                        if (sel >= gridRefs[p])
                            propagationPaths.push_back(p);
                }
                auto timelineRefs = findTimelineRefs(propagationPaths);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));

                // Color mattes fully removed here also leave disk; back
                // up their bytes so Ctrl+Z restores the files too.
                // propagationPaths == the set with no remaining reference.
                auto matteBackups =
                    std::make_shared<std::vector<MatteFileBackup>>();
                for (const auto& p : propagationPaths) {
                    if (!ProjectBin::isColorMatte(p)) continue;
                    QFile mf(QString::fromStdString(pathToUtf8(p)));
                    if (mf.open(QIODevice::ReadOnly)) {
                        MatteFileBackup b;
                        b.path = p; b.bytes = mf.readAll(); b.active = true;
                        matteBackups->push_back(std::move(b));
                    }
                }
                auto deleteMatteFiles = [matteBackups]() {
                    for (const auto& b : *matteBackups) {
                        if (!b.active) continue;
                        std::error_code ec;
                        std::filesystem::remove(b.path, ec);
                    }
                };
                auto restoreMatteFiles = [matteBackups]() {
                    for (const auto& b : *matteBackups) {
                        if (!b.active) continue;
                        QFile out(QString::fromStdString(pathToUtf8(b.path)));
                        if (out.open(QIODevice::WriteOnly))
                            out.write(b.bytes);
                    }
                };

                if (m_grid)
                    for (uint64_t id : mediaIdsToRemove)
                        m_grid->removeItemById(id);
                for (const auto& path : toRemove)
                    removeFile(path);
                deleteMatteFiles();
                for (auto* binItem : binsToDelete) {
                    int idx = m_listWidget->indexOfTopLevelItem(binItem);
                    if (idx >= 0) {
                        delete m_listWidget->takeTopLevelItem(idx);
                    } else if (auto* parent = binItem->parent()) {
                        int ci = parent->indexOfChild(binItem);
                        if (ci >= 0) delete parent->takeChild(ci);
                    }
                }
                const bool anyRemoved = !toRemove.empty()
                    || !binsToDelete.empty() || !mediaIdsToRemove.empty();
                if (anyRemoved)
                    syncListView();

                if (!extractedTimelineClips->empty())
                    emit timelineClipsMutated();

                if (anyRemoved && m_commandStack) {
                    auto after = std::make_shared<ProjectBin::BinSnapshot>(captureBinSnapshot());
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin Items",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips,
                         deleteMatteFiles]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            deleteMatteFiles();
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips,
                         restoreMatteFiles]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            restoreMatteFiles();
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: sequence
            if (isSeq && m_project->sequenceCount() > 1) {
                if (m_commandStack) {
                    auto extracted = m_project->extractSequence(seqIdx);
                    if (!extracted) return false;
                    auto shared = std::make_shared<std::unique_ptr<Timeline>>(std::move(extracted));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Sequence",
                        [this, seqIdx]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            m_project->removeSequence(seqIdx);
                            syncListView();
                            emit sequencesChanged();
                        },
                        [this, seqIdx, shared]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            m_project->insertSequence(seqIdx, std::move(*shared));
                            syncListView();
                            emit sequencesChanged();
                        }));
                    syncListView();
                    emit sequencesChanged();
                } else {
                    m_project->removeSequence(seqIdx);
                    syncListView();
                    emit sequencesChanged();
                }
                return true;
            }

            // Single selection: bin (delete bin AND its descendants)
            bool isBin = selected && selected->data(0, Qt::UserRole + 2).toBool();
            if (isBin) {
                auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());
                QString binName = selected->text(0);
                spdlog::info("ProjectBin: deleting bin '{}'", binName.toStdString());

                std::vector<std::filesystem::path> contents;
                collectDescendantFiles(selected, contents);

                auto timelineRefs = findTimelineRefs(contents);
                if (!confirmTimelinePropagation(timelineRefs))
                    return true;
                auto extractedTimelineClips =
                    std::make_shared<std::vector<ExtractedClip>>(
                        extractTimelineClips(timelineRefs));

                // Color mattes inside the bin are generated assets — also
                // delete their files, restorable on undo.
                auto matteBackups =
                    std::make_shared<std::vector<MatteFileBackup>>();
                for (const auto& p : contents) {
                    if (!ProjectBin::isColorMatte(p)) continue;
                    QFile mf(QString::fromStdString(pathToUtf8(p)));
                    if (mf.open(QIODevice::ReadOnly)) {
                        MatteFileBackup b;
                        b.path = p; b.bytes = mf.readAll(); b.active = true;
                        matteBackups->push_back(std::move(b));
                    }
                }
                auto deleteMatteFiles = [matteBackups]() {
                    for (const auto& b : *matteBackups) {
                        if (!b.active) continue;
                        std::error_code ec;
                        std::filesystem::remove(b.path, ec);
                    }
                };
                auto restoreMatteFiles = [matteBackups]() {
                    for (const auto& b : *matteBackups) {
                        if (!b.active) continue;
                        QFile out(QString::fromStdString(pathToUtf8(b.path)));
                        if (out.open(QIODevice::WriteOnly))
                            out.write(b.bytes);
                    }
                };

                for (const auto& p : contents)
                    removeFile(p);
                deleteMatteFiles();

                int idx = m_listWidget->indexOfTopLevelItem(selected);
                if (idx >= 0) {
                    delete m_listWidget->takeTopLevelItem(idx);
                } else if (auto* parent = selected->parent()) {
                    int ci = parent->indexOfChild(selected);
                    if (ci >= 0) delete parent->takeChild(ci);
                }
                syncListView();

                if (!extractedTimelineClips->empty())
                    emit timelineClipsMutated();

                if (m_commandStack) {
                    auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                    auto timelineRefsShared =
                        std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                        "Delete Bin",
                        [this, after, timelineRefsShared,
                         extractedTimelineClips, extractTimelineClips,
                         deleteMatteFiles]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                *extractedTimelineClips =
                                    extractTimelineClips(*timelineRefsShared);
                            this->applyBinSnapshot(*after);
                            deleteMatteFiles();
                            if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                        },
                        [this, before,
                         extractedTimelineClips, restoreTimelineClips,
                         restoreMatteFiles]() {
                            if (m_destroying.load(std::memory_order_acquire)) return;
                            restoreMatteFiles();
                            this->applyBinSnapshot(*before);
                            if (!extractedTimelineClips->empty()) {
                                restoreTimelineClips(*extractedTimelineClips);
                                emit timelineClipsMutated();
                            }
                        }));
                }
                return true;
            }

            // Single selection: media item
            if (!isSeq) {
                QString fp = selected ? selected->data(0, Qt::UserRole).toString() : QString();
                if (!fp.isEmpty()) {
                    auto before = std::make_shared<BinSnapshot>(captureBinSnapshot());

                    const std::filesystem::path mediaPath(fp.toStdString());
                    const uint64_t itemId = selected
                        ? selected->data(0, kBinItemIdRole).toULongLong()
                        : 0;

                    // Premiere: deleting one of several bin entries that
                    // reference the same media must NOT pull its clips off
                    // the timeline while another bin reference remains.
                    // Only propagate when this is the last reference.
                    int refsForPath = 0;
                    if (m_grid)
                        for (const auto& gi : m_grid->items())
                            if (!gi.isFolder && gi.filePath == mediaPath)
                                ++refsForPath;

                    std::vector<std::filesystem::path> contents;
                    if (refsForPath <= 1)
                        contents.push_back(mediaPath);
                    auto timelineRefs = findTimelineRefs(contents);
                    if (!confirmTimelinePropagation(timelineRefs))
                        return true;
                    auto extractedTimelineClips =
                        std::make_shared<std::vector<ExtractedClip>>(
                            extractTimelineClips(timelineRefs));

                    // Color matte: it is a generated, self-contained asset,
                    // so deleting it also removes the file from disk. Back
                    // up its bytes so Ctrl+Z restores the file too. Only
                    // when this is the last bin reference to it.
                    auto matteBackup = std::make_shared<MatteFileBackup>();
                    if (refsForPath <= 1 && ProjectBin::isColorMatte(mediaPath)) {
                        QFile mf(QString::fromStdString(pathToUtf8(mediaPath)));
                        if (mf.open(QIODevice::ReadOnly)) {
                            matteBackup->path  = mediaPath;
                            matteBackup->bytes = mf.readAll();
                            matteBackup->active = true;
                        }
                    }
                    auto deleteMatteFile = [matteBackup]() {
                        if (!matteBackup->active) return;
                        std::error_code ec;
                        std::filesystem::remove(matteBackup->path, ec);
                    };
                    auto restoreMatteFile = [matteBackup]() {
                        if (!matteBackup->active) return;
                        QFile out(QString::fromStdString(
                            pathToUtf8(matteBackup->path)));
                        if (out.open(QIODevice::WriteOnly))
                            out.write(matteBackup->bytes);
                    };

                    if (itemId && m_grid)
                        m_grid->removeItemById(itemId);
                    else
                        removeFile(mediaPath);
                    deleteMatteFile();
                    syncListView();

                    if (!extractedTimelineClips->empty())
                        emit timelineClipsMutated();

                    if (m_commandStack) {
                        auto after = std::make_shared<BinSnapshot>(captureBinSnapshot());
                        auto timelineRefsShared =
                            std::make_shared<std::vector<TimelineRef>>(std::move(timelineRefs));
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Delete Bin Item",
                            [this, after, timelineRefsShared,
                             extractedTimelineClips, extractTimelineClips,
                             deleteMatteFile]() {
                                if (m_destroying.load(std::memory_order_acquire)) return;
                                if (extractedTimelineClips->empty() && !timelineRefsShared->empty())
                                    *extractedTimelineClips =
                                        extractTimelineClips(*timelineRefsShared);
                                this->applyBinSnapshot(*after);
                                deleteMatteFile();
                                if (!timelineRefsShared->empty())
                                emit timelineClipsMutated();
                            },
                            [this, before,
                             extractedTimelineClips, restoreTimelineClips,
                             restoreMatteFile]() {
                                if (m_destroying.load(std::memory_order_acquire)) return;
                                restoreMatteFile();
                                this->applyBinSnapshot(*before);
                                if (!extractedTimelineClips->empty()) {
                                    restoreTimelineClips(*extractedTimelineClips);
                                    emit timelineClipsMutated();
                                }
                            }));
                    }
                }
                return true;
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

} // namespace rt
