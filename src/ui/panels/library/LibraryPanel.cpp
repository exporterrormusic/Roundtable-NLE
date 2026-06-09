/*
 * LibraryPanel.cpp — tabbed reusable-asset browser implementation.
 */

#include "panels/library/LibraryPanel.h"
#include "panels/backgrounds/BackgroundDownloadPanel.h"
#include "panels/characters/CharactersPanel.h"
#include "panels/characters/PuppetLibraryPanel.h"
#include "widgets/MediaDragTreeWidget.h"
#include "Theme.h"
#include "Settings.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QProcess>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

namespace {

constexpr const char* kBackgroundsDir = "assets/backgrounds";
constexpr const char* kVideosDir      = "assets/videos";
constexpr const char* kAudioDir       = "assets/audio";

QStringList kImageFilters() {
    return {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif",
            "*.tif", "*.tiff", "*.webp", "*.tga", "*.dds"};
}
QStringList kVideoFilters() {
    return {"*.mp4", "*.mov", "*.mkv", "*.webm", "*.avi", "*.m4v"};
}
QStringList kAudioFilters() {
    return {"*.wav", "*.mp3", "*.flac", "*.ogg", "*.m4a", "*.aac", "*.opus"};
}

// Build a small toolbar (search field + view-mode toggle) above a tree.
struct FolderTabWidgets {
    QWidget*             container;
    QLineEdit*           search;
    MediaDragTreeWidget* tree;
    QToolButton*         btnDetail;
    QToolButton*         btnIcons;
};

FolderTabWidgets buildFolderTab(QWidget* parent, const QString& placeholder)
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    auto* container = new QWidget(parent);
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // Toolbar
    auto* toolbar = new QWidget(container);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* tlay = new QHBoxLayout(toolbar);
    tlay->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    tlay->setSpacing(m.spacingXs);

    auto* search = new QLineEdit(toolbar);
    search->setPlaceholderText(placeholder);
    search->setClearButtonEnabled(true);
    search->setFixedHeight(22);
    search->setStyleSheet(
        QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; padding: 2px 4px; font-size: 12px; }"
                "QLineEdit:focus { border: 1px solid %5; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent)));
    tlay->addWidget(search, 1);

    // View-mode toggle (Detail list / Icon thumbnails) — Premiere style.
    const QString tbStyle = QString(
        "QToolButton { background: transparent; color: %1; border: none; "
        "padding: 2px 4px; }"
        "QToolButton:hover { background: %2; }"
        "QToolButton:checked { background: %3; color: %4; }")
        .arg(Theme::hex(tc.textSecondary),
             Theme::hex(tc.controlBgHover),
             Theme::hex(tc.accent),
             Theme::hex(tc.text));

    auto* btnDetail = new QToolButton(toolbar);
    btnDetail->setText(QStringLiteral("\u2630")); // ☰ list glyph
    btnDetail->setToolTip(QObject::tr("List view (details)"));
    btnDetail->setCheckable(true);
    btnDetail->setAutoRaise(true);
    btnDetail->setFixedSize(22, 22);
    btnDetail->setStyleSheet(tbStyle);
    tlay->addWidget(btnDetail);

    auto* btnIcons = new QToolButton(toolbar);
    btnIcons->setText(QStringLiteral("\u25A6")); // ▦ icon-grid glyph
    btnIcons->setToolTip(QObject::tr("Icon view (thumbnails)"));
    btnIcons->setCheckable(true);
    btnIcons->setAutoRaise(true);
    btnIcons->setFixedSize(22, 22);
    btnIcons->setStyleSheet(tbStyle);
    tlay->addWidget(btnIcons);

    lay->addWidget(toolbar);

    // Tree
    auto* tree = new MediaDragTreeWidget;
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setIndentation(16);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setDragEnabled(true);
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setIconSize(QSize(16, 16));
    tree->setStyleSheet(
        QString("QTreeWidget { background: %1; color: %2; border: none; }"
                "QTreeWidget::item { padding: 2px 0; }"
                "QTreeWidget::item:hover { background: %3; }"
                "QTreeWidget::item:selected { background: %4; }")
            .arg(Theme::hex(tc.surface0), Theme::hex(tc.text),
                 Theme::hex(tc.controlBgHover),
                 Theme::hex(tc.accent)));
    lay->addWidget(tree, 1);

