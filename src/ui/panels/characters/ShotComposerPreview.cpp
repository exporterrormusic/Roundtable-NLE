/*
 * ShotComposerPreview.cpp - Preview rendering for ShotComposer.
 * Extracted from ShotComposer.cpp for maintainability.
 */

#include "panels/characters/ShotComposer.h"

#include "panels/characters/ShotComposerInternal.h"
#include "panels/characters/PuppetLibrary.h"
#include "timeline/PngPuppetClip.h"
#include "Theme.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#include "spine/SpineEngine.h"
#include "spine/AnimationVideoCache.h"
#include "widgets/SpinePreviewWidget.h"
#endif

#include <QApplication>
#include <QComboBox>
#include <QFileInfo>
#include <QImage>
#include <QListWidget>
#include <QSet>

#include <spdlog/spdlog.h>

namespace rt {

// =====================================================================
//  updatePreview — build preview layers from the current shot and
//  push them to the SpinePreviewWidget for compositing.
// =====================================================================
void ShotComposer::updatePreview()
{
#ifdef ROUNDTABLE_HAS_SPINE
    if (!m_spinePreview || !m_modelManager)
        return;

    m_spinePreview->stopAnimation();
    m_spinePreview->clearBackgroundImage();   // BG is now in the layer stack

    // ── Resize engine pools to match character count ────────────────────
    size_t needed = static_cast<size_t>(m_currentShot.characterCount());
    while (m_layerEngines.size() < needed) {
        m_layerEngines.push_back(std::make_unique<SpineEngine>());
        m_layerTextures.emplace_back();
    }
    if (m_layerEngines.size() > needed) {
        m_layerEngines.resize(needed);
        m_layerTextures.resize(needed);
    }

    // ── Build unified layer list — backgrounds + characters in z-order ──
    // layerOrder[0] = FRONT (top of UI list, drawn on top)
    // We iterate back-to-front so previewLayers[0] = BACK, previewLayers[last] = FRONT
    std::vector<PreviewCharLayer> previewLayers;
    int selectedCharIdx = -1;
    std::vector<AnimationInfo> selectedAnims;

    spdlog::debug("ShotComposer::updatePreview — {} layers in order:", m_currentShot.layerCount());
    for (int dbg = 0; dbg < m_currentShot.layerCount(); ++dbg) {
        const auto& r = m_currentShot.layerOrder()[static_cast<size_t>(dbg)];
        if (r.type == LayerType::Background) {
            const auto* bg = m_currentShot.background(r.index);
            spdlog::debug("  [{}] BG idx={} path='{}' visible={}",
                dbg, r.index, bg ? bg->path : "?", bg ? bg->visible : false);
        } else {
            const auto* ch = m_currentShot.character(r.index);
            spdlog::debug("  [{}] CH idx={} name='{}' visible={}",
                dbg, r.index, ch ? ch->characterName : "?", ch ? ch->visible : false);
        }
    }

    for (int i = m_currentShot.layerCount() - 1; i >= 0; --i) {
        const auto& ref = m_currentShot.layerOrder()[static_cast<size_t>(i)];

        if (ref.type == LayerType::Background) {
            // ── Background / video layer ────────────────────────────────
            const auto* bg = m_currentShot.background(ref.index);
            if (!bg) continue;

            PreviewCharLayer layer;
            layer.isBackground = true;
            layer.visible      = bg->visible;
            layer.opacity      = bg->opacity;
            layer.posX         = bg->posX;
            layer.posY         = bg->posY;
            layer.scale        = bg->scale;
            layer.layerIndex   = i;
            layer.cropLeft     = bg->cropLeft;
            layer.cropRight    = bg->cropRight;
            layer.cropTop      = bg->cropTop;
            layer.cropBottom   = bg->cropBottom;
            layer.blur         = bg->blur;

            if (!bg->path.empty()) {
                if (bg->isVideo()) {
                    // Set up looping video playback via VideoDecoder
                    auto player = getOrCreateVideoPlayer(bg->path);
                    if (player) {
                        // Set initial frame
                        if (!player->lastFrame.isNull())
                            layer.backgroundImage = player->lastFrame;
                        else {
                            // Fall back to thumbnail for initial display
                            QImage thumb = extractVideoThumbnail(bg->path);
                            if (!thumb.isNull())
                                layer.backgroundImage = thumb;
                        }
                        // Install video frame provider callback (looping)
                        layer.videoFrameProvider = [player](float vdt) -> QImage {
                            return ShotComposer::advanceVideoPlayer(player, vdt);
                        };
                    } else {
                        // Fallback to static thumbnail
                        QImage frame = extractVideoThumbnail(bg->path);
                        if (!frame.isNull())
                            layer.backgroundImage = frame;
                    }
                } else {
                    // Static background image — use cache to avoid disk I/O every frame
                    auto cacheIt = m_bgImageCache.find(bg->path);
                    if (cacheIt != m_bgImageCache.end()) {
                        layer.backgroundImage = cacheIt->second;
                    } else {
                        QImage bgImg(QString::fromStdString(bg->path));
                        if (bgImg.isNull()) {
                            bgImg = QImage(QString("assets/backgrounds/%1")
                                .arg(QString::fromStdString(bg->path)));
                        }
                        if (bgImg.isNull()) {
                            // Try from app root
                            QString altPath = QApplication::applicationDirPath()
                                + "/../../../" + QString::fromStdString(bg->path);
                            if (QFileInfo::exists(altPath))
                                bgImg = QImage(altPath);
                        }
                        if (!bgImg.isNull()) {
                            bgImg = bgImg.convertToFormat(QImage::Format_ARGB32);
                            m_bgImageCache[bg->path] = bgImg;
                            layer.backgroundImage = bgImg;
                        }
                    }
                }
            }

            previewLayers.push_back(std::move(layer));
            continue;
        }

        // ── Character (Spine) layer ──────────────────────────────────────
        const auto* ch = m_currentShot.character(ref.index);
        if (!ch) continue;

        if (i == m_selectedLayer)
            selectedCharIdx = ref.index;

        // ── PNG puppet ───────────────────────────────────────────────────
        // Rendered as an image layer (character-style sizing) whose displayed
        // face is chosen by the same deterministic talk/blink logic the
        // timeline/export PngPuppetClip uses, advancing a per-layer clock so
        // the preview feels alive — just like a Spine idle.
        if (ch->isPuppet()) {
            PreviewCharLayer layer;
            layer.isBackground     = true;   // image layer
            layer.isVideoCharacter = true;   // character-style 0.85 contain fit
            layer.visible      = ch->visible;
            layer.opacity      = ch->opacity;
            layer.posX         = ch->posX;
            layer.posY         = ch->posY;
            layer.scale        = ch->scale;
            layer.layerIndex   = i;
            layer.cropLeft     = ch->cropLeft;
            layer.cropRight    = ch->cropRight;
            layer.cropTop      = ch->cropTop;
            layer.cropBottom   = ch->cropBottom;
            layer.flipX        = ch->flipX;
            layer.flipY        = ch->flipY;
            layer.rotation     = ch->rotation;
            layer.blur         = ch->blur;

            const std::string variant =
                ch->puppetVariant.empty() ? "default" : ch->puppetVariant;
            const std::string key = ch->puppetFolder + "|" + variant;

            auto stIt = m_puppetPreview.find(key);
            if (stIt == m_puppetPreview.end()) {
                auto st = std::make_shared<PuppetPreviewState>();
                // Seed from the puppet folder so blinks look natural + stable.
                uint32_t h = 2166136261u;
                for (char c : ch->puppetFolder) {
                    h ^= static_cast<uint8_t>(c);
                    h *= 16777619u;
                }
                st->seed = h ? h : 1u;
                PuppetManifest man;
                if (puppetlib::load(QString::fromStdString(ch->puppetFolder), man)) {
                    auto vit = man.variants.find(QString::fromStdString(variant));
                    if (vit == man.variants.end() && !man.variantOrder.isEmpty())
                        vit = man.variants.find(man.variantOrder.first());
                    if (vit != man.variants.end()) {
                        for (int f = 0; f < 4; ++f) {
                            const QString& fp = vit->faces[f];
                            if (!fp.isEmpty()) st->faces[f] = QImage(fp);
                        }
                    }
                }
                stIt = m_puppetPreview.emplace(key, std::move(st)).first;
            }
            auto state = stIt->second;
            state->talking = ch->isTalking;

            // Initial frame: resting face.
            if (!state->faces[0].isNull())
                layer.backgroundImage = state->faces[0];

            const uint32_t seed = state->seed;
            layer.videoFrameProvider =
                [state, seed](float vdt) -> QImage {
                state->clock += static_cast<double>(vdt);
                PngPuppetClip eval;
                eval.setTalking(state->talking);
                eval.setSeed(seed);
                int face = eval.selectFace(state->clock);
                if (face < 0 || face > 3) face = 0;
                if (!state->faces[face].isNull())
                    return state->faces[face];
                return state->faces[0];
            };
            // Breathing/sway — same deterministic motion as the timeline clip,
            // read from the clock the frame provider advances each tick.
            layer.breathProvider =
                [state](float& dxRef, float& dyRef, float& scaleMul, float& rotDeg) {
                PngPuppetClip eval;
                auto b = eval.breathing(state->clock);
                dxRef = b.dx;
                dyRef = b.dy;
                scaleMul = b.scale;
                rotDeg = b.rot;
            };

            previewLayers.push_back(std::move(layer));
            continue;
        }

        // ── Video character (e.g. Wells) ─────────────────────────────────
        if (ch->isVideoCharacter()) {
            const std::string& videoPath = ch->activeVideoPath();
            PreviewCharLayer layer;
            layer.isBackground = true;       // render as image layer
            layer.isVideoCharacter = true;   // use character-style sizing
            layer.visible      = ch->visible;
            layer.opacity      = ch->opacity;
            layer.posX         = ch->posX;
            layer.posY         = ch->posY;
            layer.scale        = ch->scale;
            layer.layerIndex   = i;
            layer.cropLeft     = ch->cropLeft;
            layer.cropRight    = ch->cropRight;
            layer.cropTop      = ch->cropTop;
            layer.cropBottom   = ch->cropBottom;
            layer.flipX        = ch->flipX;
            layer.flipY        = ch->flipY;
            layer.rotation     = ch->rotation;
            layer.blur         = ch->blur;

            if (!videoPath.empty()) {
                // Helper to detect and unpack packed-alpha frames
                // for the initial frame / thumbnail only. The worker thread
                // now unpacks every decoded frame before storing it.
                auto maybeUnpack = [](QImage img) -> QImage {
                    if (img.isNull()) return img;
                    if (img.height() > img.width() && (img.height() % 2 == 0) &&
                        img.height() >= img.width() * 1.8) {
                        return unpackPackedAlpha(img.bits(),
                            static_cast<uint32_t>(img.width()),
                            static_cast<uint32_t>(img.height()));
                    }
                    return img;
                };

                auto player = getOrCreateVideoPlayer(videoPath);
                if (player) {
                    if (!player->lastFrame.isNull())
                        layer.backgroundImage = player->lastFrame; // already unpacked at open
                    else {
                        QImage thumb = extractVideoThumbnail(videoPath);
                        if (!thumb.isNull())
                            layer.backgroundImage = maybeUnpack(thumb);
                    }
                    // Frames from advanceVideoPlayer are already unpacked
                    // by the worker thread — no per-frame pixel processing
                    // on the UI thread.
                    layer.videoFrameProvider = [player](float vdt) -> QImage {
                        return ShotComposer::advanceVideoPlayer(player, vdt);
                    };
                } else {
                    QImage frame = extractVideoThumbnail(videoPath);
                    if (!frame.isNull())
                        layer.backgroundImage = maybeUnpack(frame);
                }
            }

            previewLayers.push_back(std::move(layer));
            continue;
        }

        // ── Normal Spine character ────────────────────────────────────────
        size_t ei = static_cast<size_t>(ref.index);
        if (ei >= m_layerEngines.size()) {
            spdlog::error("ShotComposer::updatePreview: character index {} >= engine pool size {} — skipping",
                          ei, m_layerEngines.size());
            continue;
        }
        auto& engine = m_layerEngines[ei];

        std::string outfitKey = ch->outfit.empty() ? "default" : ch->outfit;
        CharacterStance stance = ch->stance;

        const auto* variant = m_modelManager->findVariant(ch->characterName, outfitKey, stance);
        if (!variant || variant->skelPath.empty() || variant->atlasPath.empty())
            continue;

        // Clear cached textures if the skeleton/atlas is about to change
        if (engine->loadedSkelPath() != variant->skelPath ||
            engine->loadedAtlasPath() != variant->atlasPath) {
            m_layerTextures[ei].clear();
        }

        // loadSkeleton now skips if same files already loaded (fast path)
        if (!engine->loadSkeleton(variant->skelPath, variant->atlasPath, 0.5f)) {
            spdlog::warn("ShotComposer: Failed to load skeleton for '{}': {}",
                         ch->characterName, variant->skelPath);
            continue;
        }

        std::string animName = ch->animation.empty() ? "idle" : ch->animation;
        auto anims = engine->animation().listAnimations();
        bool foundAnim = false;
        for (const auto& a : anims) {
            if (a.name == animName) { foundAnim = true; break; }
        }
        if (foundAnim) {
            engine->animation().setBodyAnimation(animName, true);
        } else if (!anims.empty()) {
            engine->animation().setBodyAnimation(anims[0].name, true);
        }

        if (ch->isTalking)
            engine->animation().startTalking();
        else
            engine->animation().stopTalking();

        if (ref.index == selectedCharIdx)
            selectedAnims = anims;

        // Only reload textures if engine was actually reloaded (atlas changed)
        auto& textures = m_layerTextures[ei];
        if (textures.empty()) {
            const auto& pages = engine->atlas().pages();
            const auto& atlasDir = engine->atlas().directory();
            for (const auto& page : pages) {
                std::string fullPath = atlasDir + "/" + page.texturePath;
                QImage img(QString::fromStdString(fullPath));
                if (img.isNull()) {
                    textures.emplace_back();
                } else {
                    img = img.convertToFormat(QImage::Format_ARGB32);
                    // PMA atlas: un-premultiply so the rasteriser doesn't
                    // double-premultiply, which causes dark edge borders.
                    if (page.pma) {
                        uint8_t* px = img.bits();
                        const int total = img.width() * img.height();
                        for (int p = 0; p < total; ++p) {
                            const uint8_t a = px[p * 4 + 3];
                            if (a > 0 && a < 255) {
                                px[p * 4 + 0] = static_cast<uint8_t>(std::min(255, px[p * 4 + 0] * 255 / a));
                                px[p * 4 + 1] = static_cast<uint8_t>(std::min(255, px[p * 4 + 1] * 255 / a));
                                px[p * 4 + 2] = static_cast<uint8_t>(std::min(255, px[p * 4 + 2] * 255 / a));
                            } else if (a == 0) {
                                px[p * 4 + 0] = 0; px[p * 4 + 1] = 0; px[p * 4 + 2] = 0;
                            }
                        }
                    }
                    textures.push_back(std::move(img));
                }
            }
        }

        PreviewCharLayer layer;
        layer.engine      = engine.get();
        layer.textures    = textures;
        layer.posX        = ch->posX;
        layer.posY        = ch->posY;
        layer.scale       = ch->scale;
        layer.rotation    = ch->rotation;
        layer.flipX       = ch->flipX;
        layer.flipY       = ch->flipY;
        layer.opacity     = ch->opacity;
        layer.visible     = ch->visible;
        layer.layerIndex  = i;
        layer.cropLeft    = ch->cropLeft;
        layer.cropRight   = ch->cropRight;
        layer.cropTop     = ch->cropTop;
        layer.cropBottom  = ch->cropBottom;
        layer.blur        = ch->blur;
        previewLayers.push_back(std::move(layer));
    }

    spdlog::debug("ShotComposer::updatePreview — built {} preview layers (back-to-front)",
        previewLayers.size());

    // previewLayers is already ordered back-to-front (we iterated in reverse)
    // Pass all layers to the preview widget
    m_spinePreview->setCharacterLayers(std::move(previewLayers));
    m_spinePreview->setSelectedLayer(m_selectedLayer);

    // Keep SpinePreviewWidget's multi-select set up-to-date after rebuilding layers
    {
        QSet<int> sel;
        const auto rows = m_layerList->selectionModel()->selectedRows();
        for (const auto& idx : rows)
            sel.insert(idx.row());
        if (sel.isEmpty() && m_selectedLayer >= 0)
            sel.insert(m_selectedLayer);
        m_spinePreview->setSelectedLayers(sel);
    }

    // NOTE: camera transform is NOT applied here — it is set once when the
    // shot is loaded (setCurrentShot) or when the user changes camera spins
    // (onCameraPropertyChanged).  Applying it here would reset the user's
    // interactive viewport zoom/pan every time a layer is clicked.

    // Start animation timer for any scene with layers (characters OR videos)
    bool hasAnimatedContent = !m_currentShot.characters().empty();
    // Check if any video layers exist (they need the timer for playback)
    for (int i = 0; i < m_currentShot.backgroundCount(); ++i) {
        const auto* bg = m_currentShot.background(i);
        if (bg && bg->isVideo()) { hasAnimatedContent = true; break; }
    }
    if (hasAnimatedContent)
        m_spinePreview->startAnimation();
    else
        m_spinePreview->update(); // Just repaint for static BG-only scenes

    // Populate animation dropdown for the SELECTED character layer only.
    // Each character has its own unique set of animation names — filter
    // out internal bookend animations (talk_start / talk_end) so only
    // user-meaningful body animations are shown.
    if (selectedCharIdx >= 0 && !selectedAnims.empty()) {
        m_updating = true;
        QString currentAnim = m_animCombo->currentText();
        m_animCombo->clear();
        for (const auto& a : selectedAnims) {
            if (a.name == "talk_start" || a.name == "talk_end") continue;
            if (a.duration <= 0.0f) continue;  // skip zero-length markers
            m_animCombo->addItem(QString::fromStdString(a.name));
        }
        int idx = m_animCombo->findText(currentAnim);
        if (idx >= 0) {
            m_animCombo->setCurrentIndex(idx);
        } else if (m_animCombo->count() > 0) {
            int idleIdx = m_animCombo->findText("idle");
            m_animCombo->setCurrentIndex(idleIdx >= 0 ? idleIdx : 0);
        }
        m_updating = false;
    }
#endif
}

} // namespace rt
