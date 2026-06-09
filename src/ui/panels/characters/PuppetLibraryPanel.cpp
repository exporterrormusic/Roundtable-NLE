/*
 * PuppetLibraryPanel.cpp — PNG puppet character browser / manager.
 *
 * readOnly  : drag-only tree for the timeline Library dock.
 * editable  : SHOTS-style cascading editor — Character → Outfit → Action
 *             thumbnail columns, a Faces column (4 PNG drop slots), and a
 *             smooth live preview. No pop-up dialogs.
 */

#include "panels/characters/PuppetLibraryPanel.h"
#include "panels/characters/PuppetLibrary.h"
#include "widgets/MediaDragTreeWidget.h"
#include "timeline/PngPuppetClip.h"

#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QAbstractItemView>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QStyledItemDelegate>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace rt {

// ─── FaceSlot — a click-to-choose / drop-target PNG well ─────────────────────
namespace {

class FaceSlot : public QFrame {
public:
    FaceSlot(int index, QWidget* parent = nullptr)
        : QFrame(parent), m_index(index)
    {
        setAcceptDrops(true);
        setFrameShape(QFrame::StyledPanel);
        setMinimumSize(84, 84);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QObject::tr("Drop a PNG here, or click to choose"));
    }

    std::function<void(int, const QString&)> onPick;

    void setFacePixmap(const QPixmap& p) { m_pix = p; update(); }

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QFrame::mousePressEvent(e); return; }
        const QString f = QFileDialog::getOpenFileName(
            this, QObject::tr("Choose face PNG"), QString(),
            QObject::tr("Images (*.png *.webp *.jpg *.jpeg)"));
        if (!f.isEmpty() && onPick) onPick(m_index, f);
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        const auto urls = e->mimeData()->urls();
        if (urls.isEmpty()) return;
        const QString f = urls.first().toLocalFile();
        if (!f.isEmpty() && onPick) onPick(m_index, f);
        e->acceptProposedAction();
    }
    void paintEvent(QPaintEvent* ev) override {
        QFrame::paintEvent(ev);
        QPainter p(this);
        const QRect r = rect().adjusted(4, 4, -4, -4);
        if (!m_pix.isNull()) {
            const QPixmap s = m_pix.scaled(r.size(), Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
            p.drawPixmap(r.center().x() - s.width() / 2,
                         r.center().y() - s.height() / 2, s);
        } else {
            p.setPen(QColor(150, 150, 150));
            p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap,
                       QObject::tr("Drop PNG\nor click"));
        }
    }

private:
    int     m_index;
    QPixmap m_pix;
};

// ─── PuppetPreview — smooth animated preview (painter transforms) ────────────
// Constant widget size + sub-pixel QPainter transforms → no re-centering jitter
// (the old QLabel-rescale approach stepped in whole pixels each frame).
class PuppetPreview : public QWidget {
public:
    explicit PuppetPreview(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(180, 220);
        m_timer = new QTimer(this);
        QObject::connect(m_timer, &QTimer::timeout, this, [this]() {
            m_clock += 0.016;
            update();
        });
        m_timer->start(16);  // ~60 fps
    }

    void setFaces(const std::array<QPixmap, 4>& f) { m_faces = f; m_clock = 0.0; update(); }
    void setTalking(bool t) { m_talking = t; m_clock = 0.0; update(); }
    void setSeed(uint32_t s) { m_seed = s ? s : 1u; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));

        PngPuppetClip eval;
        eval.setTalking(m_talking);
        eval.setSeed(m_seed);
        int face = eval.selectFace(m_clock);
        if (face < 0 || face > 3) face = 0;
        QPixmap pm = m_faces[static_cast<size_t>(face)];
        if (pm.isNull()) pm = m_faces[0];
        if (pm.isNull()) {
            p.setPen(QColor(120, 120, 120));
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("No faces yet"));
            return;
        }

        const auto b = eval.breathing(m_clock);
        const double availH = height() * 0.85;
        const double base   = (pm.height() > 0) ? availH / pm.height() : 1.0;
        const double s      = base * b.scale;
        const double w      = pm.width()  * s;
        const double h      = pm.height() * s;
        const double cx     = width()  * 0.5 + b.dx / 1920.0 * width();
        const double cy     = height() * 0.5 + b.dy / 1080.0 * height();

        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.translate(cx, cy);
        p.rotate(b.rot);
        p.drawPixmap(QRectF(-w / 2.0, -h / 2.0, w, h), pm, QRectF(pm.rect()));
    }

