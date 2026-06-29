/*
 * ProjectBinUI.cpp - setupUI() and static helpers extracted from ProjectBin.cpp.
 *
 * Contains: Premiere Pro label colors, icon generators, BinItemDelegate,
 * and the full setupUI() method.
 */

#include "panels/project/ProjectBin.h"
#include "PathUtils.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"
#include "widgets/ThumbnailGrid.h"
#include "project/Project.h"
#include "project/Settings.h"
#include "dialogs/InterpretFootageDialog.h"
#include "dialogs/SequenceDialog.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "timeline/Timeline.h"
#include "playback/MediaPool.h"

#include <QColorDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <cmath>

#include "panels/project/ProjectBinInternal.h"

namespace rt {

// -----------------------------------------------------------------------------
//  Custom delegate — Premiere Pro label color bar on left edge
// -----------------------------------------------------------------------------

class BinItemDelegate : public QStyledItemDelegate
{
public:
    explicit BinItemDelegate(QTreeWidget* tree, QTreeWidgetItem** dropTarget, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_tree(tree), m_dropTarget(dropTarget) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Draw drop-target highlight for the entire row of the hovered bin
        if (m_dropTarget && *m_dropTarget && m_tree) {
            auto* hovered = *m_dropTarget;
            auto* item = m_tree->itemFromIndex(index);
            if (item == hovered) {
                painter->save();
                painter->setPen(Qt::NoPen);
                QColor hl = Theme::colors().accent;
                hl.setAlpha(50);
                painter->fillRect(option.rect, hl);
                // Draw accent border on left + right of name column
                if (index.column() == 0) {
                    painter->setPen(QPen(Theme::colors().accent, 2));
                    painter->drawRect(option.rect.adjusted(0, 0, 0, -1));
                }
                painter->restore();
            }
        }

        // Only draw label bar in the Name column (column 0)
        if (index.column() == 0) {
            QColor labelCol = index.data(Qt::UserRole + 10).value<QColor>();
            if (labelCol.isValid()) {
                painter->save();
                painter->setPen(Qt::NoPen);
                painter->setBrush(labelCol);
                QRect bar(option.rect.left(), option.rect.top(),
                          4, option.rect.height());
                painter->drawRect(bar);
                painter->restore();
            }
        }
        // Shift drawing right to leave room for the color bar
        QStyleOptionViewItem shifted = option;
        if (index.column() == 0) {
            shifted.rect.setLeft(shifted.rect.left() + 6);
        }
        QStyledItemDelegate::paint(painter, shifted, index);
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(std::max(s.height(), 22));
        return s;
    }

private:
    QTreeWidget* m_tree{nullptr};
    QTreeWidgetItem** m_dropTarget{nullptr};
};

// -----------------------------------------------------------------------------
//  Construction
// -----------------------------------------------------------------------------

ProjectBin::ProjectBin(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

ProjectBin::~ProjectBin()
{
    m_destroying.store(true, std::memory_order_release);
}

// -----------------------------------------------------------------------------
//  UI Setup (Premiere Pro style)
// -----------------------------------------------------------------------------

void ProjectBin::setupUI()
{
    setAcceptDrops(true);
    const auto& m = Theme::metrics();
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Common button style for toolbar/bottom bar
    QString smallBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; "
        "font-size: 12px; padding: 2px; border-radius: 3px; }"
        "QToolButton:hover { background: %2; color: %3; }"
        "QToolButton:checked { background: %4; color: %5; }")
        .arg(Theme::hex(Theme::colors().textTertiary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().accentSubtle))
        .arg(Theme::hex(Theme::colors().textBright));

    // -- Top toolbar (search + filter icon) -----------------------------
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().border)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    // Search field
    m_searchField = new QLineEdit(toolbar);
    m_searchField->setPlaceholderText(QStringLiteral("\U0001F50D Search..."));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setFixedHeight(22);
    m_searchField->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; border: 1px solid %2; border-radius: 3px; "
        "color: %3; font-size: 12px; padding: 0 4px; }"
        "QLineEdit:focus { border: 1px solid %4; }")
        .arg(Theme::hex(Theme::colors().inputBg))
        .arg(Theme::hex(Theme::colors().controlBorder))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().controlBorderFocus)));
    m_searchField->setFocusPolicy(Qt::ClickFocus);
    m_searchField->installEventFilter(this);
    connect(m_searchField, &QLineEdit::textChanged,
            this, &ProjectBin::onSearchChanged);
    toolbarLayout->addWidget(m_searchField, 1);

    mainLayout->addWidget(toolbar);
    mainLayout->addSpacing(2);  // Padding between search bar and bin tab bar

    // -- Bin tab bar (Premiere Pro style: double-click bin → new tab) ----
    m_binTabBar = new QTabBar(this);
    m_binTabBar->setExpanding(false);
    m_binTabBar->setTabsClosable(true);
    m_binTabBar->setMovable(false);
    m_binTabBar->setDrawBase(false);
    m_binTabBar->setFixedHeight(24);
    m_binTabBar->setStyleSheet(QStringLiteral(
        "QTabBar { background: %1; border-bottom: 1px solid %2; }"
        "QTabBar::tab { background: %1; color: %3; border: none; "
        "border-right: 1px solid %2; padding: 2px 10px; font-size: 11px; min-width: 60px; }"
        "QTabBar::tab:selected { background: %4; color: %5; }"
        "QTabBar::tab:hover { background: %6; }"
        "QTabBar::close-button { image: none; width: 10px; height: 10px; "
        "subcontrol-position: right; padding: 2px; }"
        "QTabBar::close-button:hover { background: %7; border-radius: 2px; }")
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().border))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().controlBgHover)));
    // Root "Project" tab — not closeable
    m_binTabBar->addTab("Project");
    m_binTabPaths.append(QStringList{});  // empty path = root
    m_binTabViewModes.append(true);       // root tab defaults to list view
    m_binTabBar->setTabButton(0, QTabBar::RightSide, nullptr);
    connect(m_binTabBar, &QTabBar::currentChanged,
            this, &ProjectBin::onBinTabChanged);
    connect(m_binTabBar, &QTabBar::tabCloseRequested,
            this, &ProjectBin::onBinTabCloseRequested);
    mainLayout->addWidget(m_binTabBar);

    // -- List view (Premiere default: columns) ---------------------------
    m_listWidget = new MediaDragTreeWidget(this);
    m_listWidget->setHeaderLabels({"Name", "Type", "Duration", "Frame Rate", "Resolution", "Sample Rate"});
    m_listWidget->setRootIsDecorated(true);   // Show tree expanders for bins
    m_listWidget->setAlternatingRowColors(true);
    m_listWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_listWidget->setSortingEnabled(true);
    m_listWidget->sortByColumn(0, Qt::AscendingOrder);
    m_listWidget->setDragEnabled(false);  // We handle drag manually in mouse events
    m_listWidget->setAcceptDrops(true);
    m_listWidget->viewport()->setAcceptDrops(true);
    m_listWidget->setDropIndicatorShown(true);
    m_listWidget->setDragDropMode(QAbstractItemView::DropOnly);  // Manual drag start; allow drops
    m_listWidget->setDefaultDropAction(Qt::MoveAction);
    m_listWidget->setAutoExpandDelay(600);  // Hover 600ms during drag to expand a bin
    m_listWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listWidget->setEditTriggers(QAbstractItemView::SelectedClicked);
    // Force selection to remain visible even when the widget loses focus
    // (e.g. during a drag operation).  Without :selected:!active the
    // system palette may hide the selection highlight entirely.
    //
    // Also set the palette Inactive/Highlight to the same accent colour
    // so that QStyledItemDelegate::paint() uses it when State_Active is
    // not set (which happens during Windows OLE drag when the window
    // temporarily loses activation).
    {
        QPalette pal = m_listWidget->palette();
        const QColor accent = Theme::colors().accent;
        pal.setBrush(QPalette::Inactive, QPalette::Highlight, accent);
        pal.setBrush(QPalette::Inactive, QPalette::HighlightedText, Theme::colors().textBright);
        m_listWidget->setPalette(pal);
    }
    m_listWidget->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; color: %2; border: none; "
        "font-size: 12px; alternate-background-color: %3; "
        "show-decoration-selected: 1; }"
        "QTreeWidget::item { padding: 2px 4px; height: 22px; }"
        "QTreeWidget::item:selected { background: %4; }"
        "QTreeWidget::item:selected:!active { background: %4; }"
        "QTreeWidget::item:selected:hover { background: %4; }"
        "QTreeWidget::item:hover { background: %5; }"
        "QHeaderView::section { background: %6; color: %7; border: none; "
        "border-right: 1px solid %8; padding: 3px 6px; font-size: 11px; font-weight: bold; }"
        "QTreeWidget { selection-background-color: %4; }"
        "QRubberBand { background: rgba(50, 130, 220, 60); border: 1px solid %4; }")
        .arg(Theme::hex(Theme::colors().surface0))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().alternateBase))
        .arg(Theme::hex(Theme::colors().accent))
        .arg(Theme::hex(Theme::colors().surface2))
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().border)));
    m_listWidget->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_listWidget->header()->setStretchLastSection(true);
    m_listWidget->setColumnWidth(0, 200);  // Name
    m_listWidget->setColumnWidth(1, 60);   // Type
    m_listWidget->setColumnWidth(2, 70);   // Duration
    m_listWidget->setColumnWidth(3, 75);   // Frame Rate
    m_listWidget->setColumnWidth(4, 80);   // Resolution
    m_listWidget->setColumnWidth(5, 85);   // Sample Rate
    m_listWidget->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_listWidget->setItemDelegate(new BinItemDelegate(m_listWidget, &m_dropHighlightItem, m_listWidget));
    m_listWidget->viewport()->setMouseTracking(true);  // Enable hover-scrub
    m_listWidget->installEventFilter(this);
    m_listWidget->viewport()->installEventFilter(this);

    // ── Header right-click: toggle column visibility (Premiere Pro style) ──
    m_listWidget->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_listWidget->header(), &QHeaderView::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(m_listWidget);
        auto* header = m_listWidget->header();
        for (int col = 0; col < m_listWidget->columnCount(); ++col) {
            QString label = m_listWidget->headerItem()->text(col);
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(!header->isSectionHidden(col));
            // Don't allow hiding the Name column
            if (col == 0) {
                action->setEnabled(false);
            }
            connect(action, &QAction::toggled, this, [header, col](bool visible) {
                header->setSectionHidden(col, !visible);
            });
        }
        menu.exec(header->mapToGlobal(pos));
    });

    connect(m_listWidget, &QTreeWidget::itemDoubleClicked,
            this, &ProjectBin::onListItemDoubleClicked);

    // Handle rename completion � update underlying data when Name column is edited
    connect(m_listWidget, &QTreeWidget::itemChanged,
            this, [this](QTreeWidgetItem* item, int column) {
        if (column != 0) return;
        QString newName = item->text(0).trimmed();
        if (newName.isEmpty()) return;

        // Sequence rename
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIdx = item->data(0, Qt::UserRole + 4).toULongLong();
            if (m_project && seqIdx < m_project->sequenceCount()) {
                auto* seq = m_project->sequence(seqIdx);
                if (seq) seq->setName(newName.toStdString());
                emit sequencesChanged();
            }
            return;
        }

        // Bin rename � just the tree item text (already updated)
        if (item->data(0, Qt::UserRole + 2).toBool())
            return;

        // Media item rename — update displayName on the matching grid
        // item. Match by per-instance itemId so renaming one duplicate
        // doesn't rename its siblings that share the same source file.
        QString filePath = item->data(0, Qt::UserRole).toString();
        const uint64_t itemId = item->data(0, kBinItemIdRole).toULongLong();
        if (!filePath.isEmpty()) {
            auto& items = m_grid->mutableItems();
            bool done = false;
            if (itemId) {
                for (auto& gi : items) {
                    if (gi.itemId == itemId) {
                        gi.displayName = newName;
                        done = true;
                        break;
                    }
                }
            }
            if (!done) {
                for (auto& gi : items) {
                    if (QString::fromStdString(pathToUtf8(gi.filePath)) == filePath) {
                        gi.displayName = newName;
                        break;
                    }
                }
            }
        }
    });

    connect(m_listWidget, &QTreeWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accent)));

        auto* selected = m_listWidget->itemAt(pos);

        if (!selected) {
            // ── Empty-area context menu (no item under cursor) ─────
            menu.addAction("Import Media...", this, &ProjectBin::importFiles);
            menu.addAction("New Bin", this, &ProjectBin::createNewBin);
            menu.addSeparator();
            QMenu* newItemMenu = menu.addMenu("New Item");
            newItemMenu->setStyleSheet(QStringLiteral(
                "QMenu { background: %1; color: %2; border: 1px solid %3; }"
                "QMenu::item:selected { background: %4; }")
                .arg(Theme::hex(Theme::colors().surface2))
                .arg(Theme::hex(Theme::colors().textPrimary))
                .arg(Theme::hex(Theme::colors().border))
                .arg(Theme::hex(Theme::colors().accent)));
            {
                auto* act = newItemMenu->addAction("Sequence");
                QObject::connect(act, &QAction::triggered, this, [this]() {
                    createNewSequence();
                });
            }
            {
                auto* act = newItemMenu->addAction("Color Matte...");
                QObject::connect(act, &QAction::triggered, this, [this]() {
                    createColorMatte();
                });
            }
            {
                auto* act = newItemMenu->addAction("Adjustment Layer...");
                QObject::connect(act, &QAction::triggered, this, [this]() {
                    createAdjustmentLayer();
                });
            }
            if (hasClipboard()) {
                menu.addSeparator();
                menu.addAction("Paste", this, [this]() { pasteClipboard(); });
            }
        } else {
            // ── Item-specific context menu ──────────────────────────
            bool isSequence = selected->data(0, Qt::UserRole + 3).toBool();
            bool isBin = selected->data(0, Qt::UserRole + 2).toBool();

            if (isSequence) {
                // Rename
                menu.addAction("Rename", this, [this, selected]() {
                    m_listWidget->editItem(selected, 0);
                });
                menu.addSeparator();
                size_t seqIdx = selected->data(0, Qt::UserRole + 4).toULongLong();
                menu.addAction("Sequence Settings...", this, [this, seqIdx]() {
                    if (!m_project) return;
                    auto* curSeq = m_project->sequence(seqIdx);
                    if (!curSeq) return;
                    SequenceDialog dlg(this);
                    dlg.setWindowTitle(tr("Sequence Settings"));
                    dlg.setAcceptButtonText(tr("Save Settings"));
                    // Edit THIS sequence's own settings — sequences are independent.
                    dlg.setMediaProperties(
                        curSeq->settings().resolution().width,
                        curSeq->settings().resolution().height,
                        curSeq->settings().frameRate());
                    QString oldName = QString::fromStdString(curSeq->name());
                    dlg.setSequenceName(oldName);
                    if (dlg.exec() != QDialog::Accepted)
                        return;
                    auto oldRes = curSeq->settings().resolution();
                    double oldFps = curSeq->settings().frameRate();
                    auto newRes = Resolution{dlg.width(), dlg.height()};
                    double newFps = dlg.frameRate();
                    QString newName = dlg.sequenceName();
                    if (newRes == oldRes && newFps == oldFps && newName == oldName)
                        return; // nothing changed
                    auto applySeq = [this, seqIdx](const Resolution& res, double fps,
                                                   const QString& name,
                                                   const Resolution& from) {
                        auto* seq = m_project->sequence(seqIdx);
                        if (!seq) return;
                        if (from != res && from.width > 0 && from.height > 0)
                            scaleClipsToResolution(seq, from, res);
                        seq->settings().setResolution(res);
                        seq->settings().setFrameRate(fps);
                        seq->setName(name.toStdString());
                        m_project->setModified(true);
                        syncListView();
                        emit sequenceSettingsChanged();
                    };
                    if (m_commandStack) {
                        m_commandStack->execute(std::make_unique<LambdaCommand>(
                            "Sequence Settings",
                            [applySeq, newRes, newFps, newName, oldRes]() {
                                applySeq(newRes, newFps, newName, oldRes);
                            },
                            [applySeq, oldRes, oldFps, oldName, newRes]() {
                                applySeq(oldRes, oldFps, oldName, newRes);
                            }));
                    } else {
                        applySeq(newRes, newFps, newName, oldRes);
                    }
                });
                menu.addAction("Copy", this, [this, seqIdx]() {
                    if (!m_project) return;
                    if (auto* seq = m_project->sequence(seqIdx)) {
                        m_clipboardSequence = seq->clone();
                        m_clipboardItems.clear();
                        spdlog::info("ProjectBin: copied sequence '{}' to clipboard (context menu)", seq->name());
                    }
                });
                menu.addAction("Duplicate Sequence", this, [this, seqIdx]() {
                    if (!m_project) return;
                    if (m_commandStack) {
                        size_t newIdx = m_project->sequenceCount();
                        m_commandStack->execute(std::make_unique<LambdaCommand>(
                            "Duplicate Sequence",
                            [this, seqIdx]() {
                                m_project->duplicateSequence(seqIdx);
                                syncListView();
                                emit sequencesChanged();
                            },
                            [this, newIdx]() {
                                m_project->removeSequence(newIdx);
                                syncListView();
                                emit sequencesChanged();
                            }));
                    } else {
                        m_project->duplicateSequence(seqIdx);
                        syncListView();
                        emit sequencesChanged();
                    }
                });
                // Nest on active timeline (skip if this IS the active sequence)
                if (m_project && seqIdx != m_project->activeSequenceIndex()) {
                    QString seqName;
                    if (auto* seq = m_project->sequence(seqIdx))
                        seqName = QString::fromStdString(seq->name());
                    menu.addAction("Nest in Active Sequence", this, [this, seqIdx, seqName]() {
                        emit nestSequenceRequested(seqIdx, seqName);
                    });
                }
                menu.addAction("Delete Sequence", this, [this, seqIdx]() {
                    if (!m_project || m_project->sequenceCount() <= 1) return;
                    if (m_commandStack) {
                        // Extract the sequence so undo can re-insert it
                        auto extracted = m_project->extractSequence(seqIdx);
                        if (!extracted) return;
                        auto shared = std::make_shared<std::unique_ptr<Timeline>>(std::move(extracted));
                        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                            "Delete Sequence",
                            [this, seqIdx]() {
                                // Re-execute: extract again (for redo)
                                m_project->removeSequence(seqIdx);
                                syncListView();
                                emit sequencesChanged();
                            },
                            [this, seqIdx, shared]() {
                                // Undo: re-insert at original position
                                m_project->insertSequence(seqIdx, std::move(*shared));
                                syncListView();
                                emit sequencesChanged();
                            }));
                        syncListView();
                        emit sequencesChanged();
                    } else {
                        m_project->removeSequence(seqIdx);
                        syncListView();
                        emit sequencesChanged();
                    }
                });
            } else if (isBin) {
                    menu.addAction("Rename", this, [this, selected]() {
                        m_listWidget->editItem(selected, 0);
                    });
                    menu.addSeparator();
                    menu.addAction("Delete Bin", this, [this, selected]() {
                        int idx = m_listWidget->indexOfTopLevelItem(selected);
                        if (idx >= 0) {
                            delete m_listWidget->takeTopLevelItem(idx);
                        } else if (auto* parent = selected->parent()) {
                            int ci = parent->indexOfChild(selected);
                            if (ci >= 0) delete parent->takeChild(ci);
                        }
                        syncListView();
                    });
                } else {
                    menu.addAction("Rename", this, [this, selected]() {
                        m_listWidget->editItem(selected, 0);
                    });
                    menu.addSeparator();
                    menu.addAction("Replace Media...", this, [this, selected]() {
                        replaceMedia(selected);
                    });
                    menu.addSeparator();
                    menu.addAction("Copy", this, [this, selected]() {
                        m_listWidget->setCurrentItem(selected);
                        copySelection();
                    });
                    menu.addAction("Duplicate", this, [this, selected]() {
                        m_listWidget->setCurrentItem(selected);
                        duplicateSelection();
                    });
                    menu.addSeparator();
                    menu.addAction("Reveal in Explorer", this, [selected]() {
                        const QString fp =
                            selected->data(0, Qt::UserRole).toString();
                        if (fp.isEmpty()) return;
                        QFileInfo info(QFileInfo(fp).absoluteFilePath());
                        if (info.exists()) {
                            // explorer.exe has non-standard argument parsing;
                            // the Qt arg-list overload quotes each token and
                            // breaks "/select,". Pass the exact native command
                            // line instead so the file is highlighted.
                            const QString native = QDir::toNativeSeparators(
                                info.canonicalFilePath());
                            QProcess proc;
                            proc.setProgram(QStringLiteral("explorer.exe"));
                            proc.setNativeArguments(
                                QStringLiteral("/select,\"") + native +
                                QStringLiteral("\""));
                            proc.startDetached();
                        } else {
                            // File gone — open its containing folder if we can.
                            const QString dir = info.absolutePath();
                            if (QFileInfo::exists(dir))
                                QDesktopServices::openUrl(
                                    QUrl::fromLocalFile(dir));
                        }
                    });
                    menu.addAction("Remove", this, [this, selected]() {
                        // Route through the unified delete path so it is
                        // undoable and, for color mattes, also removes the
                        // file from disk (and restores it on undo).
                        m_listWidget->setCurrentItem(selected);
                        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Delete,
                                     Qt::NoModifier);
                        eventFilter(m_listWidget, &ev);
                    });

                    // -- Label Color submenu -----------------------------
                    QMenu* colorMenu = menu.addMenu("Label Color");
                    struct LabelColor { const char* name; uint32_t rgba; };
                    static const LabelColor kLabelColors[] = {
                        {"Default",     0xFF888888},
                        {"Violet",      0xFF9966CC},
                        {"Cerulean",    0xFF4A90D9},
                        {"Forest",      0xFF4A9B4A},
                        {"Rose",        0xFFCC6699},
                        {"Mango",       0xFFCC9933},
                        {"Lavender",    0xFFBB88DD},
                        {"Caribbean",   0xFF33CCAA},
                        {"Iris",        0xFF6666CC},
                        {"Custom...",   0x00000000},
                    };
                    QString filePath = selected->data(0, Qt::UserRole).toString();
                    const uint64_t labelItemId =
                        selected->data(0, kBinItemIdRole).toULongLong();
                    for (const auto& lc : kLabelColors) {
                        QAction* a = colorMenu->addAction(lc.name);
                        if (lc.rgba != 0x00000000) {
                            QPixmap px(12, 12);
                            px.fill(QColor::fromRgba(lc.rgba));
                            a->setIcon(QIcon(px));
                        }
                        uint32_t rgba = lc.rgba;
                        connect(a, &QAction::triggered, this,
                                [this, filePath, labelItemId, rgba]() {
                            uint32_t finalColor = rgba;
                            if (rgba == 0x00000000) {
                                QColor picked = QColorDialog::getColor(Qt::white, this, "Label Color");
                                if (!picked.isValid()) return;
                                finalColor = picked.rgba();
                            }
                            // Apply to the matching grid item — by per-instance
                            // itemId so duplicates colour independently.
                            auto& items = m_grid->mutableItems();
                            int target = -1;
                            if (labelItemId) {
                                for (size_t i = 0; i < items.size(); ++i)
                                    if (items[i].itemId == labelItemId) {
                                        target = static_cast<int>(i); break;
                                    }
                            }
                            if (target < 0) {
                                for (size_t i = 0; i < items.size(); ++i)
                                    if (QString::fromStdString(pathToUtf8(items[i].filePath)) == filePath) {
                                        target = static_cast<int>(i); break;
                                    }
                            }
                            if (target >= 0)
                                m_grid->setItemLabelColor(target, finalColor);
                            syncListView();
                        });
                    }

                    // -- Interpret Footage --------------------------------
                    menu.addSeparator();
                    menu.addAction("Interpret Footage...", this, [this, selected]() {
                        if (!m_project) return;
                        auto* db = m_project->assets();
                        std::filesystem::path fp(selected->data(0, Qt::UserRole).toString().toStdString());
                        double nativeFps = 30.0;
                        bool isVFR = false;
                        uint64_t mediaHandle = selected->data(0, Qt::UserRole + 1).toULongLong();
                        if (m_pool && mediaHandle) {
                            auto* info = m_pool->getInfo(mediaHandle);
                            if (info) { nativeFps = info->fps; isVFR = info->isVFR; }
                        }
                        AssetEntry* entry = db->findByPath(fp);
                        FootageInterpretation current;
                        if (entry) current = entry->interpretation;
                        InterpretFootageDialog dlg(current, nativeFps, isVFR, this);
                        if (dlg.exec() == QDialog::Accepted && entry) {
                            entry->interpretation = dlg.result();
                        }
                    });
                }
            }
            menu.addSeparator();
        menu.addAction("Auto-Sort into Bins", this, [this]() { ensureDefaultBins(); });
        menu.addAction("Clear All", this, [this]() { clearAll(); syncListView(); });
        menu.exec(m_listWidget->mapToGlobal(pos));
    });

    mainLayout->addWidget(m_listWidget, 1);

    // ── Empty state (shown when bin has no media) ───────────────────────
    m_emptyLabel = new QLabel(tr("Import media to get started.\nDrag files here or click the Import button."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setObjectName("EmptyStateLabel");
    m_emptyLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 12px; background: transparent;")
        .arg(Theme::hex(Theme::colors().textDisabled)));
    m_emptyLabel->setVisible(false);
    mainLayout->addWidget(m_emptyLabel, 1);

    // -- Icon view navigation bar (breadcrumb) ---------------------------
    m_iconNavBar = new QWidget(this);
    m_iconNavBar->setFixedHeight(24);
    m_iconNavBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().border)));
    auto* navLayout = new QHBoxLayout(m_iconNavBar);
    navLayout->setContentsMargins(4, 0, 4, 0);
    navLayout->setSpacing(4);

    auto* backBtn = new QToolButton(m_iconNavBar);
    backBtn->setText(QStringLiteral("\u2190"));  // ←
    backBtn->setToolTip("Back");
    backBtn->setFixedSize(20, 20);
    backBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; font-size: 13px; }"
        "QToolButton:hover { color: %2; }")
        .arg(Theme::hex(Theme::colors().textSecondary))
        .arg(Theme::hex(Theme::colors().textPrimary)));
    connect(backBtn, &QToolButton::clicked, this, [this]() {
        if (!m_iconBinPath.isEmpty()) {
            m_iconBinPath.removeLast();
            // Icon view never flattens the tree (only focusListViewOnBin, in
            // list mode, does), so the tree is already full here — just re-scope.
            // (Rebuilding from the stale m_rootFolderState snapshot would relocate
            // post-snapshot imports to root; rebuilding from the live tree is
            // unnecessary since it's already full.)
            syncIconView();
        }
    });
    navLayout->addWidget(backBtn);

    m_breadcrumbLabel = new QLabel("Project", m_iconNavBar);
    m_breadcrumbLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; color: %1; font-size: 12px; border: none; }")
        .arg(Theme::hex(Theme::colors().textSecondary)));
    navLayout->addWidget(m_breadcrumbLabel, 1);

    m_iconNavBar->setVisible(false);
    mainLayout->addWidget(m_iconNavBar);

    // -- Thumbnail grid in scroll area (icon view) -----------------------
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral("QScrollArea { background: %1; border: none; }")
        .arg(Theme::hex(Theme::colors().surface0)));

    m_grid = new ThumbnailGrid(this);
    connect(m_grid, &ThumbnailGrid::itemDoubleClicked,
            this, &ProjectBin::onItemDoubleClicked);
    connect(m_grid, &ThumbnailGrid::emptySpaceDoubleClicked,
            this, &ProjectBin::importFiles);
    connect(m_grid, &ThumbnailGrid::itemCountChanged,
            this, [this](int count) {
                m_statusLabel->setText(QString("%1 items").arg(count));
                emit itemCountChanged(count);
            });

    connect(m_grid, &ThumbnailGrid::itemContextMenu,
            this, [this](int index, const QPoint& globalPos) {
        if (index < 0 || index >= static_cast<int>(m_grid->items().size()))
            return;
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accent)));

        QMenu* newItemMenu = menu.addMenu("New Item");
        newItemMenu->setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accent)));
        {
            auto* act = newItemMenu->addAction("Sequence");
            QObject::connect(act, &QAction::triggered, this, [this]() {
                createNewSequence();
            });
        }
        {
            auto* act = newItemMenu->addAction("Color Matte...");
            QObject::connect(act, &QAction::triggered, this, [this]() {
                createColorMatte();
            });
        }
        {
            auto* act = newItemMenu->addAction("Adjustment Layer...");
            QObject::connect(act, &QAction::triggered, this, [this]() {
                createAdjustmentLayer();
            });
        }
        menu.addSeparator();

        // Check if this is a sequence item (folder-named items that match a sequence)
        bool isSequence = false;
        size_t seqIdx = static_cast<size_t>(-1);
        if (m_project) {
            const auto& gi = m_grid->items()[index];
            if (gi.isFolder && !gi.folderName.isEmpty()) {
                for (size_t si = 0; si < m_project->sequenceCount(); ++si) {
                    if (auto* seq = m_project->sequence(si)) {
                        if (QString::fromStdString(seq->name()) == gi.folderName) {
                            isSequence = true;
                            seqIdx = si;
                            break;
                        }
                    }
                }
            }
        }

        if (isSequence) {
            menu.addAction("Sequence Settings...", this, [this, seqIdx]() {
                if (!m_project) return;
                auto* curSeq = m_project->sequence(seqIdx);
                if (!curSeq) return;
                SequenceDialog dlg(this);
                dlg.setWindowTitle(tr("Sequence Settings"));
                dlg.setAcceptButtonText(tr("Save Settings"));
                // Edit THIS sequence's own settings — sequences are independent.
                dlg.setMediaProperties(
                    curSeq->settings().resolution().width,
                    curSeq->settings().resolution().height,
                    curSeq->settings().frameRate());
                QString oldName = QString::fromStdString(curSeq->name());
                dlg.setSequenceName(oldName);
                if (dlg.exec() != QDialog::Accepted)
                    return;
                auto oldRes = curSeq->settings().resolution();
                double oldFps = curSeq->settings().frameRate();
                auto newRes = Resolution{dlg.width(), dlg.height()};
                double newFps = dlg.frameRate();
                QString newName = dlg.sequenceName();
                if (newRes == oldRes && newFps == oldFps && newName == oldName)
                    return; // nothing changed
                auto applySeq = [this, seqIdx](const Resolution& res, double fps,
                                               const QString& name,
                                               const Resolution& from) {
                    auto* seq = m_project->sequence(seqIdx);
                    if (!seq) return;
                    if (from != res && from.width > 0 && from.height > 0)
                        scaleClipsToResolution(seq, from, res);
                    seq->settings().setResolution(res);
                    seq->settings().setFrameRate(fps);
                    seq->setName(name.toStdString());
                    m_project->setModified(true);
                    syncListView();
                    emit sequenceSettingsChanged();
                };
                if (m_commandStack) {
                    m_commandStack->execute(std::make_unique<LambdaCommand>(
                        "Sequence Settings",
                        [applySeq, newRes, newFps, newName, oldRes]() {
                            applySeq(newRes, newFps, newName, oldRes);
                        },
                        [applySeq, oldRes, oldFps, oldName, newRes]() {
                            applySeq(oldRes, oldFps, oldName, newRes);
                        }));
                } else {
                    applySeq(newRes, newFps, newName, oldRes);
                }
            });
        } else if (!m_grid->items()[index].isFolder) {
            menu.addAction("Remove", this, [this, index]() {
                const auto& item = m_grid->items()[index];
                removeFile(item.filePath);
                syncListView();
            });
        }

        QMenu* colorMenu = menu.addMenu("Label Color");
        struct LabelColor { const char* name; uint32_t rgba; };
        static const LabelColor kColors[] = {
            {"Default",     0xFF888888},
            {"Violet",      0xFF9966CC},
            {"Cerulean",    0xFF4A90D9},
            {"Forest",      0xFF4A9B4A},
            {"Rose",        0xFFCC6699},
            {"Mango",       0xFFCC9933},
            {"Lavender",    0xFFBB88DD},
            {"Caribbean",   0xFF33CCAA},
            {"Iris",        0xFF6666CC},
            {"Custom...",   0x00000000},
        };
        for (const auto& lc : kColors) {
            QAction* a = colorMenu->addAction(lc.name);
            if (lc.rgba != 0x00000000) {
                QPixmap px(12, 12);
                px.fill(QColor::fromRgba(lc.rgba));
                a->setIcon(QIcon(px));
            }
            uint32_t rgba = lc.rgba;
            connect(a, &QAction::triggered, this, [this, index, rgba]() {
                uint32_t finalColor = rgba;
                if (rgba == 0x00000000) {
                    QColor picked = QColorDialog::getColor(Qt::white, this, "Label Color");
                    if (!picked.isValid()) return;
                    finalColor = picked.rgba();
                }
                m_grid->setItemLabelColor(index, finalColor);
                syncListView();
            });
        }

        menu.exec(globalPos);
    });

    m_scrollArea->setWidget(m_grid);
    m_scrollArea->setVisible(false); // Start in list view
    mainLayout->addWidget(m_scrollArea, 1);

    // -- Bottom bar (Premiere Pro style) ---------------------------------
    // Left: list view, icon view, zoom slider, sort button
    // Right: new bin, import media, delete
    auto* bottomBar = new QWidget(this);
    bottomBar->setFixedHeight(26);
    bottomBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(Theme::colors().surface1))
        .arg(Theme::hex(Theme::colors().border)));
    auto* bottomLayout = new QHBoxLayout(bottomBar);
    bottomLayout->setContentsMargins(4, 0, 4, 0);
    bottomLayout->setSpacing(2);

    // List view button
    m_btnListView = new QToolButton(bottomBar);
    m_btnListView->setText(QStringLiteral("\u2630")); // ?
    m_btnListView->setToolTip("List View");
    m_btnListView->setFixedSize(22, 22);
    m_btnListView->setCheckable(true);
    m_btnListView->setChecked(true);
    m_btnListView->setStyleSheet(smallBtnStyle);
    connect(m_btnListView, &QToolButton::clicked, this, [this]() { setListView(true); });
    bottomLayout->addWidget(m_btnListView);

    // Icon view button
    m_btnIconView = new QToolButton(bottomBar);
    m_btnIconView->setText(QStringLiteral("\u25A6")); // ?
    m_btnIconView->setToolTip("Icon View");
    m_btnIconView->setFixedSize(22, 22);
    m_btnIconView->setCheckable(true);
    m_btnIconView->setStyleSheet(smallBtnStyle);
    connect(m_btnIconView, &QToolButton::clicked, this, [this]() { setListView(false); });
    bottomLayout->addWidget(m_btnIconView);

    // Zoom slider
    m_zoomSlider = new QSlider(Qt::Horizontal, bottomBar);
    m_zoomSlider->setRange(30, 300);
    m_zoomSlider->setValue(100);
    m_zoomSlider->setFixedWidth(120);
    m_zoomSlider->setFixedHeight(20);
    m_zoomSlider->setToolTip("Zoom");
    m_zoomSlider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: %2; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
        "QSlider::handle:horizontal:hover { background: %3; }")
        .arg(Theme::hex(Theme::colors().surface3))
        .arg(Theme::hex(Theme::colors().textTertiary))
        .arg(Theme::hex(Theme::colors().textSecondary)));
    connect(m_zoomSlider, &QSlider::valueChanged, this, &ProjectBin::onZoomChanged);
    bottomLayout->addWidget(m_zoomSlider);

    // Sort/menu button
    auto* btnSort = new QToolButton(bottomBar);
    btnSort->setText(QStringLiteral("\u2261")); // =
    btnSort->setToolTip("Sort");
    btnSort->setFixedSize(22, 22);
    btnSort->setStyleSheet(smallBtnStyle);
    connect(btnSort, &QToolButton::clicked, this, [this, btnSort]() {
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item:selected { background: %4; }")
            .arg(Theme::hex(Theme::colors().surface2))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().accent)));
        menu.addAction("Sort by Name", this, [this]() {
            m_listWidget->sortByColumn(0, Qt::AscendingOrder);
        });
        menu.addAction("Sort by Type", this, [this]() {
            m_listWidget->sortByColumn(1, Qt::AscendingOrder);
        });
        menu.addAction("Sort by Duration", this, [this]() {
            m_listWidget->sortByColumn(2, Qt::AscendingOrder);
        });
        menu.addSeparator();
        menu.addAction("Auto-Sort into Bins", this, [this]() {
            ensureDefaultBins();
        });
        menu.exec(btnSort->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    });
    bottomLayout->addWidget(btnSort);

    // Status label (item count)
    m_statusLabel = new QLabel("0 items", this);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; color: %1; font-size: 11px; padding: 0 4px; border: none; }")
        .arg(Theme::hex(Theme::colors().textTertiary)));
    bottomLayout->addWidget(m_statusLabel);

    bottomLayout->addStretch();

    // Right side buttons

    // New Bin button
    auto* newBinBtn = new QToolButton(bottomBar);
    newBinBtn->setText(QStringLiteral("\U0001F4C1")); // ??
    newBinBtn->setToolTip("New Bin (Ctrl+B)");
    newBinBtn->setFixedSize(22, 22);
    newBinBtn->setStyleSheet(smallBtnStyle);
    connect(newBinBtn, &QToolButton::clicked, this, &ProjectBin::createNewBin);
    bottomLayout->addWidget(newBinBtn);

    // Import button
    m_importBtn = new QToolButton(bottomBar);
    m_importBtn->setText(QStringLiteral("+"));
    m_importBtn->setToolTip("Import Media");
    m_importBtn->setFixedSize(22, 22);
    m_importBtn->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; "
        "font-size: 16px; font-weight: bold; border-radius: 3px; }"
        "QToolButton:hover { background: %2; color: %3; }")
        .arg(Theme::hex(Theme::colors().textTertiary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().textPrimary)));
    connect(m_importBtn, &QToolButton::clicked, this, &ProjectBin::importFiles);
    bottomLayout->addWidget(m_importBtn);

    // Create Sequence button (Premiere Pro style — accepts drag-and-drop)
    m_btnCreateSequence = new QToolButton(bottomBar);
    m_btnCreateSequence->setObjectName("CreateSequenceBtn");
    m_btnCreateSequence->setToolTip("Create Sequence (drag media here)");
    m_btnCreateSequence->setFixedSize(22, 22);
    m_btnCreateSequence->setAcceptDrops(true);
    m_btnCreateSequence->setIconSize(QSize(18, 18));
    // Generate a film-reel frame icon matching the sequence icon in the bin
    {
        QColor seqCol = kLabelSequence; // 64, 186, 96 — green tint
        int isz = 18;
        qreal dpr = 2.0;
        QPixmap px(isz * dpr, isz * dpr);
        px.setDevicePixelRatio(dpr);
        px.fill(Qt::transparent);
        QPainter ip(&px);
        ip.setRenderHint(QPainter::Antialiasing);
        ip.setPen(Qt::NoPen);
        // Film frame body
        ip.setBrush(seqCol);
        ip.drawRoundedRect(QRectF(2, 1, isz - 4, isz - 2), 1.5, 1.5);
        // Inner frame
        QColor inner = seqCol.darker(130);
        inner.setAlpha(200);
        ip.setBrush(inner);
        ip.drawRoundedRect(QRectF(4, 3, isz - 8, isz - 6), 1, 1);
        // Sprocket holes
        ip.setBrush(seqCol.lighter(150));
        double holeH = std::max(1.5, (isz - 6) / 5.0);
        for (int i = 0; i < 4; ++i) {
            double y = 2 + (i + 0.5) * ((isz - 4) / 4.0);
            ip.drawRoundedRect(QRectF(0.5, y - holeH / 2, 2, holeH), 0.5, 0.5);
            ip.drawRoundedRect(QRectF(isz - 2.5, y - holeH / 2, 2, holeH), 0.5, 0.5);
        }
        // Play triangle
        ip.setBrush(seqCol.lighter(180));
        QPainterPath tri;
        double cx = isz / 2.0 + 1;
        double cy = isz / 2.0;
        tri.moveTo(cx - 2.5, cy - 2);
        tri.lineTo(cx + 2, cy);
        tri.lineTo(cx - 2.5, cy + 2);
        tri.closeSubpath();
        ip.drawPath(tri);
        ip.end();
        m_btnCreateSequence->setIcon(QIcon(px));
    }
    m_btnCreateSequence->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; "
        "font-size: 13px; border-radius: 3px; }"
        "QToolButton:hover { background: %2; color: %3; }"
        "QToolButton:pressed { background: %4; color: %5; }")
        .arg(Theme::hex(Theme::colors().textTertiary))
        .arg(Theme::hex(Theme::colors().controlBgHover))
        .arg(Theme::hex(Theme::colors().textPrimary))
        .arg(Theme::hex(Theme::colors().accentDim))
        .arg(Theme::hex(Theme::colors().accent)));
    connect(m_btnCreateSequence, &QToolButton::clicked, this, &ProjectBin::createNewSequence);
    // Drag-drop on the button is handled by the button's own event filter
    m_btnCreateSequence->installEventFilter(this);
    bottomLayout->addWidget(m_btnCreateSequence);

    // Delete button
    auto* btnDelete = new QToolButton(bottomBar);
    btnDelete->setText(QStringLiteral("\U0001F5D1")); // ??
    btnDelete->setToolTip("Delete Selected");
    btnDelete->setFixedSize(22, 22);
    btnDelete->setStyleSheet(smallBtnStyle);
    connect(btnDelete, &QToolButton::clicked, this, [this]() {
        // Simulate Delete key on current selection
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        eventFilter(m_listWidget, &ev);
    });
    bottomLayout->addWidget(btnDelete);

    mainLayout->addWidget(bottomBar);
}

} // namespace rt
