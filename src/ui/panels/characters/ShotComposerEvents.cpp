/*
 * ShotComposerEvents.cpp — Event filter and drag-drop handling for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
#include <QToolTip>
#include <QUrl>

#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  Drop indicator line
// =====================================================================

void ShotComposer::updateDropIndicatorLine()
{
    if (!m_dropIndicatorLine || !m_layerList) return;
    if (m_dropIndicatorIndex < 0) {
        m_dropIndicatorLine->hide();
        return;
    }
    int count = m_layerList->count();
    int y = 0;
    if (count == 0) {
        y = 0;
    } else if (m_dropIndicatorIndex < count) {
        auto* item = m_layerList->item(m_dropIndicatorIndex);
        QRect r = m_layerList->visualItemRect(item);
        y = r.top() - 1;
    } else {
        auto* item = m_layerList->item(count - 1);
        QRect r = m_layerList->visualItemRect(item);
        y = r.bottom();
    }
    m_dropIndicatorLine->setGeometry(0, y, m_layerList->viewport()->width(), 3);
    m_dropIndicatorLine->show();
    m_dropIndicatorLine->raise();
}

// =====================================================================
//  eventFilter — drag-drop from asset library + keyboard shortcuts
// =====================================================================

bool ShotComposer::eventFilter(QObject* obj, QEvent* event)
{
    static constexpr const char* kAssetMime = "application/x-roundtable-asset";

    // ── Drop image files from the OS onto the background library to import ──
    if (m_backgroundLibrary &&
        (obj == m_backgroundLibrary || obj == m_backgroundLibrary->viewport())) {
        auto hasImageUrls = [](const QMimeData* md) {
            if (!md || !md->hasUrls()) return false;
            static const QStringList ext = {"png", "jpg", "jpeg", "bmp", "webp"};
            for (const QUrl& u : md->urls()) {
                if (!u.isLocalFile()) continue;
                QString suffix = QFileInfo(u.toLocalFile()).suffix().toLower();
                if (ext.contains(suffix)) return true;
            }
            return false;
        };
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (hasImageUrls(de->mimeData())) {
                de->setDropAction(Qt::CopyAction);
                de->acceptProposedAction();
                return true;
            }
            return false;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            if (!hasImageUrls(de->mimeData())) return false;
            QStringList paths;
            for (const QUrl& u : de->mimeData()->urls())
                if (u.isLocalFile()) paths << u.toLocalFile();
            de->setDropAction(Qt::CopyAction);
            de->acceptProposedAction();
            importBackgroundFiles(paths);
            return true;
        }
    }

    // ── Drag-and-drop from asset library onto layer list or preview ──────
    bool isLayerList = m_layerList &&
                       (obj == m_layerList || obj == m_layerList->viewport());
    bool isPreview = false;
#ifdef ROUNDTABLE_HAS_SPINE
    if (obj == m_spinePreview) isPreview = true;
#endif
    bool isDropTarget = isLayerList || isPreview;

    if (isDropTarget) {
        if (event->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
                // Build drag thumbnail for compose preview overlay
                if (m_dragThumb.isNull()) {
                    QByteArray payload = de->mimeData()->data(QString::fromLatin1(kAssetMime));
                    QDataStream ds(&payload, QIODevice::ReadOnly);
                    QString assetType;
                    ds >> assetType;
                    if (assetType == QStringLiteral("character") ||
                        assetType == QStringLiteral("videoChar")) {
                        QString name;
                        ds >> name;
                        m_dragThumb = makeCharacterThumbnail(name.toStdString(), 120);
                    } else if (assetType == QStringLiteral("background")) {
                        QString bgName;
                        ds >> bgName;
                        // Use the background library icon if available
                        for (int i = 0; i < m_backgroundLibrary->count(); ++i) {
                            auto* item = m_backgroundLibrary->item(i);
                            if (item->text() == bgName) {
                                m_dragThumb = item->icon().pixmap(120, 120);
                                break;
                            }
                        }
                    } else if (assetType == QStringLiteral("video")) {
                        QString filename;
                        ds >> filename;
                        for (int i = 0; i < m_videoLibrary->count(); ++i) {
                            auto* item = m_videoLibrary->item(i);
                            if (item->text() == filename) {
                                m_dragThumb = item->icon().pixmap(120, 120);
                                break;
                            }
                        }
                    }
                }
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragMove) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
                if (isLayerList) {
                    // Compute drop indicator position between layer rows
                    QPoint pos = de->position().toPoint();
                    int count = m_layerList->count();
                    int insertIdx = count; // default: append at end
                    for (int i = 0; i < count; ++i) {
                        auto* item = m_layerList->item(i);
                        QRect r = m_layerList->visualItemRect(item);
                        if (pos.y() < r.top() + r.height() / 2) {
                            insertIdx = i;
                            break;
                        }
                    }
                    if (m_dropIndicatorIndex != insertIdx) {
                        m_dropIndicatorIndex = insertIdx;
                        updateDropIndicatorLine();
                    }
                }
#ifdef ROUNDTABLE_HAS_SPINE
                if (isPreview) {
                    m_dragOverPreview = true;
                    m_dragPreviewPos = de->position().toPoint();
                    if (!m_dragThumb.isNull())
                        m_spinePreview->setDragOverlay(m_dragThumb, m_dragPreviewPos);
                }
#endif
                de->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::DragLeave) {
            if (isLayerList && m_dropIndicatorIndex >= 0) {
                m_dropIndicatorIndex = -1;
                updateDropIndicatorLine();
            }
#ifdef ROUNDTABLE_HAS_SPINE
            if (isPreview && m_dragOverPreview) {
                m_dragOverPreview = false;
                m_dragThumb = QPixmap();
                m_spinePreview->clearDragOverlay();
            }
#endif
            return false;
        }
        if (event->type() == QEvent::Drop) {
            auto* de = static_cast<QDropEvent*>(event);
            QByteArray payload = de->mimeData()->data(QString::fromLatin1(kAssetMime));
            if (!payload.isEmpty()) {
                QDataStream ds(&payload, QIODevice::ReadOnly);
                QString assetType;
                ds >> assetType;

                // Capture insertion index before adding (layer count may change)
                int insertAt = m_dropIndicatorIndex;

                int newLayerIdx = -1;
                if (assetType == QStringLiteral("character")) {
                    QString folderName;
                    ds >> folderName;
                    addCharacter(folderName.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("videoChar")) {
                    QString charName, mutePath, talkPath;
                    ds >> charName >> mutePath >> talkPath;
                    addCharacter(charName.toStdString(),
                                 mutePath.toStdString(),
                                 talkPath.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("background")) {
                    QString bgName;
                    ds >> bgName;
                    addBackground(bgName.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                } else if (assetType == QStringLiteral("video")) {
                    QString filename;
                    ds >> filename;
                    addVideoLayer(filename.toStdString());
                    newLayerIdx = m_currentShot.layerCount() - 1;
                }

                // Move to insertion position if dropped on layer list
                if (isLayerList && insertAt >= 0 && newLayerIdx >= 0 &&
                    insertAt != newLayerIdx) {
                    m_currentShot.moveLayerTo(newLayerIdx, insertAt);
                    refreshLayerList();
                    selectLayer(insertAt);
                }

                // Clear drag state
                m_dropIndicatorIndex = -1;
                m_dragOverPreview = false;
                m_dragThumb = QPixmap();
                updateDropIndicatorLine();
#ifdef ROUNDTABLE_HAS_SPINE
                if (m_spinePreview) m_spinePreview->clearDragOverlay();
#endif

                de->acceptProposedAction();
                return true;
            }
        }
    }

    // Intercept Ctrl+S on the preview widget so it saves the shot
    // instead of triggering MainWindow's project-save action.
    if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->matches(QKeySequence::Save) ||
            ke->matches(QKeySequence::Copy) ||
            ke->matches(QKeySequence::Paste)) {
            event->accept();
            return true;
        }
        // Ctrl+Shift+C / Ctrl+Shift+V for transform copy-paste
        if ((ke->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) &&
            (ke->key() == Qt::Key_C || ke->key() == Qt::Key_V)) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->matches(QKeySequence::Save)) {
            if (saveCurrentShot())
                QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
                                   "Shot saved", this, {}, 1500);
            return true;
        }
        // Ctrl+Shift+C/V — transform copy-paste (check before plain Copy/Paste)
        if (ke->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
            if (ke->key() == Qt::Key_C) {
                copyTransform();
                return true;
            }
            if (ke->key() == Qt::Key_V) {
                pasteTransform();
                return true;
            }
        }
        // Copy/Paste — the shot list and char filter list are reparented
        // outside ShotComposer, so QShortcuts with WidgetWithChildrenShortcut
        // don't reach them.  Handle via eventFilter instead.
        if (ke->matches(QKeySequence::Copy)) {
            copySelectedLayer();
            return true;
        }
        if (ke->matches(QKeySequence::Paste)) {
            pasteLayer();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

} // namespace rt