private:
    std::array<QPixmap, 4> m_faces;
    double   m_clock{0.0};
    bool     m_talking{false};
    uint32_t m_seed{1u};
    QTimer*  m_timer{nullptr};
};

// ─── CascadeThumbDelegate — full-width thumbnail row with a divider ──────────
// Renders each cascade entry (Character / Outfit / Action) as the SHOTS-style
// "big thumbnail above a centered name" cell, sized to fill the column width
// (matching the Faces column), with a hairline divider between entries so the
// rows read as clearly separated. The thumbnail is upscaled if the source face
// PNG is small so it always fills the column rather than sitting tiny in the
// middle.
class CascadeThumbDelegate : public QStyledItemDelegate {
public:
    explicit CascadeThumbDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    static constexpr int kBox     = 4;   // gap between adjacent entry boxes
    static constexpr int kMargin  = 6;   // inset from the box edges to content
    static constexpr int kGap     = 4;   // thumbnail → name gap
    static constexpr int kTextH   = 30;  // room for a (possibly wrapped) name

    // Width available for content inside the viewport.
    static int contentWidth(const QStyleOptionViewItem& opt) {
        if (auto* view = qobject_cast<const QAbstractItemView*>(opt.widget))
            return view->viewport()->width();
        return opt.rect.width();
    }

    QSize sizeHint(const QStyleOptionViewItem& opt,
                   const QModelIndex&) const override {
        const int w = contentWidth(opt);
        // Inner box width = cell width minus the box gap on each side; the
        // thumbnail then fills that box minus its content margins.
        const int thumb = std::max(0, w - 2 * kBox - 2 * kMargin);
        return QSize(w, 2 * kBox + 2 * kMargin + thumb + kGap + kTextH);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        // Each entry is drawn as its own framed box, inset by kBox from the
        // cell edges so adjacent boxes are visibly separated.
        const QRectF box = QRectF(opt.rect).adjusted(kBox, kBox, -kBox, -kBox);
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hover    = opt.state & QStyle::State_MouseOver;

        p->setPen(Qt::NoPen);
        p->setBrush(selected ? QColor(0x2c, 0x3a, 0x55)
                             : (hover ? QColor(0x2a, 0x2a, 0x2a)
                                      : QColor(0x1d, 0x1d, 0x1d)));
        p->drawRoundedRect(box, 6, 6);

        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(selected ? QColor(0x5a, 0x8c, 0xff) : QColor(0x4a, 0x4a, 0x4a),
                       selected ? 2.0 : 1.0));
        p->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

        const QRect ir = box.toRect();
        const int thumb = std::max(0, ir.width() - 2 * kMargin);
        const QRect thumbRect(ir.left() + kMargin, ir.top() + kMargin, thumb, thumb);

        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            // Pull the icon's native pixmap, then scale to fill the cell width
            // (QIcon::pixmap won't upscale past native, so scale explicitly).
            QPixmap pm = icon.pixmap(QSize(4096, 4096));
            if (!pm.isNull()) {
                const QPixmap s = pm.scaled(thumbRect.size(), Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
                p->setRenderHint(QPainter::SmoothPixmapTransform, true);
                p->drawPixmap(thumbRect.center().x() - s.width() / 2,
                              thumbRect.center().y() - s.height() / 2, s);
            }
        }

        const QString text = index.data(Qt::DisplayRole).toString();
        if (!text.isEmpty()) {
            const QRect textRect(ir.left() + kMargin, thumbRect.bottom() + kGap,
                                 ir.width() - 2 * kMargin, kTextH);
            p->setPen(selected ? QColor(0xff, 0xff, 0xff) : QColor(0xdc, 0xdc, 0xdc));
            p->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                        text);
        }

        p->restore();
    }

    // Inline-rename editor sits over the name strip, not the whole tall cell.
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt,
                              const QModelIndex&) const override {
        const QRect ir = QRect(opt.rect).adjusted(kBox, kBox, -kBox, -kBox);
        const int thumb = std::max(0, ir.width() - 2 * kMargin);
        editor->setGeometry(ir.left() + kMargin, ir.top() + kMargin + thumb + kGap,
                            ir.width() - 2 * kMargin, kTextH);
    }
};

