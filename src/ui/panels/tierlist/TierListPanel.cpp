/*
 * TierListPanel.cpp — authoring UI for a TierListClip.
 *
 * See TierListPanel.h. Edits mutate the bound clip's entries()/events()
 * directly and emit tierListEdited() so the workspace re-composites the frame
 * live (same pattern as CaptionsPanel).
 */

#include "panels/tierlist/TierListPanel.h"

#include "timeline/TierListClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "Constants.h"
#include "PathUtils.h"   // utf8ToPath — Unicode-safe path handling
#include "Settings.h"    // rt::appSettings — shared "last import folder"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"
#include "Theme.h"       // themed bottom-bar styling (match the Project Bin)

#include <QDir>
#include <QToolButton>
#include <QSlider>
#include <QDialog>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSignalBlocker>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QMenu>
#include <QInputDialog>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QAbstractItemView>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QTimer>
#include <QPainter>
#include <QFontMetrics>
#include <QMimeData>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QIcon>
#include <QPixmap>
#include <QImage>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

// MIME payload for a pool-entry drag: the uint64 entry id as ASCII.
static const char* kTierEntryMime = "application/x-roundtable-tierentry";

// MIME payload for an intra-row reorder drag on the board: "tier,index".
static const char* kTierReorderMime = "application/x-roundtable-tierreorder";

// Item-data role holding the entry's subbin name (pool filtering).
static constexpr int kTlSubbinRole = Qt::UserRole + 2;

// Shared style for the Set Start / Set End buttons — clear hover so it's obvious
// which one the cursor is over.
static const char* kTlBtnStyle =
    "QPushButton{ background:#33343E; color:#DCDCE4; border:1px solid #45464F;"
    "             border-radius:3px; padding:2px 10px; }"
    "QPushButton:hover{ background:#4A6CD4; border-color:#6A8CF4; color:#FFFFFF; }"
    "QPushButton:pressed{ background:#3858B8; }";

// ── file-local helpers ──────────────────────────────────────────────────────

static QColor tlDecodeArgb(uint32_t c)
{
    return QColor(static_cast<int>((c >> 16) & 0xFF),
                  static_cast<int>((c >> 8)  & 0xFF),
                  static_cast<int>( c        & 0xFF),
                  static_cast<int>((c >> 24) & 0xFF));
}

static QString tlDisplayTitle(const std::string& title, uint64_t id)
{
    if (!title.empty()) return QString::fromStdString(title);
    return QStringLiteral("#%1").arg(static_cast<qulonglong>(id));
}

static QIcon tlEntryIcon(const QString& title, const std::string& imagePath)
{
    QImage im;
    if (!imagePath.empty())
        im.load(QString::fromStdWString(utf8ToPath(imagePath).wstring()));

    // 128px base so the pool can zoom up without going soft.
    QPixmap pm(128, 128);
    if (!im.isNull()) {
        QImage sc = im.scaled(128, 128, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        pm = QPixmap::fromImage(sc.copy((sc.width() - 128) / 2, (sc.height() - 128) / 2, 128, 128));
    } else {
        pm.fill(QColor(0x33, 0x33, 0x3E));
        QPainter p(&pm);
        p.setPen(QColor(0xC0, 0xC0, 0xC8));
        QFont f("Arial");
        f.setPixelSize(22);
        p.setFont(f);
        p.drawText(QRect(6, 6, 116, 116), Qt::AlignCenter | Qt::TextWordWrap, title.left(14));
        p.end();
    }
    return QIcon(pm);
}

// A board thumbnail (cached by path). Missing image → a labelled placeholder.
static QPixmap tlEntryPixmap(const QString& title, const std::string& imagePath)
{
    static std::unordered_map<std::string, QPixmap> s_cache;
    const std::string key = imagePath.empty()
                          ? ("#" + title.toStdString())   // placeholder per title
                          : imagePath;
    auto it = s_cache.find(key);
    if (it != s_cache.end()) return it->second;

    QImage im;
    if (!imagePath.empty())
        im.load(QString::fromStdWString(utf8ToPath(imagePath).wstring()));

    QPixmap pm;
    if (!im.isNull()) {
        pm = QPixmap::fromImage(im.scaledToHeight(96, Qt::SmoothTransformation));
    } else {
        pm = QPixmap(96, 96);
        pm.fill(QColor(0x33, 0x33, 0x3E));
        QPainter p(&pm);
        p.setPen(QColor(0xC8, 0xC8, 0xD0));
        QFont f("Arial");
        f.setPixelSize(13);
        p.setFont(f);
        p.drawText(QRect(3, 3, 90, 90), Qt::AlignCenter | Qt::TextWordWrap, title.left(14));
        p.end();
    }
    s_cache.emplace(key, pm);
    return pm;
}

static QString tlTc(int64_t ticks)
{
    if (ticks < 0) ticks = 0;
    const double s = static_cast<double>(ticks) / static_cast<double>(kTicksPerSecond);
    const int m = static_cast<int>(s) / 60;
    const double sec = s - m * 60;
    return QString("%1:%2").arg(m).arg(sec, 4, 'f', 1, QChar('0'));
}

static QFrame* tlSep()
{
    auto* f = new QFrame();
    f->setFrameShape(QFrame::VLine);
    f->setFrameShadow(QFrame::Plain);
    f->setStyleSheet("color:#3A3A44;");
    f->setFixedWidth(6);
    return f;
}

// ════════════════════════════════════════════════════════════════════════════
// TierListBoardWidget
// ════════════════════════════════════════════════════════════════════════════

TierListBoardWidget::TierListBoardWidget(QWidget* parent) : QWidget(parent)
{
    setAcceptDrops(true);
    setMinimumHeight(140);
}

void TierListBoardWidget::setBoard(std::vector<QString> labels,
                                   std::vector<QColor>  colors,
                                   std::vector<std::vector<QPixmap>> rows,
                                   std::vector<std::vector<uint64_t>> ids)
{
    m_labels = std::move(labels);
    m_colors = std::move(colors);
    m_rows   = std::move(rows);
    m_ids    = std::move(ids);
    m_rows.resize(m_labels.size());   // keep in lockstep with the tier count
    m_ids.resize(m_labels.size());
    update();
}

void TierListBoardWidget::setListTitle(const QString& title)
{
    if (m_title == title) return;
    m_title = title;
    update();
}

QSize TierListBoardWidget::minimumSizeHint() const { return QSize(240, 160); }

int TierListBoardWidget::tierAt(int y) const
{
    const int n = static_cast<int>(m_labels.size());
    if (n <= 0) return -1;
    const int t = static_cast<int>(static_cast<double>(y) / std::max(1.0, static_cast<double>(height()) / n));
    return std::clamp(t, 0, n - 1);
}

std::vector<QRectF> TierListBoardWidget::entryRects(int tier) const
{
    std::vector<QRectF> out;
    const int n = static_cast<int>(m_labels.size());
    if (tier < 0 || tier >= n || tier >= static_cast<int>(m_rows.size())) return out;

    const double sideW     = std::clamp(width() * 0.07, 18.0, 40.0);
    const double boardLeft = sideW;
    const double rowH      = static_cast<double>(height()) / n;
    const double labelW    = std::max(24.0, rowH * 0.9);
    const QRectF ccell(boardLeft + labelW, tier * rowH,
                       width() - boardLeft - labelW, rowH);
    const double pad = rowH * 0.14;
    const double ih  = std::max(4.0, rowH - 2.0 * pad);

    double x = ccell.left() + pad;
    for (const QPixmap& pm : m_rows[static_cast<size_t>(tier)]) {
        if (pm.isNull() || pm.height() <= 0) {
            out.push_back(QRectF());   // keeps indices aligned with the row
            continue;
        }
        const double iw = ih * (static_cast<double>(pm.width()) / pm.height());
        if (x + iw > ccell.right() - 4) break;   // overflow cutoff ("…")
        out.push_back(QRectF(x, ccell.top() + pad, iw, ih));
        x += iw + pad;
    }
    return out;
}

void TierListBoardWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(rect(), QColor(0x14, 0x14, 0x16));

    const int n = static_cast<int>(m_labels.size());
    if (n == 0) {
        p.setPen(QColor(0x80, 0x80, 0x88));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No tiers"));
        return;
    }

    // Left title sidebar (mirrors the render's vertical Impact side-label).
    const double sideW = std::clamp(width() * 0.07, 18.0, 40.0);
    const QRectF sidebar(0, 0, sideW, height());
    p.fillRect(sidebar, QColor(0, 0, 0));
    if (!m_title.isEmpty()) {
        p.save();
        p.translate(sidebar.center());
        p.rotate(-90.0);
        QFont tf("Impact");
        tf.setPixelSize(std::max(10, static_cast<int>(sideW * 0.62)));
        p.setFont(tf);
        p.setPen(QColor(255, 255, 255));
        const QRectF tr(-sidebar.height() * 0.5, -sidebar.width() * 0.5,
                        sidebar.height(), sidebar.width());
        p.drawText(tr, Qt::AlignCenter, m_title);
        p.restore();
    }

    const double boardLeft = sideW;
    const double rowH   = static_cast<double>(height()) / n;
    const double labelW = std::max(24.0, rowH * 0.9);

    for (int i = 0; i < n; ++i) {
        const QRectF row(boardLeft, i * rowH, width() - boardLeft, rowH);

        // Label cell + letter.
        const QRectF lcell(boardLeft, row.top(), labelW, rowH);
        p.fillRect(lcell, m_colors[static_cast<size_t>(i)]);
        {
            QFont f("Arial");
            f.setBold(true);
            f.setPixelSize(std::max(10, static_cast<int>(rowH * 0.42)));
            p.setFont(f);
            p.setPen(QColor(20, 20, 20));
            p.drawText(lcell, Qt::AlignCenter, m_labels[static_cast<size_t>(i)]);
        }

        // Content strip (highlighted when a drag hovers this tier).
        const QRectF ccell(lcell.right(), row.top(), row.right() - lcell.right(), rowH);
        p.fillRect(ccell, (i == m_hoverTier) ? QColor(0x2E, 0x2E, 0x3A) : QColor(0x1A, 0x1A, 0x17));

        // Entry thumbnails, drawn left→right (overflow shows "…").  Layout
        // comes from entryRects() so paint and reorder hit-testing agree.
        const double pad   = rowH * 0.14;
        const auto   rects = entryRects(i);
        const auto&  pms   = m_rows[static_cast<size_t>(i)];
        double lastRight = ccell.left() + pad;
        for (size_t k = 0; k < rects.size() && k < pms.size(); ++k) {
            const QRectF& tgt = rects[k];
            if (!tgt.isValid()) continue;
            p.drawPixmap(tgt, pms[k], QRectF(0, 0, pms[k].width(), pms[k].height()));
            p.setPen(QColor(255, 255, 255));   // white frame, matching the render
            p.setBrush(Qt::NoBrush);
            p.drawRect(tgt);
            lastRight = tgt.right() + pad;
        }
        if (rects.size() < pms.size()) {
            p.setPen(QColor(0x88, 0x88, 0x90));
            QFont ef("Arial");
            ef.setPixelSize(std::max(10, static_cast<int>(rowH * 0.3)));
            p.setFont(ef);
            p.drawText(QRectF(lastRight, ccell.top(), ccell.right() - lastRight - 2, rowH),
                       Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("…"));
        }

        if (i > 0) {
            p.setPen(QColor(0, 0, 0));
            p.drawLine(QPointF(boardLeft, row.top()), QPointF(width(), row.top()));
        }
    }
}

