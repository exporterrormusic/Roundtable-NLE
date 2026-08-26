/*
 * CharacterBrowser.cpp â€” Character download and preview panel.
 *
 * Ported from the original Python CharacterPanel.
 */

#include "panels/characters/CharacterBrowser.h"
#include "panels/characters/PuppetLibrary.h"

#include "Theme.h"
#include "QtHelpers.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#include <QAbstractItemView>
#include <spdlog/spdlog.h>

namespace rt {

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Construction
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

CharacterBrowser::CharacterBrowser(QWidget* parent)
    : QWidget(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    setupUI();

    // Load permanently hidden characters from settings
    QSettings settings;
    const QStringList hidden = settings.value("CharacterBrowser/HiddenChars").toStringList();
    for (const auto& h : hidden)
        m_hiddenCharNames.insert(h);

    // Default display name overrides for characters with duplicate/suffixed entries
    // (these can be overridden by the user via the Rename... context menu)
    auto setDefaultName = [&](const QString& folderName, const QString& customName) {
        m_defaultDisplayNames[folderName] = customName;
    };
    setDefaultName("E.H. (c113)", "E.H.");
    setDefaultName("E.H. (c940)", "E.H. (Original)");
    setDefaultName("Freesia (c9020)", "Freesia (Pretty)");
    setDefaultName("Freesia (c960)", "Freesia (Child)");
    setDefaultName("Laplace (c100)", "Laplace");
    setDefaultName("Laplace (c103)", "Laplace (Upgraded)");
    setDefaultName("Leviathan (c562)", "Leviathan");
    setDefaultName("Leviathan (c996)", "Leviathan (Child)");
    setDefaultName("Drake (c101)", "Drake");
    setDefaultName("Drake (c104)", "Drake (Upgraded)");
    setDefaultName("Maxwell (c102)", "Maxwell");
    setDefaultName("Maxwell (c105)", "Maxwell (Upgraded)");
    setDefaultName("Mustang (c902)", "Mustang");
    setDefaultName("Mustang (c9022)", "Mustang (Evil)");
    setDefaultName("Nayuta (c223)", "Nayuta");
    setDefaultName("Nayuta (c9008)", "Nayuta (Monk)");
    setDefaultName("Neon (c011)", "Neon");
    setDefaultName("Neon (c018)", "Neon: Vision Eye");
    setDefaultName("Anis (c012)", "Anis");
    setDefaultName("Anis (c017)", "Anis: Star");
    setDefaultName("Anis (c9021)", "Anis: Early Days");
    setDefaultName("Nihilister (c261)", "Nihilister");
    setDefaultName("Nihilister (c9009)", "Nihilister (Cloak)");
    setDefaultName("Rapi (c010)", "Rapi");
    setDefaultName("Rapi (c989)", "Rapi: Red Hood (Original)");
    setDefaultName("Rapi (c990)", "Rapi (Teen)");
    setDefaultName("Rapi (c992)", "Rapi (Child)");
    setDefaultName("Rapi (c994)", "Rapi (Mass Produced)");
    setDefaultName("Yuni (c160)", "Yuni");
    setDefaultName("Yuni (c985)", "Yuni (Punished)");

    // Load custom display name overrides from settings
    const int renameCount = settings.beginReadArray("CharacterBrowser/RenamedDisplayNames");
    for (int i = 0; i < renameCount; ++i) {
        settings.setArrayIndex(i);
        QString folderName = settings.value("folderName").toString();
        QString customName = settings.value("customName").toString();
        // Older builds wrote all built-in defaults to this settings array.
        // Ignore those generated entries so only actual user renames are
        // synchronized into Compose.
        if (!folderName.isEmpty() && !customName.isEmpty()
            && m_defaultDisplayNames.value(folderName) != customName) {
            m_customDisplayNames[folderName] = customName;
        }
    }
    settings.endArray();

    // F6: Restore last selected character
    QString lastChar = settings.value("CharacterBrowser/LastSelected").toString();
    if (!lastChar.isEmpty()) {
        for (int i = 0; i < m_characterList->count(); ++i) {
            if (m_characterList->item(i)->data(Qt::UserRole).toString() == lastChar) {
                m_characterList->setCurrentRow(i);
                break;
            }
        }
    }
}

CharacterBrowser::~CharacterBrowser() = default;

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Configuration
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void CharacterBrowser::setModelManager(ModelManager* mgr)
{
    m_modelManager = mgr;
    refreshList();
}

void CharacterBrowser::setAnimVideoCache(AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    // Refresh to show/update conversion badges
    if (m_characterList->count() > 0)
        populateCharacterList();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Actions
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void CharacterBrowser::refreshList()
{
    populateCharacterList();
}

QString CharacterBrowser::selectedCharacter() const
{
    auto items = m_characterList->selectedItems();
    if (items.isEmpty()) return {};
    return items.first()->data(Qt::UserRole).toString();
}

QStringList CharacterBrowser::selectedCharacters() const
{
    QStringList names;
    for (auto* item : m_characterList->selectedItems())
        names.append(item->data(Qt::UserRole).toString());
    return names;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// UI Setup
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•


void CharacterBrowser::discoverVideoCharacters()
{
    m_videoCharNames.clear();
    QDir presetsDir(QStringLiteral("assets/presets/shots"));
    if (!presetsDir.exists()) return;

    for (const auto& fname : presetsDir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile f(presetsDir.filePath(fname));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject()) continue;
        QJsonArray chars = doc.object().value(QStringLiteral("characters")).toArray();
        for (const auto& cv : chars) {
            QJsonObject ch = cv.toObject();
            QString mutePath = ch.value(QStringLiteral("videoMutePath")).toString();
            if (mutePath.isEmpty()) continue;
            // Only include this video character if the referenced video file
            // actually exists on disk — otherwise it won't work in the
            // exported/installer version where video assets may be absent.
            if (!QFileInfo::exists(mutePath))
                continue;
            QString charName = ch.value(QStringLiteral("characterName")).toString();
            if (!charName.isEmpty())
                m_videoCharNames.insert(charName);
        }
    }
    // Remove any that already have Spine data on disk
    for (const auto& local : m_localCharNames)
        m_videoCharNames.erase(local);

    // Remove any that now have a custom PNG puppet of the same name — the
    // puppet supersedes the legacy video character (e.g. Wells migrated from
    // her green-screen videos to a custom 4-image puppet), so we don't list
    // the video version alongside it.
    for (const QString& folder : puppetlib::listPuppetFolders()) {
        PuppetManifest pm;
        if (puppetlib::load(folder, pm))
            m_videoCharNames.erase(pm.displayName.isEmpty() ? folder : pm.displayName);
    }
}

void CharacterBrowser::populateCharacterList()
{
    m_characterList->clear();
    m_localCharNames.clear();

    // P1: Load metadata once (cached across calls)
    loadMetadata();

    // Normalize metadata names for Windows filesystem
    auto sanitize = [](const QString& name) {
        QString s = name;
        // Replace characters illegal in Windows paths
        for (QChar& c : s) {
            if (c == ':' || c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
                c = '_';
        }
        while (s.endsWith('.') || s.endsWith(' '))
            s.chop(1);
        return s;
    };

    // Gather local character names (folder names on disk)
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_modelManager && m_modelManager->isScanned()) {
        for (const auto& name : m_modelManager->characterNames())
            m_localCharNames.insert(QString::fromStdString(name));
    }
#endif

    QDir charDir("assets/characters");
    if (charDir.exists()) {
        for (const auto& entry : charDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            m_localCharNames.insert(entry);
    }

    // Discover video-only characters from shot presets
    discoverVideoCharacters();
    for (const auto& vc : m_videoCharNames)
        m_localCharNames.insert(vc);

    bool downloadedOnly = m_downloadedOnly->isChecked();
    int shown = 0;

    // --- Build character entries, preserving all metadata entries even with duplicate names ---
    struct CharEntry {
        QString folderName;   // on-disk folder name, sanitized (key for UserRole)
        QString charId;       // metadata character ID (empty for local-only)
        QString baseName;     // sanitized name for dedup/search
        QString displayName;  // visual name shown in list (preserves colons etc.)
    };

    // Detect duplicate names in metadata (using sanitized names for Windows compat)
    QMap<QString, QStringList> nameToIds;
    for (auto it = m_cachedCharacters.begin(); it != m_cachedCharacters.end(); ++it) {
        QString name = sanitize(it.value().toObject().value("name").toString());
        if (!name.isEmpty())
            nameToIds[name].append(it.key());
    }
    for (auto it = nameToIds.begin(); it != nameToIds.end(); ++it)
        it.value().sort();

    std::set<QString> addedFolders;
    std::vector<CharEntry> allEntries;

    // Add all local characters (from disk scan)
    for (const auto& localName : m_localCharNames) {
        // Try to associate a charId from metadata
        QString charId;
        // Check disambiguated folder names like "Name (charId)"
        int parenPos = localName.lastIndexOf(QStringLiteral(" ("));
        if (parenPos >= 0 && localName.endsWith(')')) {
            charId = localName.mid(parenPos + 2, localName.size() - parenPos - 3);
            if (!m_cachedCharacters.contains(charId))
                charId.clear();
        }
        // Check plain name match — assign first charId that maps to this name
        if (charId.isEmpty() && nameToIds.contains(localName)) {
            charId = nameToIds[localName].first();
        }
        // For local entries, look up original display name from metadata
        QString displayLabel = localName;
        if (!charId.isEmpty()) {
            QString rawName = m_cachedCharacters.value(charId).toObject().value("name").toString();
            if (!rawName.isEmpty()) {
                int p = localName.lastIndexOf(QStringLiteral(" ("));
                if (p >= 0 && localName.endsWith(')'))
                    displayLabel = rawName + localName.mid(p);
                else
                    displayLabel = rawName;
            }
        }
        allEntries.push_back({localName, charId, localName, displayLabel});
        addedFolders.insert(localName);
    }

    // Add metadata entries (unless "Downloaded Only" is checked)
    if (!downloadedOnly) {
        for (auto it = m_cachedCharacters.begin(); it != m_cachedCharacters.end(); ++it) {
            QString charId = it.key();
            QString rawName = it.value().toObject().value("name").toString();
            QString name = sanitize(rawName);
            if (name.isEmpty()) continue;

            // Determine folder name (disambiguate if multiple IDs share the same name)
            const QStringList& ids = nameToIds[name];
            QString folderName, displayLabel;
            if (ids.size() > 1) {
                // The first charId uses the plain name only if a local folder already exists
                if (charId == ids.first() && m_localCharNames.contains(name)) {
                    folderName = name;
                    displayLabel = rawName;
                } else {
                    folderName = name + QStringLiteral(" (") + charId + QStringLiteral(")");
                    displayLabel = rawName + QStringLiteral(" (") + charId + QStringLiteral(")");
                }
            } else {
                folderName = name;
                displayLabel = rawName;
            }

            if (!addedFolders.contains(folderName)) {
                allEntries.push_back({folderName, charId, name, displayLabel});
                addedFolders.insert(folderName);
            } else {
                // Folder already in list from local scan — update its charId if missing
                for (auto& e : allEntries) {
                    if (e.folderName == folderName && e.charId.isEmpty()) {
                        e.charId = charId;
                        e.baseName = name;
                        e.displayName = displayLabel;
                        break;
                    }
                }
            }
        }

        // Add remote IDs not in metadata (only base character IDs, not variants)
        static QRegularExpression baseCharRe(QStringLiteral("^c\\d+$"));
        for (const auto& id : m_remoteCharIds) {
            if (!m_cachedCharacters.contains(id) && !addedFolders.contains(id)
                && baseCharRe.match(id).hasMatch()) {
                allEntries.push_back({id, id, id, id});
                addedFolders.insert(id);
            }
        }
    }

    // Sort by folder name
    std::sort(allEntries.begin(), allEntries.end(),
              [](const CharEntry& a, const CharEntry& b) { return a.folderName < b.folderName; });

    for (const auto& ce : allEntries) {
        const QString& name = ce.folderName;
        const QString& display = ce.displayName;

        // Search filter — match against folder name, base name, and display name
        if (!m_currentFilter.isEmpty() &&
            !name.contains(m_currentFilter, Qt::CaseInsensitive) &&
            !ce.baseName.contains(m_currentFilter, Qt::CaseInsensitive) &&
            !display.contains(m_currentFilter, Qt::CaseInsensitive))
            continue;

        // Category filter
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_currentCategory > 0 && m_modelManager) {
            const auto* entry = m_modelManager->findByName(name.toStdString());
            if (entry) {
                static const char* cats[] = {"", "Character", "NPC", "Background", "Other"};
                if (m_currentCategory < 5 &&
                    entry->category != cats[m_currentCategory])
                    continue;
            }
        }
#endif

        // Hidden filter — skip permanently hidden characters unless "Show Hidden" is checked
        bool isHidden = m_hiddenCharNames.contains(name);
        if (isHidden && !m_showHidden->isChecked())
            continue;

        bool isLocal = m_localCharNames.contains(name);

        // Check if character has pre-rendered video cache
        bool hasCachedVideo = false;
        size_t cachedCount = 0;
#ifdef ROUNDTABLE_HAS_SPINE
        if (isLocal && m_animVideoCache) {
            hasCachedVideo = m_animVideoCache->hasAnyForCharacter(name.toStdString());
            if (hasCachedVideo)
                cachedCount = m_animVideoCache->countForCharacter(name.toStdString());
        }
#endif

        // Apply a user rename first, then the built-in label for suffixed entries.
        const QString effectiveDisplay = displayNameFor(name, display);

        // U7: Download status icon per list item
        bool isVideoOnly = m_videoCharNames.contains(name);
        QString displayText;
        if (isVideoOnly)
            displayText = QStringLiteral("\xF0\x9F\x8E\xAC  ") + effectiveDisplay;
        else if (isLocal && hasCachedVideo)
            displayText = QStringLiteral("\xE2\x9C\x85  \xF0\x9F\x8E\xAC  ") + effectiveDisplay;
        else if (isLocal)
            displayText = QStringLiteral("\xE2\x9C\x85  ") + effectiveDisplay;
        else
            displayText = QStringLiteral("\xE2\xAC\x87  ") + effectiveDisplay;

        auto* item = new QListWidgetItem(displayText, m_characterList);

        // Store folder name in UserRole, download status in UserRole+1, charId in UserRole+2
        item->setData(Qt::UserRole, name);
        item->setData(Qt::UserRole + 1, isLocal);
        item->setData(Qt::UserRole + 2, ce.charId);

        // Color-code items for quick visual distinction
        if (isVideoOnly) {
            item->setBackground(QColor(80, 60, 120, 45));
            item->setForeground(QColor(200, 170, 255));
            item->setToolTip(QString("Video-only character \xE2\x80\x94 %1").arg(display));
        } else if (isLocal && hasCachedVideo) {
            item->setBackground(QColor(40, 120, 60, 45));
            item->setForeground(QColor(140, 255, 160));
            item->setToolTip(QString("Downloaded + video cached (%1 animations) \xE2\x80\x94 %2")
                                 .arg(cachedCount).arg(display));
        } else if (isLocal) {
            item->setToolTip(QString("Downloaded (not converted) \xE2\x80\x94 %1").arg(display));
        } else {
            item->setForeground(QColor(140, 140, 140));
            item->setToolTip(QString("Not downloaded \xE2\x80\x94 select and click Download"));
        }

        ++shown;
    }

    // F5: Update title with count
    int dlCount = static_cast<int>(m_localCharNames.size());
    if (m_titleLabel) {
        m_titleLabel->setText(QString("\xF0\x9F\x91\xA5  Characters  (%1 / %2 downloaded)")
            .arg(dlCount).arg(shown));
    }
    m_statusLabel->setText(QString("%1 characters shown (%2 downloaded)")
        .arg(shown).arg(dlCount));
}

void CharacterBrowser::populateControls()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_modelManager) return;

