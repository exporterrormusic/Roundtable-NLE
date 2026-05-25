/*
 * ShotComposerUI.cpp - UI construction methods for ShotComposer.
 * Split from ShotComposer.cpp for maintainability.
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

QWidget* ShotComposer::createShotsColumn()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* column = new QWidget;
    column->setObjectName("ShotsColumn");
    column->setMinimumWidth(200);
    column->setMaximumWidth(320);
    column->setStyleSheet(QStringLiteral(
        "#ShotsColumn { background: %1; border-right: 1px solid %2; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.border)));

    auto* colLayout = new QVBoxLayout(column);
    colLayout->setContentsMargins(6, 8, 6, 6);
    colLayout->setSpacing(4);

    // Header with view toggle
    auto* headerRow = new QHBoxLayout;
    auto* headerLabel = new QLabel(QStringLiteral("\xf0\x9f\x8e\xac  SHOTS"));
    headerLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1; padding: 2px 0;")
        .arg(Theme::hex(c.textSecondary)));
    headerRow->addWidget(headerLabel);
    headerRow->addStretch();

    // View toggle buttons (list/grid) - placeholder visual indicators
    auto* listViewBtn = new QPushButton(QStringLiteral("\xe2\x98\xb0"));
    listViewBtn->setFixedSize(24, 24);
    listViewBtn->setToolTip("List view");
    listViewBtn->setCheckable(true);
    listViewBtn->setChecked(true);
    listViewBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 3px; font-size: 12px; padding: 0; }"
        "QPushButton:checked { background: %4; color: %5; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.textSecondary))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.textBright)));
    headerRow->addWidget(listViewBtn);

    auto* gridViewBtn = new QPushButton(QStringLiteral("\xe2\x96\xa4"));
    gridViewBtn->setFixedSize(24, 24);
    gridViewBtn->setToolTip("Grid view");
    gridViewBtn->setCheckable(true);
    gridViewBtn->setStyleSheet(listViewBtn->styleSheet());
    headerRow->addWidget(gridViewBtn);

    colLayout->addLayout(headerRow);

    // Search + Sort row
    auto* searchSortRow = new QHBoxLayout;
    searchSortRow->setSpacing(4);

    m_shotSearchEdit = new QLineEdit;
    m_shotSearchEdit->setPlaceholderText(QStringLiteral("\xf0\x9f\x94\x8d Search shots..."));
    m_shotSearchEdit->setClearButtonEnabled(true);
    m_shotSearchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 4px 8px; font-size: 12px; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border)));
    searchSortRow->addWidget(m_shotSearchEdit, 1);

    m_shotSortCombo = new QComboBox;
    m_shotSortCombo->addItems({"A-Z", "Favorites", "Character", "Recent"});
    m_shotSortCombo->setFixedWidth(90);
    m_shotSortCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 3px 6px; font-size: 11px; }"
        "QComboBox::drop-down { border: none; width: 16px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2;"
        "  selection-background-color: %4; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent)));
    searchSortRow->addWidget(m_shotSortCombo);

    colLayout->addLayout(searchSortRow);

    // Action buttons
    auto* shotBtnsRow = new QHBoxLayout;
    shotBtnsRow->setSpacing(m.spacingSm);

    m_newShotBtn = new QPushButton("+ NEW");
    m_newShotBtn->setToolTip("New Shot (Ctrl+N)");
    m_newShotBtn->setFixedHeight(28);
    m_newShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    shotBtnsRow->addWidget(m_newShotBtn);

    m_saveShotBtn = new QPushButton(QStringLiteral("SAVE"));
    m_saveShotBtn->setToolTip("Save Shot (Ctrl+S)");
    m_saveShotBtn->setFixedHeight(28);
    m_saveShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_saveShotBtn->setObjectName("SaveBtn");
    shotBtnsRow->addWidget(m_saveShotBtn);

    m_deleteShotBtn = new QPushButton(QStringLiteral("\xf0\x9f\x97\x91 DELETE"));
    m_deleteShotBtn->setToolTip("Delete Shot");
    m_deleteShotBtn->setFixedHeight(28);
    m_deleteShotBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_deleteShotBtn->setObjectName("DangerBtn");
    shotBtnsRow->addWidget(m_deleteShotBtn);

    colLayout->addLayout(shotBtnsRow);

    // Shot thumbnail list (vertical scrolling)
    m_shotList = new QListWidget;
    m_shotList->setObjectName("ShotThumbnailStrip");
    m_shotList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_shotList->setViewMode(QListView::ListMode);
    m_shotList->setFlow(QListView::TopToBottom);
    m_shotList->setWrapping(false);
    m_shotList->setMovement(QListView::Static);
    m_shotList->setResizeMode(QListView::Adjust);
    m_shotList->setIconSize(QSize(80, 45));
    m_shotList->setSpacing(4);
    m_shotList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_shotList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_shotList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_shotList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_shotList->setStyleSheet(QStringLiteral(
        "QListWidget { background: %1; border: none; outline: none; }"
        "QListWidget::item { background: %2; border: 2px solid transparent;"
        "  border-radius: 4px; padding: 1px; }"
        "QListWidget::item:selected { border-color: %3; background: %4; }"
        "QListWidget::item:hover { background: %5; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.surface2))
        .arg(Theme::hex(c.surface2)));

    // Custom delegate that paints shot items with structured layout
    class ShotItemDelegate : public QStyledItemDelegate {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
        {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            QStyleOptionViewItem opt = option;
            initStyleOption(&opt, index);

            QRect r = option.rect;
            const int margin = 8;
            const int innerW = r.width() - margin * 2;
            const int thumbH = static_cast<int>(innerW * 9.0 / 16.0);
            if (thumbH < 60) return; // too narrow

            bool isDefault = index.data(Qt::UserRole + 3).toBool();

            // Content rect: 14px gap at top — divider sits higher, more room below
            QRect contentR = r.adjusted(0, 14, 0, 0);

            // Background (drawn on content rect only)
            QColor bgColor;
            if (opt.state & QStyle::State_Selected) {
                bgColor = QColor(60, 60, 100, 80);
            } else if (opt.state & QStyle::State_MouseOver) {
                bgColor = QColor(60, 60, 100, 40);
            } else if (isDefault) {
                bgColor = QColor(70, 55, 15, 80);
            } else {
                bgColor = QColor(45, 45, 70, 60);
            }
            painter->setBrush(bgColor);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(QRectF(contentR).adjusted(1, 1, -1, -1), 5, 5);

            // Divider sits closer to top of gap — more padding below (3px above, 11px below)
            if (index.row() > 0) {
                int lineY = r.top() + 3;
                painter->setPen(QPen(QColor(190, 190, 220, 150), 1));
                painter->drawLine(r.left() + 8, lineY, r.right() - 8, lineY);
            }

            // Gold left accent for default shots
            if (isDefault) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(QColor(220, 180, 50, 120));
                painter->drawRoundedRect(QRectF(contentR.left() + 3, contentR.top() + 6, 4, contentR.height() - 12), 2, 2);
            }

            // Selection border
            if (opt.state & QStyle::State_Selected) {
                painter->setPen(QPen(QColor(124, 124, 240), 2));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(QRectF(contentR).adjusted(1, 1, -1, -1), 5, 5);
            }

            // Thumbnail - full width
            QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull()) {
                QPixmap pix = icon.pixmap(innerW, thumbH);
                if (!pix.isNull()) {
                    QRect thumbRect(contentR.left() + margin, contentR.top() + 3, innerW, thumbH);
                    painter->drawPixmap(thumbRect, pix);
                }
            }

            // Text area below thumbnail
            const int textMargin = 10;
            const int textTop = contentR.top() + 3 + thumbH + 6;
            const int textAreaW = innerW - textMargin * 2;
            const int textLeft = r.left() + margin + textMargin;

            // Shot name (bold, 1.5x larger, centered)
            QString name = index.data(Qt::DisplayRole).toString();
            if (!name.isEmpty()) {
                QFont nameFont = opt.font;
                nameFont.setPixelSize(18);
                nameFont.setBold(true);
                painter->setFont(nameFont);

                QColor nameColor = index.data(Qt::ForegroundRole).value<QColor>();
                if (!nameColor.isValid()) nameColor = QColor(200, 200, 200);
                painter->setPen(nameColor);

                QRect nameRect(textLeft, textTop, textAreaW, 24);
                QString elidedName = painter->fontMetrics().elidedText(name, Qt::ElideRight, textAreaW);
                painter->drawText(nameRect, Qt::AlignCenter, elidedName);
            }

            // Subtitle: character tags only (centered)
            auto charTags = index.data(Qt::UserRole + 1).toStringList();

            QStringList parts;
            for (const auto& t : charTags) {
                QString shortTag = t.length() > 20 ? t.left(18) + "..." : t;
                parts << QString::fromUtf8("\xf0\x9f\x91\xa4 ") + shortTag;
            }
            if (isDefault)
                parts << QString::fromUtf8("\xe2\xad\x90");

            QString subText = parts.isEmpty() ? QString() : parts.join("  |  ");
            if (!subText.isEmpty()) {
                QFont subFont = opt.font;
                subFont.setPixelSize(15);
                painter->setFont(subFont);
                painter->setPen(QColor(140, 140, 140));

                QRect subRect(textLeft, textTop + 26, textAreaW, 20);
                QString elidedSub = painter->fontMetrics().elidedText(subText, Qt::ElideRight, textAreaW);
                painter->drawText(subRect, Qt::AlignCenter, elidedSub);
            }

            painter->restore();
        }
        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            Q_UNUSED(option);
            Q_UNUSED(index);
            // Height: 14px top gap + thumbnail (16:9) + name (24px) + subtitle (20px) + padding
            return QSize(0, 210);
        }
    };
    m_shotList->setItemDelegate(new ShotItemDelegate(m_shotList));

    colLayout->addWidget(m_shotList, 1);

    // Connections

    // Shot thumbnail strip selection -> load preset
    connect(m_shotList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem* current, QListWidgetItem* /*previous*/) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!current) return;
        auto name = current->data(Qt::UserRole).toString().toStdString();
        if (name.empty()) return;

        auto preset = m_presetManager.load(name);
        if (preset)
            setCurrentShot(*preset);
    });

    connect(m_newShotBtn, &QPushButton::clicked, this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; newShot(); });

    connect(m_saveShotBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        saveCurrentShot();
    });

    connect(m_deleteShotBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto* current = m_shotList->currentItem();
        if (!current) return;
        auto name = current->data(Qt::UserRole).toString();
        if (name.isEmpty()) return;
        auto reply = QMessageBox::question(
            this, "Delete Shot?",
            QString("Are you sure you want to delete '%1'?\nThis cannot be undone.").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        m_presetManager.remove(name.toStdString());
        m_currentShot = ShotPreset();
        m_selectedLayer = -1;
        m_shotNameEdit->clear();
        m_shotNameEdit->setEnabled(false);
        m_defaultShotCheck->setEnabled(false);
        m_defaultCharCombo->setEnabled(false);
        m_defaultCharCombo->clear();
        m_setDefaultBtn->setEnabled(false);
        updatePreview();
        refreshShotList();
        refreshLayerList();
        clearLayerProperties();
    });

    // Shot list context menu (right-click) -> Delete / Duplicate / Rename
    connect(m_shotList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        auto* item = m_shotList->itemAt(pos);
        if (!item) return;
        QString shotName = item->data(Qt::UserRole).toString();
        if (shotName.isEmpty()) return;

        QMenu menu(this);
        QAction* actDuplicate = menu.addAction(QStringLiteral("\U0001F4CB Duplicate"));
        QAction* actRename    = menu.addAction(QStringLiteral("\u270F\uFE0F Rename"));
        menu.addSeparator();
        QAction* actDelete    = menu.addAction(QStringLiteral("\U0001F5D1 Delete"));

        QAction* chosen = menu.exec(m_shotList->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == actDuplicate) {
            auto preset = m_presetManager.load(shotName.toStdString());
            if (!preset) return;
            std::string baseName = shotName.toStdString() + " Copy";
            std::string newName  = baseName;
            int counter = 2;
            while (m_presetManager.hasPreset(newName))
                newName = baseName + " " + std::to_string(counter++);
            bool ok = false;
            QString dupeName = QInputDialog::getText(this, "Duplicate Shot",
                "Name for the duplicate:", QLineEdit::Normal,
                QString::fromStdString(newName), &ok);
            if (!ok || dupeName.trimmed().isEmpty()) return;
            ShotPreset dupe = *preset;
            dupe.setName(dupeName.trimmed().toStdString());
            m_presetManager.save(dupe);
            setCurrentShot(dupe);
            refreshShotList();
            spdlog::info("ShotComposer: Duplicated shot '{}' as '{}'",
                shotName.toStdString(), dupe.name());
        }
        else if (chosen == actRename) {
            bool ok = false;
            QString newName = QInputDialog::getText(this, "Rename Shot",
                "New name:", QLineEdit::Normal, shotName, &ok);
            if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == shotName)
                return;
            std::string newNameStd = newName.trimmed().toStdString();
            if (m_presetManager.hasPreset(newNameStd)) {
                QMessageBox::warning(this, "Rename Shot",
                    QString("A shot named '%1' already exists.").arg(newName.trimmed()));
                return;
            }
            auto preset = m_presetManager.load(shotName.toStdString());
            if (!preset) return;
            ShotPreset renamed = *preset;
            renamed.setName(newNameStd);
            m_presetManager.save(renamed);
            m_presetManager.remove(shotName.toStdString());
            setCurrentShot(renamed);
            refreshShotList();
            spdlog::info("ShotComposer: Renamed shot '{}' -> '{}'",
                shotName.toStdString(), newNameStd);
        }
        else if (chosen == actDelete) {
            auto reply = QMessageBox::question(
                this, "Delete Shot?",
                QString("Are you sure you want to delete '%1'?\nThis cannot be undone.").arg(shotName),
                QMessageBox::Yes | QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            m_presetManager.remove(shotName.toStdString());
            if (m_currentShot.name() == shotName.toStdString()) {
                m_currentShot = ShotPreset();
                m_selectedLayer = -1;
                m_shotNameEdit->clear();
                m_shotNameEdit->setEnabled(false);
                m_defaultShotCheck->setEnabled(false);
                m_defaultCharCombo->setEnabled(false);
                m_defaultCharCombo->clear();
                m_setDefaultBtn->setEnabled(false);
                updatePreview();
            }
            refreshShotList();
            refreshLayerList();
            clearLayerProperties();
            spdlog::info("ShotComposer: Deleted shot '{}'", shotName.toStdString());
        }
    });

    // Shot search -> refresh shot list
    connect(m_shotSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });

    // Sort combo -> refresh shot list
    connect(m_shotSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });

    return column;
}


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Character filter thumbnail column â€” sits between rail and shots column
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// Delegate that paints a small count badge on the right side of filter items
class FilterCountDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        QRect r = option.rect;
        QString text = index.data(Qt::DisplayRole).toString();
        int count = index.data(Qt::UserRole + 1).toInt();
        bool isSelected = option.state & QStyle::State_Selected;
        bool isHovered = option.state & QStyle::State_MouseOver;

        // Draw background
        QColor bgColor;
        if (isSelected) {
            bgColor = QColor(60, 60, 100, 100);
        } else if (isHovered) {
            bgColor = QColor(60, 60, 100, 50);
        } else {
            bgColor = QColor(45, 45, 70, 40);
        }
        painter->setBrush(bgColor);
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 5, 5);

        // Selection border
        if (isSelected) {
            painter->setPen(QPen(QColor(124, 124, 240), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(r).adjusted(1, 1, -1, -1), 5, 5);
        }

        // Calculate badge dimensions first
        int badgeWidth = 0;
        if (count > 0) {
            QString countStr = QString::number(count);
            QFont bf = option.font;
            bf.setPixelSize(11);
            bf.setBold(true);
            painter->setFont(bf);
            badgeWidth = painter->fontMetrics().horizontalAdvance(countStr) + 14;
        }

        // Draw icon (48x48)
        int iconOffset = 6; // left padding for text when no icon
        QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            QPixmap pix = icon.pixmap(36, 36);
            if (!pix.isNull()) {
                int iconY = r.top() + (r.height() - 36) / 2;
                painter->drawPixmap(r.left() + 6, iconY, 36, 36, pix);
                iconOffset = 6 + 36 + 8; // icon + gap
            }
        }

        // Draw text with proper elision, leaving room for badge
        if (!text.isEmpty()) {
            QFont tf = option.font;
            painter->setFont(tf);
            painter->setPen(option.palette.color(QPalette::Text));

            int leftEdge = r.left() + iconOffset;
            int rightEdge = (badgeWidth > 0) ? (r.right() - badgeWidth - 8) : (r.right() - 8);
            int textAvail = rightEdge - leftEdge;
            if (textAvail > 20) {
                QRect textRect(leftEdge, r.top(), textAvail, r.height());
                QString elided = painter->fontMetrics().elidedText(text, Qt::ElideRight, textAvail);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
            }
        }

        // Draw badge on top of everything
        if (count > 0 && badgeWidth > 0) {
            QString countStr = QString::number(count);
            QFont bf = option.font;
            bf.setPixelSize(11);
            bf.setBold(true);
            painter->setFont(bf);

            QRect badgeRect(r.right() - badgeWidth - 4, r.top() + 6, badgeWidth, r.height() - 12);
            QColor badgeBg = QColor(128, 128, 128, 70);
            painter->setBrush(badgeBg);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(badgeRect, 8, 8);

            painter->setPen(option.palette.color(QPalette::Text));
            painter->drawText(badgeRect, Qt::AlignCenter, countStr);
        }

        painter->restore();
    }
};

