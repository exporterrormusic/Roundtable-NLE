/*
 * ProjectBinSync.cpp - View synchronization extracted from ProjectBin.cpp.
 *
 * Contains: syncListView() (bin/sequence/media tree rebuild),
 * syncIconView() (icon-view bin navigation), and refreshSequences().
 */

#include "panels/project/ProjectBin.h"
#include "PathUtils.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "timeline/Timeline.h"
#include "timeline/AudioClip.h"
#include "media/MediaPool.h"

#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QTreeWidgetItem>
#include <QImage>
#include <QPixmap>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "panels/project/ProjectBinInternal.h"

namespace rt {

void ProjectBin::syncListView(const std::vector<BinFolderState>* savedFoldersOverride)
{
    // Block itemChanged signals during rebuild to prevent false rename triggers
    m_listWidget->blockSignals(true);

    const auto savedFolders = savedFoldersOverride ? *savedFoldersOverride : binFolderState();

    m_listWidget->clear();
    m_dropHighlightItem = nullptr;  // tree items destroyed by clear()
    int count = 0;

    // -- Sequences (shown at top, like Premiere Pro) ---------------------
    if (m_project && (m_activeType == MediaType::Unknown)) {
        for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
            const Timeline* seq = m_project->sequence(si);
            if (!seq) continue;

            auto* seqItem = new QTreeWidgetItem();
            seqItem->setText(0, QString::fromStdString(seq->name()));
            seqItem->setText(1, "Sequence");
            seqItem->setIcon(0, makePremiereBinIcon(kLabelSequence, "sequence"));
            seqItem->setData(0, Qt::UserRole + 10, QVariant::fromValue(kLabelSequence));
            seqItem->setData(0, Qt::UserRole + 3, true);   // isSequence flag
            seqItem->setData(0, Qt::UserRole + 4, QVariant::fromValue(static_cast<quint64>(si)));
            seqItem->setData(0, Qt::UserRole + 5, QVariant::fromValue(static_cast<qlonglong>(seq->duration())));
            // Pre-compute whether this sequence contains any audio clips
            // so drag-preview ghosts can omit the audio track when none exist.
            {
                bool hasAudio = false;
                for (size_t ti = 0; ti < seq->trackCount() && !hasAudio; ++ti) {
                    auto* trk = seq->track(ti);
                    if (!trk || trk->type() != TrackType::Audio) continue;
                    for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                        auto* c = trk->clip(ci);
                        if (c && dynamic_cast<const AudioClip*>(c) && c->isEnabled()) {
                            hasAudio = true; break;
                        }
                    }
                }
                seqItem->setData(0, Qt::UserRole + 7, hasAudio);
            }
            // Sequences are draggable but NOT drop targets
            seqItem->setFlags((seqItem->flags() & ~Qt::ItemIsDropEnabled) | Qt::ItemIsEditable);

            // Bold the active sequence
            if (si == m_project->activeSequenceIndex()) {
                QFont f = seqItem->font(0);
                f.setBold(true);
                seqItem->setFont(0, f);
            }

            // Sequence metadata
            int trackCount = static_cast<int>(seq->trackCount());
            {
                // Duration from timeline length
                TimeTick durTicks = seq->duration();
                double durSec = ticksToSeconds(durTicks);
                double seqFps = seq->settings().frameRate();
                if (seqFps <= 0.0) seqFps = 24.0;
                int totalFrames = static_cast<int>(durSec * seqFps);
                int hh = totalFrames / (3600 * static_cast<int>(seqFps));
                int mm = (totalFrames / (60 * static_cast<int>(seqFps))) % 60;
                int ss = (totalFrames / static_cast<int>(seqFps)) % 60;
                int ff = totalFrames % static_cast<int>(seqFps);
                seqItem->setText(2, QString("%1:%2:%3:%4  (%5 tracks)")
                    .arg(hh, 2, 10, QChar('0'))
                    .arg(mm, 2, 10, QChar('0'))
                    .arg(ss, 2, 10, QChar('0'))
                    .arg(ff, 2, 10, QChar('0'))
                    .arg(trackCount));

                // FPS and resolution from THIS sequence's own settings
                seqItem->setText(3, QString::number(seqFps, 'f',
                    (seqFps == std::floor(seqFps)) ? 0 : 2));
                const auto& res = seq->settings().resolution();
                seqItem->setText(4, QStringLiteral("%1x%2").arg(res.width).arg(res.height));
                seqItem->setText(5, "");
            }

            m_listWidget->addTopLevelItem(seqItem);
            ++count;
        }
    }

    // -- Media items -----------------------------------------------------
    const auto& items = m_grid->items();

    for (const auto& item : items)
    {
        // Skip folder items â they are synthetic icon-view items, not real media
        if (item.isFolder)
            continue;

        // Apply search filter (don't use item.visible â icon-view navigation
        // may have set it to false for items not at the current level).
        const QString searchText = m_searchField->text().trimmed();
        if (!searchText.isEmpty()) {
            QString name = item.displayName.isEmpty()
                ? QString::fromStdString(pathToUtf8(item.filePath.filename()))
                : item.displayName;
            if (!name.contains(searchText, Qt::CaseInsensitive))
                continue;
        }

        const bool isAdjustment = projectBinIsAdjustmentPath(item.filePath);

        // Apply type filter (adjustment layers only show on the "All" tab —
        // they aren't a media type, so they shouldn't appear under Video/etc.)
        if (m_activeType != MediaType::Unknown) {
            if (isAdjustment || item.type != m_activeType)
                continue;
        }

        auto* treeItem = new QTreeWidgetItem();

        // Name column ï¿½ use displayName if set, otherwise filename
        QString name = item.displayName.isEmpty()
            ? QString::fromStdString(pathToUtf8(item.filePath.filename()))
            : item.displayName;
        treeItem->setText(0, name);
        treeItem->setData(0, Qt::UserRole, QString::fromStdString(pathToUtf8(item.filePath)));
        treeItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(item.mediaHandle));
        treeItem->setData(0, kBinItemIdRole,
                          QVariant::fromValue(static_cast<qulonglong>(item.itemId)));
        // Media items are draggable but NOT drop targets
        treeItem->setFlags((treeItem->flags() & ~Qt::ItemIsDropEnabled) | Qt::ItemIsEditable);

        // Premiere Pro-style type icon and label color bar
        QColor itemLabelColor;
        if (isAdjustment) {
            itemLabelColor = kLabelAdjustment;
        } else if (item.labelColor != 0xFF888888) {
            itemLabelColor = QColor::fromRgba(item.labelColor);
        } else {
            itemLabelColor = premiereDefaultLabel(item.type);
        }
        treeItem->setData(0, Qt::UserRole + 10, QVariant::fromValue(itemLabelColor));

        // Type-specific icon
        QString shape;
        if (isAdjustment) {
            shape = "adjustment";
        } else {
            switch (item.type) {
            case MediaType::Video: shape = "video"; break;
            case MediaType::Audio: shape = "audio"; break;
            case MediaType::Image: shape = "image"; break;
            case MediaType::Spine: shape = "spine"; break;
            default:               shape = "file";  break;
            }
        }
        treeItem->setIcon(0, makePremiereBinIcon(itemLabelColor, shape));

        // Media Offline indicator â only flag if both: no valid media handle
        // AND file not found on disk (a valid handle means MediaPool opened it)
        // Adjustment layers are synthetic — never offline.
        bool isOffline = !isAdjustment &&
            (item.mediaHandle == 0) &&
            !item.filePath.empty() &&
            !std::filesystem::exists(item.filePath);
        if (isOffline) {
            treeItem->setForeground(0, QColor(220, 60, 60));
            treeItem->setForeground(1, QColor(220, 60, 60));
            QFont offlineFont = treeItem->font(0);
            offlineFont.setItalic(true);
            treeItem->setFont(0, offlineFont);
            treeItem->setToolTip(0, QStringLiteral("Media Offline â ") +
                QString::fromStdString(pathToUtf8(item.filePath)));
        }

        // Type column
        QString typeStr;
        if (isAdjustment) {
            typeStr = "Adjustment";
        } else {
            switch (item.type)
            {
            case MediaType::Video: typeStr = "Video"; break;
            case MediaType::Image: typeStr = "Image"; break;
            case MediaType::Audio: typeStr = "Audio"; break;
            case MediaType::Spine: typeStr = "Spine"; break;
            default:               typeStr = "Unknown"; break;
            }
        }
        treeItem->setText(1, typeStr);

        // Mark synthetic adjustment-layer items so the drag widget can emit
        // the right MIME type and the timeline drop handler knows what to
        // create. Read by MediaDragTreeWidget::startDrag().
        if (isAdjustment) {
            treeItem->setData(0, Qt::UserRole + 6, true);
            treeItem->setData(0, Qt::UserRole + 7, name);
        }

        // Metadata columns from MediaPool (adjustment layers have no source
        // and never get queried). Leave the metadata columns blank for them
        // — duration is sequence-driven, not item-driven.
        const VideoStreamInfo* info = (m_pool && item.mediaHandle && !isAdjustment)
            ? m_pool->getInfo(item.mediaHandle) : nullptr;
        if (isAdjustment) {
            treeItem->setText(2, "");
            treeItem->setText(3, "");
            treeItem->setText(4, "");
            treeItem->setText(5, "");
            m_listWidget->addTopLevelItem(treeItem);
            ++count;
            continue;
        }


        if (info) {
            // Duration
            if (info->duration > 0.0) {
                double fps = (info->fps > 0.0) ? info->fps : 24.0;
                int totalFrames = static_cast<int>(info->duration * fps);
                int hh = totalFrames / (3600 * static_cast<int>(fps));
                int mm = (totalFrames / (60 * static_cast<int>(fps))) % 60;
                int ss = (totalFrames / static_cast<int>(fps)) % 60;
                int ff = totalFrames % static_cast<int>(fps);
                treeItem->setText(2, QString("%1:%2:%3:%4")
                    .arg(hh, 2, 10, QChar('0'))
                    .arg(mm, 2, 10, QChar('0'))
                    .arg(ss, 2, 10, QChar('0'))
                    .arg(ff, 2, 10, QChar('0')));
            } else {
                treeItem->setText(2, "--:--:--:--");
            }

            // Frame rate
            if (info->fps > 0.0)
                treeItem->setText(3, QString::number(info->fps, 'f',
                    (info->fps == std::floor(info->fps)) ? 0 : 2));
            else
                treeItem->setText(3, "--");

            // Resolution
            if (info->width > 0 && info->height > 0)
                treeItem->setText(4, QStringLiteral("%1x%2").arg(info->width).arg(info->height));
            else
                treeItem->setText(4, "");

            // Sample rate (audio files ï¿½ use project audio sample rate)
            if (item.type == MediaType::Audio && info->hasAudio && m_project)
                treeItem->setText(5, QString("%1 Hz")
                    .arg(m_project->settings().audioFormat().sampleRate));
            else
                treeItem->setText(5, "");
        } else {
            treeItem->setText(2, "--:--:--:--");
            treeItem->setText(3, "--");
            treeItem->setText(4, (item.type == MediaType::Video || item.type == MediaType::Image) ? "--" : "");
            treeItem->setText(5, (item.type == MediaType::Audio) ? "--" : "");
        }

        // Store media type and frame count for hover-scrub
        treeItem->setData(0, kMediaTypeRole, static_cast<int>(item.type));
        if (info && info->frameCount > 0)
            treeItem->setData(0, kFrameCountRole, QVariant::fromValue(info->frameCount));

        m_listWidget->addTopLevelItem(treeItem);
        ++count;
    }

    if (!savedFolders.empty())
        restoreBinFolders(savedFolders);

    // Expand bins when searching: only expand bins whose subtree contains
    // at least one visible child.  When NOT searching, leave expanded state
    // untouched so that dropping files doesn't blow open every bin.
    const bool searching = !m_searchField->text().trimmed().isEmpty();
    if (searching) {
        std::function<bool(QTreeWidgetItem*)> expandBins =
            [&](QTreeWidgetItem* item) -> bool {
            if (!item->data(0, Qt::UserRole + 2).toBool()) return false;
            bool hasVisibleChild = false;
            for (int c = 0; c < item->childCount(); ++c) {
                auto* child = item->child(c);
                if (child->data(0, Qt::UserRole + 2).toBool()) {
                    if (expandBins(child))
                        hasVisibleChild = true;
                } else {
                    hasVisibleChild = true;
                }
            }
            item->setExpanded(hasVisibleChild);
            return hasVisibleChild;
        };
        for (int i = 0; i < m_listWidget->topLevelItemCount(); ++i)
            expandBins(m_listWidget->topLevelItem(i));
    }

    m_statusLabel->setText(QString("%1 items").arg(count));

    // Toggle empty state
    bool empty = (m_grid->items().empty() && m_listWidget->topLevelItemCount() == 0);
    if (m_emptyLabel) m_emptyLabel->setVisible(empty && m_listView);

    // Re-enable itemChanged signals after rebuild
    m_listWidget->blockSignals(false);
}