    auto charName = selectedCharacter();
    if (charName.isEmpty()) return;

    const auto* entry = m_modelManager->findByName(charName.toStdString());

    // Populate outfits â€” prefer metadata list (includes undownloaded), fall back to disk
    m_outfitCombo->blockSignals(true);
    m_outfitCombo->clear();

    auto metaOutfits = m_modelManager->getMetadataOutfits(charName.toStdString());

    if (!metaOutfits.empty()) {
        for (const auto& mo : metaOutfits) {
            // Check if this outfit is actually on disk
            bool onDisk = false;
            if (entry) {
                for (const auto& o : entry->outfits) {
                    if (o.name == mo.key) { onDisk = true; break; }
                }
            }
            QString label = QString::fromStdString(mo.displayName);
            // Mark default outfit clearly
            if (mo.key == "default" && metaOutfits.size() > 1)
                label += QStringLiteral("  (Default)");
            if (!onDisk)
                label += QString::fromUtf8("  \u2B07");  // â¬‡ not downloaded
            m_outfitCombo->addItem(label, QString::fromStdString(mo.key));
        }
    } else if (entry) {
        // Fallback: use disk-based outfits
        for (const auto& outfit : entry->outfits) {
            QString label = QString::fromStdString(outfit.displayName);
            if (outfit.name == "default" && entry->outfits.size() > 1)
                label += QStringLiteral("  (Default)");
            m_outfitCombo->addItem(label, QString::fromStdString(outfit.name));
        }
    }
    if (m_outfitCombo->count() == 0)
        m_outfitCombo->addItem("default", "default");
    m_outfitCombo->blockSignals(false);