    return {container, search, tree, btnDetail, btnIcons};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

LibraryPanel::LibraryPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

void LibraryPanel::buildUI()
{
    const auto& tc = Theme::colors();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setStyleSheet(
        QString("QTabWidget::pane { border: none; background: %1; }"
                "QTabBar::tab { background: %2; color: %3; "
                "padding: 4px 12px; border: none; }"
                "QTabBar::tab:selected { background: %4; color: %5; }"
                "QTabBar::tab:hover { background: %6; }")
            .arg(Theme::hex(tc.surface0),
                 Theme::hex(tc.surface2),
                 Theme::hex(tc.textSecondary),
                 Theme::hex(tc.surface0),
                 Theme::hex(tc.text),
                 Theme::hex(tc.controlBgHover)));
    root->addWidget(m_tabs, 1);

    // Tab 1: Characters (delegates to existing Spine panel)
    m_characters = new CharactersPanel(this);
    m_tabs->addTab(m_characters, tr("Characters"));

    // Tab 2: Custom — drag-only library (created in CHARACTERS → CUSTOM).
    m_puppets = new PuppetLibraryPanel(this, /*readOnly=*/true);
    m_tabs->addTab(m_puppets, tr("Custom"));

    // Tab 2: Backgrounds
    {
        auto w = buildFolderTab(this, tr("\U0001F50D Search backgrounds…"));
        m_bgSearch = w.search;
        m_bgTree   = w.tree;
        m_tabs->addTab(w.container, tr("Backgrounds"));
        setupContextMenu(m_bgTree, tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.tga *.dds)"), kBackgroundsDir);
        setupDoubleClick(m_bgTree);
        connect(w.btnDetail, &QToolButton::clicked, this, [this]() { setIconMode(false); });
        connect(w.btnIcons,  &QToolButton::clicked, this, [this]() { setIconMode(true);  });
        w.btnDetail->setChecked(!m_iconMode);
        w.btnIcons->setChecked(m_iconMode);

        auto* debounce = new QTimer(this);
        debounce->setSingleShot(true);
        debounce->setInterval(200);
        connect(debounce, &QTimer::timeout, this, [this]() {
            refreshFolderTree(m_bgTree, kBackgroundsDir, kImageFilters(),
                              m_bgSearch ? m_bgSearch->text().trimmed().toLower() : QString());
        });
        connect(m_bgSearch, &QLineEdit::textChanged,
                this, [debounce]() { debounce->start(); });
    }

    // Tab 3: NikkeBKG (supports double-click + drag like the Backgrounds tab)
    {
        m_nikkeBgsPanel = new BackgroundDownloadPanel(this);
        // Double-click → load in Source Monitor (mirrors setupDoubleClick on folder tabs)
        connect(m_nikkeBgsPanel, &BackgroundDownloadPanel::backgroundActivated,
                this, [this](const QString& filePath) {
            emit loadInSourceMonitor(filePath);
        });
        m_tabs->addTab(m_nikkeBgsPanel, tr("NikkeBKG"));
    }

    // Tab 4: Videos
    {
        auto w = buildFolderTab(this, tr("\U0001F50D Search videos…"));
        m_videoSearch = w.search;
        m_videoTree   = w.tree;
        m_tabs->addTab(w.container, tr("Videos"));
        setupContextMenu(m_videoTree, tr("Videos (*.mp4 *.mov *.mkv *.webm *.avi *.m4v)"), kVideosDir);
        setupDoubleClick(m_videoTree);
        connect(w.btnDetail, &QToolButton::clicked, this, [this]() { setIconMode(false); });
        connect(w.btnIcons,  &QToolButton::clicked, this, [this]() { setIconMode(true);  });
        w.btnDetail->setChecked(!m_iconMode);
        w.btnIcons->setChecked(m_iconMode);

        auto* debounce = new QTimer(this);
        debounce->setSingleShot(true);
        debounce->setInterval(200);
        connect(debounce, &QTimer::timeout, this, [this]() {
            refreshFolderTree(m_videoTree, kVideosDir, kVideoFilters(),
                              m_videoSearch ? m_videoSearch->text().trimmed().toLower() : QString());
        });
        connect(m_videoSearch, &QLineEdit::textChanged,
                this, [debounce]() { debounce->start(); });
    }

    // Tab 5: Audio
    {
        auto w = buildFolderTab(this, tr("\U0001F50D Search audio…"));
        m_audioSearch = w.search;
        m_audioTree   = w.tree;
        m_tabs->addTab(w.container, tr("Audio"));
        setupContextMenu(m_audioTree, tr("Audio (*.wav *.mp3 *.flac *.ogg *.m4a *.aac *.opus)"), kAudioDir);
        setupDoubleClick(m_audioTree);
        connect(w.btnDetail, &QToolButton::clicked, this, [this]() { setIconMode(false); });
        connect(w.btnIcons,  &QToolButton::clicked, this, [this]() { setIconMode(true);  });
        w.btnDetail->setChecked(!m_iconMode);
        w.btnIcons->setChecked(m_iconMode);

        auto* debounce = new QTimer(this);
        debounce->setSingleShot(true);
        debounce->setInterval(200);
        connect(debounce, &QTimer::timeout, this, [this]() {
            refreshFolderTree(m_audioTree, kAudioDir, kAudioFilters(),
                              m_audioSearch ? m_audioSearch->text().trimmed().toLower() : QString());
        });
        connect(m_audioSearch, &QLineEdit::textChanged,
                this, [debounce]() { debounce->start(); });
    }

    // Refresh the current folder tab when it becomes visible. Deferred so the
    // tab visibly switches before the (synchronous) directory walk / view
    // rebuild runs on the next event-loop tick.
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        QTimer::singleShot(0, this, [this]() { refreshCurrentTab(); });
    });
}

