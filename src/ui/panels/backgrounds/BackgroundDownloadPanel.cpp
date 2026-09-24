// BackgroundDownloadPanel.cpp — Auto-scanning NikkeBKG panel.
//
// Opens the MEGA folder in the user's browser for manual download.
// Uses QFileSystemWatcher to auto-detect new files in assets/NikkeBKG/
// and refresh the thumbnail grid — no manual "Scan Folder" button needed.

#include "panels/backgrounds/BackgroundDownloadPanel.h"
#include "Theme.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QImageReader>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPixmap>
#include <QPixmapCache>
#include <QShowEvent>
#include <QUrl>

#include <spdlog/spdlog.h>

namespace rt {

// Shared statics — Timeline and Shot panels share file list
QSet<QString>         BackgroundDownloadPanel::s_sharedFileNames;
bool                  BackgroundDownloadPanel::s_scanDone = false;

// Cache key prefix for QPixmapCache entries
static const QString kCacheKeyPrefix = QStringLiteral("nikke_thumb_");

// ═════════════════════════════════════════════════════════════════════════════
// BackgroundGridWidget — custom list widget supporting drag + double-click
// ═════════════════════════════════════════════════════════════════════════════

QMimeData* BackgroundGridWidget::mimeData(const QList<QListWidgetItem*>& items) const
{
    if (items.isEmpty()) return nullptr;
    auto* item = items.first();
    if (!item || !(item->flags() & Qt::ItemIsDragEnabled))
        return nullptr;

    const QString filePath = item->data(Qt::UserRole).toString();
    if (filePath.isEmpty()) return nullptr;

    auto* md = new QMimeData;

    // Provide application/x-roundtable-asset for ShotComposer compatibility
    // Use the full file path (e.g. "assets/NikkeBKG/NikkeRoom.png") instead of
    // just the basename, so the drop handler can resolve the file correctly.
    // This matches how the normal backgrounds panel passes the relative path
    // and how the double-click handler emits the full path.
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds << QStringLiteral("background") << filePath;
    md->setData("application/x-roundtable-asset", payload);

    // Provide file URLs for Timeline / universal drop support
    md->setUrls({QUrl::fromLocalFile(filePath)});

    return md;
}

void BackgroundGridWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    QListWidget::mouseDoubleClickEvent(event);
    auto* item = itemAt(event->pos());
    if (item) {
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            emit backgroundActivated(path);
    }
}

// ═════════════════════════════════════════════════════════════════════════════

BackgroundDownloadPanel::BackgroundDownloadPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    // No scanning, no file watcher, no timers — absolutely nothing
    // that could slow down startup. Everything initializes lazily
    // the first time the panel is shown or when the user switches
    // to the NikkeBKG tab.
}

BackgroundDownloadPanel::~BackgroundDownloadPanel()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop timers — prevents them firing during destruction
    if (m_batchTimer) m_batchTimer->stop();
    if (m_scanDebounce) m_scanDebounce->stop();
}

void BackgroundDownloadPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // First time this panel is shown (user clicked the NikkeBKG tab),
    // do the actual scanning and file watching — never during startup.
    lazyInit();
}

void BackgroundDownloadPanel::onOpenMegaClicked()
{
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kMegaUrl)));
    setStatus(QStringLiteral("Opened MEGA in your browser. Download and place files in assets/NikkeBKG/"));
}

// ═════════════════════════════════════════════════════════════════════════════
// Lazy init — called once on first show
// ═════════════════════════════════════════════════════════════════════════════

void BackgroundDownloadPanel::lazyInit()
{
    if (m_initDone) return;
    m_initDone = true;

    scanLocalFiles();
    startPopulateGrid();
    startWatching();
}

