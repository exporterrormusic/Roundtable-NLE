/*
 * FontFamilyCombo.cpp — Premiere Pro-style font family picker.
 */

#include "widgets/FontFamilyCombo.h"

#include "Settings.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmapCache>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QStyledItemDelegate>

#include <algorithm>

namespace rt {

namespace {

constexpr int  kFontRowHeight   = 26;
constexpr int  kFontStarWidth   = 22;   // clickable favorite star column
constexpr int  kFontSamplePx    = 17;   // sample text pixel size
constexpr int  kFontPreviewDwellMs = 90;
const char*    kFavoriteFontsKey = "Text/FavoriteFonts";
const char*    kRecentFontsKey   = "Text/RecentFonts";

QStringList& favoriteFontCache()
{
    static QStringList list = appSettings().value(kFavoriteFontsKey).toStringList();
    return list;
}

QStringList& recentFontCache()
{
    static QStringList list = appSettings().value(kRecentFontsKey).toStringList();
    return list;
}

QStringList installedFontFamilies()
{
    QStringList families;
    for (const QString& family : QFontDatabase::families()) {
        // '@' families are Windows vertical-writing aliases; private
        // families are Qt/system internals. Neither belongs in a picker.
        if (family.startsWith(QLatin1Char('@'))
            || QFontDatabase::isPrivateFamily(family))
            continue;
        families.push_back(family);
    }
    families.sort(Qt::CaseInsensitive);
    families.removeDuplicates();
    return families;
}

/// Row painter: [star] [family name in the UI font] [sample in the font].
/// Section headers draw as small caps labels.
class FontSampleRowDelegate final : public QStyledItemDelegate
{
public:
    FontSampleRowDelegate(bool showStars, QObject* parent)
        : QStyledItemDelegate(parent), m_showStars(showStars) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        // Fixed size: never build a QFont per row just to measure (the menu
        // holds every installed family).
        return {360, kFontRowHeight};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto& colors = Theme::colors();
        const QRect rect = option.rect;
        const QString text = index.data(Qt::DisplayRole).toString();
        painter->save();

        if (index.data(FontFamilyCombo::kRowKindRole).toInt()
            == FontFamilyCombo::HeaderRow) {
            painter->fillRect(rect, colors.surface3);
            QFont font = option.font;
            font.setPixelSize(11);
            font.setBold(true);
            painter->setFont(font);
            painter->setPen(colors.textSecondary);
            painter->drawText(rect.adjusted(8, 0, -8, 0),
                              Qt::AlignVCenter | Qt::AlignLeft, text.toUpper());
            painter->restore();
            return;
        }

        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered  = option.state & QStyle::State_MouseOver;
        if (selected)
            painter->fillRect(rect, colors.highlight);
        else if (hovered)
            painter->fillRect(rect, colors.controlBgHover);
        const QColor textColor = selected ? colors.highlightedText
                                          : colors.textPrimary;

        int x = rect.left() + 6;
        if (m_showStars) {
            const bool favorite = FontFamilyCombo::isFavorite(text);
            if (favorite || hovered || selected) {
                QFont starFont = option.font;
                starFont.setPixelSize(14);
                painter->setFont(starFont);
                painter->setPen(favorite ? QColor(240, 196, 64)
                                         : (selected ? textColor
                                                     : colors.textTertiary));
                painter->drawText(QRect(rect.left(), rect.top(),
                                        kFontStarWidth, rect.height()),
                                  Qt::AlignCenter,
                                  favorite ? QStringLiteral("★")
                                           : QStringLiteral("☆"));
            }
            x = rect.left() + kFontStarWidth + 2;
        }

        const int available = rect.right() - x - 8;
        const int nameWidth = std::max(80, available * 11 / 20);
        QFont nameFont = option.font;
        nameFont.setPixelSize(Theme::typography().sizeXs + 1);
        painter->setFont(nameFont);
        painter->setPen(textColor);
        const QRect nameRect(x, rect.top(), nameWidth, rect.height());
        painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                          painter->fontMetrics().elidedText(
                              text, Qt::ElideRight, nameRect.width()));

