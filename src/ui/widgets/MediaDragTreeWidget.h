/*
 * MediaDragTreeWidget — QTreeWidget subclass with Premiere Pro-style drag.
 *
 * Provides:
 *   - Custom startDrag() that creates a clean single-row drag pixmap
 *   - Proper MIME data with application/x-roundtable-media type
 *   - Items store media data in Qt::UserRole (path), UserRole+1 (handle),
 *     UserRole+2 (is-folder flag)
 */

#pragma once

#include <QTreeWidget>
#include <QRubberBand>
#include <QPoint>
#include <QTimer>
#include <QList>

namespace rt {

class MediaDragTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit MediaDragTreeWidget(QWidget* parent = nullptr);

    /// Abort any pending or in-progress inline rename: stops the rename timer,
    /// drops the candidate item pointer, and closes an open editor (reverting
    /// its text). MUST be called before the tree is cleared/rebuilt — otherwise
    /// the deferred editItem() fires on a freed QTreeWidgetItem, or a queued
    /// editor commit writes into deleted memory (an access violation observed
    /// while idle after a background bin refresh).
    void cancelPendingRename();

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // The model is reset by QTreeWidget::clear(); abort renames and drop all
    // raw item pointers before the base class frees the items.
    void reset() override;

private:
    QPoint m_dragStartPos;
    bool   m_rubberBanding{false};
    bool   m_pendingRubberBand{false};
    bool   m_leftPressActive{false};
    QRubberBand* m_rubberBand{nullptr};
    QTreeWidgetItem* m_pressItem{nullptr};
    bool m_deferSingleSelectOnRelease{false};
    bool m_dragStarted{false};
    QTimer*          m_renameTimer{nullptr};
    QTreeWidgetItem* m_renameCandidate{nullptr};
    QList<QTreeWidgetItem*> m_dragItemsSnapshot;
};

} // namespace rt
