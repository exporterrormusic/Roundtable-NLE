/*
 * ShotComposerClipboard.cpp — Copy/paste operations for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "Theme.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QToolTip>

#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  copySelectedLayer / pasteLayer
// =====================================================================

void ShotComposer::copySelectedLayer()
{
    // ── Shot-level copy when the shot list has focus ───────────────────
    if (m_shotList && (m_shotList->hasFocus() ||
        (m_charFilterList && m_charFilterList->hasFocus())) && m_shotList->currentItem()) {
        auto name = m_shotList->currentItem()->data(Qt::UserRole).toString().toStdString();
        auto preset = m_presetManager.load(name);
        if (preset) {
            m_shotClipboard = *preset;
            spdlog::info("ShotComposer: copied shot '{}' to clipboard", name);
            QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
                QString("Shot \"%1\" copied").arg(QString::fromStdString(name)), this, {}, 1200);
        }
        return;
    }

    // ── Layer-level copy ───────────────────────────────────────────────
    // Copy all selected layers (multi-select supported)
    const auto rows = m_layerList->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    m_layerClipboard.clear();
    for (const auto& idx : rows) {
        int row = idx.row();
        if (row < 0 || row >= m_currentShot.layerCount()) continue;

        const auto& ref = m_currentShot.layerOrder()[row];
        LayerClipboardEntry entry;
        entry.type = ref.type;

        if (ref.type == LayerType::Character) {
            const auto* ch = m_currentShot.character(ref.index);
            if (!ch) continue;
            entry.character = *ch;
        } else {
            const auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;
            entry.background = *bg;
        }
        m_layerClipboard.push_back(entry);
    }

    spdlog::info("ShotComposer: copied {} layer(s) to clipboard", m_layerClipboard.size());
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("%1 layer(s) copied").arg(m_layerClipboard.size()), this, {}, 1200);
}

void ShotComposer::pasteLayer()
{
    // ── Shot-level paste when the shot list or char filter has focus ───
    if (m_shotClipboard && m_shotList &&
        (m_shotList->hasFocus() ||
         (m_charFilterList && m_charFilterList->hasFocus()) ||
         !m_layerList->hasFocus())) {
        // Determine target character from character filter
        QString charFilter = activeCharFilter();
        // Group filters behave like ALL for paste purposes.
        if (charFilter.startsWith(QStringLiteral("__")))
            charFilter.clear();

        // Build a new name: replace source character prefix with target character
        std::string srcName = m_shotClipboard->name();
        std::string newName;
        if (!charFilter.isEmpty()) {
            // Try to replace source character prefix with the target character
            // Shot names typically start with "CharName_..."
            auto srcPos = srcName.find('_');
            if (srcPos != std::string::npos)
                newName = charFilter.toStdString() + srcName.substr(srcPos);
            else
                newName = charFilter.toStdString() + "_" + srcName;
        } else {
            newName = srcName + " Copy";
        }

        // Ensure unique name
        std::string baseName = newName;
        int counter = 2;
        while (m_presetManager.hasPreset(newName))
            newName = baseName + " " + std::to_string(counter++);

        bool ok = false;
        QString name = QInputDialog::getText(this, "Paste Shot",
            "Name for the pasted shot:", QLineEdit::Normal,
            QString::fromStdString(newName), &ok);
        if (!ok || name.trimmed().isEmpty()) return;

        ShotPreset pasted = *m_shotClipboard;
        pasted.setName(name.trimmed().toStdString());
        m_presetManager.save(pasted);
        saveShotThumbnail(pasted);
        setCurrentShot(pasted);
        refreshShotList();

        spdlog::info("ShotComposer: pasted shot '{}' as '{}'", srcName, pasted.name());
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
            QString("Shot \"%1\" pasted").arg(name.trimmed()), this, {}, 1200);
        return;
    }

    // ── Layer-level paste ──────────────────────────────────────────────
    if (m_layerClipboard.empty()) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)), "Nothing to paste", this, {}, 1200);
        return;
    }

    pushUndoState();

    int lastIdx = -1;
    for (const auto& entry : m_layerClipboard) {
        if (entry.type == LayerType::Character) {
            lastIdx = m_currentShot.addCharacter(entry.character);
            spdlog::info("ShotComposer: pasted character '{}' as index {}",
                         entry.character.characterName, lastIdx);
        } else {
            lastIdx = m_currentShot.addBackground(entry.background);
            spdlog::info("ShotComposer: pasted background '{}' as index {}",
                         entry.background.path, lastIdx);
        }
    }

    refreshLayerList();
    // Select the last pasted layer
    m_selectedLayer = m_currentShot.layerCount() - 1;
    m_layerList->setCurrentRow(m_selectedLayer);
    populateLayerProperties();
    updatePreview();
    emit shotChanged();
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("%1 layer(s) pasted").arg(m_layerClipboard.size()), this, {}, 1200);
}

// =====================================================================
//  copyTransform / pasteTransform — Ctrl+Shift+C / Ctrl+Shift+V for
//  transform attributes between any layer types
// =====================================================================

void ShotComposer::copyTransform()
{
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount()) return;

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    TransformClipboard tc;

    if (ref.type == LayerType::Character) {
        const auto* ch = m_currentShot.character(ref.index);
        if (!ch) return;
        tc.posX       = ch->posX;
        tc.posY       = ch->posY;
        tc.scale      = ch->scale;
        tc.rotation   = ch->rotation;
        tc.opacity    = ch->opacity;
        tc.flipX      = ch->flipX;
        tc.flipY      = ch->flipY;
        tc.visible    = ch->visible;
        tc.cropLeft   = ch->cropLeft;
        tc.cropRight  = ch->cropRight;
        tc.cropTop    = ch->cropTop;
        tc.cropBottom = ch->cropBottom;
        tc.blur       = ch->blur;
    } else {
        const auto* bg = m_currentShot.background(ref.index);
        if (!bg) return;
        tc.posX       = bg->posX;
        tc.posY       = bg->posY;
        tc.scale      = bg->scale;
        tc.rotation   = 0.0f;
        tc.opacity    = bg->opacity;
        tc.flipX      = false;
        tc.flipY      = false;
        tc.visible    = bg->visible;
        tc.cropLeft   = bg->cropLeft;
        tc.cropRight  = bg->cropRight;
        tc.cropTop    = bg->cropTop;
        tc.cropBottom = bg->cropBottom;
        tc.blur       = bg->blur;
    }

    m_transformClipboard = tc;
    spdlog::info("ShotComposer: copied transform from layer {}", m_selectedLayer);
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        "Transform copied", this, {}, 1200);
}

void ShotComposer::pasteTransform()
{
    if (!m_transformClipboard) {
        QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
            "No transform to paste", this, {}, 1200);
        return;
    }

    const auto rows = m_layerList->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;

    pushUndoState();
    const auto& tc = *m_transformClipboard;

    for (const auto& idx : rows) {
        int row = idx.row();
        if (row < 0 || row >= m_currentShot.layerCount()) continue;

        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(row)];
        if (ref.type == LayerType::Character) {
            auto* ch = m_currentShot.character(ref.index);
            if (!ch) continue;
            ch->posX       = tc.posX;
            ch->posY       = tc.posY;
            ch->scale      = tc.scale;
            ch->rotation   = tc.rotation;
            ch->opacity    = tc.opacity;
            ch->flipX      = tc.flipX;
            ch->flipY      = tc.flipY;
            ch->visible    = tc.visible;
            ch->cropLeft   = tc.cropLeft;
            ch->cropRight  = tc.cropRight;
            ch->cropTop    = tc.cropTop;
            ch->cropBottom = tc.cropBottom;
            ch->blur       = tc.blur;
        } else {
            auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;
            bg->posX       = tc.posX;
            bg->posY       = tc.posY;
            bg->scale      = tc.scale;
            bg->opacity    = tc.opacity;
            bg->visible    = tc.visible;
            bg->cropLeft   = tc.cropLeft;
            bg->cropRight  = tc.cropRight;
            bg->cropTop    = tc.cropTop;
            bg->cropBottom = tc.cropBottom;
            bg->blur       = tc.blur;
        }
    }

    refreshLayerList();
    populateLayerProperties();
    updatePreview();
    emit shotChanged();

    int count = static_cast<int>(rows.size());
    spdlog::info("ShotComposer: pasted transform onto {} layer(s)", count);
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, 10)),
        QString("Transform pasted to %1 layer(s)").arg(count), this, {}, 1200);
}

} // namespace rt