        const QRect sampleRect(nameRect.right() + 10, rect.top(),
                               rect.right() - nameRect.right() - 14,
                               rect.height());
        if (sampleRect.width() > 8)
            painter->drawPixmap(sampleRect.topLeft(),
                                samplePixmap(text, sampleRect.size(), textColor,
                                             painter->device()->devicePixelRatioF()));
        painter->restore();
    }

private:
    /// "Sample" drawn in `family`, cached — re-shaping text in a different
    /// font for every visible row on every repaint is what makes font menus
    /// feel sluggish.
    static QPixmap samplePixmap(const QString& family, QSize size,
                                const QColor& color, qreal dpr)
    {
        const QString key = QStringLiteral("rtFontSample|%1|%2x%3|%4|%5")
            .arg(family).arg(size.width()).arg(size.height())
            .arg(color.rgba()).arg(dpr);
        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap)) return pixmap;

        pixmap = QPixmap(size * dpr);
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont font(family);
        font.setPixelSize(kFontSamplePx);
        p.setFont(font);
        p.setPen(color);
        p.drawText(QRect(QPoint(0, 0), size), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Sample"));
        p.end();
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }

    bool m_showStars;
};

} // namespace

// ═════════════════════════════════════════════════════════════════════════════

FontFamilyCombo::FontFamilyCombo(QWidget* parent)
    : QComboBox(parent)
    , m_families(installedFontFamilies())
{
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    setMaxVisibleItems(22);
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(14);
    addItems(m_families);   // rows default to AllRow (kind 0)

    if (auto* edit = lineEdit()) {
        edit->setClearButtonEnabled(true);
        edit->setPlaceholderText(tr("Type to find a font"));
    }

    // Type-to-filter over the plain family list (no headers / duplicates),
    // with the same sample rows as the menu.
    auto* completer = new QCompleter(new QStringListModel(m_families, this), this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(16);
    setCompleter(completer);
    completer->popup()->setItemDelegate(
        new FontSampleRowDelegate(false, completer->popup()));
    completer->popup()->setMinimumWidth(360);
    completer->popup()->installEventFilter(this);

    // view() creates the popup container, which installs its own viewport
    // filter; ours is installed after it, so it sees star clicks first.
    QAbstractItemView* menu = view();
    menu->setItemDelegate(new FontSampleRowDelegate(true, menu));
    menu->setMouseTracking(true);
    if (auto* list = qobject_cast<QListView*>(menu))
        list->setUniformItemSizes(true);
    menu->viewport()->installEventFilter(this);
    menu->window()->installEventFilter(this);

    m_previewTimer.setSingleShot(true);
    m_previewTimer.setInterval(kFontPreviewDwellMs);
    connect(&m_previewTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingPreview.isEmpty()) return;
        m_previewing = true;
        emit familyPreviewRequested(m_pendingPreview);
    });

    connect(this, &QComboBox::highlighted, this, [this](int row) {
        if (isFamilyRow(row)) requestPreview(itemText(row));
    });
    connect(completer, qOverload<const QString&>(&QCompleter::highlighted),
            this, [this](const QString& family) { requestPreview(family); });

    // A choice commits: forget the pending revert. These connections are
    // made before any caller's, so they run first.
    connect(this, &QComboBox::activated, this, [this](int row) {
        m_previewTimer.stop();
        m_pendingPreview.clear();
        m_previewing = false;
        if (isFamilyRow(row)) noteFamilyUsed(itemText(row));
    });
    connect(completer, qOverload<const QString&>(&QCompleter::activated),
            this, [this](const QString& family) {
        m_previewTimer.stop();
        m_pendingPreview.clear();
        m_previewing = false;
        noteFamilyUsed(family);
    });
}

QString FontFamilyCombo::installedFamily(const QString& text) const
{
    const QString wanted = text.trimmed();
    if (wanted.isEmpty()) return {};
    for (const QString& family : m_families)
        if (family.compare(wanted, Qt::CaseInsensitive) == 0) return family;
    return {};
}

void FontFamilyCombo::setCurrentFamily(const QString& family)
{
    QSignalBlocker blocker(this);
    const int start = allSectionStart();
    for (int row = start; row < count(); ++row) {
        if (itemText(row).compare(family, Qt::CaseInsensitive) == 0) {
            setCurrentIndex(row);
            return;
        }
    }
    setEditText(family);
}

void FontFamilyCombo::showPopup()
{
    rebuildTopSections();
    view()->setMinimumWidth(std::max(width(), 400));
    QComboBox::showPopup();
}

void FontFamilyCombo::hidePopup()
{
    QComboBox::hidePopup();
    scheduleEndPreview();
}