void BackgroundDownloadPanel::startPopulateGrid()
{
    m_backgroundGrid->clear();

    if (m_localFileNames.isEmpty()) {
        setStatus(QStringLiteral("No backgrounds found yet"));
        return;
    }

    const auto& tc = Theme::colors();

    // Build the sorted list of files to process
    m_batchQueue = m_localFileNames.values();
    m_batchQueue.sort();
    m_batchTotal = m_batchQueue.size();

    // Build the full path prefix once, as an ABSOLUTE path: the Source
    // Monitor drop hands it to MediaSourceService/MediaPool, which
    // canonicalise paths. The Backgrounds folder tab in LibraryPanel
    // already stores fi.absoluteFilePath(); match that.
    const QString dirPath = QDir(QString::fromLatin1(kTargetDir)).absolutePath()
                            + QStringLiteral("/");

    // Create all items upfront (no thumbnails yet — fast)
    for (const QString& name : m_batchQueue) {
        auto* item = new QListWidgetItem(name);
        // Store the full file path in UserRole for drag/double-click
        item->setData(Qt::UserRole, dirPath + name);
        // Check shared pixmap cache first (QPixmapCache avoids GPU resource lifetime issues)
        QPixmap cached;
        if (QPixmapCache::find(kCacheKeyPrefix + name, &cached))
            item->setIcon(QIcon(cached));
        item->setForeground(tc.textSecondary);
        item->setToolTip(name);
        m_backgroundGrid->addItem(item);
    }

    // Count how many thumbnails we still need
    int cached = 0;
    for (const QString& name : m_batchQueue) {
        QPixmap dummy;
        if (QPixmapCache::find(kCacheKeyPrefix + name, &dummy)) cached++;
    }

    if (cached == m_batchTotal) {
        // All thumbnails already cached — done instantly
        setStatus(QStringLiteral("%1 background%2 in folder")
                  .arg(m_localFileNames.size())
                  .arg(m_localFileNames.size() == 1 ? QString() : QStringLiteral("s")));
        if (m_localFileNames.size() > 0)
            emit backgroundsDownloaded();
        return;
    }

    // Remove cached items from the batch queue
    m_batchQueue.erase(
        std::remove_if(m_batchQueue.begin(), m_batchQueue.end(),
                       [](const QString& n) {
                           QPixmap dummy;
                           return QPixmapCache::find(kCacheKeyPrefix + n, &dummy);
                       }),
        m_batchQueue.end());

    setStatus(QStringLiteral("Loading thumbnails: %1/%2\u2026")
              .arg(m_batchTotal - m_batchQueue.size()).arg(m_batchTotal));

    // Start batch processing for uncached items
    if (m_batchTimer) m_batchTimer->stop();
    m_batchTimer = new QTimer(this);
    m_batchTimer->setInterval(kBatchIntervalMs);
    connect(m_batchTimer, &QTimer::timeout, this, &BackgroundDownloadPanel::processBatch);
    m_batchTimer->start();
}

void BackgroundDownloadPanel::processBatch()
{
    // Process up to kBatchSize items per tick
    int count = 0;
    while (!m_batchQueue.isEmpty() && count < kBatchSize) {
        QString name = m_batchQueue.takeFirst();
        QListWidgetItem* item = nullptr;
        // Find the item by text (they were added in sorted order)
        for (int i = 0; i < m_backgroundGrid->count(); ++i) {
            if (m_backgroundGrid->item(i)->text() == name) {
                item = m_backgroundGrid->item(i);
                break;
            }
        }
        if (item) {
            QString path = QString::fromLatin1(kTargetDir) + QStringLiteral("/") + name;
            QString thumbPath = QString::fromLatin1(kTargetDir)
                + QStringLiteral("/.thumbnails/") + name;

            // Try loading cached thumbnail first
            QPixmap cachedPix(thumbPath);
            if (!cachedPix.isNull()) {
                item->setIcon(QIcon(cachedPix));
                QPixmapCache::insert(kCacheKeyPrefix + name, cachedPix);
            } else {
                // Decode at small size and save as cached thumbnail
                QImageReader reader(path);
                reader.setScaledSize(QSize(192, 192));
                reader.setAutoTransform(true);
                QImage img = reader.read();
                if (!img.isNull()) {
                    // Save thumbnail to disk for next time
                    QDir().mkpath(QString::fromLatin1(kTargetDir) + QStringLiteral("/.thumbnails"));
                    img.save(thumbPath, "PNG");

                    QPixmap thumbPix = QPixmap::fromImage(img);
                    item->setIcon(QIcon(thumbPix));
                    QPixmapCache::insert(kCacheKeyPrefix + name, thumbPix);
                }
            }
        }
        count++;
    }

    // Update status with progress
    int done = m_batchTotal - m_batchQueue.size();
    setStatus(QStringLiteral("Loading thumbnails: %1/%2\u2026").arg(done).arg(m_batchTotal));

    // Stop when done
    if (m_batchQueue.isEmpty()) {
        m_batchTimer->stop();
        setStatus(QStringLiteral("%1 background%2 in folder")
                  .arg(m_localFileNames.size())
                  .arg(m_localFileNames.size() == 1 ? QString() : QStringLiteral("s")));
        if (m_localFileNames.size() > 0)
            emit backgroundsDownloaded();
    }
}