    // Populate stances from what's actually available in the current outfit
    m_stanceCombo->blockSignals(true);
    m_stanceCombo->clear();
    if (entry) {
        QString currentOutfitKey = m_outfitCombo->currentData().toString();
        for (const auto& outfit : entry->outfits) {
            if (QString::fromStdString(outfit.name) == currentOutfitKey) {
                for (const auto& variant : outfit.variants) {
                    switch (variant.stance) {
                    case CharacterStance::Default: m_stanceCombo->addItem("Default"); break;
                    case CharacterStance::Aim:     m_stanceCombo->addItem("Aim"); break;
                    case CharacterStance::Cover:   m_stanceCombo->addItem("Cover"); break;
                    }
                }
                break;
            }
        }
    }
    if (m_stanceCombo->count() == 0)
        m_stanceCombo->addItem("Default");
    m_stanceCombo->blockSignals(false);

    // Load the skeleton and populate animations
    loadPreviewModel();
#endif
}

void CharacterBrowser::loadPreviewModel()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_modelManager) return;

    // Clear the live preview unconditionally on entry — every early-return
    // failure path below leaves the widget in a clean state (no leftover
    // textures or skeleton pose from a previously-selected character).
    // Without this, when findByName / findVariant fails, the user sees
    // the *previous* character's textures sitting in the preview pane and
    // perceives it as a stale / corrupt thumbnail for the just-clicked
    // character (the post-canceled-download repro path).  The widget is
    // re-populated below on the success branch via setSpineEngine + the
    // SpinePreviewWidget::loadTextures() call.
    auto clearPreview = [this]() {
        if (m_spinePreview) {
            m_spinePreview->stopAnimation();
            m_spinePreview->setSpineEngine(nullptr);
            m_spinePreview->loadTextures();  // null engine → clears m_textures
            m_spinePreview->update();
        }
    };

    auto charName = selectedCharacter();
    if (charName.isEmpty()) { clearPreview(); return; }

    const auto* entry = m_modelManager->findByName(charName.toStdString());
    if (!entry) {
        spdlog::warn("CharacterBrowser::loadPreviewModel: findByName('{}') returned null — {} total entries in catalog",
                     charName.toStdString(), m_modelManager->entries().size());
        // Show display name (with colons) in status
        QString dispName = QString::fromStdString(m_modelManager->getDisplayName(charName.toStdString()));
        m_statusLabel->setText(QString("%1 \xe2\x80\x94 not downloaded").arg(dispName));
        clearPreview();
        return;
    }
    spdlog::info("CharacterBrowser::loadPreviewModel: found '{}' id={} outfits={}",
                 entry->name, entry->id, entry->outfits.size());

    // Determine current outfit key from combo data
    QString outfitKey = m_outfitCombo->currentData().toString();
    if (outfitKey.isEmpty()) outfitKey = "default";

    CharacterStance stance = CharacterStance::Default;
    QString stanceText = m_stanceCombo->currentText();
    if (stanceText == "Aim") stance = CharacterStance::Aim;
    else if (stanceText == "Cover") stance = CharacterStance::Cover;

    // Find the variant
    const auto* variant = m_modelManager->findVariant(
        charName.toStdString(), outfitKey.toStdString(), stance);

    if (!variant || variant->skelPath.empty() || variant->atlasPath.empty()) {
        spdlog::warn("CharacterBrowser::loadPreviewModel: no variant for '{}' outfit='{}' stance={} — variant={}",
                     charName.toStdString(), outfitKey.toStdString(), static_cast<int>(stance),
                     variant ? "found-but-empty" : "null");
        for (const auto& o : entry->outfits) {
            spdlog::warn("  outfit '{}': {} variants", o.name, o.variants.size());
            for (const auto& v : o.variants)
                spdlog::warn("    stance={} skel='{}' atlas='{}'",
                             static_cast<int>(v.stance), v.skelPath, v.atlasPath);
        }
        m_statusLabel->setText(QString("Outfit not downloaded \xe2\x80\x94 click Download"));
        clearPreview();
        return;
    }
    spdlog::info("CharacterBrowser::loadPreviewModel: loading skel='{}' atlas='{}'",
                 variant->skelPath, variant->atlasPath);

    // Load skeleton into SpineEngine to get animation list
    if (!m_spineEngine)
        m_spineEngine = std::make_unique<SpineEngine>();

    // Stop any running animation before reloading
    if (m_spinePreview) m_spinePreview->stopAnimation();

    if (!m_spineEngine->loadSkeleton(variant->skelPath, variant->atlasPath, 0.5f)) {
        m_statusLabel->setText("Failed to load skeleton");
        spdlog::warn("CharacterBrowser: Failed to load skeleton: {}", variant->skelPath);
        clearPreview();
        return;
    }

    // Populate animation dropdown â€” each character has unique animation names.
    // Filter out internal talk bookend animations.
    auto anims = m_spineEngine->animation().listAnimations();
    m_animationCombo->blockSignals(true);
    m_animationCombo->clear();
    int idleIdx = -1;
    int comboIdx = 0;
    for (size_t i = 0; i < anims.size(); ++i) {
        // Skip internal talk bookend / zero-length marker animations
        if (anims[i].name == "talk_start" || anims[i].name == "talk_end")
            continue;
        if (anims[i].duration <= 0.0f) continue;

        QString animName = QString::fromStdString(anims[i].name);
        m_animationCombo->addItem(animName);
        // Prefer "idle" as default selection
        if (anims[i].name == "idle" || anims[i].name == "Idle")
            idleIdx = comboIdx;
        ++comboIdx;
    }
    if (m_animationCombo->count() == 0)
        m_animationCombo->addItem("(none)");
    if (idleIdx >= 0)
        m_animationCombo->setCurrentIndex(idleIdx);
    m_animationCombo->blockSignals(false);

    // Reset talking checkbox for new model
    m_talkingCheck->blockSignals(true);
    m_talkingCheck->setChecked(false);
    m_talkingCheck->blockSignals(false);

    // Set default body animation
    if (idleIdx >= 0) {
        m_spineEngine->animation().setBodyAnimation(anims[static_cast<size_t>(idleIdx)].name, true);
    } else if (!anims.empty()) {
        m_spineEngine->animation().setBodyAnimation(anims[0].name, true);
    }

    // Update the animated preview widget
    if (m_spinePreview) {
        m_spinePreview->setSpineEngine(m_spineEngine.get());
        m_spinePreview->loadTextures();
        m_spinePreview->startAnimation();
    }

    m_statusLabel->setText(QString("Loaded %1 \xe2\x80\x94 %2 animations")
        .arg(QString::fromStdString(entry->displayName)).arg(anims.size()));
    spdlog::info("CharacterBrowser: Loaded {} \xe2\x80\x94 {} animations, v{}",
                 entry->displayName, anims.size(), m_spineEngine->version());
