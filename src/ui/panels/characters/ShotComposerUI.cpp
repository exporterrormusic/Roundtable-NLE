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
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        QRect r = option.rect;
        QString text = index.data(Qt::DisplayRole).toString();
        int count = index.data(Qt::UserRole + 1).toInt();
        QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const bool hasIcon = !icon.isNull();
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

        // Helper to paint the count badge in the top-right corner.
        auto paintBadge = [&](int topY) {
            if (count <= 0) return;
            QString countStr = QString::number(count);
            QFont bf = option.font;
            bf.setPixelSize(11);
            bf.setBold(true);
            painter->setFont(bf);
            int bw = painter->fontMetrics().horizontalAdvance(countStr) + 12;
            QRect badge(r.right() - bw - 6, topY, bw, 18);
            painter->setBrush(QColor(35, 35, 55, 210));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(badge, 9, 9);
            painter->setPen(QColor(230, 230, 240));
            painter->drawText(badge, Qt::AlignCenter, countStr);
        };

        if (hasIcon) {
            // ── Vertical layout: big thumbnail on top, name centered below ──
            const int pad = 6;
            const int textH = 20;
            int availH = r.height() - 2 * pad - textH;
            int boxSide = qMin(availH, r.width() - 2 * pad);
            if (boxSide < 1) boxSide = qMax(1, availH);

            QRect box(r.left() + (r.width() - boxSide) / 2, r.top() + pad,
                      boxSide, boxSide);
            QPixmap pix = icon.pixmap(QSize(boxSide, boxSide));
            if (!pix.isNull()) {
                // icon.pixmap preserves aspect ratio — center it in the box.
                int px = box.left() + (box.width()  - pix.width())  / 2;
                int py = box.top()  + (box.height() - pix.height()) / 2;
                painter->drawPixmap(px, py, pix);
            }

            // Name below the thumbnail
            if (!text.isEmpty()) {
                painter->setFont(option.font);
                painter->setPen(isSelected ? QColor(255, 255, 255)
                                           : option.palette.color(QPalette::Text));
                QRect textRect(r.left() + 2, box.bottom() + 2, r.width() - 4, textH);
                QString elided = painter->fontMetrics().elidedText(
                    text, Qt::ElideRight, textRect.width());
                painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter, elided);
            }

            paintBadge(r.top() + pad + 2);
        } else {
            // ── Text-only rows (ALL / UNASSIGNED / shows without thumbnail) ──
            int badgeWidth = 0;
            if (count > 0) {
                QFont bf = option.font;
                bf.setPixelSize(11);
                bf.setBold(true);
                painter->setFont(bf);
                badgeWidth = painter->fontMetrics().horizontalAdvance(
                    QString::number(count)) + 14;
            }
            if (!text.isEmpty()) {
                painter->setFont(option.font);
                painter->setPen(option.palette.color(QPalette::Text));
                int leftEdge = r.left() + 10;
                int rightEdge = (badgeWidth > 0) ? (r.right() - badgeWidth - 8)
                                                 : (r.right() - 8);
                int textAvail = rightEdge - leftEdge;
                if (textAvail > 20) {
                    QRect textRect(leftEdge, r.top(), textAvail, r.height());
                    QString elided = painter->fontMetrics().elidedText(
                        text, Qt::ElideRight, textAvail);
                    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
                }
            }
            if (badgeWidth > 0) {
                QFont bf = option.font;
                bf.setPixelSize(11);
                bf.setBold(true);
                painter->setFont(bf);
                QRect badgeRect(r.right() - badgeWidth - 4, r.top() + 6,
                                badgeWidth, r.height() - 12);
                painter->setBrush(QColor(128, 128, 128, 70));
                painter->setPen(Qt::NoPen);
                painter->drawRoundedRect(badgeRect, 8, 8);
                painter->setPen(option.palette.color(QPalette::Text));
                painter->drawText(badgeRect, Qt::AlignCenter, QString::number(count));
            }
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

// ════════════════════════════════════════════════════════════════════════════
// Show filter column — sits between the rail and the character filter column
// ════════════════════════════════════════════════════════════════════════════

QWidget* ShotComposer::createShowFilterColumn()
{
    const auto& c = Theme::colors();

    auto* column = new QWidget;
    column->setObjectName("ShowFilterColumn");
    column->setFixedWidth(180);
    column->setStyleSheet(QStringLiteral(
        "#ShowFilterColumn { background: %1; border-right: 1px solid %2; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.border)));

    auto* colLayout = new QVBoxLayout(column);
    colLayout->setContentsMargins(6, 8, 6, 6);
    colLayout->setSpacing(4);

    // Header "SHOWS" + "add show" button
    auto* headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(4);

    auto* headerLabel = new QLabel(QStringLiteral("SHOWS"));
    headerLabel->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 13px; color: %1; padding: 2px 0; letter-spacing: 0.5px;")
        .arg(Theme::hex(c.textSecondary)));
    headerRow->addWidget(headerLabel);
    headerRow->addStretch();

    auto* addShowBtn = new QPushButton(QStringLiteral("+"));
    addShowBtn->setToolTip(QStringLiteral("Add a new show"));
    addShowBtn->setFixedSize(22, 22);
    addShowBtn->setCursor(Qt::PointingHandCursor);
    addShowBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; font-size: 15px; font-weight: bold; padding: 0; }"
        "QPushButton:hover { background: %4; color: %5; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.textSecondary))
        .arg(Theme::hex(c.border))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.textBright)));
    headerRow->addWidget(addShowBtn);
    colLayout->addLayout(headerRow);

    connect(addShowBtn, &QPushButton::clicked, this, [this]() {
        if (m_destroying.load(std::memory_order_acquire)) return;
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Add Show"),
            tr("Show name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok) return;
        name = name.trimmed();
        if (name.isEmpty()) return;
        m_presetManager.addShow(name.toStdString());
        refreshShotList();
        // Select the newly added show so the user can immediately assign shots.
        if (m_showFilterList) {
            for (int i = 0; i < m_showFilterList->count(); ++i) {
                auto* it = m_showFilterList->item(i);
                if (it && it->data(Qt::UserRole).toString()
                              .compare(name, Qt::CaseInsensitive) == 0) {
                    m_showFilterList->setCurrentRow(i);
                    break;
                }
            }
        }
    });

    // Search bar
    m_showFilterSearchEdit = new QLineEdit;
    m_showFilterSearchEdit->setPlaceholderText(QStringLiteral("Filter shows..."));
    m_showFilterSearchEdit->setClearButtonEnabled(true);
    m_showFilterSearchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        "  border-radius: 4px; padding: 4px 8px; font-size: 12px; }")
        .arg(Theme::hex(c.surface1))
        .arg(Theme::hex(c.text))
        .arg(Theme::hex(c.border)));
    colLayout->addWidget(m_showFilterSearchEdit);

    // Show filter chip list (ListMode, compact rows)
    m_showFilterList = new QListWidget;
    m_showFilterList->setObjectName("ShowFilterList");
    m_showFilterList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_showFilterList->setViewMode(QListView::ListMode);
    m_showFilterList->setMovement(QListView::Static);
    m_showFilterList->setSpacing(2);
    m_showFilterList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_showFilterList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_showFilterList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { background: %1; border: 1.5px solid transparent;"
        "  border-radius: 6px; padding: 6px 10px; min-height: 30px; }"
        "QListWidget::item:selected { border-color: %2; background: %3; }"
        "QListWidget::item:hover { background: %4; }")
        .arg(Theme::hex(c.surface0))
        .arg(Theme::hex(c.accent))
        .arg(Theme::hex(c.surface2))
        .arg(Theme::hex(c.surface2)));
    m_showFilterList->setItemDelegate(new FilterCountDelegate(m_showFilterList));
    colLayout->addWidget(m_showFilterList, 1);

    // Selecting a show or typing in the search box refreshes the shot list
    // (which also rebuilds the character filter list — cascade narrowing).
    connect(m_showFilterList, &QListWidget::currentItemChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });
    connect(m_showFilterSearchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refreshShotList(); });

    // Right-click context menu: rename / remove a show across all shots.
    m_showFilterList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_showFilterList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        if (!m_showFilterList) return;
        auto* item = m_showFilterList->itemAt(pos);
        if (!item) return;
        const QString show = item->data(Qt::UserRole).toString();
        if (show.isEmpty() || show == QStringLiteral("__UNASSIGNED__"))
            return;

        const bool hasThumb =
            !m_presetManager.showThumbnail(show.toStdString()).empty();

        QMenu menu(this);
        auto* renameAct = menu.addAction(QStringLiteral("Rename Show..."));
        auto* thumbAct  = menu.addAction(hasThumb
            ? QStringLiteral("Change Show Thumbnail…")
            : QStringLiteral("Add Show Thumbnail…"));
        QAction* clearThumbAct = hasThumb
            ? menu.addAction(QStringLiteral("Remove Show Thumbnail"))
            : nullptr;
        menu.addSeparator();
        auto* removeAct = menu.addAction(QStringLiteral("Remove from all shots"));
        QAction* chosen = menu.exec(m_showFilterList->mapToGlobal(pos));
        if (!chosen) return;

        // ── Thumbnail actions (handled independently of rename/remove) ──
        if (chosen == thumbAct) {
            auto settings = rt::appSettings();
            QString lastDir = settings.value("Import/lastDir", QString()).toString();
            if (lastDir.isEmpty()) lastDir = QDir::homePath();
            QString file = QFileDialog::getOpenFileName(
                this, tr("Choose Show Thumbnail"), lastDir,
                tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"));
            if (file.isEmpty()) return;
            settings.setValue("Import/lastDir", QFileInfo(file).absolutePath());
            settings.sync();
            m_presetManager.setShowThumbnail(show.toStdString(), file.toStdString());
            refreshShotList();
            return;
        }
        if (clearThumbAct && chosen == clearThumbAct) {
            m_presetManager.setShowThumbnail(show.toStdString(), std::string{});
            refreshShotList();
            return;
        }

        QString newName;  // empty => remove the show entirely
        if (chosen == renameAct) {
            bool ok = false;
            newName = QInputDialog::getText(
                this, tr("Rename Show"),
                tr("New name for show \"%1\":").arg(show),
                QLineEdit::Normal, show, &ok).trimmed();
            if (!ok || newName.isEmpty() || newName == show) return;
        } else if (chosen == removeAct) {
            auto reply = QMessageBox::question(this, tr("Remove Show"),
                tr("Remove the show \"%1\" from every shot?\n"
                   "The shots themselves are not deleted.").arg(show),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
        } else {
            return;
        }

        // Move every shot in this show into the new show (rename) or into
        // No Show (remove). Each move relocates the file to the new namespace.
        const std::string newStd = newName.toStdString();      // empty = remove
        // Snapshot the keys first — saving/removing mutates the preset list.
        std::vector<std::string> keysInShow;
        for (const auto& [k, p] : m_presetManager.allPresets()) {
            if (p.hasShow(show.toStdString())) keysInShow.push_back(k);
        }
        for (const auto& oldKey : keysInShow) {
            auto preset = m_presetManager.load(oldKey);
            if (!preset) continue;
            preset->setShow(newStd);
            m_presetManager.save(*preset);
            m_presetManager.remove(oldKey);
            if (m_currentShot.hasShow(show.toStdString()) &&
                m_currentShot.name() == preset->name()) {
                m_currentShot.setShow(newStd);
                m_lastSavedName = ShotPresetManager::makeKey(newStd, m_currentShot.name());
            }
        }
        // Keep the show registry in sync with the rename/remove.
        if (newStd.empty())
            m_presetManager.removeShow(show.toStdString());
        else
            m_presetManager.renameShow(show.toStdString(), newStd);

        // Reflect the change in the COMPOSE shows edit if relevant.
        if (m_showsEdit && m_showsEdit->isEnabled())
            m_showsEdit->setText(QString::fromStdString(m_currentShot.show()));
        refreshShotList();
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
    // Create the show filter column (will be reparented into CharacterShotPanel)
    m_showFilterColumn = createShowFilterColumn();

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


} // namespace rt