void TierListBoardWidget::mousePressEvent(QMouseEvent* e)
{
    // Press on a row thumbnail starts an intra-row reorder drag.
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    const int t = tierAt(static_cast<int>(e->position().y()));
    if (t < 0 || t >= static_cast<int>(m_ids.size())) { QWidget::mousePressEvent(e); return; }

    const auto rects = entryRects(t);
    for (size_t k = 0; k < rects.size() && k < m_ids[static_cast<size_t>(t)].size(); ++k) {
        if (!rects[k].isValid() || !rects[k].contains(e->position())) continue;
        auto* mime = new QMimeData();
        mime->setData(kTierReorderMime,
                      QByteArray::number(t) + ',' +
                      QByteArray::number(static_cast<qulonglong>(k)));
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        const QPixmap& pm = m_rows[static_cast<size_t>(t)][k];
        if (!pm.isNull())
            drag->setPixmap(pm.scaledToHeight(48, Qt::SmoothTransformation));
        drag->exec(Qt::MoveAction);
        return;
    }
    QWidget::mousePressEvent(e);
}

void TierListBoardWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(kTierEntryMime) ||
        e->mimeData()->hasFormat(kTierReorderMime))
        e->acceptProposedAction();
}

void TierListBoardWidget::dragMoveEvent(QDragMoveEvent* e)
{
    const bool reorder = e->mimeData()->hasFormat(kTierReorderMime);
    if (!reorder && !e->mimeData()->hasFormat(kTierEntryMime)) return;
    int t = tierAt(static_cast<int>(e->position().y()));
    if (reorder) {
        // Placement is final — a reorder can only land in its own row.
        const QList<QByteArray> parts = e->mimeData()->data(kTierReorderMime).split(',');
        if (parts.size() == 2 && t != parts[0].toInt()) t = -1;
    }
    if (t != m_hoverTier) { m_hoverTier = t; update(); }
    e->acceptProposedAction();
}

void TierListBoardWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    if (m_hoverTier != -1) { m_hoverTier = -1; update(); }
}

void TierListBoardWidget::dropEvent(QDropEvent* e)
{
    m_hoverTier = -1;
    update();

    // Intra-row reorder drop.
    if (e->mimeData()->hasFormat(kTierReorderMime)) {
        const QList<QByteArray> parts = e->mimeData()->data(kTierReorderMime).split(',');
        if (parts.size() == 2) {
            const int fromTier = parts[0].toInt();
            const int fromIdx  = parts[1].toInt();
            const int t = tierAt(static_cast<int>(e->position().y()));
            if (t == fromTier && onReorderEntry) {
                // Insertion slot = count of visible thumbnails whose centre
                // is left of the drop point.
                const auto rects = entryRects(t);
                int to = 0;
                for (const QRectF& r : rects)
                    if (r.isValid() && e->position().x() > r.center().x()) ++to;
                if (to > fromIdx) --to;   // removal shifts the row left first
                if (to != fromIdx) onReorderEntry(t, fromIdx, to);
            }
        }
        e->acceptProposedAction();
        return;
    }

    if (!e->mimeData()->hasFormat(kTierEntryMime)) return;
    bool ok = false;
    const qulonglong id = e->mimeData()->data(kTierEntryMime).toULongLong(&ok);
    const int t = tierAt(static_cast<int>(e->position().y()));
    if (ok && onDropEntry) onDropEntry(static_cast<uint64_t>(id), t);
    e->acceptProposedAction();
}

// ════════════════════════════════════════════════════════════════════════════
// TierListPoolListWidget
// ════════════════════════════════════════════════════════════════════════════

TierListPoolListWidget::TierListPoolListWidget(QWidget* parent) : QListWidget(parent) {}

void TierListPoolListWidget::startDrag(Qt::DropActions /*supportedActions*/)
{
    auto* it = currentItem();
    if (!it) return;
    const qulonglong id = it->data(kEntryIdRole).toULongLong();

    auto* mime = new QMimeData();
    mime->setData(kTierEntryMime, QByteArray::number(id));

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    if (!it->icon().isNull()) drag->setPixmap(it->icon().pixmap(QSize(64, 64)));
    drag->exec(Qt::CopyAction);
}

// ════════════════════════════════════════════════════════════════════════════
// TierListPoolTreeWidget — detail list view (Name / Rank / Used)
// ════════════════════════════════════════════════════════════════════════════

class TierListPoolTreeWidget : public QTreeWidget
{
public:
    explicit TierListPoolTreeWidget(QWidget* parent = nullptr) : QTreeWidget(parent) {}

protected:
    void startDrag(Qt::DropActions /*supportedActions*/) override
    {
        auto* it = currentItem();
        if (!it) return;
        const qulonglong id = it->data(0, TierListPoolListWidget::kEntryIdRole).toULongLong();
        auto* mime = new QMimeData();
        mime->setData(kTierEntryMime, QByteArray::number(id));
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        if (!it->icon(0).isNull()) drag->setPixmap(it->icon(0).pixmap(QSize(48, 48)));
        drag->exec(Qt::CopyAction);
    }
};

