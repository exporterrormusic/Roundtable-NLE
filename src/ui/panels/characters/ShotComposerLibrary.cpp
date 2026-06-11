/*
 * ShotComposerLibrary.cpp - Library refresh and layer property handlers
 * extracted from ShotComposerThumbnails.cpp.
 */

#include "panels/characters/ShotComposer.h"
#include "panels/characters/ShotComposerInternal.h"
#include "panels/characters/CharacterThumbnailCache.h"
#include "panels/characters/PuppetLibrary.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QPixmap>

#include <spdlog/spdlog.h>

#include <unordered_set>

namespace rt {

void ShotComposer::clearCharacterThumbCache()
{
    m_charThumbCache.clear();
}

void ShotComposer::refreshCharacterLibrary()
{
    m_charThumbCache.clear();
    m_characterLibrary->clear();

    QString searchTerm;
    if (m_charSearchEdit)
        searchTerm = m_charSearchEdit->text().toLower();

#ifdef ROUNDTABLE_HAS_SPINE
    if (m_modelManager && m_modelManager->isScanned()) {
        auto names = m_modelManager->characterNames();

        // Backfill persistent thumbnails for characters that don't have one yet
        for (const auto& name : names) {
            if (!hasCachedCharacterThumbnail(name))
                renderAndCacheCharacterThumbnail(name);
        }

        for (const auto& name : names) {
            // Get display name (with colons) for UI
            std::string dispName = m_modelManager->getDisplayName(name);

            // Apply search filter
            if (!searchTerm.isEmpty()) {
                QString qname = QString::fromStdString(dispName).toLower();
                if (!qname.contains(searchTerm))
                    continue;
            }

            // Add cache status indicator
            QString displayName = QString::fromStdString(dispName);

            // Generate thumbnail icon
            QPixmap thumb = makeCharacterThumbnail(name, m_iconSize);
            auto* item = new QListWidgetItem(QIcon(thumb), displayName);
            item->setData(Qt::UserRole, QString::fromStdString(name));  // folder name for addCharacter

            if (m_animVideoCache && m_animVideoCache->hasAnyForCharacter(name)) {
                size_t count = m_animVideoCache->countForCharacter(name);
                item->setText(QString::fromUtf8("%1\n\u2714 %2 cached")
                              .arg(displayName).arg(count));
                item->setForeground(Theme::colors().success);
            }

            m_characterLibrary->addItem(item);
        }
    }
#endif

    // Add video characters (always available regardless of Spine)
    static const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> videoCharacters = {
        // Lightweight HEVC packed-alpha source (fast NVDEC scrub/playback),
        // used for preview AND export.  (The old export-time ProRes-master
        // substitution, wellsExportSource, was removed with FrameRenderer.
        // The ProRes 4444 master shares the 1080x1888 nominal size, so
        // transforms/fit would be identical if it is ever re-wired.)
        {"Wells", {"assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4", "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4"}}
    };
    for (const auto& [name, paths] : videoCharacters) {
        if (!searchTerm.isEmpty()) {
            QString qname = QString::fromStdString(name).toLower();
            if (!qname.contains(searchTerm))
                continue;
        }
        // Only add if video files exist on disk
        if (QFileInfo::exists(QString::fromStdString(paths.first)) ||
            QFileInfo::exists(QString::fromStdString(paths.second))) {
            // Use makeCharacterThumbnail which crops to the character and produces a square thumb
            QPixmap thumb = makeCharacterThumbnail(name, m_iconSize);

            auto* item = new QListWidgetItem(QIcon(thumb),
                QString::fromStdString(name) + "\n(video)");
            item->setData(Qt::UserRole, QStringLiteral("video"));  // tag as video character
            item->setData(Qt::UserRole + 1, QString::fromStdString(paths.first));
            item->setData(Qt::UserRole + 2, QString::fromStdString(paths.second));
            item->setForeground(Theme::colors().accent);  // blue tint to distinguish
            m_characterLibrary->addItem(item);
        }
    }
}

void ShotComposer::refreshPuppetLibrary()
{
    if (!m_puppetLibrary) return;
    m_puppetLibrary->clear();

    const QString searchTerm = m_puppetSearchEdit
        ? m_puppetSearchEdit->text().trimmed().toLower() : QString();

    const QStringList folders = puppetlib::listPuppetFolders();
    for (const QString& folder : folders) {
        PuppetManifest man;
        if (!puppetlib::load(folder, man))
            continue;

        const QString displayName = man.displayName.isEmpty() ? folder : man.displayName;
        if (!searchTerm.isEmpty() && !displayName.toLower().contains(searchTerm))
            continue;

        // Use the first variant (default if present) for the drag payload + thumb.
        QString variant = man.variantOrder.isEmpty()
            ? QStringLiteral("default") : man.variantOrder.first();
        if (man.variants.contains(QStringLiteral("default")))
            variant = QStringLiteral("default");

        // Thumbnail: the resting face (index 0) of the chosen variant.
        QPixmap thumb;
        auto vit = man.variants.find(variant);
        if (vit != man.variants.end()) {
            const QString facePath = vit->faces[0];
            if (!facePath.isEmpty()) {
                QImage img(facePath);
                if (!img.isNull())
                    thumb = QPixmap::fromImage(img.scaled(
                        m_iconSize, m_iconSize,
                        Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        }

        auto* item = new QListWidgetItem(QIcon(thumb),
            displayName + QStringLiteral("\n(puppet)"));
        item->setData(Qt::UserRole, QStringLiteral("puppet"));
        item->setData(Qt::UserRole + 1, folder);
        item->setData(Qt::UserRole + 2, variant);
        item->setForeground(Theme::colors().accent);
        m_puppetLibrary->addItem(item);
    }

    spdlog::debug("ShotComposer: Found {} puppets", m_puppetLibrary->count());
}

void ShotComposer::refreshBackgroundLibrary()
{
    m_backgroundLibrary->clear();

    // Scan assets/backgrounds/ directory
    QDir bgDir("assets/backgrounds");
    if (!bgDir.exists()) return;

    const auto& tc = Theme::colors();
    QStringList filters;
    filters << "*.png" << "*.jpg" << "*.jpeg" << "*.bmp" << "*.webp";

    // Search filter (matches the file base name, case-insensitive)
    const QString searchText = m_bgSearchEdit
        ? m_bgSearchEdit->text().trimmed().toLower() : QString();
    auto matchesSearch = [&](const QFileInfo& fi) {
        return searchText.isEmpty() || fi.baseName().toLower().contains(searchText);
    };

    // Helper: add a single background item (cached thumbnail).
    // For subfolder items, store the relative-from-backgrounds-dir path in UserRole+2
    // so DragAssetList::mimeData() can pass the correct path to addBackground().
    auto addBgItem = [&](const QFileInfo& entry, const QString& subdirName = QString()) {
        QPixmap thumb;
        std::string pathKey = entry.absoluteFilePath().toStdString();
        auto cacheIt = m_bgImageCache.find(pathKey);
        if (cacheIt != m_bgImageCache.end()) {
            thumb = QPixmap::fromImage(cacheIt->second.scaled(
                m_iconSize, m_iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            QImage img(entry.absoluteFilePath());
            if (!img.isNull()) {
                m_bgImageCache[pathKey] = img;
                thumb = QPixmap::fromImage(img.scaled(
                    m_iconSize, m_iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
        }
        auto* item = new QListWidgetItem(QIcon(thumb), entry.baseName());
        // Store absolute path in UserRole+1 so UserRole stays empty,
        // allowing DragAssetList::mimeData() to correctly identify this as a background.
        item->setData(Qt::UserRole + 1, entry.absoluteFilePath());
        if (!subdirName.isEmpty()) {
            // Store full relative-from-app-root path for subfolder items
            // (e.g. "assets/backgrounds/Nikke In-Game Backgrounds/bg_chapter_01.png")
            // so existing rendering code (which prepends "assets/backgrounds/") works.
            QString relPath = QString("assets/backgrounds/") + subdirName + "/" + entry.fileName();
            item->setData(Qt::UserRole + 2, relPath);
            item->setForeground(tc.textSecondary);  // slightly dimmed for subfolder items
        }
        m_backgroundLibrary->addItem(item);
    };

    // ── Step 1: root-level files (user-added backgrounds) ───────────────
    auto rootFiles = bgDir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const auto& entry : rootFiles) {
        if (!matchesSearch(entry)) continue;
        addBgItem(entry, QString());
    }

    // ── Step 2: subdirectories — each as a group ────────────────────────
    auto subdirs = bgDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& sd : subdirs) {
        QDir subDir(sd.absoluteFilePath());
        auto subFiles = subDir.entryInfoList(filters, QDir::Files, QDir::Name);

        // Apply the search filter to the subfolder's files first.
        QList<QFileInfo> matched;
        for (const auto& entry : subFiles)
            if (matchesSearch(entry)) matched.append(entry);
        if (matched.isEmpty()) continue; // hide groups with no matches

        // Non-selectable group header
        auto* header = new QListWidgetItem(QStringLiteral("\xF0\x9F\x93\x81  ") + sd.fileName());
        header->setFlags(header->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsDragEnabled));
        header->setForeground(tc.accent);
        QFont headerFont = header->font();
        headerFont.setBold(true);
        headerFont.setPointSize(headerFont.pointSize() + 1);
        header->setFont(headerFont);
        m_backgroundLibrary->addItem(header);

        // Files under this subdirectory — pass subdir name for relative path
        for (const auto& entry : matched) {
            addBgItem(entry, sd.fileName());
        }
    }

    spdlog::debug("ShotComposer: Found {} backgrounds", m_backgroundLibrary->count());
}

void ShotComposer::importBackgroundFiles(const QStringList& sourcePaths)
{
    if (sourcePaths.isEmpty() || !m_backgroundLibrary) return;

    static const QStringList kImgExt =
        {"png", "jpg", "jpeg", "bmp", "webp"};

    QDir().mkpath(QStringLiteral("assets/backgrounds"));
    QStringList importedAbsPaths;
    for (const QString& srcPath : sourcePaths) {
        QFileInfo fi(srcPath);
        if (!fi.isFile() || !kImgExt.contains(fi.suffix().toLower()))
            continue; // ignore non-image files
        QString dstPath = QStringLiteral("assets/backgrounds/") + fi.fileName();
        if (!QFile::exists(dstPath)) {
            if (!QFile::copy(srcPath, dstPath)) {
                spdlog::warn("ShotComposer: failed to import background '{}'",
                             srcPath.toStdString());
                continue;
            }
        }
        importedAbsPaths << QFileInfo(dstPath).absoluteFilePath();
    }
    if (importedAbsPaths.isEmpty()) return;

    // Import into the library only. Adding a background to the shot is done by
    // dragging the thumbnail onto the layers/preview, or double-clicking it.
    refreshBackgroundLibrary();
}

void ShotComposer::refreshVideoLibrary()
{
    m_videoLibrary->clear();

    // Scan assets/videos/ directory
    QDir vidDir("assets/videos");
    if (!vidDir.exists()) return;

    QStringList filters;
    filters << "*.mp4" << "*.avi" << "*.mov" << "*.mkv" << "*.webm" << "*.wmv";
    auto entries = vidDir.entryInfoList(filters, QDir::Files, QDir::Name);

    // Search filter (matches video file or video-character name, case-insensitive)
    const QString searchText = m_videoSearchEdit
        ? m_videoSearchEdit->text().trimmed().toLower() : QString();

    // Track which video-character names we've already added (avoid duplicates)
    std::unordered_set<std::string> addedVideoChars;

    for (const auto& entry : entries) {
        std::string lower = entry.fileName().toLower().toStdString();
        std::string fullPath = entry.absoluteFilePath().toStdString();
        auto it = videoCharacterFiles().find(lower);
        if (it != videoCharacterFiles().end()) {
            // This file belongs to a video character â€” add as character entry
            const auto& [charName, mutePath, talkPath] = it->second;
            if (addedVideoChars.count(charName)) continue;
            if (!searchText.isEmpty() &&
                !QString::fromStdString(charName).toLower().contains(searchText))
                continue;
            addedVideoChars.insert(charName);

            // Extract thumbnail from mute video
            QPixmap thumb;
            QImage frame = extractVideoThumbnail(mutePath);
            if (!frame.isNull())
                thumb = QPixmap::fromImage(frame.scaled(m_iconSize, m_iconSize,
                    Qt::KeepAspectRatio, Qt::SmoothTransformation));

            auto* item = new QListWidgetItem(QIcon(thumb),
                QString::fromStdString(charName) + "\n(video char)");
            item->setData(Qt::UserRole, QStringLiteral("videoChar"));
            item->setData(Qt::UserRole + 1, QString::fromStdString(charName));
            item->setData(Qt::UserRole + 2, QString::fromStdString(mutePath));
            item->setData(Qt::UserRole + 3, QString::fromStdString(talkPath));
            item->setForeground(Theme::colors().accent);
            m_videoLibrary->addItem(item);
        } else {
            // Regular video â€” apply search filter on the file base name
            if (!searchText.isEmpty() &&
                !entry.baseName().toLower().contains(searchText))
                continue;
            // extract first frame as thumbnail
            QPixmap thumb;
            QImage frame = extractVideoThumbnail(fullPath);
            if (!frame.isNull())
                thumb = QPixmap::fromImage(frame.scaled(m_iconSize, m_iconSize,
                    Qt::KeepAspectRatio, Qt::SmoothTransformation));

            auto* item = new QListWidgetItem(QIcon(thumb), entry.baseName());
            item->setData(Qt::UserRole, QStringLiteral("video_file"));
            item->setData(Qt::UserRole + 1, entry.absoluteFilePath());
            m_videoLibrary->addItem(item);
        }
    }

    spdlog::debug("ShotComposer: Found {} videos", entries.size());
}

int ShotComposer::addVideoLayer(const std::string& filename)
{
    pushUndoState();
    BackgroundState bg;
    bg.path      = "assets/videos/" + filename;
    bg.layerType = "video";
    bg.posX      = 0.5f;
    bg.posY      = 0.5f;
    bg.scale     = 1.0f;

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

void ShotComposer::onVideoTimingChanged()
{
    if (m_updating) return;
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount())
        return;

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    if (ref.type != LayerType::Background)
        return;

    auto* bg = m_currentShot.background(ref.index);
    if (!bg || !bg->isVideo()) return;

    bg->inPoint  = static_cast<float>(m_videoInSpin->value());
    bg->outPoint = static_cast<float>(m_videoOutSpin->value());

    emit shotChanged();
}

void ShotComposer::populateLayerProperties()
{
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount()) {
        clearLayerProperties();
        updatePreview();
        return;
    }

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    if (ref.type == LayerType::Character) {
        const auto* ch = m_currentShot.character(ref.index);
        if (ch)
            showCharacterProperties(*ch);
    } else {
        const auto* bg = m_currentShot.background(ref.index);
        if (bg)
            showBackgroundProperties(*bg);
    }

    updatePreview();
}

void ShotComposer::clearLayerProperties()
{
    if (m_propsStack)
        m_propsStack->setCurrentIndex(0);  // empty placeholder
}

void ShotComposer::showCharacterProperties(const CharacterState& ch)
{
    m_updating = true;

    // Switch stacked widget to character page
    if (m_propsStack)
        m_propsStack->setCurrentIndex(1);
    m_charPropsGroup->setVisible(true);

    // Convert normalized 0â€“1 storage â†’ percentage display
    m_posXSpin->setValue(static_cast<double>(ch.posX) * 100.0);
    m_posYSpin->setValue(static_cast<double>(ch.posY) * 100.0);
    m_scaleSpin->setValue(static_cast<double>(ch.scale) * 100.0);
    m_rotationSpin->setValue(static_cast<double>(ch.rotation));
    m_opacitySpin->setValue(static_cast<double>(ch.opacity) * 100.0);
    m_blurSpin->setValue(static_cast<double>(ch.blur));

    // Populate outfit combo â€” ensure "default" always appears first
    m_outfitCombo->clear();
    bool isVideoChar = ch.isVideoCharacter();
    bool isPuppetChar = ch.isPuppet();
    bool videoHasOutfits = false;
    if (isPuppetChar) {
        // Puppets have no Spine outfit/stance/animation — leave the combo empty.
    } else if (isVideoChar) {
        // Video characters: outfits come from the video-outfit catalog, each
        // swapping the mute/talk video pair (mirrors Spine outfit switching).
        const auto& vcOutfits = videoCharacterOutfitsFor(ch.characterName);
        for (const auto& o : vcOutfits)
            m_outfitCombo->addItem(QString::fromStdString(o.name));
        videoHasOutfits = m_outfitCombo->count() > 1;

        // Select by matching the character's current video path first (robust
        // to legacy presets that stored outfit="default"), then by outfit name.
        int sel = -1;
        for (int i = 0; i < static_cast<int>(vcOutfits.size()); ++i) {
            if (vcOutfits[static_cast<size_t>(i)].mutePath == ch.videoMutePath ||
                vcOutfits[static_cast<size_t>(i)].talkPath == ch.videoTalkPath) {
                sel = i; break;
            }
        }
        if (sel < 0)
            sel = m_outfitCombo->findText(QString::fromStdString(ch.outfit));
        if (sel < 0 && m_outfitCombo->count() > 0) sel = 0;
        if (sel >= 0) m_outfitCombo->setCurrentIndex(sel);
    } else {
#ifdef ROUNDTABLE_HAS_SPINE
        if (m_modelManager) {
            const auto* entry = m_modelManager->findByName(ch.characterName);
            if (entry) {
                // Add "default" first if it exists, then the rest
                bool hasDefault = false;
                for (const auto& outfit : entry->outfits) {
                    if (outfit.name == "default") { hasDefault = true; break; }
                }
                if (hasDefault)
                    m_outfitCombo->addItem("default");
                for (const auto& outfit : entry->outfits) {
                    if (outfit.name != "default")
                        m_outfitCombo->addItem(QString::fromStdString(outfit.name));
                }
            }
        }
#endif
        // Select the character's current outfit, fall back to "default"
        std::string targetOutfit = ch.outfit.empty() ? "default" : ch.outfit;
        int outfitIdx = m_outfitCombo->findText(QString::fromStdString(targetOutfit));
        if (outfitIdx >= 0) m_outfitCombo->setCurrentIndex(outfitIdx);
        else if (m_outfitCombo->count() > 0) {
            m_outfitCombo->setCurrentIndex(0);
        } else {
            m_outfitCombo->addItem(QString::fromStdString(targetOutfit));
            m_outfitCombo->setCurrentIndex(0);
        }
    }

    int stanceIdx = static_cast<int>(ch.stance);
    if (stanceIdx >= 0 && stanceIdx < m_stanceCombo->count())
        m_stanceCombo->setCurrentIndex(stanceIdx);

    m_animCombo->setCurrentText(QString::fromStdString(ch.animation));
    m_talkingCheck->setChecked(ch.isTalking);
    m_flipXCheck->setChecked(ch.flipX);
    m_flipYCheck->setChecked(ch.flipY);
    m_visibleCheck->setChecked(ch.visible);

    // Hide Spine-specific controls for video characters and puppets, but keep
    // the Outfit combo when a video character has selectable costumes (Wells).
    bool isVideo = ch.isVideoCharacter();
    bool isPuppet = ch.isPuppet();
    bool spineLike = !isVideo && !isPuppet;
    m_outfitCombo->setVisible(spineLike || videoHasOutfits);
    m_stanceCombo->setVisible(spineLike);
    m_animCombo->setVisible(spineLike);
    // Flip + Talking are available for ALL character types (Spine/video/puppet)

    // Hide the "Character" tab entirely for video characters and puppets
    if (m_layerPropsTabs) {
        int charTabIdx = 1; // "Character" tab
        m_layerPropsTabs->setTabEnabled(charTabIdx, spineLike);
        m_layerPropsTabs->setTabVisible(charTabIdx, spineLike);
    }

    // Crop values are already 0â€“100
    m_cropLeftSpin->setValue(static_cast<double>(ch.cropLeft));
    m_cropRightSpin->setValue(static_cast<double>(ch.cropRight));
    m_cropTopSpin->setValue(static_cast<double>(ch.cropTop));
    m_cropBottomSpin->setValue(static_cast<double>(ch.cropBottom));
    m_cropGroup->setChecked(ch.cropLeft > 0 || ch.cropRight > 0 ||
                            ch.cropTop > 0 || ch.cropBottom > 0);

    m_updating = false;
}

void ShotComposer::showBackgroundProperties(const BackgroundState& bg)
{
    m_updating = true;

    // Switch stacked widget to background page
    if (m_propsStack)
        m_propsStack->setCurrentIndex(2);
    m_bgPropsGroup->setVisible(true);

    m_bgPosXSpin->setValue(static_cast<double>(bg.posX) * 100.0);
    m_bgPosYSpin->setValue(static_cast<double>(bg.posY) * 100.0);
    m_bgScaleSpin->setValue(static_cast<double>(bg.scale) * 100.0);
    m_bgOpacitySpin->setValue(static_cast<double>(bg.opacity) * 100.0);
    m_bgBlurSpin->setValue(static_cast<double>(bg.blur));

    // Show video timing controls only for video layers
    bool isVideo = bg.isVideo();
    m_videoTimingGroup->setVisible(isVideo);
    if (isVideo) {
        m_videoInSpin->setValue(static_cast<double>(bg.inPoint));
        m_videoOutSpin->setValue(static_cast<double>(bg.outPoint));
    }

    m_updating = false;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// Slots
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

void ShotComposer::onShotListSelectionChanged()
{
    auto* current = m_shotList ? m_shotList->currentItem() : nullptr;
    if (!current) return;
    auto name = current->data(Qt::UserRole).toString().toStdString();
    if (name.empty()) return;
    auto preset = m_presetManager.load(name);
    if (preset)
        setCurrentShot(*preset);
}

void ShotComposer::onLayerListSelectionChanged()
{
    int row = m_layerList->currentRow();
    if (row == m_selectedLayer) return;

    m_selectedLayer = row;
    populateLayerProperties();
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview) {
        m_spinePreview->setSelectedLayer(row);
        // Pass all selected layer indices for multi-layer transform
        QSet<int> selectedIndices;
        const auto selItems = m_layerList->selectionModel()->selectedRows();
        for (const auto& idx : selItems)
            selectedIndices.insert(idx.row());
        // If no multi-selection via selectedRows, include at least the current row
        if (selectedIndices.isEmpty() && row >= 0)
            selectedIndices.insert(row);
        m_spinePreview->setSelectedLayers(selectedIndices);
    }
#endif
    emit layerSelected(row);
}

void ShotComposer::onShotNameChanged(const QString& name)
{
    if (m_updating) return;
    m_currentShot.setName(name.toStdString());
}

void ShotComposer::onCharacterPropertyChanged()
{
    if (m_updating) return;
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount())
        return;

    // Flip H/V toggles always get their own undo state (never coalesce).
    // Flip is stored as a separate bool on the character, not as a
    // continuous value like scale/position.  Coalescing a flip with a
    // subsequent scale means one Ctrl+Z reverts BOTH — the user loses
    // the flip even though it was a deliberate earlier action.
    const bool isFlipToggle = (sender() == m_flipXCheck || sender() == m_flipYCheck);

    // Push undo before the first change in a burst of edits.
    // Flip toggles always force a new undo state.
    if (!m_undoPropertyPushed || isFlipToggle) {
        pushUndoState();
        m_undoPropertyPushed = true;
    }
    m_undoCoalesceTimer->start();  // restart the 600ms window

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    if (ref.type != LayerType::Character)
        return;

    auto* ch = m_currentShot.character(ref.index);
    if (!ch) return;

    // Convert percentage display â†’ normalized 0â€“1 storage
    ch->posX      = static_cast<float>(m_posXSpin->value() / 100.0);
    ch->posY      = static_cast<float>(m_posYSpin->value() / 100.0);
    ch->scale     = static_cast<float>(m_scaleSpin->value() / 100.0);
    ch->rotation  = static_cast<float>(m_rotationSpin->value());
    ch->opacity   = static_cast<float>(m_opacitySpin->value() / 100.0);
    ch->blur      = static_cast<float>(m_blurSpin->value());
    ch->outfit    = m_outfitCombo->currentText().toStdString();
    ch->animation = m_animCombo->currentText().toStdString();
    ch->isTalking = m_talkingCheck->isChecked();
    ch->flipX     = m_flipXCheck->isChecked();
    ch->flipY     = m_flipYCheck->isChecked();
    ch->visible   = m_visibleCheck->isChecked();

    ch->cropLeft   = static_cast<float>(m_cropLeftSpin->value());
    ch->cropRight  = static_cast<float>(m_cropRightSpin->value());
    ch->cropTop    = static_cast<float>(m_cropTopSpin->value());
    ch->cropBottom = static_cast<float>(m_cropBottomSpin->value());

    int stanceIdx = m_stanceCombo->currentIndex();
    ch->stance = static_cast<CharacterStance>(stanceIdx >= 0 ? stanceIdx : 0);

    // For video characters, the outfit selects a different mute/talk video
    // pair — swap the stored paths so the preview/export pick up the costume.
    if (ch->isVideoCharacter()) {
        for (const auto& o : videoCharacterOutfitsFor(ch->characterName)) {
            if (o.name == ch->outfit) {
                ch->videoMutePath = o.mutePath;
                ch->videoTalkPath = o.talkPath;
                break;
            }
        }
    }

    updatePreview();
    emit shotChanged();
}

void ShotComposer::onBackgroundPropertyChanged()
{
    if (m_updating) return;
    if (m_selectedLayer < 0 || m_selectedLayer >= m_currentShot.layerCount())
        return;

    // Push undo before the first change in a burst of edits
    if (!m_undoPropertyPushed) {
        pushUndoState();
        m_undoPropertyPushed = true;
    }
    m_undoCoalesceTimer->start();  // restart the 600ms window

    const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(m_selectedLayer)];
    if (ref.type != LayerType::Background)
        return;

    auto* bg = m_currentShot.background(ref.index);
    if (!bg) return;

    bg->posX    = static_cast<float>(m_bgPosXSpin->value() / 100.0);
    bg->posY    = static_cast<float>(m_bgPosYSpin->value() / 100.0);
    bg->scale   = static_cast<float>(m_bgScaleSpin->value() / 100.0);
    bg->opacity = static_cast<float>(m_bgOpacitySpin->value() / 100.0);
    bg->blur    = static_cast<float>(m_bgBlurSpin->value());

    updatePreview();
    emit shotChanged();
}

void ShotComposer::onCameraPropertyChanged()
{
    if (m_updating) return;
    m_currentShot.setCameraZoom(static_cast<float>(m_cameraZoomSpin->value() / 100.0));
    m_currentShot.setCameraX(static_cast<float>(m_cameraPanXSpin->value() / 100.0));
    m_currentShot.setCameraY(static_cast<float>(m_cameraPanYSpin->value() / 100.0));
#ifdef ROUNDTABLE_HAS_SPINE
    if (m_spinePreview)
        m_spinePreview->setCameraTransform(
            m_currentShot.cameraZoom(),
            m_currentShot.cameraX(),
            m_currentShot.cameraY());
#endif
    emit shotChanged();
}

} // namespace rt