// ─────────────────────────────────────────────────────────────────────────────

void LibraryPanel::setModelManager(ModelManager* mgr)
{
    if (m_characters) m_characters->setModelManager(mgr);
}

void LibraryPanel::setAnimVideoCache(AnimationVideoCache* cache)
{
    if (m_characters) m_characters->setAnimVideoCache(cache);
}

void LibraryPanel::setMediaPool(MediaPool* pool)
{
    if (m_characters) m_characters->setMediaPool(pool);
}

void LibraryPanel::refresh()
{
    if (m_characters) m_characters->refresh();
    if (m_puppets) m_puppets->refresh();

    refreshFolderTree(m_bgTree,    kBackgroundsDir, kImageFilters(),
                      m_bgSearch    ? m_bgSearch->text().trimmed().toLower()    : QString());
    refreshFolderTree(m_videoTree, kVideosDir,      kVideoFilters(),
                      m_videoSearch ? m_videoSearch->text().trimmed().toLower() : QString());
    refreshFolderTree(m_audioTree, kAudioDir,       kAudioFilters(),
                      m_audioSearch ? m_audioSearch->text().trimmed().toLower() : QString());
}

void LibraryPanel::refreshCurrentTab()
{
    if (!m_tabs) return;

    // Tab order: 0 Characters, 1 PNG Puppets, 2 Backgrounds, 3 NikkeBKG,
    //            4 Videos, 5 Audio.
    const int index = m_tabs->currentIndex();
    if (index == 0) {
        if (m_characters) m_characters->refresh();
        return;
    }
    if (index == 1) {
        if (m_puppets) m_puppets->refresh();
        return;
    }
    if (index == 2) {
        refreshFolderTree(m_bgTree, kBackgroundsDir, kImageFilters(),
                          m_bgSearch ? m_bgSearch->text().trimmed().toLower() : QString());
        return;
    }
    if (index == 3) {
        // NikkeBKG — auto-scans via QFileSystemWatcher, no manual refresh needed
        return;
    }
    if (index == 4) {
        refreshFolderTree(m_videoTree, kVideosDir, kVideoFilters(),
                          m_videoSearch ? m_videoSearch->text().trimmed().toLower() : QString());
        return;
    }
    if (index == 5) {
        refreshFolderTree(m_audioTree, kAudioDir, kAudioFilters(),
                          m_audioSearch ? m_audioSearch->text().trimmed().toLower() : QString());
    }
}

void LibraryPanel::setIconMode(bool iconMode)
{
    if (m_iconMode == iconMode) return;
    m_iconMode = iconMode;

    // Update all toggle buttons across tabs (find by traversing tab containers).
    if (m_tabs) {
        for (int i = 1; i < m_tabs->count(); ++i) {
            QWidget* page = m_tabs->widget(i);
            if (!page) continue;
            const auto buttons = page->findChildren<QToolButton*>();
            for (QToolButton* b : buttons) {
                if (b->toolTip() == tr("List view (details)"))
                    b->setChecked(!iconMode);
                else if (b->toolTip() == tr("Icon view (thumbnails)"))
                    b->setChecked(iconMode);
            }
        }
    }
    refreshCurrentTab();
}