// -----------------------------------------------------------------------------
//  Icon view â Explorer-style bin navigation
// -----------------------------------------------------------------------------

void ProjectBin::syncIconView()
{
    // Update breadcrumb label
    QString crumb = "Project";
    for (const auto& seg : m_iconBinPath)
        crumb += QStringLiteral("  \u203A  ") + seg;  // âº
    m_breadcrumbLabel->setText(crumb);

    // -- Find the target container in the QTreeWidget --------------------
    // Walk m_iconBinPath to locate the parent QTreeWidgetItem (nullptr = root).
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
            // Bin path segment not found â reset to root
            m_iconBinPath.clear();
            m_breadcrumbLabel->setText("Project");
            container = nullptr;
            break;
        }
    }

    // -- Collect children at the current level ---------------------------
    int childCount = container ? container->childCount()
                               : m_listWidget->topLevelItemCount();

    // Collect the set of file paths that are direct children here
    std::set<std::string> visiblePaths;
    // Collect bin names at this level
    std::vector<QString> binNames;

    for (int i = 0; i < childCount; ++i) {
        auto* child = container ? container->child(i)
                                : m_listWidget->topLevelItem(i);
        if (child->data(0, Qt::UserRole + 2).toBool()) {
            // It's a bin/folder
            binNames.push_back(child->text(0));
        } else {
            // It's a media item or sequence
            QString fp = child->data(0, Qt::UserRole).toString();
            if (!fp.isEmpty())
                visiblePaths.insert(fp.toStdString());
        }
    }

    // -- Remove old synthetic items (folders + sequences) from the grid ---
    auto& items = m_grid->mutableItems();
    items.erase(std::remove_if(items.begin(), items.end(),
        [](const ThumbnailItem& it) { return it.isFolder; }), items.end());

    // -- Add folder items for bins at this level (at the front) ----------
    // Also collect sequence names so we can show them
    std::vector<QString> seqNames;
    for (auto it = binNames.rbegin(); it != binNames.rend(); ++it) {
        ThumbnailItem fi;
        fi.isFolder = true;
        fi.folderName = *it;
        fi.displayName = *it;
        fi.visible = true;
        fi.type = MediaType::Unknown;
        items.insert(items.begin(), std::move(fi));
    }

    // -- Collect sequences at this level ---------------------------------
    for (int i = 0; i < childCount; ++i) {
        auto* child = container ? container->child(i)
                                : m_listWidget->topLevelItem(i);
        if (child->data(0, Qt::UserRole + 3).toBool()) {
            // Sequence â add as a synthetic grid item so it's visible
            seqNames.push_back(child->text(0));
        }
    }
    // Insert sequence items after folders but before media
    int insertPos = static_cast<int>(binNames.size());
    for (const auto& sn : seqNames) {
        ThumbnailItem si;
        si.isFolder = true;   // Use folder rendering (no thumbnail)
        si.folderName = sn;
        si.displayName = sn;
        si.visible = true;
        si.type = MediaType::Unknown;
        // Mark as sequence folder so double-click doesn't navigate into it
        si.labelColor = 0xFF6666CC;  // sequence violet
        items.insert(items.begin() + insertPos, std::move(si));
        ++insertPos;
    }

    // -- Set visibility on media items -----------------------------------
    for (auto& item : items) {
        if (item.isFolder) continue;
        std::string key = pathToUtf8(item.filePath);
        item.visible = visiblePaths.count(key) > 0;
    }

    m_grid->clearSelection();
    m_grid->loadVisibleThumbnails();
    // Force relayout + repaint
    m_grid->setZoom(m_grid->zoom());
}

void ProjectBin::refreshSequences()
{
    if (m_listView) {
        syncListView();
    } else {
        // In icon view mode, we still need to rebuild the tree (which
        // syncIconView reads from) so sequences appear in the grid.
        syncListView();
    }
    // Always sync the icon view so sequences show up in both modes.
    // syncListView rebuilds the tree; syncIconView then reads it to
    // populate the grid.  This is cheap when nothing changed because
    // the grid only rebuilds visible items.
    syncIconView();
}


} // namespace rt
