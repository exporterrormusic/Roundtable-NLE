/*
 * ProjectPanelHelpers.cpp — static helper functions extracted from
 * ProjectPanel.cpp. Also contains addRecentSaveLocation and
 * rebuildRecentPathButtons.
 */

#include "panels/project/ProjectPanel.h"

#include "Theme.h"
#include "Settings.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

namespace rt {

// =============================================================================
// Helpers
// =============================================================================

QString formatFileSize(qint64 bytes)
{
    if (bytes < 1024)        return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
}

QString formatDate(const QDateTime& dt)
{
    if (!dt.isValid()) return "Unknown";
    QDateTime now = QDateTime::currentDateTime();
    int daysAgo = dt.daysTo(now);
    if (daysAgo == 0) return "Today " + dt.toString("h:mm AP");
    if (daysAgo == 1) return "Yesterday " + dt.toString("h:mm AP");
    if (daysAgo < 7)  return dt.toString("dddd h:mm AP");
    return dt.toString("MMM d, yyyy");
}

/// Create a project thumbnail widget for a table cell.
/// Shows actual screenshot if available (projectDir/thumbs/<name>.png or .jpg),
/// otherwise renders a gradient preview with aspect-ratio badge.
/// Includes a small camera button to set a custom thumbnail.
QWidget* createProjectThumb(const ProjectInfo& info, QWidget* parent,
                             ProjectPanel* panel)
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* container = new QWidget(parent);
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* lay = new QHBoxLayout(container);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setAlignment(Qt::AlignCenter);

    double aspect = static_cast<double>(info.resW)
                  / std::max(1u, info.resH);
    int thumbH = 160;
    int thumbW = static_cast<int>(thumbH * aspect);
    thumbW = qBound(60, thumbW, 260);

    auto* thumbFrame = new QWidget(container);
    thumbFrame->setFixedSize(thumbW, thumbH);
    thumbFrame->setStyleSheet("background: transparent;");

    auto* thumb = new QLabel(thumbFrame);
    thumb->setGeometry(0, 0, thumbW, thumbH);

    // Load via QImage to bypass QPixmap's filename cache.  QPixmap(QString)
    // keys QPixmapCache on path + mtime(SECONDS) + size, so two captures
    // within the same wall-clock second that encode to the same byte size
    // return the previously cached pixmap — the thumbnail appears frozen.
    QFileInfo fi(info.filePath);
    QString thumbPathPng = fi.absolutePath() + "/" + fi.baseName() + ".png";
    QString thumbPathJpg = fi.absolutePath() + "/" + fi.baseName() + ".jpg";
    QImage thumbImg(thumbPathPng);
    if (thumbImg.isNull()) thumbImg.load(thumbPathJpg);
    QPixmap pix = thumbImg.isNull() ? QPixmap()
                                    : QPixmap::fromImage(std::move(thumbImg));

    if (!pix.isNull()) {
        thumb->setPixmap(pix.scaled(thumbW, thumbH,
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
        thumb->setAlignment(Qt::AlignCenter);
        thumb->setStyleSheet(QStringLiteral(
            "border-radius: %1px; border: 1px solid %2; background: %3;")
            .arg(m.radiusSm)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.surface2)));
    } else {
        QColor bg = info.isCurrent ? c.accentDim : c.surface3;
        QString tag = (info.resH > info.resW) ? "9:16"
                    : (info.resW >= 3840)     ? "4K"
                    :                           "16:9";
        thumb->setText(tag);
        thumb->setStyleSheet(QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
            "stop:0 %1, stop:1 %2);"
            " border-radius: %3px; border: 1px solid %4;"
            " color: %5; font-size: 22px; font-weight: 700;")
            .arg(Theme::rgb(bg))
            .arg(Theme::rgb(info.isCurrent ? c.accent : c.surface2))
            .arg(m.radiusSm)
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(info.isCurrent ? c.textPrimary : c.textTertiary)));
        thumb->setAlignment(Qt::AlignCenter);
    }

    auto* camBtn = new QPushButton(QStringLiteral("\U0001F4F7"), thumbFrame);
    camBtn->setFixedSize(30, 30);
    camBtn->move(thumbW - 34, thumbH - 34);
    camBtn->setCursor(Qt::PointingHandCursor);
    camBtn->setToolTip("Set thumbnail image...");
    camBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: rgba(0,0,0,160); border: none;"
        "  border-radius: 6px; font-size: %1px; color: white; padding: 0; }"
        "QPushButton:hover { background: rgba(0,0,0,220); }")
        .arg(Theme::typography().sizeBody));

    QString projectName = info.name;
    QObject::connect(camBtn, &QPushButton::clicked, panel,
                     [panel, projectName]() {
                         panel->setThumbnailForProject(projectName);
                     });

    lay->addWidget(thumbFrame);
    return container;
}