// Configure a QListWidget as a SHOTS-style filter column: a single stacked
// column (one entry per row), full-width thumbnail above a centered name,
// rendered by CascadeThumbDelegate.
void setupThumbList(QListWidget* list) {
    list->setViewMode(QListView::ListMode);
    list->setFlow(QListView::TopToBottom);
    list->setWrapping(false);
    list->setMovement(QListView::Static);
    list->setResizeMode(QListView::Adjust);
    list->setSpacing(0);
    list->setWordWrap(true);
    list->setUniformItemSizes(false);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setContextMenuPolicy(Qt::CustomContextMenu);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setItemDelegate(new CascadeThumbDelegate(list));
    list->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; }"));
}

// Resting-face thumbnail for a manifest variant key, or a null pixmap.
QIcon variantIcon(const PuppetManifest& m, const QString& key) {
    const QString face = m.variants.value(key).faces[0];
    if (face.isEmpty()) return {};
    QPixmap pm(face);
    return pm.isNull() ? QIcon{} : QIcon(pm);
}

} // namespace

// ─── Construction ────────────────────────────────────────────────────────────

PuppetLibraryPanel::PuppetLibraryPanel(QWidget* parent, bool readOnly)
    : QWidget(parent)
    , m_readOnly(readOnly)
{
    if (m_readOnly) buildReadOnlyUI();
    else            buildEditableUI();
    refresh();
}

void PuppetLibraryPanel::refresh()
{
    if (m_readOnly) refreshTree();
    else            refreshCharacters();
}

// ═════════════════════════════════════════════════════════════════════════════
// Read-only drag tree (timeline Library dock) — unchanged behavior
// ═════════════════════════════════════════════════════════════════════════════

void PuppetLibraryPanel::buildReadOnlyUI()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_tree = new MediaDragTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(14);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setIconSize(QSize(48, 48));
    root->addWidget(m_tree, 1);
}

void PuppetLibraryPanel::refreshTree()
{
    if (!m_tree) return;
    const QString prevSel = selectedFolder();
    m_tree->clear();

    const QStringList folders = puppetlib::listPuppetFolders();
    for (const QString& folder : folders) {
        PuppetManifest m;
        if (!puppetlib::load(folder, m)) continue;

        auto* charItem = new QTreeWidgetItem(m_tree);
        charItem->setText(0, m.displayName);
        charItem->setData(0, Qt::UserRole + 5, folder);
        if (!m.variantOrder.isEmpty()) {
            const QString restFace = m.variants.value(m.variantOrder.first()).faces[0];
            if (!restFace.isEmpty()) {
                QPixmap pm(restFace);
                if (!pm.isNull()) charItem->setIcon(0, QIcon(pm));
            }
        }

        const bool singleVariant = (m.variantOrder.size() == 1);
        if (singleVariant) {
            const QString variant = m.variantOrder.first();
            charItem->setData(0, Qt::UserRole,
                              QStringLiteral("puppet:") + folder + QStringLiteral("|") + variant);
            charItem->setData(0, Qt::UserRole + 1, QVariant::fromValue<quint64>(0));
            charItem->setData(0, Qt::UserRole + 2, false);
            charItem->setFlags(charItem->flags() | Qt::ItemIsDragEnabled);
        } else {
            charItem->setFlags(charItem->flags() & ~Qt::ItemIsDragEnabled);
            for (const QString& variant : m.variantOrder) {
                auto* vItem = new QTreeWidgetItem(charItem);
                vItem->setText(0, variant);
                vItem->setData(0, Qt::UserRole,
                               QStringLiteral("puppet:") + folder + QStringLiteral("|") + variant);
                vItem->setData(0, Qt::UserRole + 1, QVariant::fromValue<quint64>(0));
                vItem->setData(0, Qt::UserRole + 2, false);
                vItem->setData(0, Qt::UserRole + 5, folder);
                vItem->setFlags(vItem->flags() | Qt::ItemIsDragEnabled);
                const QString face = m.variants.value(variant).faces[0];
                if (!face.isEmpty()) {
                    QPixmap pm(face);
                    if (!pm.isNull()) vItem->setIcon(0, QIcon(pm));
                }
            }
            charItem->setExpanded(true);
        }

        if (folder == prevSel) charItem->setSelected(true);
    }
}

QString PuppetLibraryPanel::selectedFolder() const
{
    if (!m_tree) return {};
    const auto items = m_tree->selectedItems();
    if (items.isEmpty()) return {};
    return items.first()->data(0, Qt::UserRole + 5).toString();
}

// ═════════════════════════════════════════════════════════════════════════════
// Editable cascade (CHARACTERS → PUPPETS page)
// ═════════════════════════════════════════════════════════════════════════════