// Reveal a file in Explorer (highlighted), or open its folder if it's gone.
// Mirrors the Project Bin's "Reveal in Explorer".
static void tlReveal(const QString& path)
{
    if (path.isEmpty()) return;
    QFileInfo info(QFileInfo(path).absoluteFilePath());
    if (info.exists()) {
        const QString native = QDir::toNativeSeparators(info.canonicalFilePath());
        QProcess proc;
        proc.setProgram(QStringLiteral("explorer.exe"));
        proc.setNativeArguments(QStringLiteral("/select,\"") + native + QStringLiteral("\""));
        proc.startDetached();
    } else {
        const QString dir = info.absolutePath();
        if (QFileInfo::exists(dir))
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    }
}

// ════════════════════════════════════════════════════════════════════════════
// TierListPanel
// ════════════════════════════════════════════════════════════════════════════

TierListPanel::TierListPanel(QWidget* parent) : QWidget(parent)
{
    buildUI();
    showBound(false);
}

TierListPanel::~TierListPanel()
{
    if (m_timeline)
        m_timeline->removeObserver(this);
}

void TierListPanel::setTimeline(Timeline* timeline)
{
    if (m_timeline)
        m_timeline->removeObserver(this);
    m_timeline = timeline;
    if (m_timeline)
        m_timeline->addObserver(this);
}

void TierListPanel::onTimelineDestroyed(Timeline* tl)
{
    if (tl != m_timeline) return;
    // The timeline owns every clip/track — drop all cached pointers together.
    // Do NOT call removeObserver(): the observer list dies with the timeline.
    m_timeline = nullptr;
    m_clip     = nullptr;
    m_track    = nullptr;
    showBound(false);
}

void TierListPanel::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // Placeholder shown when no tier-list clip is selected.
    m_placeholder = new QWidget(this);
    {
        auto* pl = new QVBoxLayout(m_placeholder);
        auto* lbl = new QLabel(QStringLiteral(
            "Select a Tier List clip on the timeline to edit its entries and events."));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setWordWrap(true);
        lbl->setStyleSheet("color:#888;");
        pl->addStretch(1);
        pl->addWidget(lbl);
        pl->addStretch(1);
    }
    outer->addWidget(m_placeholder);

    // Content.
    m_content = new QWidget(this);
    auto* cl = new QHBoxLayout(m_content);
    cl->setContentsMargins(4, 4, 4, 4);
    auto* split = new QSplitter(Qt::Horizontal, m_content);

    // ── Left column: title + [board / pool] vertical splitter ──
    auto* left = new QWidget();
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(4);

    {
        auto* titleRow = new QHBoxLayout();
        auto* tlbl = new QLabel(QStringLiteral("List Title:"));
        tlbl->setStyleSheet("font-weight:bold; padding:2px;");
        m_titleEdit = new QLineEdit();
        m_titleEdit->setPlaceholderText(QStringLiteral("Vertical side label (e.g. TIER LIST)"));
        auto* gridBtn = new QPushButton(QStringLiteral("Grid…"));
        gridBtn->setStyleSheet(kTlBtnStyle);
        gridBtn->setToolTip(QStringLiteral("Tiers, entry shape, background and safe margins"));
        gridBtn->setCursor(Qt::PointingHandCursor);
        titleRow->addWidget(tlbl);
        titleRow->addWidget(m_titleEdit, 1);
        titleRow->addWidget(gridBtn);
        lv->addLayout(titleRow);
        connect(gridBtn, &QPushButton::clicked, this, [this]() { openGridDialog(); });
    }
    connect(m_titleEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_clip) return;
        const std::string nt = m_titleEdit->text().toUtf8().toStdString();
        if (nt == m_clip->title()) return;   // no-op (e.g. plain focus-out)
        auto before = snapshot();
        m_clip->setTitle(nt);
        pushUndo(QStringLiteral("Edit tier list title"), before);
        emit tierListEdited();
    });

    auto* leftSplit = new QSplitter(Qt::Vertical, left);

    m_board = new TierListBoardWidget(left);
    m_board->onDropEntry = [this](uint64_t id, int tier) { onEntryDroppedOnTier(id, tier); };
    m_board->onReorderEntry = [this](int tier, int from, int to) { onReorderInRow(tier, from, to); };
    leftSplit->addWidget(m_board);

    auto* poolWrap = new QWidget();
    auto* pv = new QVBoxLayout(poolWrap);
    pv->setContentsMargins(0, 0, 0, 0);
    pv->setSpacing(0);

    // Top toolbar: add + subbin filter + search.
    {
        auto* bar = new QHBoxLayout();
        bar->setContentsMargins(0, 0, 0, 3);
        m_addBtn = new QPushButton(QStringLiteral("+ Add Images…"));
        m_subbinFilter = new QComboBox();
        m_subbinFilter->setToolTip(QStringLiteral("Filter the pool by subbin"));
        m_subbinFilter->addItem(QStringLiteral("All Entries"), QString());
        m_search = new QLineEdit();
        m_search->setPlaceholderText(QStringLiteral("Search entries…"));
        m_search->setClearButtonEnabled(true);
        bar->addWidget(m_addBtn);
        bar->addWidget(m_subbinFilter);
        bar->addWidget(m_search, 1);
        pv->addLayout(bar);
    }

    m_pool = new TierListPoolListWidget(poolWrap);
    m_pool->setDragEnabled(true);
    m_pool->setDragDropMode(QAbstractItemView::DragOnly);
    m_pool->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pool->setContextMenuPolicy(Qt::CustomContextMenu);
    pv->addWidget(m_pool, 1);

    // Detail list view (Name / Rank / Used) — shown in list mode.
    m_poolTree = new TierListPoolTreeWidget(poolWrap);
    m_poolTree->setColumnCount(3);
    m_poolTree->setHeaderLabels({ QStringLiteral("Entry"),
                                  QStringLiteral("Rank"),
                                  QStringLiteral("Used") });
    m_poolTree->setRootIsDecorated(false);
    m_poolTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_poolTree->setDragEnabled(true);
    m_poolTree->setDragDropMode(QAbstractItemView::DragOnly);
    m_poolTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_poolTree->header()->setStretchLastSection(false);
    m_poolTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_poolTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_poolTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_poolTree->hide();
    pv->addWidget(m_poolTree, 1);

    // Bottom bar — matches the Project Bin: zoom slider + list/icon toggle,
    // right-aligned at the bottom of the image bin.
    {
        const QString smallBtnStyle = QStringLiteral(
            "QToolButton { background: transparent; border: none; color: %1; "
            "font-size: 12px; padding: 2px; border-radius: 3px; }"
            "QToolButton:hover { background: %2; color: %3; }"
            "QToolButton:checked { background: %4; color: %5; }")
            .arg(Theme::hex(Theme::colors().textTertiary))
            .arg(Theme::hex(Theme::colors().controlBgHover))
            .arg(Theme::hex(Theme::colors().textPrimary))
            .arg(Theme::hex(Theme::colors().accentSubtle))
            .arg(Theme::hex(Theme::colors().textBright));

        auto* bottomBar = new QWidget(poolWrap);
        bottomBar->setFixedHeight(26);
        bottomBar->setStyleSheet(QStringLiteral(
            "QWidget { background: %1; border-top: 1px solid %2; }")
            .arg(Theme::hex(Theme::colors().surface1))
            .arg(Theme::hex(Theme::colors().border)));
        auto* bl = new QHBoxLayout(bottomBar);
        bl->setContentsMargins(4, 0, 4, 0);
        bl->setSpacing(2);
        bl->addStretch(1);   // push controls to the RIGHT

        m_zoomSlider = new QSlider(Qt::Horizontal, bottomBar);
        m_zoomSlider->setRange(30, 300);
        m_zoomSlider->setValue(m_poolZoom);
        m_zoomSlider->setFixedWidth(120);
        m_zoomSlider->setFixedHeight(20);
        m_zoomSlider->setToolTip(QStringLiteral("Zoom"));
        m_zoomSlider->setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { background: %1; height: 4px; border-radius: 2px; }"
            "QSlider::handle:horizontal { background: %2; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
            "QSlider::handle:horizontal:hover { background: %3; }")
            .arg(Theme::hex(Theme::colors().border))
            .arg(Theme::hex(Theme::colors().textTertiary))
            .arg(Theme::hex(Theme::colors().textSecondary)));
        bl->addWidget(m_zoomSlider);

        m_listViewBtn = new QToolButton(bottomBar);
        m_listViewBtn->setText(QStringLiteral("☰"));   // ☰ list
        m_listViewBtn->setToolTip(QStringLiteral("List View"));
        m_listViewBtn->setFixedSize(22, 22);
        m_listViewBtn->setCheckable(true);
        m_listViewBtn->setStyleSheet(smallBtnStyle);
        bl->addWidget(m_listViewBtn);

        m_iconViewBtn = new QToolButton(bottomBar);
        m_iconViewBtn->setText(QStringLiteral("▦"));   // ▦ icon
        m_iconViewBtn->setToolTip(QStringLiteral("Icon View"));
        m_iconViewBtn->setFixedSize(22, 22);
        m_iconViewBtn->setCheckable(true);
        m_iconViewBtn->setStyleSheet(smallBtnStyle);
        bl->addWidget(m_iconViewBtn);

        pv->addWidget(bottomBar);
    }

    setPoolView(true);   // icon mode by default (syncs buttons + metrics)
    leftSplit->addWidget(poolWrap);
    leftSplit->setStretchFactor(0, 1);
    leftSplit->setStretchFactor(1, 1);
    lv->addWidget(leftSplit, 1);
    split->addWidget(left);

    // ── Right column: EVENTS rail ──
    auto* right = new QWidget();
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(4);
    auto* evTitle = new QLabel(QStringLiteral("EVENTS"));
    evTitle->setStyleSheet("font-weight:bold; padding:2px;");
    rv->addWidget(evTitle);
    auto* scroll = new QScrollArea(right);
    scroll->setWidgetResizable(true);
    m_eventsHost = new QWidget();
    m_eventsLayout = new QVBoxLayout(m_eventsHost);
    m_eventsLayout->setContentsMargins(2, 2, 2, 2);
    m_eventsLayout->setSpacing(3);
    m_eventsLayout->addStretch(1);
    scroll->setWidget(m_eventsHost);
    rv->addWidget(scroll, 1);
    split->addWidget(right);

    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    cl->addWidget(split);
    outer->addWidget(m_content);

    connect(m_addBtn, &QPushButton::clicked, this, [this]() { onAddImages(); });
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) { onSearch(t); });
    connect(m_subbinFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { onSearch(m_search ? m_search->text() : QString()); });
    connect(m_listViewBtn, &QToolButton::clicked, this, [this]() { setPoolView(false); });
    connect(m_iconViewBtn, &QToolButton::clicked, this, [this]() { setPoolView(true); });
    connect(m_zoomSlider, &QSlider::valueChanged, this, [this](int v) { onPoolZoom(v); });

    connect(m_pool, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* it = m_pool->itemAt(m_pool->viewport()->mapFrom(m_pool, pos));
        if (!it) return;
        const uint64_t id = it->data(TierListPoolListWidget::kEntryIdRole).toULongLong();
        showEntryContextMenu(id, m_pool->mapToGlobal(pos));
    });
    connect(m_poolTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* it = m_poolTree->itemAt(m_poolTree->viewport()->mapFrom(m_poolTree, pos));
        if (!it) return;
        const uint64_t id = it->data(0, TierListPoolListWidget::kEntryIdRole).toULongLong();
        showEntryContextMenu(id, m_poolTree->mapToGlobal(pos));
    });
}

