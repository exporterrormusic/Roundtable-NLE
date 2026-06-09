/*
 * ShotComposerUILeftPanel.cpp - createLeftPanel() UI builder + the
 * DragAssetList helper (its only user).
 * Extracted from ShotComposerUI.cpp for file size (behavior-preserving).
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"
#include "panels/backgrounds/BackgroundDownloadPanel.h"

#include "Theme.h"
#include "Settings.h"
#include "timeline/MediaRelinker.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

namespace rt {

// ─── Draggable library list ─────────────────────────────────────────────────
// Subclass QListWidget so that drags from the asset library carry a custom MIME
// type ("application/x-roundtable-asset") with the asset kind + identifying data.
// Drop handling lives in ShotComposer::eventFilter().

static constexpr const char* kAssetMime = "application/x-roundtable-asset";

class DragAssetList : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        if (items.isEmpty()) return nullptr;
        auto* item = items.first();
        // Skip group-header items (non-draggable separators)
        if (item && !(item->flags() & Qt::ItemIsDragEnabled))
            return nullptr;

        QByteArray payload;
        QDataStream ds(&payload, QIODevice::WriteOnly);

        // Determine asset kind from item data
        QString kind = item->data(Qt::UserRole).toString();
        if (kind == QStringLiteral("video")) {
            // Video character in Characters tab
            ds << QStringLiteral("videoChar")
               << item->text()
               << item->data(Qt::UserRole + 1).toString()   // mute path
               << item->data(Qt::UserRole + 2).toString();   // talk path
        } else if (kind == QStringLiteral("videoChar")) {
            // Video character in Videos tab
            ds << QStringLiteral("videoChar")
               << item->data(Qt::UserRole + 1).toString()   // char name
               << item->data(Qt::UserRole + 2).toString()   // mute path
               << item->data(Qt::UserRole + 3).toString();   // talk path
        } else if (kind == QStringLiteral("puppet")) {
            // PNG puppet in the Puppets tab
            ds << QStringLiteral("puppet")
               << item->data(Qt::UserRole + 1).toString()    // folder name
               << item->data(Qt::UserRole + 2).toString();   // variant
        } else if (kind == QStringLiteral("video_file")) {
            // Standalone video file
            ds << QStringLiteral("video") << item->text();
        } else if (kind.isEmpty()) {
            // Background — text is the display name (baseName without extension).
            // For subfolder backgrounds, UserRole+2 holds the relative path
            // (e.g. "Nikke In-Game Backgrounds/bg_chapter_01.png") so pass that
            // instead of the display name for correct file resolution.
            QString relPath = item->data(Qt::UserRole + 2).toString();
            if (relPath.isEmpty()) {
                ds << QStringLiteral("background") << item->text();
            } else {
                ds << QStringLiteral("background") << relPath;
            }
        } else {
            // Regular character — UserRole is the folder name
            ds << QStringLiteral("character") << kind;
        }

        auto* md = new QMimeData;
        md->setData(QString::fromLatin1(kAssetMime), payload);
        return md;
    }
};

QWidget* ShotComposer::createLeftPanel()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* leftSplitter = new QSplitter(Qt::Vertical);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // TOP: Shot Header + Preview Viewport
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* topSection = new QWidget;
    topSection->setObjectName("ShotTopSection");
    topSection->setStyleSheet(QStringLiteral(
        "QWidget#ShotTopSection { background: %1; }")
        .arg(Theme::hex(c.surface0)));
    auto* topLayout = new QVBoxLayout(topSection);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(0);

    // â”€â”€ Shot Header Bar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* shotHeader = new QWidget;
    shotHeader->setObjectName("PreviewHeader");
    auto* shotHeaderLayout = new QHBoxLayout(shotHeader);
    shotHeaderLayout->setContentsMargins(m.spacingMd, m.spacingXs, m.spacingMd, m.spacingXs);
    shotHeaderLayout->setSpacing(m.spacingSm);

    auto* shotIcon = new QLabel(QStringLiteral("\xF0\x9F\x8E\xAC"));
    shotIcon->setFixedWidth(24);
    shotHeaderLayout->addWidget(shotIcon);

    auto* shotLabel = new QLabel("COMPOSE");
    shotLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1;")
        .arg(Theme::hex(c.textSecondary)));
    shotHeaderLayout->addWidget(shotLabel);

    shotHeaderLayout->addStretch();

    topLayout->addWidget(shotHeader);

    // â”€â”€ Preview Viewport â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    m_previewArea = new QWidget;
    m_previewArea->setObjectName("PreviewArea");

    auto* arContainer = new AspectRatioContainer(m_previewArea, 16.0 / 9.0);

    auto* previewInner = new QVBoxLayout(m_previewArea);
    previewInner->setContentsMargins(0, 0, 0, 0);
    previewInner->setSpacing(2);

#ifdef ROUNDTABLE_HAS_SPINE
    m_spinePreview = new SpinePreviewWidget(m_previewArea);
    m_spinePreview->setBackgroundColor(c.surface0);
    previewInner->addWidget(m_spinePreview);

    connect(m_spinePreview, &SpinePreviewWidget::dragStarted,
            this, &ShotComposer::pushUndoState);

    // When the user clicks a layer directly in the preview, sync the layer
    // list selection so spinboxes show the right layer's properties and
    // m_selectedLayer stays consistent.
    connect(m_spinePreview, &SpinePreviewWidget::layerClicked,
            this, [this](int layerIndex) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (layerIndex < 0 || layerIndex >= m_layerList->count()) return;
        if (layerIndex == m_selectedLayer) return; // already synced
        m_layerList->blockSignals(true);
        m_layerList->setCurrentRow(layerIndex);
        m_layerList->blockSignals(false);
        m_selectedLayer = layerIndex;
        populateLayerProperties();
    });

    connect(m_spinePreview, &SpinePreviewWidget::layerTransformChanged,
            this, [this](int layerIndex, float posX, float posY, float scale) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_updating) return;
        if (layerIndex < 0 || layerIndex >= m_currentShot.layerCount()) return;
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(layerIndex)];

        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) return;
            ch->posX  = posX;
            ch->posY  = posY;
            ch->scale = scale;
            if (layerIndex == m_selectedLayer) {
                m_updating = true;
                m_posXSpin->setValue(static_cast<double>(posX * 100.0));
                m_posYSpin->setValue(static_cast<double>(posY * 100.0));
                m_scaleSpin->setValue(static_cast<double>(scale * 100.0));
                m_updating = false;
            }
        } else if (ref.type == LayerType::Background) {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) return;
            bg->posX  = posX;
            bg->posY  = posY;
            bg->scale = scale;
            if (layerIndex == m_selectedLayer) {
                m_updating = true;
                m_bgPosXSpin->setValue(static_cast<double>(posX * 100.0));
                m_bgPosYSpin->setValue(static_cast<double>(posY * 100.0));
                m_bgScaleSpin->setValue(static_cast<double>(scale * 100.0));
                m_updating = false;
            }
        }

        emit shotChanged();
    });

    connect(m_spinePreview, &SpinePreviewWidget::layerCropChanged,
            this, [this](int layerIndex, float cropL, float cropR, float cropT, float cropB) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_updating) return;
        if (layerIndex < 0 || layerIndex >= m_currentShot.layerCount()) return;
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(layerIndex)];

        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) return;
            ch->cropLeft   = cropL;
            ch->cropRight  = cropR;
            ch->cropTop    = cropT;
            ch->cropBottom = cropB;
            if (layerIndex == m_selectedLayer && m_cropLeftSpin) {
                m_updating = true;
                m_cropLeftSpin->setValue(static_cast<double>(cropL));
                m_cropRightSpin->setValue(static_cast<double>(cropR));
                m_cropTopSpin->setValue(static_cast<double>(cropT));
                m_cropBottomSpin->setValue(static_cast<double>(cropB));
                m_updating = false;
            }
        } else if (ref.type == LayerType::Background) {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) return;
            bg->cropLeft   = cropL;
            bg->cropRight  = cropR;
            bg->cropTop    = cropT;
            bg->cropBottom = cropB;
        }
        emit shotChanged();
    });
#else
    auto* label = new QLabel(QStringLiteral("\xF0\x9F\x8E\xAC Shot Preview\n(Spine not available)"));
    label->setAlignment(Qt::AlignCenter);
    label->setObjectName("PlaceholderLabel");
    previewInner->addWidget(label);
#endif

    topLayout->addWidget(arContainer, 1);

    // â”€â”€ Preview Toolbar â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* previewToolbar = new QWidget;
    previewToolbar->setObjectName("PreviewToolbar");
    auto* tbLayout = new QHBoxLayout(previewToolbar);
    tbLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    tbLayout->setSpacing(m.spacingSm);

    auto* resetViewBtn = new QPushButton("Reset View");
    resetViewBtn->setObjectName("ResetViewBtn");
    resetViewBtn->setToolTip("Reset viewport zoom and pan to 1:1 (Home)");
    tbLayout->addWidget(resetViewBtn);

#ifdef ROUNDTABLE_HAS_SPINE
    connect(resetViewBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_spinePreview) {
            m_spinePreview->resetViewport();
            m_updating = true;
            m_cameraZoomSpin->setValue(100.0);
            m_cameraPanXSpin->setValue(0.0);
            m_cameraPanYSpin->setValue(0.0);
            m_updating = false;
            m_currentShot.setCameraZoom(1.0f);
            m_currentShot.setCameraX(0.0f);
            m_currentShot.setCameraY(0.0f);
            emit shotChanged();
        }
    });
#endif

    tbLayout->addStretch();

    auto* safeAreasBtn = new QPushButton(QStringLiteral("Safe Margins"));
    safeAreasBtn->setCheckable(true);
    safeAreasBtn->setChecked(false);
    safeAreasBtn->setFixedHeight(22);
    safeAreasBtn->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid %3; "
                       "border-radius: 3px; padding: 0 8px; font-size: 12px; }"
                       "QPushButton:checked { background: %4; color: white; }")
        .arg(Theme::hex(c.surface1),
             Theme::hex(c.textSecondary),
             Theme::hex(c.border),
             Theme::hex(c.accent)));
    tbLayout->addWidget(safeAreasBtn);
#ifdef ROUNDTABLE_HAS_SPINE
    connect(safeAreasBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (m_spinePreview) m_spinePreview->setSafeAreasVisible(on);
    });
#endif

    topLayout->addWidget(previewToolbar);

    leftSplitter->addWidget(topSection);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // BOTTOM: Asset Library Tabs
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    auto* librarySection = new QWidget;
    auto* libraryLayout = new QVBoxLayout(librarySection);
    libraryLayout->setContentsMargins(m.spacingSm, 0, m.spacingSm, m.spacingSm);
    libraryLayout->setSpacing(m.spacingXs);

    // â”€â”€ Zoom slider row (controls thumbnail size) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* zoomRow = new QHBoxLayout;
    zoomRow->setSpacing(4);

    auto* zoomMinus = new QLabel(QStringLiteral("\u2212"));
    zoomMinus->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(Theme::hex(c.textTertiary)));
    zoomRow->addWidget(zoomMinus);

    m_iconZoomSlider = new QSlider(Qt::Horizontal);
    m_iconZoomSlider->setRange(80, 280);
    m_iconZoomSlider->setValue(m_iconSize);
    m_iconZoomSlider->setFixedWidth(100);
    m_iconZoomSlider->setToolTip("Thumbnail Size");
    m_iconZoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 10px; margin: -3px 0; border-radius: 5px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.textSecondary))
        .arg(Theme::hex(c.textPrimary)));
    zoomRow->addWidget(m_iconZoomSlider);

    auto* zoomPlus = new QLabel(QStringLiteral("+"));
    zoomPlus->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;")
        .arg(Theme::hex(c.textTertiary)));
    zoomRow->addWidget(zoomPlus);
    zoomRow->addStretch();

    libraryLayout->addLayout(zoomRow);

    // Helper: configure a QListWidget for icon-mode thumbnail display
    auto setupIconList = [&](QListWidget* list) {
        list->setObjectName("LibraryList");
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setViewMode(QListView::IconMode);
        list->setIconSize(QSize(m_iconSize, m_iconSize));
        list->setGridSize(QSize(m_iconSize + 8, m_iconSize + 22));
        list->setResizeMode(QListView::Adjust);
        list->setWordWrap(true);
        list->setSpacing(2);
        list->setUniformItemSizes(false);
        list->setMovement(QListView::Static);
        list->setWrapping(true);
        list->setFlow(QListView::LeftToRight);
        list->setStyleSheet(QStringLiteral(
            "QListWidget { background: %1; border: none; outline: none; }"
            "QListWidget::item { background: %2; border: 1px solid transparent;"
            "  border-radius: 4px; padding: 2px; }"
            "QListWidget::item:selected { border-color: %3; background: %4; }"
            "QListWidget::item:hover { background: %5; }")
            .arg(Theme::hex(c.surface0))
            .arg(Theme::hex(c.surface1))
            .arg(Theme::hex(c.accent))
            .arg(Theme::hex(c.accentDim))
            .arg(Theme::hex(c.surface2)));
    };

    m_libraryTabs = new QTabWidget;

    // â”€â”€ Characters tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* charTab = new QWidget;
    auto* charLayout = new QVBoxLayout(charTab);
    charLayout->setContentsMargins(5, 5, 5, 5);

    m_charSearchEdit = new QLineEdit;
    m_charSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search characters..."));
    charLayout->addWidget(m_charSearchEdit);

    auto* filterRow = new QHBoxLayout;
    m_namedOnlyCheck = new QCheckBox("Named Only");
    m_namedOnlyCheck->setChecked(true);
    filterRow->addWidget(m_namedOnlyCheck);
    filterRow->addStretch();
    charLayout->addLayout(filterRow);

    m_characterLibrary = new DragAssetList;
    setupIconList(m_characterLibrary);
    m_characterLibrary->setDragEnabled(true);
    m_characterLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    m_characterLibrary->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_characterLibrary, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto* item = m_characterLibrary->itemAt(pos);
        if (!item) return;
        const QString itemType = item->data(Qt::UserRole).toString();

        QMenu menu(m_characterLibrary);

        if (itemType == QStringLiteral("video")) {
            // ── Video character ────────────────────────────────────────
            const QString mutePath  = item->data(Qt::UserRole + 1).toString();
            const QString talkPath  = item->data(Qt::UserRole + 2).toString();

            QAction* showAct = menu.addAction(tr("Show in Explorer"));
            menu.addSeparator();
            QAction* deleteAct = menu.addAction(tr("Delete"));
            QAction* chosen = menu.exec(m_characterLibrary->viewport()->mapToGlobal(pos));
            if (!chosen) return;

            if (chosen == showAct) {
                QString dir = !mutePath.isEmpty() ? QFileInfo(mutePath).absolutePath()
                                                  : QFileInfo(talkPath).absolutePath();
#ifdef _WIN32
                QProcess::startDetached("explorer.exe",
                    {QDir::toNativeSeparators(dir)});
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
            } else if (chosen == deleteAct) {
                auto reply = QMessageBox::question(m_characterLibrary, tr("Delete"),
                    tr("Permanently delete this video character's files?\nThis cannot be undone."),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply != QMessageBox::Yes) return;
                if (!mutePath.isEmpty()) QFile::remove(mutePath);
                if (!talkPath.isEmpty()) QFile::remove(talkPath);
                refreshCharacterLibrary();
            }
        } else {
            // ── Spine character ────────────────────────────────────────
            const QString charFolder = item->data(Qt::UserRole).toString();
            const QString charDir = QStringLiteral("assets/characters/") + charFolder;

            QAction* showAct = menu.addAction(tr("Show in Explorer"));
            menu.addSeparator();
            QAction* deleteAct = menu.addAction(tr("Delete"));
            QAction* chosen = menu.exec(m_characterLibrary->viewport()->mapToGlobal(pos));
            if (!chosen) return;

            if (chosen == showAct) {
                if (QDir(charDir).exists()) {
#ifdef _WIN32
                    QProcess::startDetached("explorer.exe",
                        {QDir::toNativeSeparators(QDir(charDir).absolutePath())});
#else
                    QDesktopServices::openUrl(QUrl::fromLocalFile(
                        QDir(charDir).absolutePath()));
#endif
                }
            } else if (chosen == deleteAct) {
                auto reply = QMessageBox::question(m_characterLibrary, tr("Delete"),
                    tr("Permanently delete character \"%1\" and all its files?\nThis cannot be undone.")
                        .arg(charFolder),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (reply != QMessageBox::Yes) return;
                // Recursively remove the character directory
                QDir dir(charDir);
                if (dir.exists()) {
                    dir.removeRecursively();
                }
                // Rescan the model manager so the character disappears
                if (m_modelManager) {
                    m_modelManager->scan("assets");
                }
                refreshCharacterLibrary();
            }
        }
    });
    charLayout->addWidget(m_characterLibrary, 1);

    auto* btnAddChar = new QPushButton(QStringLiteral("Add to Shot \xE2\x86\x92"));
    charLayout->addWidget(btnAddChar);

    m_libraryTabs->addTab(charTab, "Characters");

    // ── Puppets tab (between Characters and Backgrounds) ─────────────────
    // PNG puppets are used in COMPOSE just like Spine characters: drag (or
    // double-click / "Add to Shot") to place them on the canvas. Creation /
    // editing of puppets lives in the CHARACTERS-page "PUPPETS" sub-panel, so
    // this tab is drag/select only (no import toolbar here).
    auto* puppetTab = new QWidget;
    auto* puppetLayout = new QVBoxLayout(puppetTab);
    puppetLayout->setContentsMargins(5, 5, 5, 5);

    m_puppetSearchEdit = new QLineEdit;
    m_puppetSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search puppets..."));
    m_puppetSearchEdit->setClearButtonEnabled(true);
    puppetLayout->addWidget(m_puppetSearchEdit);

    m_puppetLibrary = new DragAssetList;
    setupIconList(m_puppetLibrary);
    m_puppetLibrary->setDragEnabled(true);
    m_puppetLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    puppetLayout->addWidget(m_puppetLibrary, 1);

    auto* btnAddPuppet = new QPushButton(QStringLiteral("Add to Shot \xE2\x86\x92"));
    puppetLayout->addWidget(btnAddPuppet);

    m_libraryTabs->addTab(puppetTab, "Custom");

    // â”€â”€ Backgrounds tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* bgTab = new QWidget;
    auto* bgLayout = new QVBoxLayout(bgTab);
    bgLayout->setContentsMargins(5, 5, 5, 5);

    m_bgSearchEdit = new QLineEdit;
    m_bgSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search backgrounds..."));
    m_bgSearchEdit->setClearButtonEnabled(true);
    bgLayout->addWidget(m_bgSearchEdit);
    connect(m_bgSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshBackgroundLibrary(); });

    m_backgroundLibrary = new DragAssetList;
    setupIconList(m_backgroundLibrary);
    m_backgroundLibrary->setDragEnabled(true);
    m_backgroundLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    // Accept image files dropped from the OS (Explorer) to import them.
    // Handled in ShotComposer::eventFilter (Drop on the list's viewport).
    m_backgroundLibrary->setAcceptDrops(true);
    m_backgroundLibrary->viewport()->setAcceptDrops(true);
    m_backgroundLibrary->installEventFilter(this);
    m_backgroundLibrary->viewport()->installEventFilter(this);
    m_backgroundLibrary->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_backgroundLibrary, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto* item = m_backgroundLibrary->itemAt(pos);

        QMenu menu(m_backgroundLibrary);

        // ── Right-click on empty space: Import background ───────────────
        if (!item) {
            QAction* importAct = menu.addAction(tr("Import Background…"));
            QAction* chosen = menu.exec(m_backgroundLibrary->viewport()->mapToGlobal(pos));
            if (!chosen || chosen != importAct) return;

            auto settings = rt::appSettings();
            QString lastDir = settings.value("Import/lastDir", QString()).toString();
            if (lastDir.isEmpty())
                lastDir = QDir::homePath();
            QStringList files = QFileDialog::getOpenFileNames(
                m_backgroundLibrary, tr("Import Background"),
                lastDir,
                tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
            if (files.isEmpty()) return;
            QString dir = QFileInfo(files.first()).absolutePath();
            settings.setValue("Import/lastDir", dir);
            settings.sync();
            QDir().mkpath(QStringLiteral("assets/backgrounds"));
            for (const QString& srcPath : files) {
                QFileInfo fi(srcPath);
                QString dstPath = QStringLiteral("assets/backgrounds/") + fi.fileName();
                if (QFile::exists(dstPath)) {
                    auto reply = QMessageBox::question(
                        m_backgroundLibrary, tr("File Exists"),
                        tr("\"%1\" already exists in backgrounds.\nOverwrite?")
                            .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                        QMessageBox::No);
                    if (reply == QMessageBox::Cancel) break;
                    if (reply == QMessageBox::No) continue;
                    QFile::remove(dstPath);
                }
                if (!QFile::copy(srcPath, dstPath)) {
                    QMessageBox::warning(m_backgroundLibrary, tr("Import Failed"),
                        tr("Failed to copy \"%1\".").arg(fi.fileName()));
                }
            }
            refreshBackgroundLibrary();
            return;
        }

        // ── Right-click on an existing item ─────────────────────────────
        const QString oldPath = item->data(Qt::UserRole + 1).toString();
        if (oldPath.isEmpty()) return;
        QAction* relinkAct = menu.addAction(tr("Re-link…"));
        QAction* showAct   = menu.addAction(tr("Show in Explorer"));
        menu.addSeparator();
        QAction* deleteAct = menu.addAction(tr("Delete"));
        QAction* chosen = menu.exec(m_backgroundLibrary->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == deleteAct) {
            auto reply = QMessageBox::question(m_backgroundLibrary, tr("Delete"),
                tr("Permanently delete \"%1\"?\nThis cannot be undone.")
                    .arg(QFileInfo(oldPath).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            QFile::remove(oldPath);
            refreshBackgroundLibrary();
            return;
        }

        if (chosen == showAct) {
            QFileInfo fi(oldPath);
            if (fi.exists()) {
#ifdef _WIN32
                QProcess::startDetached("explorer.exe",
                    {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
            }
            return;
        }

        if (chosen != relinkAct) return;
        const QFileInfo oldFi(oldPath);
        const QString startDir = oldFi.exists() ? oldFi.absolutePath() : QDir::homePath();
        // Premiere-style "Display Only Exact Name Matches" — default filter
        // shows only files whose name matches the offline asset.
        const QString exactLabel = tr("Exact name match (%1)").arg(oldFi.fileName());
        const QString filters = exactLabel
            + QStringLiteral(";;") + tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)")
            + QStringLiteral(";;") + tr("All Files (*.*)");
        QFileDialog dlg(this, tr("Re-link: %1").arg(oldFi.fileName()), startDir);
        dlg.setNameFilter(filters);
        dlg.setFileMode(QFileDialog::ExistingFile);
        dlg.selectNameFilter(exactLabel);
        dlg.selectFile(oldFi.fileName());
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        const QString newPath = sel.first();
        if (newPath.isEmpty() || newPath == oldPath) return;
        // Update this composer's own preset library on disk.
        const int n = MediaRelinker::relinkPresetBackground(
            &m_presetManager, oldPath.toStdString(), newPath.toStdString());
        // Also patch the in-memory current shot if it matches.
        for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
            if (auto* bg = m_currentShot.background(i)) {
                if (bg->path == oldPath.toStdString())
                    bg->path = newPath.toStdString();
            }
        }
        // Let the host wire up timeline-side relinking.
        emit mediaRelinkRequested(oldPath, newPath);
        if (n > 0) emit shotChanged();
        refreshBackgroundLibrary();
    });
    bgLayout->addWidget(m_backgroundLibrary, 1);

    auto* btnAddBg = new QPushButton("Add Background");
    btnAddBg->setToolTip(QStringLiteral(
        "Add the selected background to the shot. To import new images, drag "
        "them in or right-click → Import Background…"));
    bgLayout->addWidget(btnAddBg);

    m_libraryTabs->addTab(bgTab, "Backgrounds");
    // ---- Nikke BGs tab (supports double-click and drag just like Backgrounds tab) ----
    {
        auto* nikkeBgPanel = new BackgroundDownloadPanel(this);
        connect(nikkeBgPanel, &BackgroundDownloadPanel::backgroundsDownloaded,
                this, &ShotComposer::refreshBackgroundLibrary);
        // Double-click → add background to shot with full file path
        connect(nikkeBgPanel, &BackgroundDownloadPanel::backgroundActivated,
                this, [this](const QString& filePath) {
            addBackground(filePath.toStdString());
        });
        // Ensure drag from the NikkeBKG grid also creates proper MIME data
        // for the ShotComposer's eventFilter (which handles asset drops on
        // the layer list and preview). The BackgroundGridWidget already
        // provides application/x-roundtable-asset MIME in its mimeData().
        nikkeBgPanel->backgroundGrid()->setDragEnabled(true);
        nikkeBgPanel->backgroundGrid()->setDragDropMode(QAbstractItemView::DragOnly);
        m_libraryTabs->addTab(nikkeBgPanel, "NikkeBKG");
    }


    // â”€â”€ Videos tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* videoTab = new QWidget;
    auto* videoLayout = new QVBoxLayout(videoTab);
    videoLayout->setContentsMargins(5, 5, 5, 5);

    m_videoSearchEdit = new QLineEdit;
    m_videoSearchEdit->setPlaceholderText(QStringLiteral("\xF0\x9F\x94\x8D Search videos..."));
    m_videoSearchEdit->setClearButtonEnabled(true);
    videoLayout->addWidget(m_videoSearchEdit);
    connect(m_videoSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshVideoLibrary(); });

    m_videoLibrary = new DragAssetList;
    setupIconList(m_videoLibrary);
    m_videoLibrary->setDragEnabled(true);
    m_videoLibrary->setDragDropMode(QAbstractItemView::DragOnly);
    m_videoLibrary->setMouseTracking(true);  // for hover-scrub
    m_videoLibrary->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_videoLibrary, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto* item = m_videoLibrary->itemAt(pos);

        QMenu menu(m_videoLibrary);

        // ── Right-click on empty space: Import video ────────────────────
        if (!item) {
            QAction* importAct = menu.addAction(tr("Import Video…"));
            QAction* chosen = menu.exec(m_videoLibrary->viewport()->mapToGlobal(pos));
            if (!chosen || chosen != importAct) return;

            auto settings = rt::appSettings();
            QString lastDir = settings.value("Import/lastDir", QString()).toString();
            if (lastDir.isEmpty())
                lastDir = QDir::homePath();
            QStringList files = QFileDialog::getOpenFileNames(
                m_videoLibrary, tr("Import Video"),
                lastDir,
                tr("Videos (*.mp4 *.avi *.mov *.mkv *.webm *.wmv)"));
            if (files.isEmpty()) return;
            QString dir = QFileInfo(files.first()).absolutePath();
            settings.setValue("Import/lastDir", dir);
            settings.sync();
            QDir().mkpath(QStringLiteral("assets/videos"));
            for (const QString& srcPath : files) {
                QFileInfo fi(srcPath);
                QString dstPath = QStringLiteral("assets/videos/") + fi.fileName();
                if (QFile::exists(dstPath)) {
                    auto reply = QMessageBox::question(
                        m_videoLibrary, tr("File Exists"),
                        tr("\"%1\" already exists in videos.\nOverwrite?")
                            .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                        QMessageBox::No);
                    if (reply == QMessageBox::Cancel) break;
                    if (reply == QMessageBox::No) continue;
                    QFile::remove(dstPath);
                }
                if (!QFile::copy(srcPath, dstPath)) {
                    QMessageBox::warning(m_videoLibrary, tr("Import Failed"),
                        tr("Failed to copy \"%1\".").arg(fi.fileName()));
                }
            }
            refreshVideoLibrary();
            return;
        }

        // ── Right-click on an existing video item ───────────────────────
        const QString itemType = item->data(Qt::UserRole).toString();
        // Only show Re-link for regular video files, not video characters
        QString oldPath;
        if (itemType == QStringLiteral("video_file"))
            oldPath = item->data(Qt::UserRole + 1).toString();
        if (oldPath.isEmpty()) return;
        QAction* relinkAct = menu.addAction(tr("Re-link…"));
        QAction* showAct   = menu.addAction(tr("Show in Explorer"));
        menu.addSeparator();
        QAction* deleteAct = menu.addAction(tr("Delete"));
        QAction* chosen = menu.exec(m_videoLibrary->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == deleteAct) {
            auto reply = QMessageBox::question(m_videoLibrary, tr("Delete"),
                tr("Permanently delete \"%1\"?\nThis cannot be undone.")
                    .arg(QFileInfo(oldPath).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            QFile::remove(oldPath);
            refreshVideoLibrary();
            return;
        }

        if (chosen == showAct) {
            QFileInfo fi(oldPath);
            if (fi.exists()) {
#ifdef _WIN32
                QProcess::startDetached("explorer.exe",
                    {"/select,", QDir::toNativeSeparators(fi.absoluteFilePath())});
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
            }
            return;
        }

        if (chosen != relinkAct) return;
        const QFileInfo oldFi(oldPath);
        const QString startDir = oldFi.exists() ? oldFi.absolutePath() : QDir::homePath();
        const QString exactLabel = tr("Exact name match (%1)").arg(oldFi.fileName());
        const QString filters = exactLabel
            + QStringLiteral(";;") + tr("Videos (*.mp4 *.avi *.mov *.mkv *.webm *.wmv)")
            + QStringLiteral(";;") + tr("All Files (*.*)");
        QFileDialog dlg(this, tr("Re-link: %1").arg(oldFi.fileName()), startDir);
        dlg.setNameFilter(filters);
        dlg.setFileMode(QFileDialog::ExistingFile);
        dlg.selectNameFilter(exactLabel);
        dlg.selectFile(oldFi.fileName());
        if (dlg.exec() != QDialog::Accepted) return;
        const QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        const QString newPath = sel.first();
        if (newPath.isEmpty() || newPath == oldPath) return;
        emit mediaRelinkRequested(oldPath, newPath);
        refreshVideoLibrary();
    });
    videoLayout->addWidget(m_videoLibrary, 1);

    auto* btnAddVideo = new QPushButton("Add Video");
    videoLayout->addWidget(btnAddVideo);

    m_libraryTabs->addTab(videoTab, "Videos");

    libraryLayout->addWidget(m_libraryTabs, 1);

    // â”€â”€ Zoom slider â†’ icon size â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    connect(m_iconZoomSlider, &QSlider::valueChanged,
            this, &ShotComposer::setLibraryIconSize);

    leftSplitter->addWidget(librarySection);

    // Splitter proportions: ~65% preview, ~35% library
    leftSplitter->setStretchFactor(0, 3);
    leftSplitter->setStretchFactor(1, 2);

    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
    // Connections
    // â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

    // Character library
    connect(m_charSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshCharacterLibrary(); });
    connect(m_namedOnlyCheck, &QCheckBox::toggled,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshCharacterLibrary(); });

    connect(m_characterLibrary, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem* item) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!item) return;
        if (item->data(Qt::UserRole).toString() == QStringLiteral("video")) {
            addCharacter(item->text().toStdString(),
                         item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString());
        } else {
            addCharacter(item->data(Qt::UserRole).toString().toStdString());
        }
    });
    connect(btnAddChar, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto items = m_characterLibrary->selectedItems();
        if (items.isEmpty()) return;
        auto* item = items.first();
        if (item->data(Qt::UserRole).toString() == QStringLiteral("video")) {
            addCharacter(item->text().toStdString(),
                         item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString());
        } else {
            addCharacter(item->data(Qt::UserRole).toString().toStdString());
        }
    });

    // Puppet library
    connect(m_puppetSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshPuppetLibrary(); });
    auto addPuppetItem = [this](QListWidgetItem* item) {
        if (!item || m_destroying.load(std::memory_order_acquire)) return;
        addPuppet(item->data(Qt::UserRole + 1).toString().toStdString(),
                  item->data(Qt::UserRole + 2).toString().toStdString());
    };
    connect(m_puppetLibrary, &QListWidget::itemDoubleClicked, this, addPuppetItem);
    connect(btnAddPuppet, &QPushButton::clicked, this, [this, addPuppetItem]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto items = m_puppetLibrary->selectedItems();
        if (!items.isEmpty()) addPuppetItem(items.first());
    });

    // Background library
    auto bgItemPath = [](QListWidgetItem* item) -> std::string {
        if (!item) return {};
        QString relPath = item->data(Qt::UserRole + 2).toString();
        return relPath.isEmpty() ? item->text().toStdString() : relPath.toStdString();
    };
    connect(m_backgroundLibrary, &QListWidget::itemDoubleClicked,
            this, [this, bgItemPath](QListWidgetItem* item) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (item) addBackground(bgItemPath(item));
    });
    connect(btnAddBg, &QPushButton::clicked, this, [this, bgItemPath]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        // Add the selected library background to the shot. Importing new images
        // is done via drag-and-drop or the right-click "Import Background…".
        auto items = m_backgroundLibrary->selectedItems();
        if (!items.isEmpty()) addBackground(bgItemPath(items.first()));
    });

    // Video library
    auto addVideoOrChar = [this](QListWidgetItem* item) {
        if (!item || m_destroying.load(std::memory_order_acquire)) return;
        std::string filename = item->text().toStdString();
        if (item->data(Qt::UserRole).toString() == QStringLiteral("videoChar")) {
            addCharacter(item->data(Qt::UserRole + 1).toString().toStdString(),
                         item->data(Qt::UserRole + 2).toString().toStdString(),
                         item->data(Qt::UserRole + 3).toString().toStdString());
        } else {
            addVideoLayer(filename);
        }
    };
    connect(m_videoLibrary, &QListWidget::itemDoubleClicked,
            this, addVideoOrChar);
    connect(btnAddVideo, &QPushButton::clicked, this, [this, addVideoOrChar]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto items = m_videoLibrary->selectedItems();
        if (!items.isEmpty()) addVideoOrChar(items.first());
    });

    return leftSplitter;
}

} // namespace rt
