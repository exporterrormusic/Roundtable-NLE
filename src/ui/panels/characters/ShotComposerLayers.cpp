/*
 * ShotComposerLayers.cpp — Layer CRUD, ordering, undo/redo for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "panels/characters/PuppetLibrary.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QListWidget>

#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  Character / background management
// =====================================================================

int ShotComposer::addCharacter(const std::string& characterName,
                               const std::string& videoMutePath,
                               const std::string& videoTalkPath)
{
    pushUndoState();
    CharacterState ch;
    ch.characterName = characterName;
    ch.outfit    = "default";
    ch.posX      = 0.5f;
    ch.posY      = 0.75f;
    ch.scale     = 1.0f;
    ch.animation = "idle";
    ch.isTalking = true;
    ch.videoMutePath = videoMutePath;
    ch.videoTalkPath = videoTalkPath;

    int idx = m_currentShot.addCharacter(ch);
    if (!m_shotNameEdit->isEnabled())
        m_shotNameEdit->setEnabled(true);
    refreshLayerList();

    int layerIdx = m_currentShot.findLayerIndex({LayerType::Character, idx});
    if (layerIdx >= 0)
        selectLayer(layerIdx);

    emit shotChanged();
    return idx;
}

int ShotComposer::addPuppet(const std::string& puppetFolder,
                            const std::string& variant)
{
    if (puppetFolder.empty()) return -1;

    pushUndoState();

    // Resolve a friendly character name from the manifest's display name.
    std::string charName = puppetFolder;
    PuppetManifest man;
    if (puppetlib::load(QString::fromStdString(puppetFolder), man) &&
        !man.displayName.isEmpty())
        charName = man.displayName.toStdString();

    CharacterState ch;
    ch.characterName = charName;
    ch.puppetFolder  = puppetFolder;
    ch.puppetVariant = variant.empty() ? "default" : variant;
    ch.posX      = 0.5f;
    ch.posY      = 0.75f;
    ch.scale     = 1.0f;
    ch.isTalking = true;

    int idx = m_currentShot.addCharacter(ch);
    if (!m_shotNameEdit->isEnabled())
        m_shotNameEdit->setEnabled(true);
    refreshLayerList();

    int layerIdx = m_currentShot.findLayerIndex({LayerType::Character, idx});
    if (layerIdx >= 0)
        selectLayer(layerIdx);

    emit shotChanged();
    return idx;
}

bool ShotComposer::removeCharacter(int index)
{
    if (!m_currentShot.removeCharacter(index))
        return false;

    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
    return true;
}

int ShotComposer::addBackground(const std::string& path)
{
    pushUndoState();
    BackgroundState bg;
    bg.path  = path;
    bg.posX  = 0.5f;
    bg.posY  = 0.5f;
    bg.scale = 1.0f;

    int idx = m_currentShot.addBackground(bg);
    if (!m_shotNameEdit->isEnabled())
        m_shotNameEdit->setEnabled(true);
    refreshLayerList();

    int layerIdx = m_currentShot.findLayerIndex({LayerType::Background, idx});
    if (layerIdx >= 0)
        selectLayer(layerIdx);

    emit shotChanged();
    return idx;
}

bool ShotComposer::removeBackground(int index)
{
    if (!m_currentShot.removeBackground(index))
        return false;

    m_selectedLayer = -1;
    refreshLayerList();
    clearLayerProperties();
    updatePreview();
    emit shotChanged();
    return true;
}

void ShotComposer::selectLayer(int index)
{
    if (index < 0 || index >= m_currentShot.layerCount()) {
        m_selectedLayer = -1;
        clearLayerProperties();
        return;
    }

    m_selectedLayer = index;
    m_layerList->setCurrentRow(index);
    populateLayerProperties();
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview)
        m_spinePreview->setSelectedLayer(index);
#endif
    emit layerSelected(index);
}

// =====================================================================
//  Undo / Redo
// =====================================================================

void ShotComposer::pushUndoState()
{
    m_undoStack.push_back({m_currentShot, m_layerGroups});
    if (static_cast<int>(m_undoStack.size()) > MAX_UNDO)
        m_undoStack.pop_front();
    m_redoStack.clear();  // New action invalidates redo history
}

void ShotComposer::undo()
{
    if (m_undoStack.empty()) return;
    m_redoStack.push_back({m_currentShot, m_layerGroups});  // Save current state for redo
    auto& prev = m_undoStack.back();
    m_currentShot = prev.preset;
    m_layerGroups = prev.groups;
    m_undoStack.pop_back();
    // Clamp selection
    if (m_selectedLayer >= m_currentShot.layerCount())
        m_selectedLayer = m_currentShot.layerCount() - 1;
    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
}

void ShotComposer::redo()
{
    if (m_redoStack.empty()) return;
    m_undoStack.push_back({m_currentShot, m_layerGroups});  // Save current state for undo
    auto& next = m_redoStack.back();
    m_currentShot = next.preset;
    m_layerGroups = next.groups;
    m_redoStack.pop_back();
    // Clamp selection
    if (m_selectedLayer >= m_currentShot.layerCount())
        m_selectedLayer = m_currentShot.layerCount() - 1;
    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
}

// =====================================================================
//  Layer ordering
// =====================================================================

void ShotComposer::moveSelectedLayerUp()
{
    if (m_selectedLayer < 0) return;
    spdlog::debug("ShotComposer::moveSelectedLayerUp — selected={}", m_selectedLayer);
    pushUndoState();
    if (m_currentShot.moveLayerUp(m_selectedLayer)) {
        --m_selectedLayer;
        spdlog::debug("  → moved to index {}", m_selectedLayer);
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    } else {
        spdlog::debug("  → moveLayerUp returned false (already at top?)");
    }
}

void ShotComposer::moveSelectedLayerDown()
{
    if (m_selectedLayer < 0) return;
    spdlog::debug("ShotComposer::moveSelectedLayerDown — selected={}", m_selectedLayer);
    pushUndoState();
    if (m_currentShot.moveLayerDown(m_selectedLayer)) {
        ++m_selectedLayer;
        spdlog::debug("  → moved to index {}", m_selectedLayer);
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    } else {
        spdlog::debug("  → moveLayerDown returned false (already at bottom?)");
    }
}

void ShotComposer::moveSelectedLayerToFront()
{
    if (m_selectedLayer < 0) return;
    pushUndoState();
    if (m_currentShot.moveLayerToFront(m_selectedLayer)) {
        m_selectedLayer = 0;
        refreshLayerList();
        m_layerList->setCurrentRow(0);
        updatePreview();
        emit shotChanged();
    }
}

void ShotComposer::moveSelectedLayerToBack()
{
    if (m_selectedLayer < 0) return;
    pushUndoState();
    if (m_currentShot.moveLayerToBack(m_selectedLayer)) {
        m_selectedLayer = m_currentShot.layerCount() - 1;
        refreshLayerList();
        m_layerList->setCurrentRow(m_selectedLayer);
        updatePreview();
        emit shotChanged();
    }
}

} // namespace rt