#endif
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Letter navigation
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void CharacterBrowser::scrollToLetter(QChar letter)
{
    if (!m_characterList || m_characterList->count() == 0) return;

    bool isDigit = letter.isDigit();
    QChar upperLetter = letter.toUpper();

    for (int i = 0; i < m_characterList->count(); ++i) {
        QString name = m_characterList->item(i)->data(Qt::UserRole).toString();
        if (name.isEmpty()) continue;

        QChar firstChar = name.at(0).toUpper();
        bool match = false;

        if (isDigit) {
            match = !firstChar.isLetter();
        } else {
            match = (firstChar == upperLetter);
        }

        if (match) {
            m_characterList->scrollToItem(m_characterList->item(i),
                                          QAbstractItemView::PositionAtTop);
            m_characterList->setCurrentRow(i);
            return;
        }
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Slots
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void CharacterBrowser::onSearchChanged(const QString& text)
{
    m_currentFilter = text;
    populateCharacterList();
}

void CharacterBrowser::onCategoryChanged(int index)
{
    m_currentCategory = index;
    populateCharacterList();
}

void CharacterBrowser::onDownloadedOnlyToggled(bool /*checked*/)
{
    populateCharacterList();
}

void CharacterBrowser::onShowHiddenToggled(bool /*checked*/)
{
    populateCharacterList();
}

void CharacterBrowser::onCharacterSelectionChanged()
{
    auto names = selectedCharacters();
    bool hasSelection = !names.isEmpty();

    m_downloadBtn->setEnabled(hasSelection);
    m_deleteBtn->setEnabled(hasSelection);

    // Update button labels with count
    if (names.size() > 1) {
        m_downloadBtn->setText(QString("\xE2\xAC\x87\xEF\xB8\x8F  Download (%1)").arg(names.size()));
        m_deleteBtn->setText(QString("\xF0\x9F\x97\x91\xEF\xB8\x8F  Delete (%1)").arg(names.size()));
    } else {
        m_downloadBtn->setText(QStringLiteral("\xE2\xAC\x87\xEF\xB8\x8F  Download"));
        m_deleteBtn->setText(QStringLiteral("\xF0\x9F\x97\x91\xEF\xB8\x8F  Delete"));
    }

    // F6: Save last selected character
    if (hasSelection) {
        QSettings settings;
        settings.setValue("CharacterBrowser/LastSelected", names.first());
    }

    // Preview first selected character
    if (hasSelection) {
        populateControls();
        emit characterSelected(names.first());
    }
}

void CharacterBrowser::onRefreshClicked()
{
    // Prevent double-clicks during fetch
    m_refreshBtn->setEnabled(false);
    m_refreshBtn->setText(QStringLiteral("\xE2\x8F\xB3  Refreshing..."));
    m_statusLabel->setText("Refreshing...");

    // Invalidate metadata cache on explicit refresh
    m_metadataLoaded = false;

#ifdef ROUNDTABLE_HAS_SPINE
    if (m_modelManager) {
        m_modelManager->scan("assets");
    }
#endif
    // Fetch remote character list from Nikke DB (async — will call populateCharacterList when done)
    fetchRemoteCharacterList();
    spdlog::info("CharacterBrowser: Refreshed character list");
}

void CharacterBrowser::onDownloadClicked()
{
    auto items = m_characterList->selectedItems();
    if (items.isEmpty()) return;

    // P2: Use cached metadata instead of re-parsing
    loadMetadata();
    const QJsonObject& metaChars = m_cachedCharacters;

    m_downloadBtn->setEnabled(false);
    m_downloadProgress->setVisible(true);
    m_downloadProgress->setValue(0);

    int queued = 0;
    for (auto* item : items) {
        QString name = item->data(Qt::UserRole).toString();       // folder name (may include charId suffix)
        QString displayName = item->text();                        // display name (with colons)
        QString charId = item->data(Qt::UserRole + 2).toString(); // metadata charId

        // Fallback: resolve charId from metadata if not stored in the list item
        if (charId.isEmpty()) {
            for (auto it = metaChars.begin(); it != metaChars.end(); ++it) {
                if (it.value().toObject().value("name").toString() == name) {
                    charId = it.key();
                    break;
                }
            }
        }
        if (charId.isEmpty()) {
            for (auto it = metaChars.begin(); it != metaChars.end(); ++it) {
                QString n = it.value().toObject().value("name").toString();
                if (n.compare(name, Qt::CaseInsensitive) == 0) {
                    charId = it.key();
                    break;
                }
            }
        }

        // Fallback to raw ID
        if (charId.isEmpty()) {
            if (m_remoteCharIds.contains(name))
                charId = name;
            else if (name.startsWith("c", Qt::CaseInsensitive) && name.length() >= 4 &&
                name.mid(1, 3).toInt() > 0)
                charId = name.toLower();
            else {
                spdlog::warn("CharacterBrowser: Cannot find ID for: {}", name.toStdString());
                continue;
            }
        }

        // Collect all outfits from metadata for this character
        QJsonObject charObj = metaChars.value(charId).toObject();
        QJsonObject outfitsObj = charObj.value("outfits").toObject();

        struct OutfitDownload { QString repoPath; QString outfitKey; };
        std::vector<OutfitDownload> outfitsToDownload;

        if (!outfitsObj.isEmpty()) {
            for (auto oit = outfitsObj.begin(); oit != outfitsObj.end(); ++oit) {
                QString outfitKey = oit.key();
                QString repoPath = charId;
                if (outfitKey != "default" && outfitKey.startsWith("outfit_")) {
                    QString suffix = outfitKey.mid(6);  // "_01" from "outfit_01"
                    repoPath = charId + suffix;
                }
                outfitsToDownload.push_back({repoPath, outfitKey});
            }
        } else {
            outfitsToDownload.push_back({charId, "default"});
        }

        // Track overall completion across all outfit downloads
        auto totalOutfits = std::make_shared<int>(static_cast<int>(outfitsToDownload.size()));
        auto completedOutfits = std::make_shared<int>(0);
        auto anyOutfitFailed = std::make_shared<bool>(false);

        for (const auto& od : outfitsToDownload) {
            spdlog::info("CharacterBrowser: Download {} [{}] outfit={} repoPath={}",
                         name.toStdString(), charId.toStdString(),
                         od.outfitKey.toStdString(), od.repoPath.toStdString());

            m_statusLabel->setText(QString("Downloading %1 (%2)...")
                .arg(displayName).arg(od.outfitKey));

            downloadCharacterModel(od.repoPath, name, od.outfitKey,
                [this, displayName, totalOutfits, completedOutfits, anyOutfitFailed](bool success) {
                    (*completedOutfits)++;
                    if (!success) *anyOutfitFailed = true;

                    if (*completedOutfits >= *totalOutfits) {
                        // All outfits for this character are done — show one message
                        m_downloadBtn->setEnabled(true);
                        m_downloadProgress->setVisible(false);
                        if (*anyOutfitFailed) {
                            m_statusLabel->setText(QString("Downloaded %1 (some failed)").arg(displayName));
                            spdlog::warn("CharacterBrowser: Some outfits failed for {}",
                                         displayName.toStdString());
                            QMessageBox::warning(this, "Download",
                                QString("Downloaded %1 but some outfits had errors.").arg(displayName));
                        } else {
                            m_statusLabel->setText("Downloaded " + displayName);
                            spdlog::info("CharacterBrowser: Successfully downloaded all outfits for {}",
                                         displayName.toStdString());
                            QMessageBox::information(this, "Download Complete",
                                QString("Successfully downloaded %1.").arg(displayName));
                        }

                        // Rescan and refresh
#ifdef ROUNDTABLE_HAS_SPINE
                        if (m_modelManager) {
                            m_modelManager->scan("assets");
                        }
#endif
                        populateCharacterList();
                        populateControls();
                        emit downloadRequested(displayName);
                    }
                });
        }

        emit downloadRequested(name);
        ++queued;
    }

    if (queued == 0) {
        m_downloadBtn->setEnabled(true);
        m_downloadProgress->setVisible(false);
        QMessageBox::warning(this, "Download Failed",
            "Could not find character IDs for any selected characters.");
    }
}
void CharacterBrowser::onDeleteClicked()
{
    auto names = selectedCharacters();
    if (names.isEmpty()) return;

    // Confirm deletion
    QString msg = (names.size() == 1)
        ? QString("Delete all local files for '%1'?").arg(names.first())
        : QString("Delete all local files for %1 characters?\n\n%2")
              .arg(names.size()).arg(names.join(", "));
    auto result = QMessageBox::question(this, "Delete Character(s)",
        msg, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (result != QMessageBox::Yes) return;

    int deleted = 0;
    QString projectRoot = rt::findProjectRoot();
    for (const auto& name : names) {
        QString charDir = QDir::toNativeSeparators(
            projectRoot + "/assets/characters/" + name);
        QDir dir(charDir);
        if (dir.exists()) {
            if (dir.removeRecursively()) {
                spdlog::info("CharacterBrowser: Deleted {}", charDir.toStdString());
                ++deleted;
            } else {
                spdlog::warn("CharacterBrowser: Failed to delete {}", charDir.toStdString());
            }
        }

        // Delete all cached animation videos for this character
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_animVideoCache)
            m_animVideoCache->removeAllForCharacter(name.toStdString());
#endif
    }

    m_statusLabel->setText(QString("Deleted %1 character(s)").arg(deleted));

    // Rescan ModelManager BEFORE emitting signals so listeners see updated data
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_modelManager) m_modelManager->scan(
        QDir::toNativeSeparators(projectRoot + "/assets").toStdString());
#endif
    populateCharacterList();
    populateControls();

    // Emit after rescan so connected slots (e.g. refreshCharacterLibrary) read current state
    for (const auto& name : names)
        emit deleteRequested(name);
}

void CharacterBrowser::onOutfitChanged(int /*index*/)
{
    // Don't call populateControls() â€” it resets the outfit combo.
    // Re-populate stances for the new outfit and reload the model.
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_modelManager) return;

    auto charName = selectedCharacter();
    if (charName.isEmpty()) return;

    const auto* entry = m_modelManager->findByName(charName.toStdString());

    m_stanceCombo->blockSignals(true);
    m_stanceCombo->clear();
    if (entry) {
        QString currentOutfitKey = m_outfitCombo->currentData().toString();
        for (const auto& outfit : entry->outfits) {
            if (QString::fromStdString(outfit.name) == currentOutfitKey) {
                for (const auto& variant : outfit.variants) {
                    switch (variant.stance) {
                    case CharacterStance::Default: m_stanceCombo->addItem("Default"); break;
                    case CharacterStance::Aim:     m_stanceCombo->addItem("Aim"); break;
                    case CharacterStance::Cover:   m_stanceCombo->addItem("Cover"); break;
                    }
                }
                break;
            }
        }
    }
    if (m_stanceCombo->count() == 0)
        m_stanceCombo->addItem("Default");
    m_stanceCombo->blockSignals(false);

    loadPreviewModel();
#endif
}

