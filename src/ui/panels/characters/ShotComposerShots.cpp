/*
 * ShotComposerShots.cpp — Shot CRUD and configuration for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QFileInfo>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>

#include <fstream>
#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  Configuration
// =====================================================================

void ShotComposer::setModelManager(ModelManager* mgr)
{
    m_modelManager = mgr;
    refreshCharacterLibrary();
}

void ShotComposer::setAnimVideoCache(const AnimationVideoCache* cache)
{
    m_animVideoCache = cache;
    refreshCharacterLibrary();
}

void ShotComposer::setPresetsDirectory(const std::filesystem::path& dir)
{
    m_presetManager.scan(dir);
    loadDefaults();
    refreshShotList();
    refreshBackgroundLibrary();
    refreshVideoLibrary();
}

void ShotComposer::saveDefaults() const
{
    auto dir = m_presetManager.directory();
    if (dir.empty()) return;
    auto path = dir / "_defaults.json";
    QJsonObject obj;
    for (const auto& [charName, shotName] : m_characterDefaults)
        obj[QString::fromStdString(charName)] = QString::fromStdString(shotName);
    QJsonDocument doc(obj);
    std::ofstream f(path, std::ios::trunc);
    if (f.is_open()) {
        auto bytes = doc.toJson(QJsonDocument::Compact);
        f.write(bytes.constData(), bytes.size());
    }
}

void ShotComposer::loadDefaults()
{
    auto dir = m_presetManager.directory();
    if (dir.empty()) return;
    auto path = dir / "_defaults.json";
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(contents));
    if (!doc.isObject()) return;
    m_characterDefaults.clear();
    auto obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        m_characterDefaults[it.key().toStdString()] = it.value().toString().toStdString();
}

void ShotComposer::ensureDefaultShotsForCharacters(const QStringList& characters)
{
    bool added = false;
    for (const auto& charName : characters) {
        std::string name = charName.toStdString();
        // Check if a default shot already exists for this character
        std::string defaultName = name + " (Default)";
        if (m_presetManager.hasPreset(defaultName))
            continue;

        // Also check upper-case variant
        std::string upperName = charName.toUpper().toStdString() + " (Default)";
        if (m_presetManager.hasPreset(upperName))
            continue;

        // Create a default shot preset for this character
        auto preset = ShotPreset::createDefault(name);
        preset.setName(defaultName);
        if (m_presetManager.save(preset)) {
            spdlog::info("ShotComposer: Created default shot for '{}'", name);
            added = true;
        }
    }
    if (added)
        refreshShotList();
}

QString ShotComposer::activeCharFilter() const
{
    // The character filter list now contains ALL (item 0, empty UserRole),
    // UNASSIGNED (item 1, "__UNASSIGNED__"), and character entries.
    if (m_charFilterList) {
        auto* curItem = m_charFilterList->currentItem();
        if (curItem) {
            QString val = curItem->data(Qt::UserRole).toString();
            if (!val.isEmpty())
                return val;
        }
    }
    return {};
}

// =====================================================================
//  Shot management
// =====================================================================

void ShotComposer::newShot()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "New Shot", "Shot Name:",
                                         QLineEdit::Normal, "", &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    newShot(name.trimmed());
}

void ShotComposer::newShot(const QString& name)
{
    m_currentShot = ShotPreset(name.toStdString());
    m_selectedLayer = -1;
    m_lastSavedName = name.toStdString();

    // If a specific character filter is active, auto-add that character
    QString filterVal = activeCharFilter();
    if (!filterVal.isEmpty() && filterVal != QStringLiteral("__UNASSIGNED__")) {
        std::string folderName = m_modelManager
            ? m_modelManager->getFolderName(filterVal.toStdString())
            : filterVal.toStdString();

        CharacterState ch;
        ch.characterName = folderName;
        ch.outfit    = "default";
        ch.animation = "idle";
        ch.isTalking = true;
        ch.posX      = 0.5f;
        ch.posY      = 0.75f;
        ch.scale     = 1.0f;

        // Check for video character (e.g. "Wells")
        {
            QString qFolder  = QString::fromStdString(folderName).toLower();
            QString qDisplay = filterVal.toLower();
            for (const auto& [fn, info] : videoCharacterFiles()) {
                if (QString::fromStdString(fn).toLower() == qFolder ||
                    QString::fromStdString(info.charName).toLower() == qDisplay) {
                    ch.videoMutePath = info.mutePath;
                    ch.videoTalkPath = info.talkPath;
                    break;
                }
            }
        }

        m_currentShot.addCharacter(ch);
    }

    // Save the new shot immediately so it appears in the preset list
    m_presetManager.save(m_currentShot);

    m_updating = true;
    m_shotNameEdit->setText(name);
    m_shotNameEdit->setEnabled(true);
    m_defaultShotCheck->setEnabled(true);
    m_defaultCharCombo->setEnabled(true);
    m_setDefaultBtn->setEnabled(true);
    m_defaultCharCombo->clear();
    m_updating = false;

    refreshLayerList();
    clearLayerProperties();
    refreshShotList();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::setCurrentShot(const ShotPreset& preset)
{
    m_currentShot = preset;
    m_selectedLayer = -1;
    m_lastSavedName = preset.name();  // Last saved name = this preset's name

    // ── Migrate legacy video-character backgrounds → proper CharacterState ──
    // Old shots stored Wells as a Background with layerType="video".
    // Convert them to CharacterState entries so they get character properties.
    {
        const auto& vcFiles = videoCharacterFiles();
        // Collect indices to migrate (reverse order so removal doesn't shift)
        struct MigrationInfo {
            int bgIndex;
            std::string charName, mutePath, talkPath;
            BackgroundState bg;
        };
        std::vector<MigrationInfo> migrations;

        for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
            const auto* bg = m_currentShot.background(i);
            if (!bg || !bg->isVideo()) continue;

            // Extract filename from path and lowercase it
            QString qpath = QString::fromStdString(bg->path);
            std::string filename = QFileInfo(qpath).fileName().toLower().toStdString();
            auto it = vcFiles.find(filename);
            if (it != vcFiles.end()) {
                const auto& [charName, mutePath, talkPath] = it->second;
                migrations.push_back({i, charName, mutePath, talkPath, *bg});
            }
        }

        // Apply migrations (reverse order to preserve indices)
        for (auto rit = migrations.rbegin(); rit != migrations.rend(); ++rit) {
            // Check if this video character was already added
            bool alreadyExists = false;
            for (const auto& ch : m_currentShot.characters()) {
                if (ch.characterName == rit->charName && ch.isVideoCharacter()) {
                    alreadyExists = true;
                    break;
                }
            }

            // Create character from the old background
            CharacterState ch;
            ch.characterName = rit->charName;
            ch.videoMutePath = rit->mutePath;
            ch.videoTalkPath = rit->talkPath;
            ch.posX      = rit->bg.posX;
            ch.posY      = rit->bg.posY;
            ch.scale     = rit->bg.scale;
            ch.opacity   = rit->bg.opacity;
            ch.visible   = rit->bg.visible;
            ch.isTalking = (rit->bg.path.find("TALK") != std::string::npos);

            if (!alreadyExists) {
                // Remove the old background first, then add as character
                // We need to update layerOrder: find the bg's position, remove it,
                // then insert the new character at the same z-position
                int layerPos = m_currentShot.findLayerIndex(
                    {LayerType::Background, rit->bgIndex});
                m_currentShot.removeBackground(rit->bgIndex);
                int chIdx = m_currentShot.addCharacter(ch);
                // Move the new character to the same visual position
                if (layerPos >= 0) {
                    // addCharacter inserts at front (index 0), move it to layerPos
                    int newLayerIdx = m_currentShot.findLayerIndex(
                        {LayerType::Character, chIdx});
                    // Shift it to the right position via layer moves
                    while (newLayerIdx >= 0 && newLayerIdx < layerPos &&
                           newLayerIdx + 1 < m_currentShot.layerCount()) {
                        m_currentShot.moveLayerDown(newLayerIdx);
                        newLayerIdx++;
                    }
                }
                spdlog::info("ShotComposer: Migrated '{}' from video background to video character",
                             rit->charName);
            } else {
                // Just remove the duplicate background
                m_currentShot.removeBackground(rit->bgIndex);
                spdlog::info("ShotComposer: Removed duplicate video bg for '{}'",
                             rit->charName);
            }
        }
    }

    // Sanitize layer order after migration — remove any stale references
    // that could cause out-of-bounds access in updatePreview
    m_currentShot.ensureLayerOrder();

    m_updating = true;
    m_shotNameEdit->setText(QString::fromStdString(preset.name()));
    m_shotNameEdit->setEnabled(true);
    m_defaultShotCheck->setEnabled(true);
    m_defaultCharCombo->setEnabled(true);
    m_setDefaultBtn->setEnabled(true);

    // Populate default-shot character dropdown with characters in this shot
    m_defaultCharCombo->clear();
    for (const auto& ch : preset.characters()) {
        m_defaultCharCombo->addItem(QString::fromStdString(ch.characterName));
    }

    m_cameraZoomSpin->setValue(preset.cameraZoom() * 100.0);
    m_cameraPanXSpin->setValue(static_cast<double>(preset.cameraX()) * 100.0);
    m_cameraPanYSpin->setValue(static_cast<double>(preset.cameraY()) * 100.0);
    m_updating = false;

    // Apply camera transform now (spins were set with m_updating=true so
    // onCameraPropertyChanged didn't fire)
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview)
        m_spinePreview->setCameraTransform(
            preset.cameraZoom(),
            preset.cameraX(),
            preset.cameraY());
#endif

    refreshLayerList();

    // Auto-select the first character layer (or first layer) for preview
    if (m_currentShot.layerCount() > 0) {
        int firstCharLayer = -1;
        for (int i = 0; i < m_currentShot.layerCount(); ++i) {
            const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(i)];
            if (ref.type == LayerType::Character) {
                firstCharLayer = i;
                break;
            }
        }
        int autoSelect = (firstCharLayer >= 0) ? firstCharLayer : 0;
        selectLayer(autoSelect);
    } else {
        clearLayerProperties();
        updatePreview();
    }

    emit shotChanged();
}

bool ShotComposer::saveCurrentShot()
{
    if (m_currentShot.name().empty())
        return false;

    // ── Clean up orphaned file on rename ────────────────────────────────
    // If the shot was previously saved under a different name, delete the
    // old file so it doesn't linger on disk and confuse things.
    if (!m_lastSavedName.empty() && m_lastSavedName != m_currentShot.name()) {
        m_presetManager.remove(m_lastSavedName);
    }

    bool ok = m_presetManager.save(m_currentShot);
    if (ok) {
        m_lastSavedName = m_currentShot.name();
        // Generate & persist a thumbnail PNG next to the preset JSON
        saveShotThumbnail(m_currentShot);
        refreshShotList();
        refreshLayerList();
        emit shotChanged();
        spdlog::info("ShotComposer: Saved shot '{}'", m_currentShot.name());
    }
    return ok;
}

void ShotComposer::duplicateCurrentShot()
{
    if (m_currentShot.name().empty())
        return;

    // Generate a unique name
    std::string baseName = m_currentShot.name() + " Copy";
    std::string newName  = baseName;
    int counter = 2;
    while (m_presetManager.hasPreset(newName)) {
        newName = baseName + " " + std::to_string(counter++);
    }

    bool ok = false;
    QString name = QInputDialog::getText(this, "Duplicate Shot",
        "Name for the duplicate:", QLineEdit::Normal,
        QString::fromStdString(newName), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    ShotPreset dupe = m_currentShot;
    dupe.setName(name.trimmed().toStdString());
    m_presetManager.save(dupe);
    setCurrentShot(dupe);
    refreshShotList();

    spdlog::info("ShotComposer: Duplicated shot '{}' as '{}'",
        m_currentShot.name(), dupe.name());
}

} // namespace rt