void TierListPanel::showBound(bool bound)
{
    if (m_placeholder) m_placeholder->setVisible(!bound);
    if (m_content)     m_content->setVisible(bound);
}

void TierListPanel::setClip(Clip* clip, Track* track)
{
    m_track = track;
    TierListClip* tl = nullptr;
    if (clip && clip->clipType() == ClipType::TierList)
        tl = static_cast<TierListClip*>(clip);
    m_clip = tl;
    refresh();
}

void TierListPanel::refresh()
{
    const bool bound = (m_clip != nullptr);
    showBound(bound);
    if (!bound) return;
    if (m_titleEdit) {
        const QSignalBlocker block(m_titleEdit);   // don't fire editingFinished
        m_titleEdit->setText(QString::fromStdString(m_clip->title()));
    }
    rebuildPool();
    rebuildBoard();
    rebuildEvents();
}

uint64_t TierListPanel::nextEntryId() const
{
    uint64_t mx = 0;
    if (m_clip)
        for (const auto& e : m_clip->entries()) mx = std::max(mx, e.id);
    return mx + 1;
}

int64_t TierListPanel::playheadLocalTick() const
{
    if (!m_timeline || !m_clip) return 0;
    const int64_t t = m_timeline->playheadPosition() - m_clip->timelineIn();
    return t < 0 ? 0 : t;
}

void TierListPanel::normalizeEvents()
{
    if (!m_clip) return;
    auto& evs = m_clip->events();
    if (evs.empty()) return;

    // Walk events in start order; each event's start is clamped up to the
    // previous event's end (an event's start can never precede the previous
    // event's end), and each event's end is clamped up to its own start.
    std::vector<size_t> order(evs.size());
    for (size_t i = 0; i < evs.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return evs[a].start < evs[b].start; });

    {
        auto& first = evs[order[0]];
        if (first.end < first.start) first.end = first.start;
    }
    for (size_t k = 1; k < order.size(); ++k) {
        auto&       cur  = evs[order[k]];
        const auto& prev = evs[order[k - 1]];
        if (cur.start < prev.end)  cur.start = prev.end;   // start ≥ previous end
        if (cur.end   < cur.start) cur.end   = cur.start;  // end ≥ start
    }
}

// ── Undo/redo ───────────────────────────────────────────────────────────────

TierListPanel::Snapshot TierListPanel::snapshot() const
{
    Snapshot s;
    if (m_clip) {
        s.tiers       = m_clip->tiers();
        s.entries     = m_clip->entries();
        s.subbins     = m_clip->subbins();
        s.events      = m_clip->events();
        s.title       = m_clip->title();
        s.aspect      = m_clip->entryAspect();
        s.customW     = m_clip->customAspectW();
        s.customH     = m_clip->customAspectH();
        s.topMargin   = m_clip->topSafeMargin();
        s.rightMargin = m_clip->rightSafeMargin();
        s.bgColor     = m_clip->backgroundColor();
    }
    return s;
}

void TierListPanel::restoreSnapshot(TierListClip* clip, const Snapshot& s)
{
    if (!clip) return;
    clip->tiers()   = s.tiers;
    clip->entries() = s.entries;
    clip->subbins() = s.subbins;
    clip->events()  = s.events;
    clip->setTitle(s.title);
    clip->setEntryAspect(s.aspect);
    clip->setCustomAspect(s.customW, s.customH);
    clip->setTopSafeMargin(s.topMargin);
    clip->setRightSafeMargin(s.rightMargin);
    clip->setBackgroundColor(s.bgColor);
    if (clip == m_clip) refresh();   // repaint the panel if it's the bound clip
    emit tierListEdited();
}

TierListClip* TierListPanel::findTierListClipById(uint64_t clipId) const
{
    if (!m_timeline) return nullptr;
    for (size_t t = 0; t < m_timeline->trackCount(); ++t) {
        Track* tr = m_timeline->track(t);
        if (!tr) continue;
        const size_t idx = tr->findClipIndexById(clipId);
        if (idx == tr->clipCount()) continue;
        Clip* c = tr->clip(idx);
        if (c && c->clipType() == ClipType::TierList)
            return static_cast<TierListClip*>(c);
    }
    return nullptr;
}

void TierListPanel::pushUndo(const QString& label, const Snapshot& before)
{
    if (!m_commandStack || !m_clip) return;
    // Capture the clip ID, never the pointer: the clip can be deleted (and its
    // memory freed) while this command is still in the stack. Undo/redo then
    // resolves the id against the live timeline and no-ops if the clip is gone.
    const uint64_t clipId = m_clip->id();
    const Snapshot after = snapshot();           // current (post-edit) state
    const std::string desc = label.toStdString();
    m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
        desc,
        [this, clipId, after]()  { restoreSnapshot(findTierListClipById(clipId), after);  },   // redo
        [this, clipId, before]() { restoreSnapshot(findTierListClipById(clipId), before); }));  // undo
}