void CharacterBrowser::onStanceChanged(int /*index*/)
{
    loadPreviewModel(); // Reload model for new stance
}

void CharacterBrowser::onAnimationChanged(int /*index*/)
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_spineEngine || !m_spineEngine->isLoaded()) return;

    QString animName = m_animationCombo->currentText();
    if (!animName.isEmpty() && animName != "(none)") {
        m_spineEngine->animation().setBodyAnimation(animName.toStdString(), true);
        spdlog::debug("CharacterBrowser: Set animation to '{}'", animName.toStdString());
    }
#endif
}

void CharacterBrowser::onTalkingChanged(bool checked)
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_spineEngine || !m_spineEngine->isLoaded()) return;

    if (checked) {
        m_spineEngine->animation().startTalking();
        spdlog::debug("CharacterBrowser: Talking ON");
    } else {
        m_spineEngine->animation().stopTalking();
        spdlog::debug("CharacterBrowser: Talking OFF");
    }
#else
    Q_UNUSED(checked);
#endif
}


void CharacterBrowser::saveHiddenChars()
{
    QSettings settings;
    QStringList hidden;
    for (const auto& h : m_hiddenCharNames)
        hidden.append(h);
    settings.setValue("CharacterBrowser/HiddenChars", hidden);
}

void CharacterBrowser::saveRenamedDisplayNames()
{
    QSettings settings;
    settings.beginWriteArray("CharacterBrowser/RenamedDisplayNames");
    int i = 0;
    for (auto it = m_customDisplayNames.cbegin(); it != m_customDisplayNames.cend(); ++it) {
        settings.setArrayIndex(i++);
        settings.setValue("folderName", it.key());
        settings.setValue("customName", it.value());
    }
    settings.endArray();
}

