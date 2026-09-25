/*
 * ProjectPanelEvents.cpp — event handlers (keyPressEvent, mousePressEvent,
 * eventFilter, resizeEvent) extracted from ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QRect>
#include <QTableWidget>

namespace rt {

// =============================================================================
// Keyboard shortcuts
// =============================================================================

void ProjectPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_projectTable->hasFocus()) {
            QString name = selectedProjectName();
            if (!name.isEmpty()) {
                emit openProject(name);
                event->accept();
                return;
            }
        }
    }
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (m_projectTable->hasFocus()) {
            QString name = selectedProjectName();
            if (!name.isEmpty()) {
                QString fpath;
                for (const auto& p : m_allProjects) {
                    if (p.name == name) { fpath = p.filePath; break; }
                }
                emit deleteProject(name, fpath);
                event->accept();
                return;
            }
        }
    }
    if (event->key() == Qt::Key_F5) {
        emit m_refreshBtn->clicked();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2) {
        QString name = selectedProjectName();
        if (!name.isEmpty()) {
            bool ok = false;
            QString newName = QInputDialog::getText(
                this, "Rename Project",
                "New name:", QLineEdit::Normal, name, &ok);
            newName = newName.trimmed();
            if (ok && !newName.isEmpty() && newName != name)
                emit renameProject(name, newName);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_N
        && (event->modifiers() & Qt::ControlModifier)) {
        toggleSidePanel(SidePanelMode::New);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_O
        && (event->modifiers() & Qt::ControlModifier)) {
        hideSidePanel();
        emit openFromFile();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_sidePanelMode != SidePanelMode::None) {
            hideSidePanel();
            event->accept();
            return;
        }
    }

    QWidget::keyPressEvent(event);
}

// =============================================================================
// Mouse press -> close side panel when clicking on the content area
// =============================================================================

void ProjectPanel::mousePressEvent(QMouseEvent* event)
{
    if (m_sidePanelMode != SidePanelMode::None) {
        QPoint pos = event->pos();
        QRect sidePanelRect = m_sidePanel->geometry();
        QRect iconRailRect = m_iconRail->geometry();

        if (!sidePanelRect.contains(pos) && !iconRailRect.contains(pos)) {
            hideSidePanel();
        }
    }

    QWidget::mousePressEvent(event);
}

// =============================================================================
// Event filter -> click on empty table area deselects
// =============================================================================

bool ProjectPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_projectTable->viewport()
        && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        QTableWidgetItem* item = m_projectTable->itemAt(me->pos());
        if (!item) {
            m_projectTable->clearSelection();
            m_projectTable->setCurrentItem(nullptr);
            updateActionButtons();
        }
        if (m_sidePanelMode != SidePanelMode::None)
            hideSidePanel();
    }

    if (obj == m_contentArea
        && event->type() == QEvent::MouseButtonPress) {
        if (m_sidePanelMode != SidePanelMode::None)
            hideSidePanel();
    }

    return QWidget::eventFilter(obj, event);
}

// =============================================================================
// Resize event -> apply responsive layout
// =============================================================================

void ProjectPanel::resizeEvent(QResizeEvent* event)
{
    static thread_local bool s_inResize = false;
    if (s_inResize) {
        QWidget::resizeEvent(event);
        return;
    }
    s_inResize = true;
    applyResponsiveLayout();
    applyNewPanelResponsiveLayout();
    QWidget::resizeEvent(event);
    s_inResize = false;
}

} // namespace rt
