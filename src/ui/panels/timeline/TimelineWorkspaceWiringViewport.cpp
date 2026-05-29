// TimelineWorkspaceWiringViewport.cpp
// Software-viewport transform, overlay tool-change, and program-monitor
// content-refresh signal wiring for TimelineWorkspace.
// Extracted from TimelineWorkspacePanelsWiringClipSelection.cpp for file size.

#include <volk.h>

#include <cmath>
#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/ClipRenderers.h"
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

#include "panels/audio/AudioMixer.h"
// ShotPanel removed � character/shot controls merged into PropertiesPanel
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

#include "widgets/MiniTimeline.h"
#include "widgets/DockTitleBar.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"

#include "command/CommandStack.h"
#include "command/CompoundCommand.h"
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TrackCommands.h"
#include "command/commands/MarkerCommands.h"
#include "command/commands/TransitionCmds.h"
#include "command/commands/EffectCommands.h"
#include "project/Project.h"
#include "MainWindow.h"
#include "media/AudioEngine.h"
#include "media/MediaPool.h"
#include "media/PlaybackController.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "timeline/ImageClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/TitleClip.h"
#include "timeline/VideoClip.h"
#include "timeline/GraphicClip.h"
#include "timeline/GraphicLayer.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Transition.h"
#include "timeline/VideoClip.h"

#include "effects/ChromaKey.h"
#include "media/FrameCache.h"
#include "media/AudioPlaybackService.h"

#include "panels/characters/ShotComposerInternal.h"
#include "spine/ShotPreset.h"

#ifdef ROUNDTABLE_HAS_SPINE
#include "spine/ModelManager.h"
#endif

