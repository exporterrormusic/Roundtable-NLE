/*
 * ShotComposerUIShotsColumn.cpp - createShotsColumn() UI builder.
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
#include <QSet>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace rt {

QWidget* ShotComposer::createShotsColumn()
{
    const auto& c = Theme::colors();
    const auto& m = Theme::metrics();

    auto* column = new QWidget;
    column->setObjectName("ShotsColumn");
    column->setMinimumWidth(200);
    column->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
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
        "  border-radius: 3px; font-size: %6px; padding: 0; }"
        "QPushButton:checked { background: %4; color: %5; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.textSecondary))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.textBright))
        .arg(Theme::typography().sizeXs));
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
        "  border-radius: 4px; padding: 4px 8px; font-size: %4px; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border))
        .arg(Theme::typography().sizeXs));
    searchSortRow->addWidget(m_shotSearchEdit, 1);

    m_shotSortCombo = new QComboBox;
    m_shotSortCombo->addItems({"A-Z", "Favorites", "Character", "Recent"});
    m_shotSortCombo->setFixedWidth(90);
    m_shotSortCombo->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 3px 6px; font-size: %5px; }"
        "QComboBox::drop-down { border: none; width: 16px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2;"
        "  selection-background-color: %4; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent))
        .arg(Theme::typography().sizeXxs));
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
                QColor goldAccent = Theme::colors().warning;
                goldAccent.setAlpha(120);
                painter->setBrush(goldAccent);
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
                subFont.setPixelSize(Theme::typography().sizeCaption);
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
        if (m_showsEdit) { m_showsEdit->clear(); m_showsEdit->setEnabled(false); }
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
        const QString shotKey = item->data(Qt::UserRole).toString();
        if (shotKey.isEmpty()) return;

        // A shot is identified by (show, name); the list item carries the full
        // "show/name" key. Split it for display and same-show operations.
        std::string shotShowStd, shotNameStd;
        ShotPresetManager::splitKey(shotKey.toStdString(), shotShowStd, shotNameStd);
        const QString shotName = QString::fromStdString(shotNameStd);
        const QString shotShow = QString::fromStdString(shotShowStd);
        const std::string curKey =
            ShotPresetManager::makeKey(m_currentShot.show(), m_currentShot.name());

        QMenu menu(this);
        QAction* actDuplicate = menu.addAction(QStringLiteral("\U0001F4CB Duplicate"));
        QAction* actRename    = menu.addAction(QStringLiteral("\u270F\uFE0F Rename"));

        // \u2500\u2500 Move to Show submenu \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
        // A shot belongs to exactly one show (its namespace). The current show
        // is checked; picking another MOVES the shot there. "No Show" / a new
        // show are also options.
        QMenu* showMenu = menu.addMenu(QStringLiteral("\U0001F3AC Move to Show"));
        QAction* actNoShow = showMenu->addAction(QStringLiteral("No Show"));
        actNoShow->setCheckable(true);
        actNoShow->setChecked(shotShowStd.empty());
        showMenu->addSeparator();
        std::vector<QAction*> showActs;
        const QStringList allShows = knownShows();
        for (const QString& sh : allShows) {
            QAction* a = showMenu->addAction(sh);
            a->setCheckable(true);
            a->setChecked(sh.compare(shotShow, Qt::CaseInsensitive) == 0);
            showActs.push_back(a);
        }
        showMenu->addSeparator();
        QAction* actNewShow = showMenu->addAction(QStringLiteral("New Show\u2026"));

        menu.addSeparator();
        QAction* actDelete    = menu.addAction(QStringLiteral("\U0001F5D1 Delete"));

        QAction* chosen = menu.exec(m_shotList->viewport()->mapToGlobal(pos));
        if (!chosen) return;

        // \u2500\u2500 Move-to-show handling \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
        {
            bool isMove = false;
            QString destShow;
            if (chosen == actNoShow) { isMove = true; destShow.clear(); }
            else if (chosen == actNewShow) {
                bool ok = false;
                QString name = QInputDialog::getText(this, tr("New Show"),
                    tr("Show name:"), QLineEdit::Normal, QString(), &ok);
                if (!ok || name.trimmed().isEmpty()) return;
                isMove = true;
                destShow = name.trimmed();
                m_presetManager.addShow(destShow.toStdString());
            } else {
                for (size_t i = 0; i < showActs.size(); ++i) {
                    if (chosen == showActs[i]) { isMove = true; destShow = allShows[static_cast<int>(i)]; break; }
                }
            }
            if (isMove) {
                if (destShow.compare(shotShow, Qt::CaseInsensitive) == 0)
                    return; // already there
                auto preset = m_presetManager.load(shotKey.toStdString());
                if (!preset) return;
                // Guard against a name collision in the destination show.
                if (m_presetManager.hasPreset(destShow.toStdString(), shotNameStd)) {
                    QMessageBox::warning(this, tr("Move to Show"),
                        tr("A shot named \"%1\" already exists in that show.").arg(shotName));
                    return;
                }
                preset->setShow(destShow.toStdString());
                m_presetManager.save(*preset);
                m_presetManager.remove(shotKey.toStdString());
                if (curKey == shotKey.toStdString()) {
                    m_currentShot.setShow(destShow.toStdString());
                    m_lastSavedName = ShotPresetManager::makeKey(
                        destShow.toStdString(), m_currentShot.name());
                    if (m_showsEdit && m_showsEdit->isEnabled())
                        m_showsEdit->setText(destShow);
                }
                refreshShotList();
                spdlog::info("ShotComposer: moved shot '{}' to show '{}'",
                             shotNameStd, destShow.toStdString());
                return;
            }
        }

        if (chosen == actDuplicate) {
            auto preset = m_presetManager.load(shotKey.toStdString());
            if (!preset) return;
            std::string baseName = shotNameStd + " Copy";
            std::string newName  = baseName;
            int counter = 2;
            while (m_presetManager.hasPreset(shotShowStd, newName))
                newName = baseName + " " + std::to_string(counter++);
            bool ok = false;
            QString dupeName = QInputDialog::getText(this, "Duplicate Shot",
                "Name for the duplicate:", QLineEdit::Normal,
                QString::fromStdString(newName), &ok);
            if (!ok || dupeName.trimmed().isEmpty()) return;
            ShotPreset dupe = *preset;       // keeps the same show
            dupe.setName(dupeName.trimmed().toStdString());
            m_presetManager.save(dupe);
            setCurrentShot(dupe);
            refreshShotList();
            spdlog::info("ShotComposer: Duplicated shot '{}' as '{}'",
                shotNameStd, dupe.name());
        }
        else if (chosen == actRename) {
            bool ok = false;
            QString newName = QInputDialog::getText(this, "Rename Shot",
                "New name:", QLineEdit::Normal, shotName, &ok);
            if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == shotName)
                return;
            std::string newNameStd = newName.trimmed().toStdString();
            if (m_presetManager.hasPreset(shotShowStd, newNameStd)) {
                QMessageBox::warning(this, "Rename Shot",
                    QString("A shot named '%1' already exists in this show.").arg(newName.trimmed()));
                return;
            }
            auto preset = m_presetManager.load(shotKey.toStdString());
            if (!preset) return;
            ShotPreset renamed = *preset;    // keeps the same show
            renamed.setName(newNameStd);
            m_presetManager.save(renamed);
            m_presetManager.remove(shotKey.toStdString());
            setCurrentShot(renamed);
            refreshShotList();
            spdlog::info("ShotComposer: Renamed shot '{}' -> '{}'",
                shotNameStd, newNameStd);
        }
        else if (chosen == actDelete) {
            auto reply = QMessageBox::question(
                this, "Delete Shot?",
                QString("Are you sure you want to delete '%1'?\nThis cannot be undone.").arg(shotName),
                QMessageBox::Yes | QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
            m_presetManager.remove(shotKey.toStdString());
            if (curKey == shotKey.toStdString()) {
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

} // namespace rt
