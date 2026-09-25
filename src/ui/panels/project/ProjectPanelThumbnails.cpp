/*
 * ProjectPanelThumbnails.cpp — thumbnail management functions extracted
 * from ProjectPanel.cpp.
 */

#include "panels/project/ProjectPanel.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QString>

#include <spdlog/spdlog.h>

namespace rt {

// =============================================================================
// Thumbnail management
// =============================================================================

// Resolve the on-disk thumbnail PNG path for a project.  Thumbnails live next
// to the project's .rtp file (createProjectThumb reads them from there), so
// the save path MUST match — we look up the project's filePath in
// m_allProjects rather than assuming the project lives under m_projectsDir
// (recent-files entries can be anywhere on disk).
QString ProjectPanel::thumbnailPathForProject(const QString& projectName) const
{
    for (const auto& p : m_allProjects) {
        if (p.name == projectName && !p.filePath.isEmpty()) {
            QFileInfo fi(p.filePath);
            return fi.absolutePath() + "/" + fi.completeBaseName() + ".png";
        }
    }
    if (m_projectsDir.isEmpty()) return {};
    return m_projectsDir + "/" + projectName + "/" + projectName + ".png";
}

void ProjectPanel::removeThumbnailForProject(const QString& projectName)
{
    QString pngPath = thumbnailPathForProject(projectName);
    if (pngPath.isEmpty()) return;
    QString jpgPath = pngPath;
    jpgPath.chop(4);
    jpgPath += ".jpg";

    bool removed = false;
    removed |= QFile::remove(pngPath);
    removed |= QFile::remove(jpgPath);

    if (removed) {
        spdlog::info("ProjectPanel: Removed thumbnail for '{}'",
                     projectName.toStdString());
        rebuildTable();
    }
}

void ProjectPanel::setThumbnailForProject(const QString& projectName)
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Thumbnail Image", QString(),
        "Images (*.png *.jpg *.jpeg *.bmp *.webp)");

    if (path.isEmpty()) return;

    QImage img(path);
    if (img.isNull()) return;

    QString dstPath = thumbnailPathForProject(projectName);
    if (dstPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(dstPath).absolutePath());

    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString jpgPath = dstPath;
    jpgPath.chop(4);
    jpgPath += ".jpg";
    QFile::remove(jpgPath);

    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Set thumbnail for '{}' from '{}'",
                     projectName.toStdString(), path.toStdString());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromPixels(const QString& projectName,
                                           const uint8_t* bgra,
                                           uint32_t width, uint32_t height)
{
    if (!bgra || width == 0 || height == 0) return;

    QString dstPath = thumbnailPathForProject(projectName);
    if (dstPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(dstPath).absolutePath());

    QImage img(bgra, static_cast<int>(width), static_cast<int>(height),
               static_cast<int>(width * 4), QImage::Format_ARGB32);

    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Auto-saved thumbnail for '{}'  ({}x{})",
                     projectName.toStdString(), width, height);
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to auto-save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

void ProjectPanel::setThumbnailFromImage(const QString& projectName, const QImage& image)
{
    if (image.isNull()) return;

    QString dstPath = thumbnailPathForProject(projectName);
    if (dstPath.isEmpty()) return;
    QDir().mkpath(QFileInfo(dstPath).absolutePath());

    QImage img = image;
    if (img.width() > 480)
        img = img.scaledToWidth(480, Qt::SmoothTransformation);

    QString jpgPath = dstPath;
    jpgPath.chop(4);
    jpgPath += ".jpg";
    QFile::remove(jpgPath);
    if (img.save(dstPath, "PNG")) {
        spdlog::info("ProjectPanel: Saved thumbnail for '{}' from viewport grab ({}x{})",
                     projectName.toStdString(), img.width(), img.height());
        rebuildTable();
    } else {
        spdlog::warn("ProjectPanel: Failed to save thumbnail to '{}'",
                     dstPath.toStdString());
    }
}

} // namespace rt