void TierListPanel::setPoolView(bool iconMode)
{
    m_poolIconMode = iconMode;
    if (m_pool)     m_pool->setVisible(iconMode);
    if (m_poolTree) m_poolTree->setVisible(!iconMode);
    applyPoolMetrics();
    if (m_iconViewBtn) m_iconViewBtn->setChecked(iconMode);
    if (m_listViewBtn) m_listViewBtn->setChecked(!iconMode);
}

void TierListPanel::applyPoolMetrics()
{
    const double z = std::clamp(m_poolZoom, 30, 300) / 100.0;
    if (m_poolTree) {
        const int ti = std::max(16, static_cast<int>(28 * z));
        m_poolTree->setIconSize(QSize(ti, ti));
    }
    if (!m_pool) return;
    m_pool->setMovement(QListView::Static);
    m_pool->setResizeMode(QListView::Adjust);
    if (m_poolIconMode) {
        const int icon = std::max(24, static_cast<int>(64 * z));
        m_pool->setViewMode(QListView::IconMode);
        m_pool->setIconSize(QSize(icon, icon));
        m_pool->setWordWrap(true);
        m_pool->setSpacing(6);
        const QSize hint(icon + 20, icon + 30);   // icon + label + padding
        for (int i = 0; i < m_pool->count(); ++i)
            m_pool->item(i)->setSizeHint(hint);
    } else {
        const int icon = std::max(16, static_cast<int>(28 * z));
        m_pool->setViewMode(QListView::ListMode);
        m_pool->setIconSize(QSize(icon, icon));
        m_pool->setWordWrap(false);
        m_pool->setSpacing(1);
        for (int i = 0; i < m_pool->count(); ++i)
            m_pool->item(i)->setSizeHint(QSize());   // default → row follows iconSize
    }
}

void TierListPanel::onPoolZoom(int percent)
{
    m_poolZoom = percent;
    applyPoolMetrics();
}

std::vector<std::vector<uint64_t>> TierListPanel::computeFinalRows() const
{
    const int n = m_clip ? static_cast<int>(m_clip->tiers().size()) : 0;
    std::vector<std::vector<uint64_t>> rows(static_cast<size_t>(std::max(0, n)));
    if (!m_clip) return rows;

    std::vector<TierEvent> evs = m_clip->events();
    std::stable_sort(evs.begin(), evs.end(),
                     [](const TierEvent& a, const TierEvent& b) { return a.start < b.start; });

    auto removeId = [&](uint64_t id) {
        for (auto& r : rows) r.erase(std::remove(r.begin(), r.end(), id), r.end());
    };
    for (const auto& e : evs) {
        if (e.type == TierEventType::Drop && e.tier >= 0 && e.tier < n) {
            removeId(e.entryId);
            auto& r = rows[static_cast<size_t>(e.tier)];
            int idx = e.index;
            if (idx < 0 || idx > static_cast<int>(r.size())) idx = static_cast<int>(r.size());
            r.insert(r.begin() + idx, e.entryId);
        } else if (e.type == TierEventType::Reorder && e.tier >= 0 && e.tier < n) {
            auto& r = rows[static_cast<size_t>(e.tier)];
            const int from = e.fromIndex;
            if (from >= 0 && from < static_cast<int>(r.size())) {
                const uint64_t v = r[static_cast<size_t>(from)];
                r.erase(r.begin() + from);
                int to = e.toIndex;
                if (to < 0) to = 0;
                if (to > static_cast<int>(r.size())) to = static_cast<int>(r.size());
                r.insert(r.begin() + to, v);
            }
        }
    }
    return rows;
}

void TierListPanel::rebuildPool()
{
    if (!m_pool || !m_clip) return;

    // Per-entry status: which tier it's finally dropped into (if any) and
    // whether it's referenced by any event at all.
    std::unordered_map<uint64_t, int> finalTier;
    const auto rows = computeFinalRows();
    for (size_t t = 0; t < rows.size(); ++t)
        for (uint64_t id : rows[t]) finalTier[id] = static_cast<int>(t);
    std::unordered_set<uint64_t> used;
    for (const auto& ev : m_clip->events()) used.insert(ev.entryId);

    m_pool->clear();
    if (m_poolTree) m_poolTree->clear();

    // Refresh the subbin filter, preserving the current selection by name.
    if (m_subbinFilter) {
        const QString keep = m_subbinFilter->currentData().toString();
        const QSignalBlocker block(m_subbinFilter);
        m_subbinFilter->clear();
        m_subbinFilter->addItem(QStringLiteral("All Entries"), QString());
        for (const auto& s : m_clip->subbins()) {
            const QString name = QString::fromStdString(s);
            m_subbinFilter->addItem(name, name);
            if (name == keep) m_subbinFilter->setCurrentIndex(m_subbinFilter->count() - 1);
        }
    }

    for (const auto& e : m_clip->entries()) {
        const QString title  = tlDisplayTitle(e.title, e.id);
        const QIcon   icon   = tlEntryIcon(title, e.imagePath);
        const QString subbin = QString::fromStdString(e.subbin);

        auto* it = new QListWidgetItem(icon, title);
        it->setData(TierListPoolListWidget::kEntryIdRole, static_cast<qulonglong>(e.id));
        it->setData(kTlSubbinRole, subbin);
        it->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        if (!subbin.isEmpty()) it->setToolTip(QStringLiteral("Subbin: %1").arg(subbin));
        m_pool->addItem(it);

        if (!m_poolTree) continue;
        auto* row = new QTreeWidgetItem(m_poolTree);
        row->setIcon(0, icon);
        row->setText(0, title);
        row->setData(0, TierListPoolListWidget::kEntryIdRole, static_cast<qulonglong>(e.id));
        row->setData(0, kTlSubbinRole, subbin);

        // Rank column — the tier the entry sits in, filled with its colour.
        const auto rit = finalTier.find(e.id);
        if (rit != finalTier.end() && rit->second < static_cast<int>(m_clip->tiers().size())) {
            const auto& tier = m_clip->tiers()[static_cast<size_t>(rit->second)];
            row->setText(1, QString::fromStdString(tier.label));
            row->setBackground(1, tlDecodeArgb(tier.color));
            row->setForeground(1, QColor(0x11, 0x11, 0x11));
        } else {
            row->setText(1, QStringLiteral("—"));
            row->setForeground(1, QColor(0x77, 0x77, 0x80));
        }
        row->setTextAlignment(1, Qt::AlignCenter);

        // Used indicator.
        const bool isUsed = used.count(e.id) > 0;
        row->setText(2, isUsed ? QStringLiteral("✓") : QString());
        row->setTextAlignment(2, Qt::AlignCenter);
        if (isUsed) row->setForeground(2, QColor(0x4C, 0xC2, 0x6A));
        row->setToolTip(2, isUsed ? QStringLiteral("Used in the events")
                                  : QStringLiteral("Not used yet"));
    }
    applyPoolMetrics();   // icon size + item hints for the current mode/zoom
    onSearch(m_search ? m_search->text() : QString());
}

void TierListPanel::rebuildBoard()
{
    if (!m_board || !m_clip) return;
    std::vector<QString> labels;
    std::vector<QColor>  colors;
    for (const auto& t : m_clip->tiers()) {
        labels.push_back(QString::fromStdString(t.label));
        colors.push_back(tlDecodeArgb(t.color));
    }
    const auto finalRows = computeFinalRows();
    std::vector<std::vector<QPixmap>> rows(labels.size());
    for (size_t i = 0; i < finalRows.size() && i < rows.size(); ++i)
        for (uint64_t id : finalRows[i]) {
            const TierEntry* e = m_clip->entryById(id);
            const QString title = e ? tlDisplayTitle(e->title, id) : QStringLiteral("?");
            rows[i].push_back(tlEntryPixmap(title, e ? e->imagePath : std::string()));
        }
    m_board->setListTitle(QString::fromStdString(m_clip->title()));
    m_board->setBoard(std::move(labels), std::move(colors), std::move(rows), finalRows);
}