void LibraryPanel::applyViewModeToTree(MediaDragTreeWidget* tree)
{
    if (!tree) return;
    if (m_iconMode) {
        tree->setIconSize(QSize(96, 96));
        tree->setUniformRowHeights(false);
    } else {
        tree->setIconSize(QSize(16, 16));
        tree->setUniformRowHeights(true);
    }
}

void LibraryPanel::refreshFolderTree(MediaDragTreeWidget* tree,
                                     const QString& dirPath,
                                     const QStringList& nameFilters,
                                     const QString& searchTerm)
{
    if (!tree) return;
    applyViewModeToTree(tree);
    tree->clear();

    QDir dir(dirPath);
    if (!dir.exists()) {
        // Auto-create the asset folder so users have a clear target to drop files into.
        QDir().mkpath(dirPath);
        dir.setPath(dirPath);
    }
    if (!dir.exists()) return;

    const auto& tc = Theme::colors();

    // Helper: build an icon for a file, honoring the current view mode.
    QFileIconProvider iconProvider;
    auto makeIcon = [&](const QFileInfo& fi) -> QIcon {
        if (m_iconMode) {
            // Try image thumbnail first; fall back to system file icon.
            const QByteArray ext = fi.suffix().toLower().toLatin1();
            const auto imgFmts = QImageReader::supportedImageFormats();
            if (imgFmts.contains(ext)) {
                QImageReader r(fi.absoluteFilePath());
                r.setAutoTransform(true);
                // Cap source decode to keep refresh snappy on huge images.
                QSize src = r.size();
                if (src.isValid()) {
                    QSize tgt = src.scaled(192, 192, Qt::KeepAspectRatio);
                    r.setScaledSize(tgt);
                }
                QImage img = r.read();
                if (!img.isNull())
                    return QIcon(QPixmap::fromImage(img));
            }
        }
        return iconProvider.icon(fi);
    };

    // Single-level scan (subfolders rendered as expandable parents).
    const QFileInfoList entries = dir.entryInfoList(nameFilters,
        QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo& fi : entries) {
        QString name = fi.fileName();
        if (!searchTerm.isEmpty() && !name.toLower().contains(searchTerm))
            continue;

        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, name);
        item->setIcon(0, makeIcon(fi));
        item->setForeground(0, tc.text);
        item->setData(0, Qt::UserRole, fi.absoluteFilePath());
        // No pre-opened media handle; timeline opens on drop if needed.
        item->setData(0, Qt::UserRole + 1, QVariant::fromValue<quint64>(0));
        item->setData(0, Qt::UserRole + 2, false); // not a folder/bin item
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    }

    // Subdirectories as expandable parents (one level deep)
    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                    QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& sd : subdirs) {
        QDir sub(sd.absoluteFilePath());
        const QFileInfoList subEntries = sub.entryInfoList(nameFilters,
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
        if (subEntries.isEmpty()) continue;

        auto* parent = new QTreeWidgetItem(tree);
        parent->setText(0, sd.fileName());
        parent->setForeground(0, tc.textSecondary);
        parent->setFlags(parent->flags() & ~Qt::ItemIsDragEnabled);

        for (const QFileInfo& fi : subEntries) {
            QString name = fi.fileName();
            if (!searchTerm.isEmpty() && !name.toLower().contains(searchTerm))
                continue;
            auto* item = new QTreeWidgetItem(parent);
            item->setText(0, name);
            item->setIcon(0, makeIcon(fi));
            item->setForeground(0, tc.text);
            item->setData(0, Qt::UserRole, fi.absoluteFilePath());
            item->setData(0, Qt::UserRole + 1, QVariant::fromValue<quint64>(0));
            item->setData(0, Qt::UserRole + 2, false);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
        }

        if (parent->childCount() == 0) {
            delete parent;
        }
    }
}

void LibraryPanel::setupDoubleClick(MediaDragTreeWidget* tree)
{
    if (!tree) return;
    connect(tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem* item, int /*col*/) {
        if (!item) return;
        const QString path = item->data(0, Qt::UserRole).toString();
        if (path.isEmpty()) return;
        QFileInfo fi(path);
        if (!fi.isFile()) return;
        emit loadInSourceMonitor(path);
    });
}

