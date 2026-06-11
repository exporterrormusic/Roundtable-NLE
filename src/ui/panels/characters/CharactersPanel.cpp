/*
 * CharactersPanel.cpp — lists downloaded characters with their Spine poses
 * and animation names, draggable to the timeline.
 *
 * Stances (Default / Aim / Cover) are shown as expandable groups.
 * Animation names are loaded lazily from the skeleton file when the user
 * expands a stance node.  Dragging an animation leaf creates a SpineClip
 * on the timeline via the "spine:" URI scheme.
 */

#include "panels/characters/CharactersPanel.h"
#include "Theme.h"
#include "widgets/MediaDragTreeWidget.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#endif
#include "playback/MediaPool.h"

#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace rt {

// ─────────────────────────────────────────────────────────────────────────────

CharactersPanel::CharactersPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
}

CharactersPanel::~CharactersPanel()
{
    m_destroying.store(true, std::memory_order_release);

    // Stop the search debounce timer — prevents it firing during destruction
    if (m_searchDebounce) {
        m_searchDebounce->stop();
    }
}

QTreeWidget* CharactersPanel::treeWidget() const noexcept
{
    return m_tree;
}

// ─────────────────────────────────────────────────────────────────────────────

void CharactersPanel::buildUI()
{
    const auto& tc = Theme::colors();
    const auto& m  = Theme::metrics();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── Toolbar ──────────────────────────────────────────────────────────
    auto* toolbar = new QWidget(this);
    toolbar->setFixedHeight(28);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-bottom: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(m.spacingXs, m.spacingXxs, m.spacingXs, m.spacingXxs);
    toolbarLayout->setSpacing(m.spacingXs);

    // Search field with magnifying glass prefix
    m_searchEdit = new QLineEdit(toolbar);
    m_searchEdit->setPlaceholderText(QStringLiteral("\U0001F50D Search characters\u2026"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedHeight(22);
    m_searchEdit->setStyleSheet(
        QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                "border-radius: %4px; padding: 2px 4px; font-size: 12px; }"
                "QLineEdit:focus { border: 1px solid %5; }")
            .arg(Theme::hex(tc.inputBg), Theme::hex(tc.text),
                 Theme::hex(tc.controlBorder),
                 QString::number(m.radiusSm),
                 Theme::hex(tc.accent)));
    toolbarLayout->addWidget(m_searchEdit, 1);

    // Debounced search — 200ms after last keystroke
    m_searchDebounce = new QTimer(this);
    m_searchDebounce->setSingleShot(true);
    m_searchDebounce->setInterval(200);
    connect(m_searchDebounce, &QTimer::timeout, this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refresh(); });
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; m_searchDebounce->start(); });

    // Small toolbar button style
    QString tbStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none; color: %1; "
        "font-size: 13px; padding: 2px; border-radius: 3px; }"
        "QToolButton:hover { background: %2; color: %3; }")
        .arg(Theme::hex(tc.textSecondary),
             Theme::hex(tc.controlBgHover),
             Theme::hex(tc.text));

    // Sort A→Z toggle
    m_btnSortAZ = new QToolButton(toolbar);
    m_btnSortAZ->setText(QStringLiteral("A\u2193Z"));
    m_btnSortAZ->setToolTip(tr("Sort alphabetically"));
    m_btnSortAZ->setFixedSize(28, 22);
    m_btnSortAZ->setCheckable(true);
    m_btnSortAZ->setChecked(true);
    m_btnSortAZ->setStyleSheet(tbStyle +
        QStringLiteral("\nQToolButton:checked { color: %1; }")
            .arg(Theme::hex(tc.accent)));
    toolbarLayout->addWidget(m_btnSortAZ);
    connect(m_btnSortAZ, &QToolButton::toggled,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refresh(); });

    // Refresh button
    m_btnRefresh = new QToolButton(toolbar);
    m_btnRefresh->setText(QStringLiteral("\U0001F504"));
    m_btnRefresh->setToolTip(tr("Refresh character list"));
    m_btnRefresh->setFixedSize(22, 22);
    m_btnRefresh->setStyleSheet(tbStyle);
    toolbarLayout->addWidget(m_btnRefresh);
    connect(m_btnRefresh, &QToolButton::clicked,
            this, [this]() { if (m_destroying.load(std::memory_order_acquire)) return; refresh(); });

    lay->addWidget(toolbar);

    // ── Tree widget ──────────────────────────────────────────────────────
    m_tree = new MediaDragTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(16);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setStyleSheet(
        QString("QTreeWidget { background: %1; color: %2; border: none; }"
                "QTreeWidget::item { padding: 2px 0; }"
                "QTreeWidget::item:hover { background: %3; }"
                "QTreeWidget::item:selected { background: %4; }")
            .arg(Theme::hex(tc.surface0), Theme::hex(tc.text),
                 Theme::hex(tc.controlBgHover),
                 Theme::hex(tc.accent)));
    lay->addWidget(m_tree, 1);

    // ── Empty state label (shown when tree is empty) ─────────────────────
    m_emptyLabel = new QLabel(tr("No characters found"), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 13px; background: %2; border: none; }")
            .arg(Theme::hex(tc.textDisabled), Theme::hex(tc.surface0)));
    m_emptyLabel->hide();
    lay->addWidget(m_emptyLabel, 1);

    // ── Status bar ───────────────────────────────────────────────────────
    auto* statusBar = new QWidget(this);
    statusBar->setFixedHeight(22);
    statusBar->setStyleSheet(QStringLiteral(
        "QWidget { background: %1; border-top: 1px solid %2; }")
        .arg(Theme::hex(tc.surface2), Theme::hex(tc.panelBorder)));
    auto* statusLayout = new QHBoxLayout(statusBar);
    statusLayout->setContentsMargins(m.spacingMd, 0, m.spacingMd, 0);
    statusLayout->setSpacing(0);

    m_statusLabel = new QLabel(QStringLiteral("0 items"), statusBar);
    m_statusLabel->setStyleSheet(
        QString("QLabel { color: %1; font-size: 12px; background: transparent; border: none; }")
            .arg(Theme::hex(tc.textSecondary)));
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();

    lay->addWidget(statusBar);
}

