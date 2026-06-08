/*
 * ProjectPanelContextMenu.cpp — context menus for the project table and
 * open list, extracted from ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QTableWidget>

namespace rt {

void ProjectPanel::setKnownShows(const QStringList& shows)
{
    m_knownShows = shows;
}

// =============================================================================
// Project table context menu
// =============================================================================

void ProjectPanel::showContextMenu(const QPoint& pos)
{
    auto* item = m_projectTable->itemAt(pos);
    if (!item) return;
    int row = item->row();
    auto* nameItem = m_projectTable->item(row, 1);
    if (!nameItem) return;

    QString name   = nameItem->data(Qt::UserRole).toString();
    QString fpath = nameItem->data(Qt::UserRole + 1).toString();
    QString currentShow = nameItem->data(Qt::UserRole + 2).toString();

    QMenu menu(this);
    menu.addAction("Open", this,
                   [this, name]() { emit openProject(name); });
    menu.addSeparator();

    // ── Assign to Show submenu ──────────────────────────────────────────
    // The project's current show is shown checked (case-insensitive match).
    {
        QMenu* showMenu = menu.addMenu("Assign to Show");
        QAction* noneAct = showMenu->addAction("(None)", this, [this, name, fpath]() {
            emit assignProjectToShow(name, fpath, QString());
        });
        noneAct->setCheckable(true);
        noneAct->setChecked(currentShow.isEmpty());
        if (!m_knownShows.isEmpty())
            showMenu->addSeparator();
        for (const QString& sh : m_knownShows) {
            QAction* act = showMenu->addAction(sh, this, [this, name, fpath, sh]() {
                emit assignProjectToShow(name, fpath, sh);
            });
            act->setCheckable(true);
            act->setChecked(QString::compare(sh, currentShow, Qt::CaseInsensitive) == 0);
        }
    }
    menu.addSeparator();
    menu.addAction("Rename...", this, [this, name]() {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Project",
            "New name:", QLineEdit::Normal, name, &ok);
        newName = newName.trimmed();
        if (ok && !newName.isEmpty() && newName != name)
            emit renameProject(name, newName);
    });
    menu.addAction("Duplicate", this,
                   [this, name]() { emit duplicateProject(name); });
    menu.addSeparator();
    menu.addAction("Show in Explorer", this,
                   [this, name]() { emit revealInExplorer(name); });
    menu.addAction("Set Thumbnail...", this,
                   [this, name]() { setThumbnailForProject(name); });
    menu.addAction("Remove Thumbnail", this,
                   [this, name]() { removeThumbnailForProject(name); });
    menu.addAction("Export...", this, [this, name]() {
        QString dst = QFileDialog::getSaveFileName(
            this, "Export Project", name + ".rtp",
            "ROUNDTABLE Projects (*.rtp)");
        if (!dst.isEmpty())
            emit exportProject(name, dst);
    });
    menu.addSeparator();
    menu.addAction("Delete", this,
                   [this, name, fpath]() { emit deleteProject(name, fpath); });

    menu.exec(m_projectTable->viewport()->mapToGlobal(pos));
}

// =============================================================================
// Open list context menu
// =============================================================================

void ProjectPanel::showOpenListContextMenu(const QPoint& pos)
{
    auto* item = m_openList->itemAt(pos);
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;
    QFileInfo fi(path);

    QMenu menu(this);
    menu.addAction("Open", this, [this, path]() {
        hideSidePanel();
        emit openFilePath(path);
    });
    menu.addSeparator();
    menu.addAction("Rename...", this, [this, path, fi]() {
        bool ok = false;
        QString newName = QInputDialog::getText(
            this, "Rename Project File",
            "New name:", QLineEdit::Normal, fi.baseName(), &ok);
        newName = newName.trimmed();
        if (!ok || newName.isEmpty() || newName == fi.baseName()) return;
        QString newPath = fi.absolutePath() + "/" + newName + "." + fi.suffix();
        if (QFile::exists(newPath)) {
            QMessageBox::warning(this, "Rename", "A file with that name already exists.");
            return;
        }
        QFile::rename(path, newPath);
        QString thumbDir = fi.absolutePath() + "/thumbs/";
        for (const auto& ext : {".png", ".jpg"}) {
            QString oldThumb = thumbDir + fi.baseName() + ext;
            if (QFile::exists(oldThumb))
                QFile::rename(oldThumb, thumbDir + newName + ext);
        }
        populateOpenList();
    });
    menu.addAction("Duplicate", this, [this, path, fi]() {
        QString baseName = fi.baseName() + " Copy";
        QString newPath = fi.absolutePath() + "/" + baseName + "." + fi.suffix();
        int counter = 2;
        while (QFile::exists(newPath)) {
            newPath = fi.absolutePath() + "/" + baseName + " " +
                      QString::number(counter++) + "." + fi.suffix();
        }
        QFile::copy(path, newPath);
        populateOpenList();
    });
    menu.addSeparator();
    menu.addAction("Delete", this, [this, path, fi]() {
        auto reply = QMessageBox::question(this, "Delete Project",
            QString("Delete \"%1\"?\n\nThis cannot be undone.").arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QFile::remove(path);
            populateOpenList();
        }
    });

    menu.exec(m_openList->viewport()->mapToGlobal(pos));
}

} // namespace rt