void LibraryPanel::setupContextMenu(MediaDragTreeWidget* tree, const QString& fileFilter, const QString& importDir)
{
    if (!tree) return;
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QWidget::customContextMenuRequested,
            this, [this, tree, fileFilter, importDir](const QPoint& pos) {
        QTreeWidgetItem* item = tree->itemAt(pos);

        QMenu menu(tree);

        // ── Right-click on empty space: Import a new asset ──────────────
        if (!item) {
            QAction* importAct = menu.addAction(tr("Import to %1…")
                .arg(QDir(importDir).dirName()));
            QAction* chosen = menu.exec(tree->viewport()->mapToGlobal(pos));
            if (!chosen) return;

            if (chosen == importAct) {
                auto settings = rt::appSettings();
                QString lastDir = settings.value("Import/lastDir", QString()).toString();
                if (lastDir.isEmpty())
                    lastDir = QDir::homePath();
                QStringList files = QFileDialog::getOpenFileNames(
                    tree, tr("Import to %1").arg(importDir),
                    lastDir, fileFilter);
                if (files.isEmpty()) return;
                QString dir = QFileInfo(files.first()).absolutePath();
                settings.setValue("Import/lastDir", dir);
                settings.sync();
                // Ensure the target directory exists
                QDir().mkpath(importDir);
                for (const QString& srcPath : files) {
                    QFileInfo fi(srcPath);
                    QString dstPath = importDir + QStringLiteral("/") + fi.fileName();
                    if (QFile::exists(dstPath)) {
                        auto reply = QMessageBox::question(
                            tree, tr("File Exists"),
                            tr("\"%1\" already exists in %2.\nOverwrite?")
                                .arg(fi.fileName(), importDir),
                            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                            QMessageBox::No);
                        if (reply == QMessageBox::Cancel)
                            break;
                        if (reply == QMessageBox::No)
                            continue;
                        QFile::remove(dstPath);
                    }
                    if (!QFile::copy(srcPath, dstPath)) {
                        QMessageBox::warning(tree, tr("Import Failed"),
                            tr("Failed to copy \"%1\" to %2.")
                                .arg(fi.fileName(), importDir));
                    }
                }
                // Refresh this tab to show the newly imported file
                refresh();
                emit mediaImported(importDir);
            }
            return;
        }

        // ── Right-click on an existing item ─────────────────────────────
        // Folder header items have UserRole+2 == false but no usable path; skip.
        const QString oldPath = item->data(0, Qt::UserRole).toString();
        if (oldPath.isEmpty()) return;

        QAction* relinkAct = menu.addAction(tr("Re-link…"));
        QAction* showAct   = menu.addAction(tr("Show in Explorer"));
        menu.addSeparator();
        QAction* deleteAct = menu.addAction(tr("Delete"));
        QAction* chosen = menu.exec(tree->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == deleteAct) {
            auto reply = QMessageBox::question(tree, tr("Delete"),
                tr("Permanently delete \"%1\"?\nThis cannot be undone.")
                    .arg(QFileInfo(oldPath).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            QFile::remove(oldPath);
            refresh();
            return;
        }

        if (chosen == relinkAct) {
            const QFileInfo oldFi(oldPath);
            const QString startDir = oldFi.exists() ? oldFi.absolutePath()
                                                    : QDir::homePath();
            const QString exactFilter = tr("Exact name match (%1)").arg(oldFi.fileName())
                + QStringLiteral(";;") + fileFilter
                + QStringLiteral(";;") + tr("All Files (*.*)");

            QFileDialog dlg(this, tr("Re-link: %1").arg(oldFi.fileName()), startDir);
            dlg.setNameFilter(exactFilter);
            dlg.setFileMode(QFileDialog::ExistingFile);
            dlg.selectNameFilter(tr("Exact name match (%1)").arg(oldFi.fileName()));
            dlg.selectFile(oldFi.fileName());
            if (dlg.exec() != QDialog::Accepted) return;
            const QStringList sel = dlg.selectedFiles();
            if (sel.isEmpty()) return;
            const QString newPath = sel.first();
            if (newPath.isEmpty() || newPath == oldPath) return;

            emit mediaRelinkRequested(oldPath, newPath);
            refresh();
        } else if (chosen == showAct) {
            QFileInfo fi(oldPath);
            if (fi.exists()) {
#ifdef _WIN32
                QProcess::startDetached("explorer.exe",
                    {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
            }
        }
    });
}

} // namespace rt