// ─────────────────────────────────────────────────────────────────────────────

void CharactersPanel::refresh()
{
    m_tree->clear();

    const auto& tc = Theme::colors();
    QString searchTerm = m_searchEdit ? m_searchEdit->text().trimmed().toLower() : QString();

#ifdef ROUNDTABLE_HAS_SPINE
    spdlog::info("CharactersPanel::refresh — mgr={}, scanned={}",
                 static_cast<const void*>(m_modelManager),
                 m_modelManager ? m_modelManager->isScanned() : false);

    if (m_modelManager && m_modelManager->isScanned()) {

    auto charNames = m_modelManager->characterNames();
    bool ascending = !m_btnSortAZ || m_btnSortAZ->isChecked();
    if (ascending)
        std::sort(charNames.begin(), charNames.end());
    else
        std::sort(charNames.begin(), charNames.end(), std::greater<>());

    for (const auto& charName : charNames) {
        // Use display name for UI
        std::string dispName = m_modelManager->getDisplayName(charName);
        QString qName = QString::fromStdString(dispName);
        if (!searchTerm.isEmpty() && !qName.toLower().contains(searchTerm))
            continue;

        // Parent item for this character
        auto* charItem = new QTreeWidgetItem;
        charItem->setText(0, qName);
        charItem->setForeground(0, tc.text);

        const auto* entry = m_modelManager->findByName(charName);
        if (!entry) { delete charItem; continue; }

        // Enable drag on the character item — dropping defaults to
        // the first outfit, Default stance, idle animation.
        // The user can always change the stance/animation later in Properties.
        if (!entry->outfits.empty()) {
            QString spineUri = QStringLiteral("spine:")
                + QString::fromStdString(charName)
                + "|" + QString::fromStdString(entry->outfits[0].name)
                + "|0|idle";
            charItem->setData(0, Qt::UserRole, spineUri);
            charItem->setData(0, Qt::UserRole + 1,
                              QVariant::fromValue(static_cast<quint64>(0)));
            charItem->setData(0, Qt::UserRole + 2, false);
            charItem->setFlags(charItem->flags() | Qt::ItemIsDragEnabled);
        }

        // Scan the skeleton directories for each outfit+stance combination.
        // We look for .skel files in:
        //   assets/characters/<charName>/<outfit>/          (Default stance)
        //   assets/characters/<charName>/<outfit>/aim/      (Aim stance)
        //   assets/characters/<charName>/<outfit>/cover/    (Cover stance)
        for (const auto& outfit : entry->outfits) {
            // Determine the base directory for this outfit
            std::string baseDir = "assets/characters/" + charName + "/" + outfit.name;

            // Collect stances that have skeleton files
            struct StanceInfo {
                std::string displayName;
                int stanceInt; // 0=Default, 1=Aim, 2=Cover
                std::string skelPath;
            };
            std::vector<StanceInfo> stances;

            // Check Default stance (files directly in outfit dir)
            {
                std::string defPath = baseDir;
                for (const auto& variant : outfit.variants) {
                    if (!variant.skelPath.empty()) {
                        int si = static_cast<int>(variant.stance);
                        std::string sName = "Default";
                        if (variant.stance == CharacterStance::Aim) sName = "Aim";
                        else if (variant.stance == CharacterStance::Cover) sName = "Cover";
                        stances.push_back({sName, si, variant.skelPath});
                    }
                }
            }

            if (stances.empty()) continue;

            // Determine parent for stance items
            QTreeWidgetItem* parentForStances = charItem;
            bool multiOutfit = (entry->outfits.size() > 1);
            if (multiOutfit) {
                auto* outfitItem = new QTreeWidgetItem(charItem);
                QString oName = outfit.displayName.empty()
                    ? QString::fromStdString(outfit.name)
                    : QString::fromStdString(outfit.displayName);
                outfitItem->setText(0, oName);
                outfitItem->setForeground(0, tc.textSecondary);

                // Enable drag on outfit items — defaults to Default stance, idle
                QString spineUri = QStringLiteral("spine:")
                    + QString::fromStdString(charName)
                    + "|" + QString::fromStdString(outfit.name)
                    + "|0|idle";
                outfitItem->setData(0, Qt::UserRole, spineUri);
                outfitItem->setData(0, Qt::UserRole + 1,
                                    QVariant::fromValue(static_cast<quint64>(0)));
                outfitItem->setData(0, Qt::UserRole + 2, false);
                outfitItem->setFlags(outfitItem->flags() | Qt::ItemIsDragEnabled);

                parentForStances = outfitItem;
            }

            // Create a stance group item for each stance
            for (const auto& si : stances) {
                auto* stanceItem = new QTreeWidgetItem(parentForStances);
                stanceItem->setText(0, QString::fromStdString(si.displayName));
                stanceItem->setForeground(0, tc.textSecondary);
                stanceItem->setFlags(stanceItem->flags() & ~Qt::ItemIsDragEnabled);

                // Store metadata for lazy loading: we'll populate children
                // when the user expands this item. Use a placeholder child
                // so the expansion arrow appears.
                auto* placeholder = new QTreeWidgetItem(stanceItem);
                placeholder->setText(0, QStringLiteral("(loading\u2026)"));
                placeholder->setForeground(0, tc.textDisabled);
                placeholder->setFlags(placeholder->flags() & ~Qt::ItemIsDragEnabled);

                // Store stance metadata for lazy loading callback
                QString stanceKey = QString::fromStdString(
                    charName + "|" + outfit.name + "|" + std::to_string(si.stanceInt));
                stanceItem->setData(0, Qt::UserRole, stanceKey);
            }
        }

        if (charItem->childCount() > 0)
            m_tree->addTopLevelItem(charItem);
        else
            delete charItem;
    }

    spdlog::info("CharactersPanel::refresh — {} characters scanned, {} tree items",
                 charNames.size(), m_tree->topLevelItemCount());

    // ── Connect itemExpanded for lazy loading ──────────────────────────
    // Disconnect old connections first to avoid duplicates on re-refresh.
    disconnect(m_tree, &QTreeWidget::itemExpanded, nullptr, nullptr);
    connect(m_tree, &QTreeWidget::itemExpanded,
            this, [this](QTreeWidgetItem* item) {
        if (m_destroying.load(std::memory_order_acquire)) return;
        // Check if this item has the stance metadata
        QString key = item->data(0, Qt::UserRole).toString();
        if (key.isEmpty()) return;

        // Parse key: "charName|outfit|stanceInt"
        QStringList parts = key.split('|');
        if (parts.size() != 3) return;

        std::string charName = parts[0].toStdString();
        std::string outfit   = parts[1].toStdString();
        int stanceInt        = parts[2].toInt();

        // Only load if the first child is still the placeholder
        if (item->childCount() == 1) {
            auto* firstChild = item->child(0);
            if (firstChild && firstChild->text(0) == QStringLiteral("(loading\u2026)")) {
                // Remove the placeholder
                item->removeChild(firstChild);
                delete firstChild;

                // Load animation names from skeleton
                populateStanceAnims(item, charName, outfit, stanceInt);
            }
        }
    });

    } // end if (m_modelManager && ...)
#endif

    // ── Video-only characters (from shot presets) ──────────────────────
    {
        // Collect names already shown as Spine characters
        std::set<QString> spineNames;
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_modelManager && m_modelManager->isScanned()) {
            for (const auto& n : m_modelManager->characterNames())
                spineNames.insert(QString::fromStdString(n));
        }
#endif
        struct VCEntry { QString name; QString mutePath; QString talkPath; };
        std::vector<VCEntry> videoChars;

        QDir presetsDir(QStringLiteral("assets/presets/shots"));
        if (presetsDir.exists()) {
            for (const auto& fname : presetsDir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
                QFile f(presetsDir.filePath(fname));
                if (!f.open(QIODevice::ReadOnly)) continue;
                QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
                f.close();
                if (!doc.isObject()) continue;
                QJsonArray chars = doc.object().value(QStringLiteral("characters")).toArray();
                for (const auto& cv : chars) {
                    QJsonObject ch = cv.toObject();
                    QString mute = ch.value(QStringLiteral("videoMutePath")).toString();
                    if (mute.isEmpty()) continue;
                    QString charName = ch.value(QStringLiteral("characterName")).toString();
                    if (charName.isEmpty() || spineNames.contains(charName)) continue;
                    // Deduplicate
                    bool already = false;
                    for (const auto& vc : videoChars)
                        if (vc.name == charName) { already = true; break; }
                    if (already) continue;
                    QString talk = ch.value(QStringLiteral("videoTalkPath")).toString();
                    videoChars.push_back({charName, mute, talk});
                }
            }
        }

        bool ascending = !m_btnSortAZ || m_btnSortAZ->isChecked();
        if (ascending)
            std::sort(videoChars.begin(), videoChars.end(),
                      [](const VCEntry& a, const VCEntry& b) { return a.name < b.name; });
        else
            std::sort(videoChars.begin(), videoChars.end(),
                      [](const VCEntry& a, const VCEntry& b) { return a.name > b.name; });

        for (const auto& vc : videoChars) {
            if (!searchTerm.isEmpty() && !vc.name.toLower().contains(searchTerm))
                continue;

            auto* charItem = new QTreeWidgetItem;
            charItem->setText(0, QStringLiteral("\xF0\x9F\x8E\xAC  ") + vc.name);
            charItem->setForeground(0, QColor(200, 170, 255));

            // Mute variant
            if (!vc.mutePath.isEmpty() && std::filesystem::exists(vc.mutePath.toStdString())) {
                auto* muteItem = new QTreeWidgetItem(charItem);
                muteItem->setText(0, QStringLiteral("Mute"));
                muteItem->setForeground(0, tc.success);
                muteItem->setData(0, Qt::UserRole, vc.mutePath);
                muteItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                muteItem->setData(0, Qt::UserRole + 2, false);
                muteItem->setFlags(muteItem->flags() | Qt::ItemIsDragEnabled);

                // Default drag data for the character header = mute variant
                charItem->setData(0, Qt::UserRole, vc.mutePath);
                charItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                charItem->setData(0, Qt::UserRole + 2, false);
                charItem->setFlags(charItem->flags() | Qt::ItemIsDragEnabled);
            }

            // Talk variant
            if (!vc.talkPath.isEmpty() && std::filesystem::exists(vc.talkPath.toStdString())) {
                auto* talkItem = new QTreeWidgetItem(charItem);
                talkItem->setText(0, QStringLiteral("Talk"));
                talkItem->setForeground(0, tc.success);
                talkItem->setData(0, Qt::UserRole, vc.talkPath);
                talkItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
                talkItem->setData(0, Qt::UserRole + 2, false);
                talkItem->setFlags(talkItem->flags() | Qt::ItemIsDragEnabled);
            }

            if (charItem->childCount() > 0)
                m_tree->addTopLevelItem(charItem);
            else
                delete charItem;
        }
    }

    // ── Update empty state + status bar ─────────────────────────────────
    int totalItems = m_tree->topLevelItemCount();
    bool empty = (totalItems == 0);
    m_tree->setVisible(!empty);
    m_emptyLabel->setVisible(empty);
    m_statusLabel->setText(empty ? tr("0 items")
        : (totalItems == 1 ? tr("1 item")
                           : tr("%1 items").arg(totalItems)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lazy loading: populate animation names under a stance group item
// ─────────────────────────────────────────────────────────────────────────────

void CharactersPanel::populateStanceAnims(QTreeWidgetItem* stanceItem,
                                           const std::string& charName,
                                           const std::string& outfit,
                                           int stanceInt)
{
    const auto& tc = Theme::colors();

#ifdef ROUNDTABLE_HAS_SPINE
    // Resolve the skeleton path for this character/outfit/stance
    CharacterStance stance = CharacterStance::Default;
    if (stanceInt == 1) stance = CharacterStance::Aim;
    else if (stanceInt == 2) stance = CharacterStance::Cover;

    // Get the working assets directory from ModelManager
    std::string assetsDir = m_modelManager ? m_modelManager->assetsDir() : "assets";

    auto paths = SpineEngine::resolvePaths(assetsDir, charName, outfit, stance);
    if (!paths.valid) {
        spdlog::warn("CharactersPanel: cannot resolve skeleton for {} / {} / stance={}",
                     charName, outfit, stanceInt);
        auto* errItem = new QTreeWidgetItem(stanceItem);
        errItem->setText(0, QStringLiteral("(skeleton not found)"));
        errItem->setForeground(0, tc.textDisabled);
        errItem->setFlags(errItem->flags() & ~Qt::ItemIsDragEnabled);
        return;
    }

    // Load the skeleton temporarily to get animation names
    SpineEngine engine;
    if (!engine.loadSkeleton(paths.skelPath, paths.atlasPath)) {
        spdlog::warn("CharactersPanel: failed to load skeleton for {} / {} / stance={}",
                     charName, outfit, stanceInt);
        auto* errItem = new QTreeWidgetItem(stanceItem);
        errItem->setText(0, QStringLiteral("(failed to load)"));
        errItem->setForeground(0, tc.textDisabled);
        errItem->setFlags(errItem->flags() & ~Qt::ItemIsDragEnabled);
        return;
    }

    auto animInfos = engine.animation().listAnimations();
    spdlog::info("CharactersPanel: loaded {} animations for {} / {} / stance={}",
                 animInfos.size(), charName, outfit, stanceInt);

    // Sort animations alphabetically
    std::sort(animInfos.begin(), animInfos.end(),
              [](const AnimationInfo& a, const AnimationInfo& b) {
                  return a.name < b.name;
              });

    for (const auto& info : animInfos) {
        // Skip zero-duration / internal animations
        if (info.duration <= 0.0f) continue;
        if (info.name == "talk_start" || info.name == "talk_end") continue;

        auto* animItem = new QTreeWidgetItem(stanceItem);
        animItem->setText(0, QString::fromStdString(info.name));
        animItem->setForeground(0, tc.success);

        // Store Spine animation data for drag-drop.
        // Format: "spine:charName|outfit|stanceInt|animName"
        QString spineData = QString::fromStdString(
            "spine:" + charName + "|" + outfit + "|"
            + std::to_string(stanceInt) + "|" + info.name);
        animItem->setData(0, Qt::UserRole, spineData);
        animItem->setData(0, Qt::UserRole + 1, QVariant::fromValue(static_cast<quint64>(0)));
        animItem->setData(0, Qt::UserRole + 2, false);
        animItem->setFlags(animItem->flags() | Qt::ItemIsDragEnabled);
    }

    if (stanceItem->childCount() == 0) {
        // No valid animations found
        auto* emptyItem = new QTreeWidgetItem(stanceItem);
        emptyItem->setText(0, QStringLiteral("(no animations)"));
        emptyItem->setForeground(0, tc.textDisabled);
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsDragEnabled);
    }
#else
    (void)stanceItem;
    (void)charName;
    (void)outfit;
    (void)stanceInt;
#endif
}

} // namespace rt