void PuppetLibraryPanel::buildEditableUI()
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    const QString frameQss = QStringLiteral(
        "QFrame#PuppetCol { background: #232323; border: 1px solid #3a3a3a;"
        " border-radius: 6px; }");
    const QString titleQss = QStringLiteral(
        "font-weight: bold; color: #dcdcdc; border: none; background: transparent;");

    // Build one framed column (header + body layout). Returns the body layout so
    // the caller can fill it; the frame is already added to `root`.
    auto makeFrame = [&](const QString& title, int fixedW) -> QVBoxLayout* {
        auto* f = new QFrame;
        f->setObjectName(QStringLiteral("PuppetCol"));
        f->setStyleSheet(frameQss);
        if (fixedW > 0) f->setFixedWidth(fixedW);
        auto* v = new QVBoxLayout(f);
        v->setContentsMargins(6, 6, 6, 6);
        v->setSpacing(4);
        auto* lbl = new QLabel(title);
        lbl->setStyleSheet(titleQss);
        v->addWidget(lbl);
        root->addWidget(f);
        return v;
    };

    // Cascade columns (narrow, fixed width — sized like the SHOTS filter column).
    // Each: a stacked thumbnail list (icon fills column width, name below) + a
    // name text box and a "New …" button beneath it.
    auto makeCascade = [&](const QString& title, QListWidget*& list, QLineEdit*& edit,
                           const QString& btnLabel, int w, std::function<void()> onCreate) {
        auto* v = makeFrame(title, w);
        list = new QListWidget;
        setupThumbList(list);
        v->addWidget(list, 1);

        edit = new QLineEdit;
        edit->setPlaceholderText(tr("Name\xE2\x80\xA6"));
        edit->setClearButtonEnabled(true);
        v->addWidget(edit);

        auto* btn = new QPushButton(btnLabel);
        v->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [onCreate]() { onCreate(); });
        connect(edit, &QLineEdit::returnPressed, this, [onCreate]() { onCreate(); });
    };

    // All three cascade columns share one width so every entry box (Character,
    // Outfit, Action) renders at an identical size.
    constexpr int kCascadeW = 200;
    makeCascade(tr("Character"), m_charList, m_newCharEdit, tr("New Character"), kCascadeW,
        [this]() { const QString n = m_newCharEdit->text().trimmed();
                   if (!n.isEmpty()) { createCharacter(n); m_newCharEdit->clear(); } });
    makeCascade(tr("Outfit"), m_outfitList, m_newOutfitEdit, tr("New Outfit"), kCascadeW,
        [this]() { const QString n = m_newOutfitEdit->text().trimmed();
                   if (!n.isEmpty()) { createOutfit(n); m_newOutfitEdit->clear(); } });
    makeCascade(tr("Action"), m_actionList, m_newActionEdit, tr("New Action"), kCascadeW,
        [this]() { const QString n = m_newActionEdit->text().trimmed();
                   if (!n.isEmpty()) { createAction(n); m_newActionEdit->clear(); } });

    // Faces column — its own framed column, slots stacked vertically.
    {
        auto* v = makeFrame(tr("Faces"), 170);
        for (int i = 0; i < 4; ++i) {
            auto* slot = new FaceSlot(i);
            slot->onPick = [this](int idx, const QString& path) { setFaceImage(idx, path); };
            m_faceSlots[static_cast<size_t>(i)] = slot;
            v->addWidget(slot, 1);

            auto* cap = new QLabel(puppetlib::faceLabels()[static_cast<size_t>(i)]);
            cap->setWordWrap(true);
            cap->setStyleSheet(QStringLiteral(
                "font-size: 10px; color: #999; border: none; background: transparent;"));
            v->addWidget(cap);

            auto* addBtn = new QPushButton(tr("Add / Replace"));
            addBtn->setStyleSheet(QStringLiteral("font-size: 11px; padding: 2px;"));
            connect(addBtn, &QPushButton::clicked, this, [this, i]() {
                const QString f = QFileDialog::getOpenFileName(
                    this, tr("Choose face PNG"), QString(),
                    tr("Images (*.png *.webp *.jpg *.jpeg)"));
                if (!f.isEmpty()) setFaceImage(i, f);
            });
            v->addWidget(addBtn);
        }
    }

    // Preview — its own framed pane (stretches), Talking toggle pinned top-right.
    {
        auto* f = new QFrame;
        f->setObjectName(QStringLiteral("PuppetCol"));
        f->setStyleSheet(frameQss);
        auto* g = new QGridLayout(f);
        g->setContentsMargins(6, 6, 6, 6);
        g->setSpacing(4);

        auto* title = new QLabel(tr("Preview"));
        title->setStyleSheet(titleQss);
        g->addWidget(title, 0, 0, Qt::AlignLeft | Qt::AlignTop);

        auto* preview = new PuppetPreview(f);
        m_preview = preview;
        g->addWidget(preview, 1, 0);

        m_talkCheck = new QCheckBox(tr("Talking"), f);
        m_talkCheck->setStyleSheet(QStringLiteral(
            "QCheckBox { background: rgba(0,0,0,160); color: white; padding: 3px 8px;"
            " border-radius: 4px; }"));
        g->addWidget(m_talkCheck, 1, 0, Qt::AlignTop | Qt::AlignRight);

        g->setRowStretch(1, 1);
        root->addWidget(f, 1);
    }

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(m_charList, &QListWidget::currentRowChanged, this,
            [this](int) { if (!m_updating) onCharRowChanged(); });
    connect(m_outfitList, &QListWidget::currentRowChanged, this,
            [this](int) { if (!m_updating) onOutfitRowChanged(); });
    connect(m_actionList, &QListWidget::currentRowChanged, this,
            [this](int) { if (!m_updating) onActionRowChanged(); });

    connect(m_charList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto* item = m_charList->itemAt(pos);
        if (!item) return;
        const QString folder = item->data(Qt::UserRole).toString();
        QMenu menu(m_charList);
        QAction* ren = menu.addAction(tr("Rename…"));
        QAction* del = menu.addAction(tr("Delete Character"));
        QAction* c = menu.exec(m_charList->viewport()->mapToGlobal(pos));
        if (c == del) {
            if (QMessageBox::question(this, tr("Delete Character"),
                    tr("Delete puppet \"%1\" and all its images?").arg(item->text()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                deleteCharacter(folder);
        } else if (c == ren) {
            m_charList->editItem(item);
        }
    });
    connect(m_charList, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (m_updating || !item) return;
        const QString folder = item->data(Qt::UserRole).toString();
        const QString newName = item->text().trimmed();
        if (!folder.isEmpty() && !newName.isEmpty())
            renameCharacter(folder, newName);
    });

    connect(m_outfitList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto* item = m_outfitList->itemAt(pos);
        if (!item) return;
        QMenu menu(m_outfitList);
        QAction* ren = menu.addAction(tr("Rename…"));
        QAction* del = menu.addAction(tr("Delete Outfit"));
        QAction* c = menu.exec(m_outfitList->viewport()->mapToGlobal(pos));
        if (c == del) {
            if (QMessageBox::question(this, tr("Delete Outfit"),
                    tr("Delete outfit \"%1\" and all its actions?").arg(item->text()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                deleteOutfit(item->text());
        } else if (c == ren) {
            bool ok = false;
            const QString nn = QInputDialog::getText(this, tr("Rename Outfit"),
                tr("New outfit name:"), QLineEdit::Normal, item->text(), &ok);
            if (ok && !nn.trimmed().isEmpty())
                renameOutfit(item->text(), nn.trimmed());
        }
    });

    connect(m_actionList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        auto* item = m_actionList->itemAt(pos);
        if (!item) return;
        QMenu menu(m_actionList);
        QAction* ren = menu.addAction(tr("Rename…"));
        QAction* del = menu.addAction(tr("Delete Action"));
        QAction* c = menu.exec(m_actionList->viewport()->mapToGlobal(pos));
        if (c == del) {
            if (QMessageBox::question(this, tr("Delete Action"),
                    tr("Delete action \"%1\"?").arg(item->text()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                deleteAction(item->text());
        } else if (c == ren) {
            bool ok = false;
            const QString nn = QInputDialog::getText(this, tr("Rename Action"),
                tr("New action name:"), QLineEdit::Normal, item->text(), &ok);
            if (ok && !nn.trimmed().isEmpty())
                renameAction(item->text(), nn.trimmed());
        }
    });

    connect(m_talkCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (auto* pv = static_cast<PuppetPreview*>(m_preview)) pv->setTalking(on);
    });
}

// ── Column refreshers ────────────────────────────────────────────────────────

void PuppetLibraryPanel::refreshCharacters()
{
    if (!m_charList) return;
    m_updating = true;
    m_charList->clear();
    int selRow = -1, row = 0;
    for (const QString& folder : puppetlib::listPuppetFolders()) {
        PuppetManifest m;
        if (!puppetlib::load(folder, m)) continue;
        auto* it = new QListWidgetItem(m.displayName);
        it->setData(Qt::UserRole, folder);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
        if (!m.variantOrder.isEmpty())
            it->setIcon(variantIcon(m, m.variantOrder.first()));
        m_charList->addItem(it);
        if (folder == m_curFolder) selRow = row;
        ++row;
    }
    if (selRow < 0 && m_charList->count() > 0) selRow = 0;
    m_charList->setCurrentRow(selRow);
    m_curFolder = (selRow >= 0)
        ? m_charList->item(selRow)->data(Qt::UserRole).toString() : QString();
    m_updating = false;
    applyLetterFilter();   // keep any active A-Z filter after a rebuild
    refreshOutfits();
}

void PuppetLibraryPanel::refreshOutfits()
{
    if (!m_outfitList) return;
    m_updating = true;
    m_outfitList->clear();
    PuppetManifest m;
    const bool loaded = !m_curFolder.isEmpty() && puppetlib::load(m_curFolder, m);
    QStringList outfits = loaded ? puppetlib::listOutfits(m) : QStringList{};
    int selRow = -1, row = 0;
    for (const QString& o : outfits) {
        auto* it = new QListWidgetItem(o);
        // Thumbnail: resting face of this outfit's first action.
        const QStringList acts = puppetlib::listActions(m, o);
        if (!acts.isEmpty())
            it->setIcon(variantIcon(m, puppetlib::resolveVariantKey(m, o, acts.first())));
        m_outfitList->addItem(it);
        if (o == m_curOutfit) selRow = row;
        ++row;
    }
    if (selRow < 0 && m_outfitList->count() > 0) selRow = 0;
    m_outfitList->setCurrentRow(selRow);
    m_curOutfit = (selRow >= 0) ? m_outfitList->item(selRow)->text() : QString();
    m_updating = false;
    refreshActions();
}

void PuppetLibraryPanel::refreshActions()
{
    if (!m_actionList) return;
    m_updating = true;
    m_actionList->clear();
    PuppetManifest m;
    const bool loaded = !m_curFolder.isEmpty() && !m_curOutfit.isEmpty() &&
                        puppetlib::load(m_curFolder, m);
    QStringList actions = loaded ? puppetlib::listActions(m, m_curOutfit) : QStringList{};
    int selRow = -1, row = 0;
    for (const QString& a : actions) {
        auto* it = new QListWidgetItem(a);
        it->setIcon(variantIcon(m, puppetlib::resolveVariantKey(m, m_curOutfit, a)));
        m_actionList->addItem(it);
        if (a == m_curAction) selRow = row;
        ++row;
    }
    if (selRow < 0 && m_actionList->count() > 0) selRow = 0;
    m_actionList->setCurrentRow(selRow);
    m_curAction = (selRow >= 0) ? m_actionList->item(selRow)->text() : QString();
    m_updating = false;
    refreshFaces();
    reloadPreviewFaces();
}

void PuppetLibraryPanel::refreshFaces()
{
    PuppetManifest m;
    PuppetVariant var;
    const QString key = currentVariantKey();
    if (!key.isEmpty() && puppetlib::load(m_curFolder, m))
        var = m.variants.value(key);
    for (int i = 0; i < 4; ++i) {
        auto* slot = static_cast<FaceSlot*>(m_faceSlots[static_cast<size_t>(i)]);
        if (!slot) continue;
        const QString& fp = var.faces[static_cast<size_t>(i)];
        slot->setFacePixmap(fp.isEmpty() ? QPixmap() : QPixmap(fp));
    }
}

// ── A-Z letter filter (CHARACTERS-page side panel) ───────────────────────────

void PuppetLibraryPanel::filterByLetter(QChar letter)
{
    m_letterFilter = letter;
    applyLetterFilter();
}

void PuppetLibraryPanel::applyLetterFilter()
{
    if (!m_charList) return;
    const bool showAll  = m_letterFilter.isNull();
    const bool wantHash = (m_letterFilter == QChar('#'));
    const QChar up = m_letterFilter.toUpper();

    QListWidgetItem* firstVisible = nullptr;
    for (int i = 0; i < m_charList->count(); ++i) {
        auto* it = m_charList->item(i);
        if (!it) continue;
        bool match = true;
        if (!showAll) {
            const QString name = it->text().trimmed();
            const QChar f = name.isEmpty() ? QChar() : name.at(0).toUpper();
            match = wantHash ? !f.isLetter() : (f == up);
        }
        it->setHidden(!match);
        if (match && !firstVisible) firstVisible = it;
    }

    // If the current selection is hidden (or there's nothing visible at all),
    // move it to the first visible match — or clear it entirely when no row
    // matches — so the Outfit / Action / Faces columns and preview follow the
    // filter instead of showing stale content for a hidden character.
    QListWidgetItem* cur = m_charList->currentItem();
    if (!cur || cur->isHidden()) {
        m_charList->setCurrentItem(firstVisible);  // nullptr → clears selection
        if (firstVisible)
            m_charList->scrollToItem(firstVisible, QAbstractItemView::PositionAtTop);
        else
            onCharRowChanged();   // no match: blank out downstream columns now
    }
}

// ── User-driven selection slots ──────────────────────────────────────────────

void PuppetLibraryPanel::onCharRowChanged()
{
    auto* it = m_charList->currentItem();
    m_curFolder = it ? it->data(Qt::UserRole).toString() : QString();
    refreshOutfits();
}

void PuppetLibraryPanel::onOutfitRowChanged()
{
    auto* it = m_outfitList->currentItem();
    m_curOutfit = it ? it->text() : QString();
    refreshActions();
}

void PuppetLibraryPanel::onActionRowChanged()
{
    auto* it = m_actionList->currentItem();
    m_curAction = it ? it->text() : QString();
    refreshFaces();
    reloadPreviewFaces();
}

// ── Create ───────────────────────────────────────────────────────────────────

void PuppetLibraryPanel::createCharacter(const QString& name)
{
    QString folder = puppetlib::sanitizeFolderName(name);
    QStringList existing = puppetlib::listPuppetFolders();
    QString base = folder; int n = 2;
    while (existing.contains(folder)) folder = base + QString::number(n++);

    PuppetManifest m;
    m.folderName  = folder;
    m.displayName = name;
    const QString key = puppetlib::makeVariantKey(QStringLiteral("Default"),
                                                  QStringLiteral("Idle"));
    m.variantOrder << key;
    m.variants.insert(key, PuppetVariant{});
    if (!puppetlib::save(m)) return;

    m_curFolder = folder;
    m_curOutfit = QStringLiteral("Default");
    m_curAction = QStringLiteral("Idle");
    refreshCharacters();
    emit puppetsChanged();
}

void PuppetLibraryPanel::createOutfit(const QString& name)
{
    if (m_curFolder.isEmpty()) return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;
    const QString key = puppetlib::makeVariantKey(name, QStringLiteral("Idle"));
    if (!m.variants.contains(key)) {
        m.variantOrder << key;
        m.variants.insert(key, PuppetVariant{});
        if (!puppetlib::save(m)) return;
    }
    m_curOutfit = name;
    m_curAction = QStringLiteral("Idle");
    refreshOutfits();
    emit puppetsChanged();
}

void PuppetLibraryPanel::createAction(const QString& name)
{
    if (m_curFolder.isEmpty() || m_curOutfit.isEmpty()) return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;
    const QString key = puppetlib::makeVariantKey(m_curOutfit, name);
    if (!m.variants.contains(key)) {
        m.variantOrder << key;
        m.variants.insert(key, PuppetVariant{});
        if (!puppetlib::save(m)) return;
    }
    m_curAction = name;
    refreshActions();
    emit puppetsChanged();
}

// ── Delete / rename ──────────────────────────────────────────────────────────

void PuppetLibraryPanel::deleteCharacter(const QString& folder)
{
    puppetlib::remove(folder);
    if (folder == m_curFolder) { m_curFolder.clear(); m_curOutfit.clear(); m_curAction.clear(); }
    refreshCharacters();
    emit puppetsChanged();
}

void PuppetLibraryPanel::deleteOutfit(const QString& outfit)
{
    if (m_curFolder.isEmpty()) return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;

    QStringList keys;
    for (const QString& key : m.variantOrder) {
        QString o, a; puppetlib::splitVariantKey(key, o, a);
        if (o == outfit) keys << key;
    }
    if (keys.isEmpty()) return;

    for (const QString& key : keys) {
        QDir vdir(puppetlib::puppetsRootDir() + QStringLiteral("/") + m_curFolder +
                  QStringLiteral("/") + key);
        if (vdir.exists()) vdir.removeRecursively();
        m.variants.remove(key);
        m.variantOrder.removeAll(key);
    }

    if (m.variantOrder.isEmpty())
        puppetlib::remove(m_curFolder);
    else
        puppetlib::save(m);

    if (outfit == m_curOutfit) { m_curOutfit.clear(); m_curAction.clear(); }
    refreshCharacters();
    emit puppetsChanged();
}

void PuppetLibraryPanel::deleteAction(const QString& action)
{
    if (m_curFolder.isEmpty() || m_curOutfit.isEmpty()) return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;
    const QString key = puppetlib::resolveVariantKey(m, m_curOutfit, action);
    if (key.isEmpty()) return;
    puppetlib::removeVariant(m_curFolder, key);
    if (action == m_curAction) m_curAction.clear();
    refreshCharacters();
    emit puppetsChanged();
}

void PuppetLibraryPanel::renameCharacter(const QString& folder, const QString& newName)
{
    PuppetManifest m;
    if (!puppetlib::load(folder, m)) return;
    if (m.displayName == newName) return;
    m.displayName = newName;
    puppetlib::save(m);
    emit puppetsChanged();
}

void PuppetLibraryPanel::renameOutfit(const QString& oldName, const QString& newName)
{
    if (m_curFolder.isEmpty() || oldName == newName || newName.isEmpty()) return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;

    // Re-key every action under the old outfit (snapshot keys first).
    const QStringList acts = puppetlib::listActions(m, oldName);
    for (const QString& a : acts) {
        const QString oldKey = puppetlib::resolveVariantKey(m, oldName, a);
        const QString newKey = puppetlib::makeVariantKey(newName, a);
        puppetlib::renameVariantKey(m_curFolder, oldKey, newKey);
    }
    // Drop the now-empty old outfit folder.
    QDir(puppetlib::puppetsRootDir() + QStringLiteral("/") + m_curFolder).rmdir(oldName);

    if (m_curOutfit == oldName) m_curOutfit = newName;
    refreshCharacters();
    emit puppetsChanged();
}

void PuppetLibraryPanel::renameAction(const QString& oldName, const QString& newName)
{
    if (m_curFolder.isEmpty() || m_curOutfit.isEmpty() ||
        oldName == newName || newName.isEmpty())
        return;
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;
    const QString oldKey = puppetlib::resolveVariantKey(m, m_curOutfit, oldName);
    if (oldKey.isEmpty()) return;
    const QString newKey = puppetlib::makeVariantKey(m_curOutfit, newName);
    puppetlib::renameVariantKey(m_curFolder, oldKey, newKey);

    if (m_curAction == oldName) m_curAction = newName;
    refreshCharacters();
    emit puppetsChanged();
}

// ── Faces ────────────────────────────────────────────────────────────────────

void PuppetLibraryPanel::setFaceImage(int faceIndex, const QString& sourcePngPath)
{
    const QString key = currentVariantKey();
    if (key.isEmpty() || sourcePngPath.isEmpty()) return;

    const QString dest = puppetlib::importFaceImage(m_curFolder, key, faceIndex, sourcePngPath);
    if (dest.isEmpty()) return;

    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return;
    auto it = m.variants.find(key);
    if (it == m.variants.end())
        it = m.variants.insert(key, PuppetVariant{});
    it->faces[static_cast<size_t>(faceIndex)] = dest;
    puppetlib::save(m);

    // Full cascade refresh (preserves selection) so the Character/Outfit/Action
    // column thumbnails update too — they key off the resting face (index 0),
    // not just the face slots/preview.
    refreshCharacters();
    emit puppetsChanged();
}

// ── Live preview ─────────────────────────────────────────────────────────────

QString PuppetLibraryPanel::currentVariantKey() const
{
    if (m_curFolder.isEmpty() || m_curOutfit.isEmpty() || m_curAction.isEmpty())
        return {};
    PuppetManifest m;
    if (!puppetlib::load(m_curFolder, m)) return {};
    return puppetlib::resolveVariantKey(m, m_curOutfit, m_curAction);
}

void PuppetLibraryPanel::reloadPreviewFaces()
{
    auto* pv = static_cast<PuppetPreview*>(m_preview);
    if (!pv) return;

    std::array<QPixmap, 4> faces{};
    PuppetManifest m;
    const QString key = currentVariantKey();
    if (!key.isEmpty() && puppetlib::load(m_curFolder, m)) {
        const PuppetVariant var = m.variants.value(key);
        for (int i = 0; i < 4; ++i) {
            const QString& fp = var.faces[static_cast<size_t>(i)];
            if (!fp.isEmpty()) faces[static_cast<size_t>(i)] = QPixmap(fp);
        }
    }
    pv->setSeed(static_cast<uint32_t>(qHash(m_curFolder)) | 1u);
    pv->setFaces(faces);
    if (m_talkCheck) pv->setTalking(m_talkCheck->isChecked());
}

} // namespace rt