QString CharacterBrowser::displayNameFor(const QString& folderName,
                                         const QString& fallback) const
{
    auto customIt = m_customDisplayNames.constFind(folderName);
    if (customIt != m_customDisplayNames.cend())
        return customIt.value();

    auto defaultIt = m_defaultDisplayNames.constFind(folderName);
    if (defaultIt != m_defaultDisplayNames.cend())
        return defaultIt.value();

    return fallback.isEmpty() ? folderName : fallback;
}

void CharacterBrowser::promptRenameCharacter(const QString& folderName)
{
    const QString currentName = displayNameFor(folderName, folderName);
    bool ok = false;
    QString newName = QInputDialog::getText(
        this, tr("Rename Character"), tr("New display name:"),
        QLineEdit::Normal, currentName, &ok);
    if (!ok)
        return;

    newName = newName.trimmed();
    if (newName == currentName)
        return;

    const QString defaultName = m_defaultDisplayNames.value(folderName, folderName);
    if (newName.isEmpty() || newName == defaultName)
        m_customDisplayNames.remove(folderName);
    else
        m_customDisplayNames[folderName] = newName;

    saveRenamedDisplayNames();
    populateCharacterList();

    // Emit the final visible Library label. An empty edit resets to the
    // built-in/folder label, and Compose should reflect that reset too.
    emit characterDisplayNameChanged(folderName,
                                     displayNameFor(folderName, folderName));
}