void BackgroundDownloadPanel::startWatching()
{
    m_fileWatcher = new QFileSystemWatcher(this);
    m_scanDebounce = new QTimer(this);
    m_scanDebounce->setSingleShot(true);
    m_scanDebounce->setInterval(500);

    connect(m_scanDebounce, &QTimer::timeout, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        QSet<QString> before = m_localFileNames;
        s_scanDone = false; // force re-scan on next call
        scanLocalFiles();
        if (m_localFileNames != before) {
            QPixmapCache::clear(); // files changed, invalidate all caches
            startPopulateGrid();
            if (m_localFileNames.size() > 0)
                emit backgroundsDownloaded();
        }
    });

    connect(m_fileWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; m_scanDebounce->start(); });

    // Watch the target directory (create it first if needed)
    QString dirPath = QString::fromLatin1(kTargetDir);
    QDir().mkpath(dirPath);
    QDir targetDir(dirPath);
    if (targetDir.exists())
        m_fileWatcher->addPath(targetDir.absolutePath());
}

void BackgroundDownloadPanel::scanLocalFiles()
{
    // If any instance has already scanned, reuse the shared file list
    if (s_scanDone) {
        m_localFileNames = s_sharedFileNames;
        spdlog::info("BackgroundDownload: Reusing shared scan ({} files)", m_localFileNames.size());
        return;
    }

    m_localFileNames.clear();
    QDir targetDir(QString::fromLatin1(kTargetDir));
    if (!targetDir.exists()) return;
    const QStringList filters = {
        QStringLiteral("*.png"), QStringLiteral("*.jpg"),
        QStringLiteral("*.jpeg"), QStringLiteral("*.webp")
    };
    for (const QString& f : targetDir.entryList(filters, QDir::Files, QDir::Name))
        m_localFileNames.insert(f);

    // Populate shared cache
    s_sharedFileNames = m_localFileNames;
    s_scanDone = true;

    spdlog::info("BackgroundDownload: Scanned {} backgrounds", m_localFileNames.size());
}

void BackgroundDownloadPanel::setStatus(const QString& text)
{
    if (m_statusLabel) m_statusLabel->setText(text);
}

// ═════════════════════════════════════════════════════════════════════════════
// Search — filter grid items as user types
// ═════════════════════════════════════════════════════════════════════════════

void BackgroundDownloadPanel::onSearchChanged()
{
    const QString filter = m_searchEdit ? m_searchEdit->text().trimmed().toLower() : QString();

    for (int i = 0; i < m_backgroundGrid->count(); ++i) {
        QListWidgetItem* item = m_backgroundGrid->item(i);
        if (!item) continue;
        bool matches = filter.isEmpty() ||
                       item->text().toLower().contains(filter);
        item->setHidden(!matches);
    }

    // Count visible items
    int visible = 0;
    for (int i = 0; i < m_backgroundGrid->count(); ++i) {
        if (!m_backgroundGrid->item(i)->isHidden())
            visible++;
    }

    if (!filter.isEmpty())
        setStatus(QStringLiteral("%1 of %2 backgrounds").arg(visible).arg(m_localFileNames.size()));
    else
        setStatus(QStringLiteral("%1 background%2 in folder")
                  .arg(m_localFileNames.size())
                  .arg(m_localFileNames.size() == 1 ? QString() : QStringLiteral("s")));
}

} // namespace rt