#include <QDockWidget>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void TimelineWorkspace::wireViewportTransformSignals()
{
    // -- Transform overlay: connect Viewport signals to update clip props --
    if (m_programMonitor && m_programMonitor->viewport()) {
        auto* vp = m_programMonitor->viewport();
        connect(vp, &Viewport::transformPositionChanged,
                this, [this](float posX, float posY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            // Per-layer: update layer transform if a layer is selected
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().posX.writeValue(relTick, posX);
                    layer->transform().posY.writeValue(relTick, posY);
                }
            } else {
                m_selectedClip->positionX().writeValue(relTick, posX);
                m_selectedClip->positionY().writeValue(relTick, posY);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            // For text/shape layers the modified transform lives on the
            // layer, not the clip — refresh the Essential Graphics panel
            // so its posX/posY/scale/rotation spinboxes reflect the drag.
            // No-op when no graphic layer is selected (early return inside).
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformScaleChanged,
                this, [this](float scX, float scY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    if (!m_scaleDragActive) {
                        m_scaleDragActive = true;
                        m_scaleXWasStaticAtDragStart = layer->transform().scaleX.isStatic();
                        m_scaleYWasStaticAtDragStart = layer->transform().scaleY.isStatic();
                        m_scaleXAtDragStart = layer->transform().scaleX.evaluate(relTick);
                        m_scaleYAtDragStart = layer->transform().scaleY.evaluate(relTick);
                    }
                    if (!layer->transform().scaleX.isStatic() || !layer->transform().scaleY.isStatic()) {
                        layer->transform().scaleX.addKeyframe(relTick, scX);
                        layer->transform().scaleY.addKeyframe(relTick, scY);
                    } else {
                        layer->transform().scaleX.setDefaultValue(scX);
                        layer->transform().scaleY.setDefaultValue(scY);
                    }
                }
            } else {
                if (!m_scaleDragActive) {
                    m_scaleDragActive = true;
                    m_scaleXWasStaticAtDragStart = m_selectedClip->scaleX().isStatic();
                    m_scaleYWasStaticAtDragStart = m_selectedClip->scaleY().isStatic();
                    // Capture the TRUE signed scale (incl. flip) before the
                    // first drag write, so the undo command restores the
                    // flipped value rather than the overlay's clamped/stale
                    // baseline (which would silently revert a flip).
                    m_scaleXAtDragStart = m_selectedClip->scaleX().evaluate(relTick);
                    m_scaleYAtDragStart = m_selectedClip->scaleY().evaluate(relTick);
                }
                // Preserve Flip H/V (stored as the SIGN of scaleX/scaleY): the
                // viewport reports a positive magnitude from the resize handle,
                // so re-apply the clip's current sign — otherwise resizing a
                // flipped clip silently un-flips it.
                if (m_selectedClip->scaleX().evaluate(relTick) < 0.0f)
                    scX = (scX < 0.0f ? scX : -scX);
                if (m_selectedClip->scaleY().evaluate(relTick) < 0.0f)
                    scY = (scY < 0.0f ? scY : -scY);
                if (!m_selectedClip->scaleX().isStatic() || !m_selectedClip->scaleY().isStatic()) {
                    m_selectedClip->scaleX().addKeyframe(relTick, scX);
                    m_selectedClip->scaleY().addKeyframe(relTick, scY);
                } else {
                    m_selectedClip->scaleX().setDefaultValue(scX);
                    m_selectedClip->scaleY().setDefaultValue(scY);
                }
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            // For text/shape layers the modified transform lives on the
            // layer, not the clip — refresh the Essential Graphics panel
            // so its posX/posY/scale/rotation spinboxes reflect the drag.
            // No-op when no graphic layer is selected (early return inside).
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformRotationChanged,
                this, [this](float rot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().rotation.writeValue(relTick, rot);
                }
            } else {
                m_selectedClip->rotation().writeValue(relTick, rot);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            // For text/shape layers the modified transform lives on the
            // layer, not the clip — refresh the Essential Graphics panel
            // so its posX/posY/scale/rotation spinboxes reflect the drag.
            // No-op when no graphic layer is selected (early return inside).
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(vp, &Viewport::transformDragFinished,
                this, [this](float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                             float newPosX, float newPosY, float newScX, float newScY, float newRot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Capture pre-drag static state before resetting
            bool sxWasStatic = m_scaleXWasStaticAtDragStart;
            bool syWasStatic = m_scaleYWasStaticAtDragStart;
            const bool wasScaleDrag = m_scaleDragActive;
            m_scaleDragActive = false;
            // Snapshot the group-move state for sibling-undo support.
            // Software Viewport doesn't currently populate group-move
            // state (only the GPU overlay does), so this is a no-op
            // snapshot of empty data — kept for symmetry with the GPU
            // handler so the cmd-construction code below compiles in
            // both branches.
            const auto groupMoveSnapshot   = m_groupMoveStart;
            const float groupFocusStartX   = m_groupMoveFocusStartX;
            const float groupFocusStartY   = m_groupMoveFocusStartY;
            const bool  groupMoveWasActive = m_groupMoveActive;
            m_groupMoveActive = false;
            m_groupMoveStart.clear();
            updateTransformOverlay();
            if (m_selectedClip) {
                auto* track = m_timeline ? m_timeline->track(m_selectedTrackIdx) : nullptr;
                if (m_propertiesPanel) m_propertiesPanel->setClip(m_selectedClip, track);
                if (m_effectControlsPanel) m_effectControlsPanel->setClip(m_selectedClip, track);
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            }
            if (m_selectedClip && m_commandStack) {
                const int64_t relTick = m_playbackController
                    ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                    : 0;
                // Source scale old/new from the actual transform track (signed)
                // rather than the overlay-reported values. The overlay clamps
                // scale to a positive magnitude and its baseline can be stale
                // after a properties flip, so trusting it would make the scale
                // undo also revert the flip (stored as the sign of scaleX/Y).
                if (wasScaleDrag) {
                    oldScX = m_scaleXAtDragStart;
                    oldScY = m_scaleYAtDragStart;
                    if (m_selectedClip->clipType() == ClipType::Graphic
                        && m_selectedGraphicLayerIdx >= 0) {
                        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                        if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                            auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                            newScX = layer->transform().scaleX.evaluate(relTick);
                            newScY = layer->transform().scaleY.evaluate(relTick);
                        }
                    } else {
                        newScX = m_selectedClip->scaleX().evaluate(relTick);
                        newScY = m_selectedClip->scaleY().evaluate(relTick);
                    }
                }
                bool posChanged = (std::abs(oldPosX - newPosX) > 0.01f ||
                                   std::abs(oldPosY - newPosY) > 0.01f);
                bool scaleChanged = (std::abs(oldScX - newScX) > 0.001f ||
                                    std::abs(oldScY - newScY) > 0.001f);
                bool rotChanged = (std::abs(oldRot - newRot) > 0.01f);

                // Detect which tracks had KFs newly created (vs pre-existing)
                auto kfCreated = [relTick](const KeyframeTrack<float>& tk, float oldVal) -> bool {
                    if (tk.isStatic() || tk.keyframeCount() < 2) return false;
                    if (!tk.hasKeyframeAt(relTick)) return false;
                    KeyframeTrack<float> tmp(tk.defaultValue());
                    for (const auto& kf : tk.keyframes()) {
                        if (kf.time != relTick) tmp.restoreKeyframe(kf);
                    }
                    return std::abs(tmp.evaluate(relTick) - oldVal) < 0.01f;
                };

                if (posChanged || scaleChanged || rotChanged) {
                    if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                        auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                        int layerIdx = m_selectedGraphicLayerIdx;
                        if (layerIdx < static_cast<int>(gc->layerCount())) {
                            auto* layer = gc->layer(static_cast<size_t>(layerIdx));
                            bool pxC = posChanged && kfCreated(layer->transform().posX, oldPosX);
                            bool pyC = posChanged && kfCreated(layer->transform().posY, oldPosY);
                            bool sxC = scaleChanged && kfCreated(layer->transform().scaleX, oldScX);
                            bool syC = scaleChanged && kfCreated(layer->transform().scaleY, oldScY);
                            bool rtC = rotChanged && kfCreated(layer->transform().rotation, oldRot);
                            bool pxA = posChanged && !layer->transform().posX.isStatic();
                            bool pyA = posChanged && !layer->transform().posY.isStatic();
                            bool sxA = scaleChanged && !layer->transform().scaleX.isStatic();
                            bool syA = scaleChanged && !layer->transform().scaleY.isStatic();
                            bool rtA = rotChanged && !layer->transform().rotation.isStatic();
                            // Group-move undo capture: snapshot each
                            // sibling's pre / post position so the
                            // CompoundCommand below restores every layer
                            // on Ctrl-Z. Position only — scale / rotation
                            // only affect the focused layer in our drag.
                            struct SibCapture {
                                int idx;
                                float oldPosX, oldPosY;
                                float newPosX, newPosY;
                                bool  pxStatic, pyStatic;
                            };
                            std::vector<SibCapture> sibCaps;
                            if (groupMoveWasActive && posChanged) {
                                const float dxFinal = newPosX - groupFocusStartX;
                                const float dyFinal = newPosY - groupFocusStartY;
                                sibCaps.reserve(groupMoveSnapshot.size());
                                for (const auto& s : groupMoveSnapshot) {
                                    if (s.idx == layerIdx) continue;
                                    if (s.idx < 0
                                        || s.idx >= static_cast<int>(gc->layerCount()))
                                        continue;
                                    auto* sl = gc->layer(static_cast<size_t>(s.idx));
                                    SibCapture sc;
                                    sc.idx     = s.idx;
                                    sc.oldPosX = s.posX;
                                    sc.oldPosY = s.posY;
                                    sc.newPosX = s.posX + dxFinal;
                                    sc.newPosY = s.posY + dyFinal;
                                    sc.pxStatic = sl->transform().posX.isStatic();
                                    sc.pyStatic = sl->transform().posY.isStatic();
                                    sibCaps.push_back(sc);
                                }
                            }
                            auto cmd = std::make_unique<LambdaCommand>(
                                "Transform Layer",
                                [gc, layerIdx, newPosX, newPosY, newScX, newScY, newRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxA, pyA, sxA, syA, rtA]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxA) l->transform().posX.addKeyframe(relTick, newPosX);
                                        else l->transform().posX.setDefaultValue(newPosX);
                                        if (pyA) l->transform().posY.addKeyframe(relTick, newPosY);
                                        else l->transform().posY.setDefaultValue(newPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxA) l->transform().scaleX.addKeyframe(relTick, newScX);
                                        else l->transform().scaleX.setDefaultValue(newScX);
                                        if (syA) l->transform().scaleY.addKeyframe(relTick, newScY);
                                        else l->transform().scaleY.setDefaultValue(newScY);
                                    }
                                    if (rotChanged) {
                                        if (rtA) l->transform().rotation.addKeyframe(relTick, newRot);
                                        else l->transform().rotation.setDefaultValue(newRot);
                                    }
                                },
                                [gc, layerIdx, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                                 relTick, posChanged, scaleChanged, rotChanged,
                                 pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                    auto* l = gc->layer(static_cast<size_t>(layerIdx));
                                    if (posChanged) {
                                        if (pxC) l->transform().posX.removeKeyframeAtTime(relTick);
                                        else l->transform().posX.writeValue(relTick, oldPosX);
                                        if (pyC) l->transform().posY.removeKeyframeAtTime(relTick);
                                        else l->transform().posY.writeValue(relTick, oldPosY);
                                    }
                                    if (scaleChanged) {
                                        if (sxWasStatic) { l->transform().scaleX.removeKeyframeAtTime(relTick); l->transform().scaleX.setDefaultValue(oldScX); }
                                        else if (sxC) l->transform().scaleX.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleX.writeValue(relTick, oldScX);
                                        if (syWasStatic) { l->transform().scaleY.removeKeyframeAtTime(relTick); l->transform().scaleY.setDefaultValue(oldScY); }
                                        else if (syC) l->transform().scaleY.removeKeyframeAtTime(relTick);
                                        else l->transform().scaleY.writeValue(relTick, oldScY);
                                    }
                                    if (rotChanged) {
                                        if (rtC) l->transform().rotation.removeKeyframeAtTime(relTick);
                                        else l->transform().rotation.writeValue(relTick, oldRot);
                                    }
                                });
                            if (!sibCaps.empty()) {
                                // Build a sibling-move LambdaCommand and
                                // wrap both commands in a CompoundCommand
                                // so a single Ctrl-Z unwinds the entire
                                // group move. Each sibling's pre-drag
                                // static flag is captured to choose
                                // between setDefaultValue (static track)
                                // and writeValue (already-keyframed).
                                auto sibCmd = std::make_unique<LambdaCommand>(
                                    "Group Move Siblings",
                                    [gc, sibCaps, relTick]() {
                                        for (const auto& s : sibCaps) {
                                            if (s.idx < 0
                                                || s.idx >= static_cast<int>(gc->layerCount()))
                                                continue;
                                            auto* sl = gc->layer(static_cast<size_t>(s.idx));
                                            if (s.pxStatic)
                                                sl->transform().posX.setDefaultValue(s.newPosX);
                                            else
                                                sl->transform().posX.writeValue(relTick, s.newPosX);
                                            if (s.pyStatic)
                                                sl->transform().posY.setDefaultValue(s.newPosY);
                                            else
                                                sl->transform().posY.writeValue(relTick, s.newPosY);
                                        }
                                    },
                                    [gc, sibCaps, relTick]() {
                                        for (const auto& s : sibCaps) {
                                            if (s.idx < 0
                                                || s.idx >= static_cast<int>(gc->layerCount()))
                                                continue;
                                            auto* sl = gc->layer(static_cast<size_t>(s.idx));
                                            if (s.pxStatic)
                                                sl->transform().posX.setDefaultValue(s.oldPosX);
                                            else
                                                sl->transform().posX.writeValue(relTick, s.oldPosX);
                                            if (s.pyStatic)
                                                sl->transform().posY.setDefaultValue(s.oldPosY);
                                            else
                                                sl->transform().posY.writeValue(relTick, s.oldPosY);
                                        }
                                    });
                                auto compound = std::make_unique<CompoundCommand>(
                                    "Group Move");
                                compound->addCommand(std::move(cmd));
                                compound->addCommand(std::move(sibCmd));
                                m_commandStack->pushWithoutExecute(std::move(compound));
                            } else {
                                m_commandStack->pushWithoutExecute(std::move(cmd));
                            }
                        }
                    } else {
                        Clip* clip = m_selectedClip;
                        bool pxC = posChanged && kfCreated(clip->positionX(), oldPosX);
                        bool pyC = posChanged && kfCreated(clip->positionY(), oldPosY);
                        bool sxC = scaleChanged && kfCreated(clip->scaleX(), oldScX);
                        bool syC = scaleChanged && kfCreated(clip->scaleY(), oldScY);
                        bool rtC = rotChanged && kfCreated(clip->rotation(), oldRot);
                        bool pxA = posChanged && !clip->positionX().isStatic();
                        bool pyA = posChanged && !clip->positionY().isStatic();
                        bool sxA = scaleChanged && !clip->scaleX().isStatic();
                        bool syA = scaleChanged && !clip->scaleY().isStatic();
                        bool rtA = rotChanged && !clip->rotation().isStatic();
                        auto cmd = std::make_unique<LambdaCommand>(
                            "Transform Clip",
                            [clip, relTick, newPosX, newPosY, newScX, newScY, newRot,
                             posChanged, scaleChanged, rotChanged,
                             pxA, pyA, sxA, syA, rtA]() {
                                if (posChanged) {
                                    if (pxA) clip->positionX().addKeyframe(relTick, newPosX);
                                    else clip->positionX().setDefaultValue(newPosX);
                                    if (pyA) clip->positionY().addKeyframe(relTick, newPosY);
                                    else clip->positionY().setDefaultValue(newPosY);
                                }
                                if (scaleChanged) {
                                    if (sxA) clip->scaleX().addKeyframe(relTick, newScX);
                                    else clip->scaleX().setDefaultValue(newScX);
                                    if (syA) clip->scaleY().addKeyframe(relTick, newScY);
                                    else clip->scaleY().setDefaultValue(newScY);
                                }
                                if (rotChanged) {
                                    if (rtA) clip->rotation().addKeyframe(relTick, newRot);
                                    else clip->rotation().setDefaultValue(newRot);
                                }
                            },
                            [clip, relTick, oldPosX, oldPosY, oldScX, oldScY, oldRot,
                             posChanged, scaleChanged, rotChanged,
                             pxC, pyC, sxC, syC, rtC, sxWasStatic, syWasStatic]() {
                                if (posChanged) {
                                    if (pxC) clip->positionX().removeKeyframeAtTime(relTick);
                                    else clip->positionX().writeValue(relTick, oldPosX);
                                    if (pyC) clip->positionY().removeKeyframeAtTime(relTick);
                                    else clip->positionY().writeValue(relTick, oldPosY);
                                }
                                if (scaleChanged) {
                                    if (sxWasStatic) { clip->scaleX().removeKeyframeAtTime(relTick); clip->scaleX().setDefaultValue(oldScX); }
                                    else if (sxC) clip->scaleX().removeKeyframeAtTime(relTick);
                                    else clip->scaleX().writeValue(relTick, oldScX);
                                    if (syWasStatic) { clip->scaleY().removeKeyframeAtTime(relTick); clip->scaleY().setDefaultValue(oldScY); }
                                    else if (syC) clip->scaleY().removeKeyframeAtTime(relTick);
                                    else clip->scaleY().writeValue(relTick, oldScY);
                                }
                                if (rotChanged) {
                                    if (rtC) clip->rotation().removeKeyframeAtTime(relTick);
                                    else clip->rotation().writeValue(relTick, oldRot);
                                }
                            });
                        m_commandStack->pushWithoutExecute(std::move(cmd));
                    }
                }
            }
        });
    }
}

