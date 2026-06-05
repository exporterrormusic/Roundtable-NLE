/*
 * ShotComposerThumbnails.cpp - Thumbnail, library, and property handler methods for ShotComposer.
 * Split from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"

#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>

#include <spdlog/spdlog.h>

namespace rt {

void ShotComposer::refreshShotList()
{
    m_shotList->blockSignals(true);
    m_shotList->clear();

    // Get filter criteria
    QString searchFilter;
    QString charFilter;
    QString showFilter;
    if (m_shotSearchEdit)
        searchFilter = m_shotSearchEdit->text().trimmed().toLower();
    charFilter = activeCharFilter();
    showFilter = activeShowFilter();

    // Helper: does a shot (by its show tags) match the active SHOWS filter?
    auto shotMatchesShow = [&](const QStringList& shotShows) -> bool {
        if (showFilter.isEmpty())
            return true; // ALL
        if (showFilter == QStringLiteral("__UNASSIGNED__"))
            return shotShows.isEmpty();
        for (const auto& s : shotShows)
            if (s.compare(showFilter, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };

    // Build set of default shot names
    std::set<std::string> defaultShotNames;
    {
        for (const auto& [ch, shotName] : m_characterDefaults)
            defaultShotNames.insert(shotName);
    }

    // Scan all presets once to count per-character shot occurrences
    // and collect character names per shot for tag display.
    struct ShotInfo {
        std::string name;            ///< bare shot name (display)
        std::string key;             ///< full "show/name" key (load reference)
        bool isDefault = false;
        QStringList charTags;        ///< real character names (for filter match)
        QStringList charTagsDisplay; ///< alias-resolved labels (for rendering)
        QStringList shows;           ///< owning show (0 or 1 entry)
        int layerCount = 0;
    };
    std::vector<ShotInfo> allShotInfos;
    std::map<std::string, int> charShotCount; // character real name -> shot count (within active show)
    int unassignedCount = 0;                  // char-unassigned shots (within active show)
    int showFilteredTotal = 0;                // ALL count for CHARACTERS column (within active show)

    // Per-show counts span ALL shots (the SHOWS column is the top-level filter
    // and is not narrowed by the character selection). Keyed by lowercased
    // show name → {first-seen display casing, shot count}.
    std::map<QString, std::pair<QString, int>> showShotCount;
    int showUnassignedCount = 0; // shots with no show tags

    // Seed with registered shows (the "+"-created registry) so a show with no
    // shots yet still appears in the column (count 0).
    for (const auto& s : m_presetManager.knownShows()) {
        QString qs = QString::fromStdString(s).trimmed();
        if (qs.isEmpty()) continue;
        auto& slot = showShotCount[qs.toLower()];
        if (slot.first.isEmpty()) slot.first = qs;
    }

    for (const auto& [presetKey, presetRef] : m_presetManager.allPresets()) {
        const ShotPreset* preset = &presetRef;

        ShotInfo si;
        si.name = preset->name();
        si.key  = presetKey;
        si.isDefault = defaultShotNames.count(preset->name()) > 0;
        si.layerCount = preset->layerCount();

        if (!preset->show().empty())
            si.shows << QString::fromStdString(preset->show());

        // Collect characters from this preset. charTags holds real
        // character names for filter matching; charTagsDisplay holds the
        // alias-resolved labels for rendering.
        QSet<QString> seenReal;
        for (const auto& ch : preset->characters()) {
            std::string real = m_modelManager
                ? m_modelManager->getDisplayName(ch.characterName)
                : ch.characterName;
            std::string disp = m_presetManager.displayNameFor(real);
            si.charTags        << QString::fromStdString(real);
            si.charTagsDisplay << QString::fromStdString(disp);
            seenReal.insert(QString::fromStdString(real));
        }

        // Per-show counts (all shots).
        if (si.shows.isEmpty()) {
            ++showUnassignedCount;
        } else {
            QSet<QString> seenShow;
            for (const auto& sh : si.shows) {
                QString key = sh.toLower();
                if (seenShow.contains(key)) continue;
                seenShow.insert(key);
                auto& slot = showShotCount[key];
                if (slot.first.isEmpty()) slot.first = sh; // first-seen casing
                slot.second++;
            }
        }

        // Per-character shot occurrences, narrowed to the active show so the
        // CHARACTERS column cascades from the selected show.
        if (shotMatchesShow(si.shows)) {
            ++showFilteredTotal; // ALL count reflects the selected show
            if (si.charTags.isEmpty()) {
                ++unassignedCount;
            } else {
                for (const auto& tag : seenReal)
                    charShotCount[tag.toStdString()]++;
            }
        }

        allShotInfos.push_back(si);
    }

    int totalShots = static_cast<int>(allShotInfos.size());

    // --- Build show filter chip list (m_showFilterList) ---
    if (m_showFilterList) {
        QString prevShow;
        if (auto* cur = m_showFilterList->currentItem())
            prevShow = cur->data(Qt::UserRole).toString();

        m_showFilterList->blockSignals(true);
        m_showFilterList->clear();

        QString showSearchText;
        if (m_showFilterSearchEdit)
            showSearchText = m_showFilterSearchEdit->text().trimmed().toLower();

        int restoreRow = -1;
        int row = 0;

        // ALL
        {
            auto* item = new QListWidgetItem(QStringLiteral("ALL SHOWS"));
            item->setData(Qt::UserRole, QString());
            item->setData(Qt::UserRole + 1, totalShots);
            item->setSizeHint(QSize(0, 34));
            QFont f = item->font(); f.setPixelSize(15); f.setBold(true); item->setFont(f);
            item->setForeground(QColor(180, 220, 180));
            m_showFilterList->addItem(item);
            if (prevShow.isEmpty()) restoreRow = row;
            ++row;
        }
        // UNASSIGNED
        {
            auto* item = new QListWidgetItem(QStringLiteral("NO SHOW"));
            item->setData(Qt::UserRole, QStringLiteral("__UNASSIGNED__"));
            item->setData(Qt::UserRole + 1, showUnassignedCount);
            item->setSizeHint(QSize(0, 34));
            QFont f = item->font(); f.setPixelSize(15); f.setBold(true); item->setFont(f);
            item->setForeground(QColor(210, 170, 80));
            m_showFilterList->addItem(item);
            if (prevShow == QStringLiteral("__UNASSIGNED__")) restoreRow = row;
            ++row;
        }
        // Separator
        {
            auto* sep = new QListWidgetItem(QString());
            sep->setFlags(sep->flags() & ~Qt::ItemIsSelectable);
            sep->setSizeHint(QSize(0, 8));
            sep->setBackground(QColor(100, 100, 130, 50));
            m_showFilterList->addItem(sep);
            ++row;
        }
        // Show entries, sorted by display name
        std::vector<std::pair<QString, int>> shows; // display, count
        shows.reserve(showShotCount.size());
        for (const auto& [key, val] : showShotCount)
            shows.emplace_back(val.first, val.second);
        std::sort(shows.begin(), shows.end(),
            [](const auto& a, const auto& b) {
                return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
            });
        for (const auto& [disp, count] : shows) {
            if (!showSearchText.isEmpty() && !disp.toLower().contains(showSearchText))
                continue;
            // Optional per-show thumbnail (set via the column context menu).
            QString thumbPath = QString::fromStdString(
                m_presetManager.showThumbnail(disp.toStdString()));
            QListWidgetItem* item = nullptr;
            if (!thumbPath.isEmpty() && QFileInfo::exists(thumbPath)) {
                QPixmap pix(thumbPath);
                item = new QListWidgetItem(QIcon(pix), disp);
                item->setSizeHint(QSize(0, 104)); // tall row: big thumbnail on top, name below
            } else {
                item = new QListWidgetItem(disp);
                item->setSizeHint(QSize(0, 34));
            }
            item->setData(Qt::UserRole, disp);
            item->setData(Qt::UserRole + 1, count);
            QFont f = item->font(); f.setPixelSize(14); item->setFont(f);
            item->setToolTip(QStringLiteral("%1 — %2 shots").arg(disp).arg(count));
            m_showFilterList->addItem(item);
            if (disp == prevShow) restoreRow = row;
            ++row;
        }

        if (restoreRow >= 0 && restoreRow < m_showFilterList->count())
            m_showFilterList->setCurrentRow(restoreRow);
        m_showFilterList->blockSignals(false);
    }

    // --- Build character filter chip list (m_charFilterList) ---
    if (m_charFilterList) {
        // Preserve current selection BEFORE clearing
        QString prevFilter;
        if (auto* cur = m_charFilterList->currentItem())
            prevFilter = cur->data(Qt::UserRole).toString();

        m_charFilterList->blockSignals(true);
        m_charFilterList->clear();

        int restoreRow = -1;
        int row = 0;

        // Collect valid character names. When no show is selected we list the
        // full roster (downloaded + video + characters used in any shot). When
        // a specific show (or NO SHOW) is active, the list cascades to only the
        // characters that appear in that show's shots (charShotCount keys).
        QSet<QString> validNames;
        if (showFilter.isEmpty()) {
#ifdef ROUNDTABLE_HAS_SPINE
            if (m_modelManager && m_modelManager->isScanned()) {
                for (const auto& name : m_modelManager->characterDisplayNames())
                    validNames.insert(QString::fromStdString(name));
            }
#endif
            for (const auto& [filename, info] : videoCharacterFiles()) {
                (void)filename;
                // Only include video characters whose media actually exists on
                // disk — the installer version may not ship these assets.
                if (QFileInfo::exists(QString::fromStdString(info.mutePath)) ||
                    QFileInfo::exists(QString::fromStdString(info.talkPath))) {
                    validNames.insert(QString::fromStdString(info.charName));
                }
            }
        }
        // Always include characters that appear in the (show-filtered) shots.
        for (const auto& [cn, count] : charShotCount) {
            (void)count;
            validNames.insert(QString::fromStdString(cn));
        }

        // Apply search filter to character names
        QString filterSearchText;
        if (m_filterSearchEdit)
            filterSearchText = m_filterSearchEdit->text().trimmed().toLower();

        // ALL item
        {
            QString label = QString("ALL");
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, QString()); // empty = ALL
            // Count reflects the active SHOWS filter (cascade): the number of
            // shots in the selected show, or every shot when no show is selected.
            item->setData(Qt::UserRole + 1, showFilteredTotal);
            item->setSizeHint(QSize(0, 56));
            QFont allFont = item->font();
            allFont.setPixelSize(18);
            allFont.setBold(true);
            item->setFont(allFont);
            item->setForeground(QColor(180, 220, 180));
            m_charFilterList->addItem(item);
            if (prevFilter.isEmpty()) restoreRow = row;
            ++row;
        }

        // UNASSIGNED item
        {
            QString label = QString("UNASSIGNED");
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, QStringLiteral("__UNASSIGNED__"));
            item->setData(Qt::UserRole + 1, unassignedCount);
            item->setSizeHint(QSize(0, 56));
            QFont uaFont = item->font();
            uaFont.setPixelSize(18);
            uaFont.setBold(true);
            item->setFont(uaFont);
            item->setForeground(QColor(210, 170, 80));
            m_charFilterList->addItem(item);
            if (prevFilter == QStringLiteral("__UNASSIGNED__")) restoreRow = row;
            ++row;
        }

        // Separator divider line (unselectable, thin grey like tab dividers)
        {
            auto* sep = new QListWidgetItem(QString());
            sep->setFlags(sep->flags() & ~Qt::ItemIsSelectable);
            sep->setSizeHint(QSize(0, 8));
            QColor sepColor(100, 100, 130, 50);
            sep->setBackground(sepColor);
            m_charFilterList->addItem(sep);
            ++row;
        }

        // Character items — sort by the *displayed* name (alias if any)
        // so the user sees alphabetical order matching the labels.
        struct CharEntry { QString real; QString display; };
        std::vector<CharEntry> entries;
        entries.reserve(validNames.size());
        for (const auto& cn : validNames) {
            QString disp = QString::fromStdString(
                m_presetManager.displayNameFor(cn.toStdString()));
            entries.push_back({cn, disp});
        }
        std::sort(entries.begin(), entries.end(),
            [](const CharEntry& a, const CharEntry& b) {
                return a.display.compare(b.display, Qt::CaseInsensitive) < 0;
            });

        for (const auto& entry : entries) {
            const QString& cn = entry.real;
            const QString& disp = entry.display;

            // Apply search filter (search the displayed label so renamed
            // entries match what the user types).
            if (!filterSearchText.isEmpty() && !disp.toLower().contains(filterSearchText))
                continue;

            int count = 0;
            auto it = charShotCount.find(cn.toStdString());
            if (it != charShotCount.end())
                count = it->second;

            // Get thumbnail for this character (real folder name)
            std::string folderName = m_modelManager
                ? m_modelManager->getFolderName(cn.toStdString())
                : cn.toStdString();
            QPixmap thumb = makeCharacterThumbnail(folderName, 96);

            // Label shows alias display name; UserRole stores real character
            // name so downstream filter matching against shot tags works.
            auto* item = new QListWidgetItem(QIcon(thumb), disp);
            item->setData(Qt::UserRole, cn);
            item->setData(Qt::UserRole + 1, count);
            item->setSizeHint(QSize(0, 104));
            QFont chFont = item->font();
            chFont.setPixelSize(16);
            item->setFont(chFont);
            if (cn == disp)
                item->setToolTip(QString("%1 - %2 shots").arg(cn).arg(count));
            else
                item->setToolTip(QString("%1 (aka %2) - %3 shots").arg(disp, cn).arg(count));
            m_charFilterList->addItem(item);
            if (cn == prevFilter) restoreRow = row;
            ++row;
        }

        if (restoreRow >= 0 && restoreRow < m_charFilterList->count())
            m_charFilterList->setCurrentRow(restoreRow);
        m_charFilterList->blockSignals(false);
    }

    // --- Build shot list (m_shotList) ---
    constexpr int kThumbW = 320;
    constexpr int kThumbH = 180;

    // Determine sort mode
    enum SortMode { SortAZ, SortFavorites, SortCharacter, SortRecent };
    SortMode sortMode = SortAZ;
    if (m_shotSortCombo) {
        int si = m_shotSortCombo->currentIndex();
        if (si == 1) sortMode = SortFavorites;
        else if (si == 2) sortMode = SortCharacter;
        else if (si == 3) sortMode = SortRecent;
    }

    // Build filtered shot list
    struct FilteredShot {
        std::string name;            ///< bare name (display)
        std::string key;             ///< full "show/name" key (load reference)
        std::string show;            ///< owning show ("" = No Show)
        bool isDefault = false;
        QStringList charTags;        ///< real names
        QStringList charTagsDisplay; ///< alias-resolved labels
        int layerCount = 0;
        int sortKey = 0;
        QString firstCharTag; // for Character sort mode
    };
    std::vector<FilteredShot> filtered;

    const bool showingAllShows = showFilter.isEmpty();

    for (const auto& si : allShotInfos) {
        // Apply name search filter
        if (!searchFilter.isEmpty()) {
            QString qn = QString::fromStdString(si.name).toLower();
            if (!qn.contains(searchFilter))
                continue;
        }

        // Apply show filter (cascade: show ∩ character)
        if (!shotMatchesShow(si.shows))
            continue;

        // Apply character filter (real name vs real name)
        if (!charFilter.isEmpty()) {
            if (charFilter == QStringLiteral("__UNASSIGNED__")) {
                if (!si.charTags.isEmpty())
                    continue;
            } else {
                bool hasChar = false;
                for (const auto& tag : si.charTags) {
                    if (tag.compare(charFilter, Qt::CaseInsensitive) == 0) {
                        hasChar = true;
                        break;
                    }
                }
                if (!hasChar)
                    continue;
            }
        }

        FilteredShot fs;
        fs.name = si.name;
        fs.key  = si.key;
        fs.show = si.shows.isEmpty() ? std::string{} : si.shows.first().toStdString();
        fs.isDefault = si.isDefault;
        fs.charTags = si.charTags;
        fs.charTagsDisplay = si.charTagsDisplay;
        fs.layerCount = si.layerCount;
        if (!si.charTagsDisplay.isEmpty())
            fs.firstCharTag = si.charTagsDisplay.first();
        filtered.push_back(fs);
    }

    // Sort
    if (sortMode == SortFavorites) {
        // Default shots first, then alphabetical
        std::stable_partition(filtered.begin(), filtered.end(),
            [](const FilteredShot& fs) { return fs.isDefault; });
        auto mid = std::stable_partition(filtered.begin(), filtered.end(),
            [](const FilteredShot& fs) { return fs.isDefault; });
        std::sort(filtered.begin(), mid,
            [](const FilteredShot& a, const FilteredShot& b) { return a.name < b.name; });
        std::sort(mid, filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) { return a.name < b.name; });
    } else if (sortMode == SortCharacter) {
        std::sort(filtered.begin(), filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) {
                if (a.isDefault != b.isDefault) return a.isDefault;
                if (a.firstCharTag != b.firstCharTag) return a.firstCharTag < b.firstCharTag;
                return a.name < b.name;
            });
    } else {
        // SortAZ (default alphabetical)
        std::sort(filtered.begin(), filtered.end(),
            [](const FilteredShot& a, const FilteredShot& b) {
                if (a.isDefault != b.isDefault) return a.isDefault;
                return a.name < b.name;
            });
    }

    int selectRow = -1;
    int visibleRow = 0;

    QString lastCharGroup; // for SortCharacter mode

    for (const auto& fs : filtered) {

        // For SortCharacter mode, insert character group headers
        if (sortMode == SortCharacter) {
            if (visibleRow == 0 || fs.firstCharTag != lastCharGroup) {
                lastCharGroup = fs.firstCharTag;
                QString groupLabel = fs.firstCharTag.isEmpty()
                    ? QStringLiteral("Other")
                    : fs.firstCharTag;
                auto* headerItem = new QListWidgetItem(QStringLiteral("\xf0\x9f\x93\x81 ") + groupLabel);
                headerItem->setFlags(headerItem->flags() & ~Qt::ItemIsSelectable);
                headerItem->setSizeHint(QSize(0, 24));
                QFont headerFont;
                headerFont.setBold(true);
                headerFont.setPixelSize(11);
                headerItem->setFont(headerFont);
                headerItem->setForeground(QColor(180, 180, 180));
                m_shotList->addItem(headerItem);
                ++visibleRow;
            }
        }

        // Build thumbnail (keyed by the full show/name key)
        QPixmap thumb;
        QString cachedPath = shotThumbnailPath(fs.key);
        if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
            thumb.load(cachedPath);
        }
        if (thumb.isNull()) {
            auto preset = m_presetManager.load(fs.key);
            if (preset)
                thumb = makeShotThumbnail(*preset, kThumbW, kThumbH);
            else
                thumb = QPixmap(kThumbW, kThumbH);
        }

        // Build shot item. Display the bare name; when viewing ALL SHOWS append
        // the owning show so same-named shots from different shows are distinct.
        QString displayName = QString::fromStdString(fs.name);
        if (showingAllShows && !fs.show.empty())
            displayName += QStringLiteral("   ·   ") + QString::fromStdString(fs.show);

        auto* item = new QListWidgetItem(QIcon(thumb), displayName);
        item->setData(Qt::UserRole, QString::fromStdString(fs.key)); // full key for load
        item->setData(Qt::UserRole + 1, fs.charTagsDisplay); // alias-resolved tags
        item->setData(Qt::UserRole + 2, fs.layerCount);    // int layer count
        item->setData(Qt::UserRole + 3, fs.isDefault);     // bool is default
        if (fs.isDefault)
            item->setForeground(QColor(255, 200, 50));

        m_shotList->addItem(item);

        const std::string curKey =
            ShotPresetManager::makeKey(m_currentShot.show(), m_currentShot.name());
        if (!m_currentShot.name().empty() && fs.key == curKey)
            selectRow = visibleRow;
        ++visibleRow;
    }

    if (selectRow >= 0)
        m_shotList->setCurrentRow(selectRow);
    m_shotList->blockSignals(false);
}


void ShotComposer::refreshLayerList()
{
    const auto& m = Theme::metrics();
    m_layerList->blockSignals(true);
    m_layerList->clear();
    const auto& order = m_currentShot.layerOrder();
    // Build a quick map: layer index → group depth (how many groups contain it)
    std::vector<int> groupDepth(order.size(), 0);
    for (const auto& grp : m_layerGroups) {
        if (!grp.expanded) continue;
        for (int i = grp.firstChild; i >= 0 && i <= grp.lastChild && i < static_cast<int>(order.size()); ++i)
            groupDepth[static_cast<size_t>(i)]++;
    }
    for (size_t li = 0; li < order.size(); ++li) {
        const auto& ref = order[li];

        // ── Insert group folder headers before the first child ─────────
        for (const auto& grp : m_layerGroups) {
            if (grp.firstChild == static_cast<int>(li) && grp.firstChild >= 0) {
                auto* folderItem = new QListWidgetItem(m_layerList);
                folderItem->setSizeHint(QSize(0, 32));
                folderItem->setFlags(folderItem->flags() & ~Qt::ItemIsDragEnabled);
                folderItem->setFlags(folderItem->flags() & ~Qt::ItemIsSelectable);

                auto* folderWidget = new QWidget;
                folderWidget->setStyleSheet("background: transparent;");
                auto* folderLayout = new QHBoxLayout(folderWidget);
                folderLayout->setContentsMargins(m.spacingXs, 0, m.spacingMd, 0);
                folderLayout->setSpacing(3);

                auto* expandLabel = new QLabel(
                    grp.expanded ? QStringLiteral("\u25BC") : QStringLiteral("\u25B6"));
                expandLabel->setFixedWidth(14);
                expandLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 10px; }")
                    .arg(Theme::hex(Theme::colors().textSecondary)));
                folderLayout->addWidget(expandLabel);

                auto* folderIcon = new QLabel(QStringLiteral("\xF0\x9F\x93\x81"));
                folderIcon->setFixedWidth(18);
                folderIcon->setStyleSheet("QLabel { font-size: 14px; }");
                folderLayout->addWidget(folderIcon);

                auto* folderName = new QLabel(QString::fromStdString(grp.name));
                folderName->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: 600; }")
                    .arg(Theme::hex(Theme::colors().textSecondary)));
                folderLayout->addWidget(folderName, 1);

                m_layerList->setItemWidget(folderItem, folderWidget);
            }
        }

        QString label;
        QString typeIcon;
        bool isVisible = true;
        float opacity = 1.0f;

        if (ref.type == LayerType::Background) {
            const auto* bg = m_currentShot.background(ref.index);
            if (bg) {
                auto fname = std::filesystem::path(bg->path).filename().string();
                typeIcon = bg->isVideo()
                    ? QStringLiteral("\xF0\x9F\x8E\xAC")   // Ã°Å¸Å½Â¬
                    : QStringLiteral("\xF0\x9F\x96\xBC");   // Ã°Å¸â€“Â¼
                label = QString::fromStdString(fname.empty() ? bg->path : fname);
                isVisible = bg->visible;
                opacity = bg->opacity;
            } else {
                typeIcon = QStringLiteral("\xF0\x9F\x96\xBC");
                label = QString("BG #%1").arg(ref.index);
            }
        } else {
            const auto* ch = m_currentShot.character(ref.index);
            if (ch) {
                typeIcon = QStringLiteral("\xF0\x9F\x91\xA4");  // 👤
                // Show display name (with colons) instead of folder name
                std::string dn = m_modelManager
                    ? m_modelManager->getDisplayName(ch->characterName)
                    : ch->characterName;
                label = QString::fromStdString(dn);
                isVisible = ch->visible;
                opacity = ch->opacity;
            } else {
                typeIcon = QStringLiteral("\xF0\x9F\x91\xA4");
                label = QString("Character #%1").arg(ref.index);
            }
        }

        int depth = (li < groupDepth.size()) ? groupDepth[li] : 0;

        // Ã¢â€â‚¬Ã¢â€â‚¬ Photoshop-style layer row Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        auto* item = new QListWidgetItem(m_layerList);
        item->setSizeHint(QSize(0, 44));
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);

        auto* rowWidget = new QWidget;
        rowWidget->setStyleSheet("background: transparent;");
        auto* rowLayout = new QHBoxLayout(rowWidget);
        // Indent based on group depth
        int indentPx = depth * 20;
        rowLayout->setContentsMargins(m.spacingSm + indentPx, m.spacingXs, m.spacingMd, m.spacingXs);
        rowLayout->setSpacing(0);

        // Eye toggle button
        auto* eyeBtn = new QPushButton(
            isVisible ? QStringLiteral("\xF0\x9F\x91\x81")    // Ã°Å¸â€˜Â
                      : QStringLiteral("\xE2\x80\x94"));       // Ã¢â‚¬â€
        eyeBtn->setFixedSize(32, 32);
        eyeBtn->setFocusPolicy(Qt::NoFocus);
        eyeBtn->setToolTip("Toggle visibility");
        eyeBtn->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        eyeBtn->setStyleSheet(
            isVisible
            ? QStringLiteral("QPushButton { background: transparent; border: none; font-size: 18px; "
              "color: %1; padding: 0; margin: 0; }"
              "QPushButton:hover { color: %2; background: rgba(255,255,255,0.1); border-radius: 3px; }")
              .arg(Theme::hex(Theme::colors().textSecondary), Theme::hex(Theme::colors().textBright))
            : QStringLiteral("QPushButton { background: transparent; border: none; font-size: 16px; "
              "color: %1; padding: 0; margin: 0; }"
              "QPushButton:hover { color: %2; background: rgba(255,255,255,0.05); border-radius: 3px; }")
              .arg(Theme::hex(Theme::colors().border), Theme::hex(Theme::colors().textDisabled))
        );

        int layerIdx = static_cast<int>(li);
        connect(eyeBtn, &QPushButton::clicked, this, [this, layerIdx]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (layerIdx < 0 || layerIdx >= m_currentShot.layerCount()) return;
            pushUndoState();
            const auto& lref = m_currentShot.layerOrder()[static_cast<size_t>(layerIdx)];
            if (lref.type == LayerType::Character) {
                auto* ch = m_currentShot.character(lref.index);
                if (ch) {
                    ch->visible = !ch->visible;
                    if (layerIdx == m_selectedLayer && m_visibleCheck)
                        m_visibleCheck->setChecked(ch->visible);
                }
            } else {
                auto* bg = m_currentShot.background(lref.index);
                if (bg) bg->visible = !bg->visible;
            }
            refreshLayerList();
            updatePreview();
            emit shotChanged();
        });
        rowLayout->addWidget(eyeBtn);

        // Thin vertical separator
        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedWidth(1);
        sep->setFixedHeight(32);
        sep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::colors().border)));
        sep->setAttribute(Qt::WA_TransparentForMouseEvents);
        rowLayout->addSpacing(3);
        rowLayout->addWidget(sep);
        rowLayout->addSpacing(5);

        // Layer type icon
        auto* iconLabel = new QLabel(typeIcon);
        iconLabel->setFixedWidth(22);
        iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        iconLabel->setStyleSheet("QLabel { font-size: 18px; padding: 0; }");
        rowLayout->addWidget(iconLabel);
        rowLayout->addSpacing(m.spacingXs);

        // Layer name
        auto* nameLabel = new QLabel(label);
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        QString nameColor = isVisible ? Theme::hex(Theme::colors().textPrimary)
                                       : Theme::hex(Theme::colors().textDisabled);
        nameLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 14px; font-weight: 500; }")
            .arg(nameColor));
        rowLayout->addWidget(nameLabel, 1);

        // Opacity indicator (if not 100%)
        if (opacity < 0.99f) {
            auto* opLabel = new QLabel(QString("%1%").arg(static_cast<int>(opacity * 100)));
            opLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            opLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; padding-right: 6px; }")
                .arg(Theme::hex(Theme::colors().textDisabled)));
            opLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            rowLayout->addWidget(opLabel);
        }

        m_layerList->setItemWidget(item, rowWidget);
    }

    if (m_selectedLayer >= 0 && m_selectedLayer < m_layerList->count())
        m_layerList->setCurrentRow(m_selectedLayer);
    m_layerList->blockSignals(false);

    // Ã¢â€â‚¬Ã¢â€â‚¬ Refresh default-shot character dropdown Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    if (m_defaultCharCombo) {
        QString prev = m_defaultCharCombo->currentText();
        m_defaultCharCombo->blockSignals(true);
        m_defaultCharCombo->clear();
        for (const auto& ch : m_currentShot.characters()) {
            // Show display name in combo
            std::string dn = m_modelManager
                ? m_modelManager->getDisplayName(ch.characterName)
                : ch.characterName;
            m_defaultCharCombo->addItem(QString::fromStdString(dn));
        }
        // Restore previous selection if still present
        int prevIdx = m_defaultCharCombo->findText(prev);
        if (prevIdx >= 0)
            m_defaultCharCombo->setCurrentIndex(prevIdx);
        m_defaultCharCombo->blockSignals(false);
        bool hasChars = m_defaultCharCombo->count() > 0;
        m_defaultCharCombo->setEnabled(hasChars && !m_currentShot.name().empty());
        m_setDefaultBtn->setEnabled(hasChars && !m_currentShot.name().empty());
    }
}

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
// Thumbnail helpers
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

void ShotComposer::setLibraryIconSize(int sz)
{
    m_iconSize = sz;
    QSize icoSz(sz, sz);
    QSize gridSz(sz + 8, sz + 22);

    for (auto* list : {m_characterLibrary, m_backgroundLibrary, m_videoLibrary}) {
        if (!list) continue;
        list->setIconSize(icoSz);
        list->setGridSize(gridSz);
    }

    // Regenerate thumbnails at the new size
    refreshCharacterLibrary();
    refreshBackgroundLibrary();
    refreshVideoLibrary();
}


} // namespace rt