bool FontFamilyCombo::eventFilter(QObject* watched, QEvent* event)
{
    QAbstractItemView* menu = view();
    if (menu && watched == menu->viewport()) {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
            || type == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint pos = mouse->position().toPoint();
            const QModelIndex index = menu->indexAt(pos);
            if (index.isValid() && isFamilyRow(index.row())
                && pos.x() < kFontStarWidth) {
                // The star toggles a favorite without choosing the font or
                // closing the menu (Premiere's per-row favorite button).
                if (type == QEvent::MouseButtonRelease
                    && mouse->button() == Qt::LeftButton) {
                    const QString family = itemText(index.row());
                    setFavorite(family, !isFavorite(family));
                    menu->viewport()->update();
                }
                return true;
            }
        }
    } else if (event->type() == QEvent::Hide
               && ((menu && watched == menu->window())
                   || (completer() && watched == completer()->popup()))) {
        // Covers click-away / Esc, which close the popup without going
        // through hidePopup().
        scheduleEndPreview();
    }
    return QComboBox::eventFilter(watched, event);
}

void FontFamilyCombo::rebuildTopSections()
{
    QSignalBlocker blocker(this);
    const QString current = currentText();
    while (count() > 0 && itemData(0, kRowKindRole).toInt() != AllRow)
        removeItem(0);

    auto* items = qobject_cast<QStandardItemModel*>(model());
    int row = 0;
    const auto addRow = [&](const QString& text, RowKind kind) {
        insertItem(row, text);
        setItemData(row, kind, kRowKindRole);
        if (kind == HeaderRow && items)
            if (auto* item = items->item(row)) item->setFlags(Qt::NoItemFlags);
        ++row;
    };
    const auto installedOnly = [this](const QStringList& names) {
        QStringList out;
        for (const QString& name : names)
            if (const QString family = installedFamily(name); !family.isEmpty())
                out.push_back(family);
        return out;
    };

    const QStringList favorites = installedOnly(favoriteFamilies());
    const QStringList recent = installedOnly(recentFamilies());
    if (!favorites.isEmpty()) {
        addRow(tr("Favorites"), HeaderRow);
        for (const QString& family : favorites) addRow(family, FavoriteRow);
    }
    if (!recent.isEmpty()) {
        addRow(tr("Recently Used"), HeaderRow);
        for (const QString& family : recent) addRow(family, RecentRow);
    }
    if (row > 0) addRow(tr("All Fonts"), HeaderRow);

    setCurrentFamily(current);
}

void FontFamilyCombo::requestPreview(const QString& family)
{
    if (family.isEmpty()) return;
    // Opening the menu highlights the current font; that is not a preview.
    if (!m_previewing && family.compare(currentText(), Qt::CaseInsensitive) == 0)
        return;
    m_pendingPreview = family;
    m_previewTimer.start();
}

void FontFamilyCombo::scheduleEndPreview()
{
    m_previewTimer.stop();
    m_pendingPreview.clear();
    if (!m_previewing) return;
    // Deferred: on a click, Qt hides the menu BEFORE emitting activated.
    // If a choice follows, the activated handler clears m_previewing and
    // the caller commits the previewed state instead of reverting it.
    QTimer::singleShot(0, this, [this]() {
        if (!m_previewing) return;
        m_previewing = false;
        emit familyPreviewEnded();
    });
}

int FontFamilyCombo::allSectionStart() const
{
    int row = 0;
    while (row < count() && itemData(row, kRowKindRole).toInt() != AllRow) ++row;
    return row;
}

bool FontFamilyCombo::isFamilyRow(int row) const
{
    return row >= 0 && row < count()
        && itemData(row, kRowKindRole).toInt() != HeaderRow;
}

// ── Favorites / recent ──────────────────────────────────────────────────────

QStringList FontFamilyCombo::favoriteFamilies()
{
    return favoriteFontCache();
}

bool FontFamilyCombo::isFavorite(const QString& family)
{
    return favoriteFontCache().contains(family, Qt::CaseInsensitive);
}

void FontFamilyCombo::setFavorite(const QString& family, bool favorite)
{
    if (family.isEmpty()) return;
    QStringList& list = favoriteFontCache();
    list.removeIf([&](const QString& f) {
        return f.compare(family, Qt::CaseInsensitive) == 0;
    });
    if (favorite) {
        list.push_back(family);
        list.sort(Qt::CaseInsensitive);
    }
    appSettings().setValue(kFavoriteFontsKey, list);
}

QStringList FontFamilyCombo::recentFamilies()
{
    return recentFontCache();
}

void FontFamilyCombo::noteFamilyUsed(const QString& family)
{
    if (family.trimmed().isEmpty()) return;
    QStringList& list = recentFontCache();
    list.removeIf([&](const QString& f) {
        return f.compare(family, Qt::CaseInsensitive) == 0;
    });
    list.push_front(family);
    while (list.size() > kMaxRecentFamilies) list.pop_back();
    appSettings().setValue(kRecentFontsKey, list);
}

} // namespace rt