void TimelineWorkspace::wireOverlayToolSignals()
{
    // -- Forward tool changes to TransformOverlayWidget -------------------
    if (m_timelinePanel && m_programMonitor && m_programMonitor->transformOverlay()) {
        auto* ov2 = m_programMonitor->transformOverlay();
        connect(m_timelinePanel, &TimelinePanel::toolChanged,
                this, [ov2](EditTool tool) {
            ov2->setEditTool(static_cast<uint8_t>(tool));
        });

        // Double-click a text layer in the Program Monitor → drop an
        // editable text box right on the layer (Premiere Pro). The single
        // click that precedes the double-click already selected the layer
        // and bound it to the panels; the Essential Graphics panel's
        // selectedLayer() is the authoritative source for which layer
        // we're editing.
        auto currentTextLayer = [this]() -> TextLayer* {
            if (!m_GraphicsEditorPanel) return nullptr;
            GraphicLayer* gl = m_GraphicsEditorPanel->selectedLayer();
            if (!gl || gl->layerType() != GraphicLayerType::Text) return nullptr;
            return static_cast<TextLayer*>(gl);
        };

        connect(ov2, &TransformOverlayWidget::textEditRequested,
                this, [this, ov2, currentTextLayer](float, float) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            TextLayer* tl = currentTextLayer();
            if (!tl) return;

            // Pick the first fill colour for the editor text colour.
            QColor textColor(Qt::white);
            const auto& fills = tl->appearance().fills;
            if (!fills.empty())
                textColor = QColor::fromRgba(fills.front().color);

            // Snapshot the current text and update the overlay BEFORE
            // clearing the text, so computeOverlayCorners can measure the
            // content bounds from the original text (otherwise the overlay
            // may have stale/zero bounds, causing beginInlineTextEdit to
            // fall back to a huge default box).
            m_preEditOriginalText = tl->text();
            m_inlineTextEditActive = true;

            // Sync m_selectedGraphicLayerIdx to the layer we're about to
            // edit. updateTransformOverlay()'s per-layer branch is gated
            // on this index (>= 0); when it's -1 the overlay falls back
            // to a full-frame box, whose centroid is the frame center —
            // making the editor appear dead-center on the first edit of a
            // freshly-created text clip (the GraphicsEditorPanel's
            // layerSelected signal hasn't propagated yet for a brand-new
            // clip auto-selected by setClip). selectedLayer() is the
            // authoritative source, so derive the index directly.
            if (m_selectedClip &&
                m_selectedClip->clipType() == ClipType::Graphic) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                size_t idx = gc->findLayerIndex(tl->layerId());
                if (idx != SIZE_MAX)
                    m_selectedGraphicLayerIdx = static_cast<int>(idx);
            }

            // Force a synchronous overlay update with the current text bounds.
            updateTransformOverlay();

            // Now clear the text for compositing so it doesn't show behind
            // the editor box (Premiere Pro).
            tl->setText(std::string{});
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();

            // Pass the layer's font in REFERENCE units. The renderer
            // multiplies the rasterised glyphs by the layer's vertical
            // scale (painter.scale(lsx, lsy) in renderGraphicClip), so a
            // text layer scaled to 2× shows glyphs twice as tall. The
            // inline editor uses a single QFont pointSize, so bake that
            // vertical scale into the effective font size — otherwise the
            // editor renders at the un-scaled size while the surrounding
            // transform box (which already accounts for scale) is much
            // larger.
            float scaleX = 1.0f;
            float scaleY = 1.0f;
            if (m_selectedClip && m_playbackController) {
                const int64_t localTick = std::max<int64_t>(
                    0, m_playbackController->currentTick() - m_selectedClip->timelineIn());
                scaleX = tl->transform().scaleX.evaluate(localTick);
                scaleY = tl->transform().scaleY.evaluate(localTick);
                if (!std::isfinite(scaleX) || scaleX <= 0.0f) scaleX = 1.0f;
                if (!std::isfinite(scaleY) || scaleY <= 0.0f) scaleY = 1.0f;
            }
            // Translate the text layer's GTextAlign into a Qt::Alignment
            // flag so the inline editor anchors and aligns its glyphs the
            // same way the renderer does (otherwise center-aligned text
            // jumps to the left edge during edit and snaps back on commit).
            Qt::Alignment hAlignFlag = Qt::AlignHCenter;
            switch (tl->alignment()) {
            case GTextAlign::Left:    hAlignFlag = Qt::AlignLeft;    break;
            case GTextAlign::Right:   hAlignFlag = Qt::AlignRight;   break;
            case GTextAlign::Justify: hAlignFlag = Qt::AlignJustify; break;
            case GTextAlign::Center:
            default:                  hAlignFlag = Qt::AlignHCenter; break;
            }

            // fontSize × scaleY → vertical match. horizontalStretch =
            // scaleX/scaleY → horizontal match for anisotropic scaling.
            ov2->beginInlineTextEdit(
                QString::fromStdString(m_preEditOriginalText),
                QString::fromStdString(tl->fontFamily()),
                tl->fontSize() * scaleY,
                tl->fontWeight(),
                tl->isItalic(),
                textColor,
                scaleX / scaleY,
                hAlignFlag);
        });

        connect(ov2, &TransformOverlayWidget::inlineTextCommitted,
                this, [this, currentTextLayer](const QString& newText) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            TextLayer* tl = currentTextLayer();
            if (!tl) {
                m_inlineTextEditActive = false;
                return;
            }
            const std::string newVal = newText.toStdString();
            const std::string oldVal = m_preEditOriginalText;
            const bool wasActive = m_inlineTextEditActive;
            m_inlineTextEditActive = false;
            m_preEditOriginalText.clear();

            auto refresh = [this]() {
                if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
                invalidateCompositeCache();
                if (m_programMonitor) m_programMonitor->requestRefresh();
                scheduleOverlayRefresh();
            };

            // If nothing changed (e.g. cancel/commit unchanged), restore
            // the original text without making an undo entry.
            if (newVal == oldVal) {
                if (wasActive) {
                    tl->setText(oldVal);
                    refresh();
                }
                return;
            }

            // Route through the command stack so Ctrl+Z reverts to the
            // pre-edit text. The layer is currently "" (cleared on begin),
            // so execute() sets it to newVal and undo restores oldVal.
            if (m_commandStack) {
                TextLayer* target = tl;
                m_commandStack->execute(std::make_unique<LambdaCommand>(
                    "Edit Text",
                    [target, newVal, refresh]() {
                        target->setText(newVal);
                        refresh();
                    },
                    [target, oldVal, refresh]() {
                        target->setText(oldVal);
                        refresh();
                    }));
            } else {
                tl->setText(newVal);
                refresh();
            }
        });
    }
}

void TimelineWorkspace::wireTimelineContentSignals()
{
    // Refresh Program Monitor whenever timeline content changes
    if (m_timelinePanel && m_programMonitor) {
        connect(m_timelinePanel, &TimelinePanel::contentChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            // Defer spine warm-up + audio reload to next event-loop iteration
            // so razor splits don't block the UI thread.
            invalidateAudioSources();
            schedulePostEditWork();
        });

        // Also refresh when a new clip is created via tools (Text tool, etc.)
        connect(m_timelinePanel, &TimelinePanel::clipCreated,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // Refresh Program Monitor when its dock becomes visible again
    // (e.g. after switching tabs away and back). Without this the
    // viewport can show stale content from whatever was rendered at
    // that screen location while tabs were switched.
    auto* dockProgramMonitor = dockForPanel(QStringLiteral("Program Monitor"));
    if (dockProgramMonitor && m_programMonitor) {
        connect(dockProgramMonitor, &QDockWidget::visibilityChanged,
                this, [this](bool visible) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (visible && m_programMonitor) {
                // Flush composite cache so we re-render from current state
                invalidateCompositeCache();
                m_programMonitor->requestRefresh();
            }
        });
    }
}

} // namespace rt
