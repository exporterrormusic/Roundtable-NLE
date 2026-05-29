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


} // namespace rt