QWidget* TierListPanel::buildEventRow(size_t eventIndex)
{
    const TierEvent& ev = m_clip->events()[eventIndex];
    const bool isDrop    = (ev.type == TierEventType::Drop);
    const bool isReorder = (ev.type == TierEventType::Reorder);

    auto* row = new QWidget();
    row->setObjectName(QStringLiteral("tlEventRow"));
    row->setAttribute(Qt::WA_StyledBackground, true);
    row->setStyleSheet(QStringLiteral("#tlEventRow{ background:#202028; border-radius:4px; }"));
    row->setMinimumHeight(40);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 7, 8, 7);
    h->setSpacing(6);

    auto* type = new QLabel(isDrop ? QStringLiteral("DROP")
                                   : isReorder ? QStringLiteral("MOVE")
                                               : QStringLiteral("POPUP"));
    type->setStyleSheet(QStringLiteral(
        "background:%1; color:white; padding:2px 6px; border-radius:3px; font-weight:bold;")
        .arg(isDrop ? QStringLiteral("#2F7D46")
                    : isReorder ? QStringLiteral("#A8742F")
                                : QStringLiteral("#4A5AA8")));
    h->addWidget(type);

    QString nameText;
    if (isReorder) {
        const QString tierLbl =
            (ev.tier >= 0 && ev.tier < static_cast<int>(m_clip->tiers().size()))
                ? QString::fromStdString(m_clip->tiers()[static_cast<size_t>(ev.tier)].label)
                : QStringLiteral("?");
        nameText = QStringLiteral("%1 row: %2 → %3")
                       .arg(tierLbl).arg(ev.fromIndex + 1).arg(ev.toIndex + 1);
    } else {
        const TierEntry* e = m_clip->entryById(ev.entryId);
        nameText = e ? tlDisplayTitle(e->title, ev.entryId)
                     : QStringLiteral("#%1").arg(static_cast<qulonglong>(ev.entryId));
    }
    auto* name = new QLabel(nameText);
    name->setMinimumWidth(60);
    h->addWidget(name, 1);

    // Ranking (drops only) — placed LEFT of Set Start so the Set Start/End
    // buttons and their times line up vertically across every row. The combo is
    // colour-coded to the tier's colour to match the tier list.
    if (isDrop) {
        auto* combo = new QComboBox();
        for (const auto& t : m_clip->tiers())
            combo->addItem(QString::fromStdString(t.label));
        combo->setCurrentIndex(std::clamp(ev.tier, 0, static_cast<int>(m_clip->tiers().size()) - 1));
        combo->setMinimumHeight(26);
        combo->setMinimumWidth(46);
        combo->setToolTip(QStringLiteral("Tier"));
        h->addWidget(combo);

        auto applyComboColor = [this, combo]() {
            const int t = combo->currentIndex();
            if (!m_clip || t < 0 || t >= static_cast<int>(m_clip->tiers().size())) return;
            const QColor c = tlDecodeArgb(m_clip->tiers()[static_cast<size_t>(t)].color);
            combo->setStyleSheet(QStringLiteral(
                "QComboBox{ background:%1; color:#111111; font-weight:bold;"
                "           border:1px solid #00000055; border-radius:3px; padding:1px 6px; }"
                "QComboBox::drop-down{ border:none; width:14px; }").arg(c.name()));
        };
        applyComboColor();

        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, eventIndex, applyComboColor](int t) {
            if (!m_clip || eventIndex >= m_clip->events().size()) return;
            applyComboColor();
            if (m_clip->events()[eventIndex].tier == t) return;
            auto before = snapshot();
            m_clip->events()[eventIndex].tier = t;
            pushUndo(QStringLiteral("Change tier"), before);
            rebuildBoard();
            emit tierListEdited();
        });
    }

    h->addWidget(tlSep());

    // Set Start · start time · divider · Set End · end time.
    auto* setStart  = new QPushButton(QStringLiteral("Set Start"));
    auto* startTime = new QLabel(tlTc(ev.start));
    auto* setEnd    = new QPushButton(QStringLiteral("Set End"));
    auto* endTime   = new QLabel(tlTc(ev.end));
    for (QPushButton* b : { setStart, setEnd }) {
        b->setMinimumHeight(26);
        b->setStyleSheet(kTlBtnStyle);
        b->setCursor(Qt::PointingHandCursor);
    }
    for (QLabel* t : { startTime, endTime }) {
        t->setStyleSheet("color:#AAB0C0;");
        t->setMinimumWidth(42);
    }
    h->addWidget(setStart);
    h->addWidget(startTime);
    h->addWidget(tlSep());
    h->addWidget(setEnd);
    h->addWidget(endTime);

    connect(setStart, &QPushButton::clicked, this, [this, eventIndex, startTime, endTime]() {
        if (!m_clip || eventIndex >= m_clip->events().size()) return;
        auto before = snapshot();
        m_clip->events()[eventIndex].start = playheadLocalTick();
        normalizeEvents();   // clamp start up to the previous event's end
        pushUndo(QStringLiteral("Set event start"), before);
        const auto& ev2 = m_clip->events()[eventIndex];
        startTime->setText(tlTc(ev2.start));
        endTime->setText(tlTc(ev2.end));
        // Later events may have been pushed forward — refresh the whole rail.
        QTimer::singleShot(0, this, [this]() { rebuildEvents(); rebuildBoard(); });
        emit tierListEdited();
    });
    connect(setEnd, &QPushButton::clicked, this, [this, eventIndex, startTime, endTime]() {
        if (!m_clip || eventIndex >= m_clip->events().size()) return;
        auto before = snapshot();
        auto& ev2 = m_clip->events()[eventIndex];
        ev2.end = playheadLocalTick();
        if (ev2.end < ev2.start) ev2.end = ev2.start;
        normalizeEvents();   // following events keep start ≥ this new end
        pushUndo(QStringLiteral("Set event end"), before);
        const auto& ev3 = m_clip->events()[eventIndex];
        startTime->setText(tlTc(ev3.start));
        endTime->setText(tlTc(ev3.end));
        QTimer::singleShot(0, this, [this]() { rebuildEvents(); rebuildBoard(); });
        emit tierListEdited();
    });

    // Delete — a centred "X" at the far right.
    h->addWidget(tlSep());
    auto* del = new QPushButton(QStringLiteral("X"));
    del->setFixedSize(26, 26);
    del->setCursor(Qt::PointingHandCursor);
    del->setToolTip(QStringLiteral("Delete event"));
    del->setStyleSheet(
        "QPushButton{ background:#33343E; color:#CCC; border:1px solid #45464F; border-radius:3px;"
        "             font-weight:bold; padding:0px; text-align:center; }"
        "QPushButton:hover{ background:#C0453C; border-color:#D9564C; color:white; }"
        "QPushButton:pressed{ background:#A03A32; }");
    h->addWidget(del, 0, Qt::AlignVCenter);
    connect(del, &QPushButton::clicked, this, [this, eventIndex]() {
        if (!m_clip || eventIndex >= m_clip->events().size()) return;
        auto before = snapshot();
        m_clip->events().erase(m_clip->events().begin() + static_cast<ptrdiff_t>(eventIndex));
        pushUndo(QStringLiteral("Delete event"), before);
        // Defer the rebuild — we're inside the deleted button's own click handler.
        QTimer::singleShot(0, this, [this]() { rebuildEvents(); rebuildBoard(); });
        emit tierListEdited();
    });

    return row;
}

void TierListPanel::rebuildEvents()
{
    if (!m_eventsLayout || !m_clip) return;

    // Clear existing rows (and the trailing stretch).
    while (QLayoutItem* item = m_eventsLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // Display in start order, but each row edits by its real index in events().
    const size_t count = m_clip->events().size();
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    const auto& evs = m_clip->events();
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return evs[a].start < evs[b].start; });

    if (count == 0) {
        auto* hint = new QLabel(QStringLiteral(
            "Drag an entry from the pool onto a tier row to create a Popup + Drop."));
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#777; padding:6px;");
        m_eventsLayout->addWidget(hint);
    } else {
        for (size_t oi : order)
            m_eventsLayout->addWidget(buildEventRow(oi));
    }
    m_eventsLayout->addStretch(1);
}

void TierListPanel::onAddImages()
{
    if (!m_clip) return;

    // Start in the last folder used (shared with the Project Bin's import).
    auto settings = rt::appSettings();
    QString lastDir = settings.value("Import/lastDir", QString()).toString();
    if (lastDir.isEmpty() || !QDir(lastDir).exists())
        lastDir = QDir::homePath();

    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Add Tier List Entries"), lastDir,
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff);;All files (*)"));
    if (files.isEmpty()) return;

    settings.setValue("Import/lastDir", QFileInfo(files.first()).absolutePath());
    settings.sync();

    auto before = snapshot();
    for (const QString& f : files) {
        TierEntry e;
        e.id        = nextEntryId();
        e.imagePath = f.toUtf8().toStdString();
        e.title     = QFileInfo(f).completeBaseName().toUtf8().toStdString();
        m_clip->entries().push_back(e);
    }
    pushUndo(QStringLiteral("Add entries"), before);
    rebuildPool();
    emit tierListEdited();
}