QWidget* ShotComposer::createCharFilterColumn()
{
    const auto& c = Theme::colors();

    auto* column = new QWidget;
    column->setObjectName("CharFilterColumn");
    column->setFixedWidth(220);
    column->setStyleSheet(QStringLiteral(
        "#CharFilterColumn { background: %1; border-right: 1px solid %2; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.border)));

    auto* colLayout = new QVBoxLayout(column);
    colLayout->setContentsMargins(6, 8, 6, 6);
    colLayout->setSpacing(4);

    // Header "CHARACTERS"
    auto* headerLabel = new QLabel(QStringLiteral("CHARACTERS"));
    headerLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1; padding: 2px 0; letter-spacing: 0.5px;")
        .arg(Theme::hex(c.textSecondary)));
    colLayout->addWidget(headerLabel);

    // Search bar
    m_filterSearchEdit = new QLineEdit;
    m_filterSearchEdit->setPlaceholderText(QStringLiteral("Filter characters..."));
    m_filterSearchEdit->setClearButtonEnabled(true);
    m_filterSearchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 4px 8px; font-size: 12px; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border)));
    colLayout->addWidget(m_filterSearchEdit);

    // Character filter chip list (ListMode, compact rows)
    m_charFilterList = new QListWidget;
    m_charFilterList->setObjectName("CharFilterList");
    m_charFilterList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_charFilterList->setViewMode(QListView::ListMode);
    m_charFilterList->setMovement(QListView::Static);
    m_charFilterList->setSpacing(2);
    m_charFilterList->setIconSize(QSize(48, 48));
    m_charFilterList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_charFilterList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_charFilterList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { background: %1; border: 1.5px solid transparent;"
        "  border-radius: 6px; padding: 6px 10px; min-height: 54px; }"
        "QListWidget::item:selected { border-color: %2; background: %3; }"
        "QListWidget::item:hover { background: %4; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.surface2))
        .arg(Theme::hex(c.surface2)));
    m_charFilterList->setItemDelegate(new FilterCountDelegate(m_charFilterList));
    colLayout->addWidget(m_charFilterList, 1);

    // Connect filter list and search to refresh shot list
    connect(m_charFilterList, &QListWidget::currentItemChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });
    connect(m_filterSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });

    // Right-click context menu: rename character (alias) so script lines
    // for the new display name resolve to the original character's default
    // shot. The underlying character/shot data is untouched.
    m_charFilterList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_charFilterList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_charFilterList) return;
        auto* item = m_charFilterList->itemAt(pos);
        if (!item) return;
        const QString tag = item->data(Qt::UserRole).toString();
        // Skip ALL ("") and UNASSIGNED — only real character entries.
        if (tag.isEmpty() || tag == QStringLiteral("__UNASSIGNED__"))
            return;

        // UserRole stores the real character name; the item's text is the
        // current display label (alias if one is set).
        const std::string realName    = tag.toStdString();
        const std::string displayName = item->text().toStdString();

        QMenu menu(this);
        auto* renameAct = menu.addAction(QStringLiteral("Rename..."));
        QAction* clearAct = nullptr;
        if (realName != displayName)
            clearAct = menu.addAction(QStringLiteral("Reset to \"%1\"")
                                          .arg(QString::fromStdString(realName)));

        QAction* chosen = menu.exec(m_charFilterList->mapToGlobal(pos));
        if (!chosen) return;

        if (chosen == clearAct) {
            m_presetManager.setAlias(realName, std::string{});
            refreshShotList();
            return;
        }

        if (chosen == renameAct) {
            bool ok = false;
            QString newName = QInputDialog::getText(
                this, tr("Rename Character"),
                tr("New display name for \"%1\":")
                    .arg(QString::fromStdString(realName)),
                QLineEdit::Normal,
                QString::fromStdString(displayName), &ok);
            if (!ok) return;
            newName = newName.trimmed();
            // Reject collisions with other real character names (would
            // shadow them in the filter).
            if (!newName.isEmpty() &&
                newName.compare(QString::fromStdString(realName), Qt::CaseInsensitive) != 0)
            {
                for (int i = 0; i < m_charFilterList->count(); ++i) {
                    auto* it = m_charFilterList->item(i);
                    if (!it) continue;
                    const QString existingTag = it->data(Qt::UserRole).toString();
                    if (existingTag.isEmpty() ||
                        existingTag == QStringLiteral("__UNASSIGNED__"))
                        continue;
                    const std::string existingReal =
                        m_presetManager.realNameFor(existingTag.toStdString());
                    if (existingReal != realName &&
                        QString::fromStdString(existingReal).compare(newName, Qt::CaseInsensitive) == 0)
                    {
                        QMessageBox::warning(this, tr("Rename Character"),
                            tr("\"%1\" is already a character name.").arg(newName));
                        return;
                    }
                }
            }
            m_presetManager.setAlias(realName, newName.toStdString());
            refreshShotList();
        }
    });

    return column;
}



// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// UI Setup â€” B3-C layout: left=preview+library, right=properties+layers
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void ShotComposer::setupUI()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Create the standalone shots column (will be reparented into CharacterShotPanel)
    m_shotsColumn = createShotsColumn();
    // Create the character filter thumbnail column (will be reparented into CharacterShotPanel)
    m_charFilterColumn = createCharFilterColumn();

    m_splitter = new QSplitter(Qt::Horizontal, this);

    m_splitter->addWidget(createLeftPanel());
    m_splitter->addWidget(createPropertiesPanel());

    m_splitter->setStretchFactor(0, 2);   // Preview + Library
    m_splitter->setStretchFactor(1, 1);   // Properties + Layers

    layout->addWidget(m_splitter);
}

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Left panel: shot header bar + preview viewport + asset library tabs
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

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

    // â”€â”€ Backgrounds tab â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    auto* bgTab = new QWidget;
    auto* bgLayout = new QVBoxLayout(bgTab);
    bgLayout->setContentsMargins(5, 5, 5, 5);

    m_backgroundLibrary = new DragAssetList;
    setupIconList(m_backgroundLibrary);
    m_backgroundLibrary->setDragEnabled(true);
    m_backgroundLibrary->setDragDropMode(QAbstractItemView::DragOnly);
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

// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Right panel: properties (tabs) + camera + layers at bottom

} // namespace rt