// =============================================================================
// Recent save location management
// =============================================================================

void ProjectPanel::addRecentSaveLocation(const QString& path)
{
    if (path.isEmpty()) return;
    QString normPath = QDir::toNativeSeparators(path).toLower();
    for (int i = m_recentSaveLocations.size() - 1; i >= 0; --i) {
        if (QDir::toNativeSeparators(m_recentSaveLocations[i]).toLower() == normPath)
            m_recentSaveLocations.removeAt(i);
    }
    m_recentSaveLocations.prepend(path);
    while (m_recentSaveLocations.size() > 8)
        m_recentSaveLocations.removeLast();

    auto settings = rt::appSettings();
    settings.setValue("ProjectPanel/RecentSaveLocations", m_recentSaveLocations);

    rebuildRecentPathButtons();
}

void ProjectPanel::rebuildRecentPathButtons()
{
    if (!m_recentPathsWidget) return;

    auto* rpLay = qobject_cast<QHBoxLayout*>(m_recentPathsWidget->layout());
    if (!rpLay) return;

    while (QLayoutItem* item = rpLay->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    const auto& c = Theme::colors();

    auto* recentLbl = new QLabel("Recent:");
    recentLbl->setObjectName("NewRecentLbl");
    recentLbl->setStyleSheet(QStringLiteral(
        "font-size: 9px; font-weight: 600; color: %1; letter-spacing: 0.4px;")
        .arg(Theme::rgb(c.textPrimary)));
    rpLay->addWidget(recentLbl);

    auto* projectsBtn = new QPushButton(" Projects");
    projectsBtn->setIcon(createFolderIcon());
    projectsBtn->setObjectName("NewRecentSample");
    projectsBtn->setCursor(Qt::PointingHandCursor);
    projectsBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2;"
        "  color: %3; font-size: 10px; padding: 2px 7px; }"
        "QPushButton:hover { background: %4; color: %5; }")
        .arg(Theme::rgb(c.surface1))
        .arg(Theme::rgb(c.border))
        .arg(Theme::rgb(c.textTertiary))
        .arg(Theme::rgb(c.surface2))
        .arg(Theme::rgb(c.textPrimary)));
    connect(projectsBtn, &QPushButton::clicked, this, [this]() {
        m_locationInput->setText(m_projectsDir);
        addRecentSaveLocation(m_projectsDir);
    });
    rpLay->addWidget(projectsBtn);

    int shown = 0;
    for (const auto& loc : m_recentSaveLocations) {
        if (shown >= 4) break;
        if (QDir::toNativeSeparators(loc).toLower()
            == QDir::toNativeSeparators(m_projectsDir).toLower())
            continue;

        QFileInfo fi(loc);
        QString label = fi.isRoot() ? loc : fi.fileName();
        if (label.length() > 22)
            label = label.left(19) + "...";

        auto* btn = new QPushButton(label);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(loc);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2;"
            "  color: %3; font-size: 10px; padding: 2px 7px; }"
            "QPushButton:hover { background: %4; color: %5; }")
            .arg(Theme::rgb(c.surface1))
            .arg(Theme::rgb(c.border))
            .arg(Theme::rgb(c.textTertiary))
            .arg(Theme::rgb(c.surface2))
            .arg(Theme::rgb(c.textPrimary)));

        connect(btn, &QPushButton::clicked, this, [this, loc]() {
            m_locationInput->setText(loc);
        });

        rpLay->addWidget(btn);
        ++shown;
    }

    rpLay->addStretch();
}

} // namespace rt