void TierListPanel::onSearch(const QString& text)
{
    const bool    noText = text.isEmpty();
    const QString subbin = m_subbinFilter ? m_subbinFilter->currentData().toString()
                                          : QString();
    const bool    noBin  = subbin.isEmpty();
    if (m_pool)
        for (int i = 0; i < m_pool->count(); ++i) {
            auto* it = m_pool->item(i);
            const bool matchText = noText || it->text().contains(text, Qt::CaseInsensitive);
            const bool matchBin  = noBin  || it->data(kTlSubbinRole).toString() == subbin;
            it->setHidden(!(matchText && matchBin));
        }
    if (m_poolTree)
        for (int i = 0; i < m_poolTree->topLevelItemCount(); ++i) {
            auto* it = m_poolTree->topLevelItem(i);
            const bool matchText = noText || it->text(0).contains(text, Qt::CaseInsensitive);
            const bool matchBin  = noBin  || it->data(0, kTlSubbinRole).toString() == subbin;
            it->setHidden(!(matchText && matchBin));
        }
}

void TierListPanel::showEntryContextMenu(uint64_t entryId, const QPoint& globalPos)
{
    if (!m_clip) return;
    const TierEntry* e = m_clip->entryById(entryId);
    if (!e) return;
    const QString path = QString::fromStdString(e->imagePath);

    QMenu menu;
    QAction* reveal = menu.addAction(QStringLiteral("Reveal in Explorer"));
    reveal->setEnabled(!path.isEmpty());
    menu.addSeparator();

    // Move to Subbin ▸  (existing subbins + "(None)" + "New Subbin…")
    QMenu* sub = menu.addMenu(QStringLiteral("Move to Subbin"));
    QAction* subNone = sub->addAction(QStringLiteral("(None)"));
    subNone->setCheckable(true);
    subNone->setChecked(e->subbin.empty());
    std::vector<std::pair<QAction*, QString>> subActs;
    if (!m_clip->subbins().empty()) sub->addSeparator();
    for (const auto& s : m_clip->subbins()) {
        const QString nm = QString::fromStdString(s);
        QAction* a = sub->addAction(nm);
        a->setCheckable(true);
        a->setChecked(e->subbin == s);
        subActs.emplace_back(a, nm);
    }
    sub->addSeparator();
    QAction* subNew = sub->addAction(QStringLiteral("New Subbin…"));

    menu.addSeparator();
    QAction* rename = menu.addAction(QStringLiteral("Rename…"));
    QAction* remove = menu.addAction(QStringLiteral("Remove from Pool"));

    QAction* chosen = menu.exec(globalPos);
    if (!chosen)               return;
    if (chosen == reveal)      tlReveal(path);
    else if (chosen == rename) renameEntry(entryId);
    else if (chosen == remove) removeEntry(entryId);
    else if (chosen == subNone) moveEntryToSubbin(entryId, QString());
    else if (chosen == subNew) {
        bool ok = false;
        const QString nm = QInputDialog::getText(
            this, QStringLiteral("New Subbin"), QStringLiteral("Subbin name:"),
            QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !nm.isEmpty()) moveEntryToSubbin(entryId, nm);
    } else {
        for (const auto& [a, nm] : subActs)
            if (chosen == a) { moveEntryToSubbin(entryId, nm); break; }
    }
}

void TierListPanel::moveEntryToSubbin(uint64_t entryId, const QString& subbin)
{
    if (!m_clip) return;
    for (auto& en : m_clip->entries()) {
        if (en.id != entryId) continue;
        const std::string s = subbin.toUtf8().toStdString();
        if (en.subbin == s) return;   // no-op
        auto before = snapshot();
        if (!s.empty()) {
            auto& bins = m_clip->subbins();
            if (std::find(bins.begin(), bins.end(), s) == bins.end())
                bins.push_back(s);
        }
        en.subbin = s;
        pushUndo(QStringLiteral("Move entry to subbin"), before);
        rebuildPool();
        emit tierListEdited();
        return;
    }
}

void TierListPanel::onReorderInRow(int tier, int fromIndex, int toIndex)
{
    if (!m_clip) return;
    if (tier < 0 || fromIndex < 0 || toIndex < 0 || fromIndex == toIndex) return;

    auto before = snapshot();
    TierEvent ev;
    ev.type      = TierEventType::Reorder;
    ev.tier      = tier;
    ev.fromIndex = fromIndex;
    ev.toIndex   = toIndex;
    ev.start     = playheadLocalTick();   // instant by default; Set End for an
    ev.end       = ev.start;              // animated slide
    m_clip->events().push_back(ev);
    pushUndo(QStringLiteral("Reorder entries"), before);
    rebuildEvents();
    rebuildBoard();
    emit tierListEdited();
}