void CharacterBrowser::onContextMenu(const QPoint& pos)
{
    auto* item = m_characterList->itemAt(pos);
    if (!item) return;

    QString name = item->data(Qt::UserRole).toString();
    bool isLocal = item->data(Qt::UserRole + 1).toBool();

    QMenu menu(this);

    bool isHidden = m_hiddenCharNames.contains(name);

    if (isLocal) {
        menu.addAction(QStringLiteral("\xE2\x96\xB6  Preview"), this, [this]() {
            populateControls();
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("\xF0\x9F\x93\x82  Reveal in Explorer"), this, [name]() {
            QString charDir = QDir::currentPath() + "/assets/characters/" + name;
            QDir dir(charDir);
            if (!dir.exists()) {
                charDir = rt::findProjectRoot() + "/assets/characters/" + name;
                dir.setPath(charDir);
            }
            if (dir.exists()) {
                QStringList patterns = {"*.skel", "*.atlas", "*.png",
                                        "*.model3.json", "*.moc3", "*.json"};
                QDirIterator it(charDir, patterns, QDir::Files,
                                QDirIterator::Subdirectories);
                QString selectPath;
                if (it.hasNext()) {
                    selectPath = QDir::toNativeSeparators(it.next());
                } else {
                    selectPath = QDir::toNativeSeparators(charDir);
                }
                QProcess::startDetached("explorer.exe",
                    {"/select,", selectPath});
            } else {
                QString baseDir = QDir::currentPath() + "/assets/characters";
                if (QDir(baseDir).exists())
                    QProcess::startDetached("explorer.exe",
                        {QDir::toNativeSeparators(baseDir)});
            }
        });
        menu.addAction(QStringLiteral("\xF0\x9F\x93\x8B  Copy Name"), this, [name]() {
            QGuiApplication::clipboard()->setText(name);
        });
        menu.addSeparator();
        if (isHidden) {
            menu.addAction(QStringLiteral("\xF0\x9F\x91\x81  Unhide"), this, [this, name]() {
                m_hiddenCharNames.erase(name);
                saveHiddenChars();
                populateCharacterList();
            });
        } else {
            menu.addAction(QStringLiteral("\xF0\x9F\x9A\xAB  Hide"), this, [this, name]() {
                m_hiddenCharNames.insert(name);
                saveHiddenChars();
                populateCharacterList();
            });
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("\xF0\x9F\x93\x9D  Rename..."), this,
                       [this, name]() { promptRenameCharacter(name); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("\xF0\x9F\x97\x91  Delete"), this,
                       &CharacterBrowser::onDeleteClicked);
    } else {
        menu.addAction(QStringLiteral("\xE2\xAC\x87  Download"), this,
                       &CharacterBrowser::onDownloadClicked);
        menu.addAction(QStringLiteral("\xF0\x9F\x93\x8B  Copy Name"), this, [name]() {
            QGuiApplication::clipboard()->setText(name);
        });
        menu.addSeparator();
        if (isHidden) {
            menu.addAction(QStringLiteral("\xF0\x9F\x91\x81  Unhide"), this, [this, name]() {
                m_hiddenCharNames.erase(name);
                saveHiddenChars();
                populateCharacterList();
            });
        } else {
            menu.addAction(QStringLiteral("\xF0\x9F\x9A\xAB  Hide"), this, [this, name]() {
                m_hiddenCharNames.insert(name);
                saveHiddenChars();
                populateCharacterList();
            });
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("\xF0\x9F\x93\x9D  Rename..."), this,
                       [this, name]() { promptRenameCharacter(name); });
    }

    menu.exec(m_characterList->mapToGlobal(pos));
}

void CharacterBrowser::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F5) {
        onRefreshClicked();
        return;
    }
    if (event->key() == Qt::Key_Delete) {
        if (m_deleteBtn->isEnabled())
            onDeleteClicked();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!selectedCharacter().isEmpty())
            populateControls();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_D) {
            if (m_downloadBtn->isEnabled())
                onDownloadClicked();
            return;
        }
        if (event->key() == Qt::Key_A) {
            m_characterList->clearSelection();
            for (int i = 0; i < m_characterList->count(); ++i) {
                auto* item = m_characterList->item(i);
                if (item->data(Qt::UserRole + 1).toBool())
                    item->setSelected(true);
            }
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

} // namespace rt