void TierListPanel::openGridDialog()
{
    if (!m_clip) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Tier List Grid"));
    dlg.setMinimumWidth(400);
    auto* outer = new QVBoxLayout(&dlg);

    // ── Tiers editor (working copy; original index kept so events can be
    //    remapped/dropped when tiers are removed) ──
    struct RowState { int orig; TierDef def; };
    std::vector<RowState> rows;
    for (size_t i = 0; i < m_clip->tiers().size(); ++i)
        rows.push_back({ static_cast<int>(i), m_clip->tiers()[i] });

    auto* tiersLabel = new QLabel(QStringLiteral("Tiers"), &dlg);
    tiersLabel->setStyleSheet("font-weight:bold;");
    outer->addWidget(tiersLabel);

    auto* tiersHost   = new QWidget(&dlg);
    auto* tiersLayout = new QVBoxLayout(tiersHost);
    tiersLayout->setContentsMargins(0, 0, 0, 0);
    tiersLayout->setSpacing(3);
    outer->addWidget(tiersHost);

    auto swatchStyle = [](uint32_t argb) {
        return QStringLiteral("background:%1; border:1px solid #00000066; border-radius:3px;")
            .arg(tlDecodeArgb(argb).name());
    };

    std::function<void()> rebuildTiers;
    rebuildTiers = [&]() {
        while (QLayoutItem* item = tiersLayout->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            auto* rowW = new QWidget(tiersHost);
            auto* h = new QHBoxLayout(rowW);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(4);

            auto* colorBtn = new QPushButton(rowW);
            colorBtn->setFixedSize(34, 22);
            colorBtn->setCursor(Qt::PointingHandCursor);
            colorBtn->setToolTip(QStringLiteral("Tier colour"));
            colorBtn->setStyleSheet(swatchStyle(rows[i].def.color));
            QObject::connect(colorBtn, &QPushButton::clicked, rowW, [&, i, colorBtn]() {
                if (i >= rows.size()) return;
                const QColor nc = QColorDialog::getColor(
                    tlDecodeArgb(rows[i].def.color), colorBtn, QStringLiteral("Tier Colour"));
                if (!nc.isValid()) return;
                rows[i].def.color = 0xFF000000u
                                  | (static_cast<uint32_t>(nc.red())   << 16)
                                  | (static_cast<uint32_t>(nc.green()) << 8)
                                  |  static_cast<uint32_t>(nc.blue());
                colorBtn->setStyleSheet(swatchStyle(rows[i].def.color));
            });
            h->addWidget(colorBtn);

            auto* labelEdit = new QLineEdit(QString::fromStdString(rows[i].def.label), rowW);
            QObject::connect(labelEdit, &QLineEdit::textChanged, rowW, [&, i](const QString& t) {
                if (i < rows.size()) rows[i].def.label = t.toUtf8().toStdString();
            });
            h->addWidget(labelEdit, 1);

            auto* del = new QPushButton(QStringLiteral("X"), rowW);
            del->setFixedSize(22, 22);
            del->setToolTip(QStringLiteral("Remove tier (its drops are removed too)"));
            del->setEnabled(rows.size() > 1);
            QObject::connect(del, &QPushButton::clicked, rowW, [&, i]() {
                if (i >= rows.size() || rows.size() <= 1) return;
                rows.erase(rows.begin() + static_cast<ptrdiff_t>(i));
                rebuildTiers();
            });
            h->addWidget(del);

            tiersLayout->addWidget(rowW);
        }
    };
    rebuildTiers();

    auto* addTier = new QPushButton(QStringLiteral("+ Add Tier"), &dlg);
    QObject::connect(addTier, &QPushButton::clicked, &dlg, [&]() {
        rows.push_back({ -1, TierDef{ "NEW", 0xFFCCCCCCu } });
        rebuildTiers();
    });
    outer->addWidget(addTier);

    // ── Entry shape / background / safe margins ──
    auto* form = new QFormLayout();
    form->setContentsMargins(0, 8, 0, 0);

    auto* aspectCombo = new QComboBox(&dlg);
    aspectCombo->addItems({ QStringLiteral("Banner (16:9)"),
                            QStringLiteral("Square (1:1)"),
                            QStringLiteral("Vertical (9:16)"),
                            QStringLiteral("Custom…") });
    aspectCombo->setCurrentIndex(static_cast<int>(m_clip->entryAspect()));
    form->addRow(QStringLiteral("Entry shape:"), aspectCombo);

    auto* customRow = new QWidget(&dlg);
    auto* ch = new QHBoxLayout(customRow);
    ch->setContentsMargins(0, 0, 0, 0);
    auto* wSpin = new QDoubleSpinBox(customRow);
    wSpin->setRange(0.1, 100.0);
    wSpin->setDecimals(1);
    wSpin->setValue(m_clip->customAspectW());
    auto* hSpin = new QDoubleSpinBox(customRow);
    hSpin->setRange(0.1, 100.0);
    hSpin->setDecimals(1);
    hSpin->setValue(m_clip->customAspectH());
    ch->addWidget(wSpin);
    ch->addWidget(new QLabel(QStringLiteral(":"), customRow));
    ch->addWidget(hSpin);
    form->addRow(QStringLiteral("Custom w:h:"), customRow);
    auto syncCustom = [aspectCombo, customRow]() {
        customRow->setEnabled(aspectCombo->currentIndex()
                              == static_cast<int>(TierEntryAspect::Custom));
    };
    syncCustom();
    QObject::connect(aspectCombo, &QComboBox::currentIndexChanged, &dlg,
                     [syncCustom](int) { syncCustom(); });

    uint32_t bg = m_clip->backgroundColor();
    auto* bgBtn = new QPushButton(&dlg);
    bgBtn->setFixedSize(48, 22);
    bgBtn->setCursor(Qt::PointingHandCursor);
    bgBtn->setStyleSheet(swatchStyle(bg));
    QObject::connect(bgBtn, &QPushButton::clicked, &dlg, [&bg, bgBtn, swatchStyle]() {
        const QColor cur = tlDecodeArgb(bg);
        const QColor nc = QColorDialog::getColor(
            cur, bgBtn, QStringLiteral("Background Colour"),
            QColorDialog::ShowAlphaChannel);
        if (!nc.isValid()) return;
        bg = (static_cast<uint32_t>(nc.alpha()) << 24)
           | (static_cast<uint32_t>(nc.red())   << 16)
           | (static_cast<uint32_t>(nc.green()) << 8)
           |  static_cast<uint32_t>(nc.blue());
        bgBtn->setStyleSheet(swatchStyle(bg));
    });
    form->addRow(QStringLiteral("Background:"), bgBtn);

    auto* topSpin = new QDoubleSpinBox(&dlg);
    topSpin->setRange(0.0, 40.0);
    topSpin->setDecimals(1);
    topSpin->setSuffix(QStringLiteral(" %"));
    topSpin->setToolTip(QStringLiteral("Reserved top band (channel banner)"));
    topSpin->setValue(m_clip->topSafeMargin() * 100.0);
    form->addRow(QStringLiteral("Top margin:"), topSpin);

    auto* rightSpin = new QDoubleSpinBox(&dlg);
    rightSpin->setRange(0.0, 40.0);
    rightSpin->setDecimals(1);
    rightSpin->setSuffix(QStringLiteral(" %"));
    rightSpin->setToolTip(QStringLiteral("Reserved right strip (commentators)"));
    rightSpin->setValue(m_clip->rightSafeMargin() * 100.0);
    form->addRow(QStringLiteral("Right margin:"), rightSpin);

    outer->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;
    if (!m_clip) return;   // clip could have been unbound while modal

    auto before = snapshot();

    // Remap Drop/Reorder events to the new tier order; drop the events whose
    // tier was removed (Popups carry no tier).
    std::vector<int> map(m_clip->tiers().size(), -1);
    for (size_t ni = 0; ni < rows.size(); ++ni) {
        const int oi = rows[ni].orig;
        if (oi >= 0 && oi < static_cast<int>(map.size()))
            map[static_cast<size_t>(oi)] = static_cast<int>(ni);
    }
    auto& evs = m_clip->events();
    evs.erase(std::remove_if(evs.begin(), evs.end(), [&](const TierEvent& ev) {
        if (ev.type == TierEventType::Popup) return false;
        return ev.tier < 0 || ev.tier >= static_cast<int>(map.size())
            || map[static_cast<size_t>(ev.tier)] < 0;
    }), evs.end());
    for (auto& ev : evs)
        if (ev.type != TierEventType::Popup)
            ev.tier = map[static_cast<size_t>(ev.tier)];

    m_clip->tiers().clear();
    for (auto& rs : rows) {
        if (rs.def.label.empty()) rs.def.label = "?";
        m_clip->tiers().push_back(rs.def);
    }
    m_clip->setEntryAspect(static_cast<TierEntryAspect>(aspectCombo->currentIndex()));
    m_clip->setCustomAspect(static_cast<float>(wSpin->value()),
                            static_cast<float>(hSpin->value()));
    m_clip->setBackgroundColor(bg);
    m_clip->setTopSafeMargin(static_cast<float>(topSpin->value() / 100.0));
    m_clip->setRightSafeMargin(static_cast<float>(rightSpin->value() / 100.0));

    pushUndo(QStringLiteral("Edit grid"), before);
    refresh();
    emit tierListEdited();
}

void TierListPanel::removeEntry(uint64_t entryId)
{
    if (!m_clip) return;
    auto before = snapshot();
    auto& entries = m_clip->entries();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                  [&](const TierEntry& e) { return e.id == entryId; }), entries.end());
    // Drop any events that referenced the removed entry.
    auto& evs = m_clip->events();
    evs.erase(std::remove_if(evs.begin(), evs.end(),
              [&](const TierEvent& ev) { return ev.entryId == entryId; }), evs.end());
    pushUndo(QStringLiteral("Remove entry"), before);
    refresh();
    emit tierListEdited();
}

void TierListPanel::renameEntry(uint64_t entryId)
{
    if (!m_clip) return;
    for (auto& en : m_clip->entries()) {
        if (en.id != entryId) continue;
        bool ok = false;
        const QString nt = QInputDialog::getText(
            this, QStringLiteral("Rename Entry"), QStringLiteral("Name:"),
            QLineEdit::Normal, QString::fromStdString(en.title), &ok);
        if (!ok) return;
        auto before = snapshot();
        en.title = nt.toUtf8().toStdString();
        pushUndo(QStringLiteral("Rename entry"), before);
        refresh();
        emit tierListEdited();
        return;
    }
}

void TierListPanel::onEntryDroppedOnTier(uint64_t entryId, int tier)
{
    if (!m_clip) return;
    auto before = snapshot();
    const int64_t s        = playheadLocalTick();
    const int64_t popupLen = secondsToTicks(2.0);

    TierEvent popup;
    popup.type    = TierEventType::Popup;
    popup.entryId = entryId;
    popup.start   = s;
    popup.end     = s + popupLen;

    TierEvent drop;
    drop.type    = TierEventType::Drop;
    drop.entryId = entryId;
    drop.tier    = tier;
    drop.index   = -1;
    drop.start   = s + popupLen;   // cuts into the row when the popup ends
    drop.end     = s + popupLen;

    m_clip->events().push_back(popup);
    m_clip->events().push_back(drop);

    pushUndo(QStringLiteral("Add entry to tier"), before);
    rebuildEvents();
    rebuildBoard();
    emit tierListEdited();
}

} // namespace rt
