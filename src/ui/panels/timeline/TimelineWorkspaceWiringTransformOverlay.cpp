// TimelineWorkspaceWiringTransformOverlay.cpp
// GPU TransformOverlayWidget signal wiring for TimelineWorkspace.
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

void TimelineWorkspace::wireTransformOverlaySignals()
{
    // -- GPU TransformOverlayWidget: same signals but for the GPU viewport --
    if (m_programMonitor && m_programMonitor->transformOverlay()) {
        auto* ov = m_programMonitor->transformOverlay();
        connect(ov, &TransformOverlayWidget::transformPositionChanged,
                this, [this](float posX, float posY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));

                    // Group-move bootstrap: on the FIRST tick of a body drag
                    // with multi-selection active, snapshot every selected
                    // layer's starting (posX,posY). The focused layer's
                    // start position is the reference; each subsequent tick
                    // applies (posX - focusStart) as Δ to siblings, so they
                    // travel in lockstep without accumulating drift.
                    if (!m_groupMoveActive && m_selectedGraphicLayerIdxs.size() > 1) {
                        m_groupMoveActive = true;
                        m_groupMoveFocusStartX = layer->transform().posX.evaluate(relTick);
                        m_groupMoveFocusStartY = layer->transform().posY.evaluate(relTick);
                        m_groupMoveStart.clear();
                        m_groupMoveStart.reserve(m_selectedGraphicLayerIdxs.size());
                        for (int idx : m_selectedGraphicLayerIdxs) {
                            if (idx == m_selectedGraphicLayerIdx) continue;
                            if (idx < 0 || idx >= static_cast<int>(gc->layerCount())) continue;
                            auto* el = gc->layer(static_cast<size_t>(idx));
                            m_groupMoveStart.push_back({
                                idx,
                                el->transform().posX.evaluate(relTick),
                                el->transform().posY.evaluate(relTick)});
                        }
                    }

                    layer->transform().posX.writeValue(relTick, posX);
                    layer->transform().posY.writeValue(relTick, posY);

                    // Apply the same Δ to every other selected layer.
                    if (m_groupMoveActive) {
                        const float dx = posX - m_groupMoveFocusStartX;
                        const float dy = posY - m_groupMoveFocusStartY;
                        for (const auto& s : m_groupMoveStart) {
                            if (s.idx < 0 || s.idx >= static_cast<int>(gc->layerCount())) continue;
                            auto* el = gc->layer(static_cast<size_t>(s.idx));
                            el->transform().posX.writeValue(relTick, s.posX + dx);
                            el->transform().posY.writeValue(relTick, s.posY + dy);
                        }
                    }
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
        connect(ov, &TransformOverlayWidget::transformScaleChanged,
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
        connect(ov, &TransformOverlayWidget::transformRotationChanged,
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
        // Anchor handle drag → write to the selected graphic layer's
        // anchorX/anchorY tracks. Anchor is meaningful only for graphic
        // layers (the renderer pivots rotation/scale around it); the
        // overlay only fires this signal in content-rect mode anyway.
        connect(ov, &TransformOverlayWidget::transformAnchorChanged,
                this, [this](float ax, float ay) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip) return;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - m_selectedClip->timelineIn())
                : 0;
            bool wroteLayer = false;
            if (m_selectedClip->clipType() == ClipType::Graphic && m_selectedGraphicLayerIdx >= 0) {
                auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                if (m_selectedGraphicLayerIdx < static_cast<int>(gc->layerCount())) {
                    auto* layer = gc->layer(static_cast<size_t>(m_selectedGraphicLayerIdx));
                    layer->transform().anchorX.writeValue(relTick, ax);
                    layer->transform().anchorY.writeValue(relTick, ay);
                    wroteLayer = true;
                }
            }
            if (!wroteLayer) {
                // Clip-level anchor (video / image / spine / etc.).
                m_selectedClip->anchorX().writeValue(relTick, ax);
                m_selectedClip->anchorY().writeValue(relTick, ay);
            }
            if (m_effectControlsPanel) m_effectControlsPanel->syncValuesFromClip();
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->refresh();
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        // Anchor drag finished → push an undo command. The per-move signal
        // already wrote the new value during the drag; here we capture the
        // pre-drag anchor and bind both old/new into a LambdaCommand so
        // Ctrl+Z reverts the whole drag in one step.
        connect(ov, &TransformOverlayWidget::transformAnchorDragFinished,
                this, [this](float oldX, float oldY, float newX, float newY) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip || !m_commandStack) return;
            if (std::abs(oldX - newX) < 1e-4f && std::abs(oldY - newY) < 1e-4f) return;
            Clip* clip = m_selectedClip;
            // For graphic layers, undo writes to the LAYER's anchor; for
            // everything else, to the clip's anchor. Resolve via clipType
            // and the current layer-idx, captured by value so a later
            // selection change doesn't redirect the undo.
            const bool toLayer = clip->clipType() == ClipType::Graphic
                              && m_selectedGraphicLayerIdx >= 0
                              && m_selectedGraphicLayerIdx < static_cast<int>(
                                     static_cast<GraphicClip*>(clip)->layerCount());
            const int  layerIdx = m_selectedGraphicLayerIdx;
            const int64_t relTick = m_playbackController
                ? std::max<int64_t>(0, m_playbackController->currentTick() - clip->timelineIn())
                : 0;
            auto apply = [clip, toLayer, layerIdx, relTick](float ax, float ay) {
                if (toLayer) {
                    auto* gc = static_cast<GraphicClip*>(clip);
                    if (layerIdx < static_cast<int>(gc->layerCount())) {
                        auto* layer = gc->layer(static_cast<size_t>(layerIdx));
                        layer->transform().anchorX.writeValue(relTick, ax);
                        layer->transform().anchorY.writeValue(relTick, ay);
                    }
                } else {
                    clip->anchorX().writeValue(relTick, ax);
                    clip->anchorY().writeValue(relTick, ay);
                }
            };
            auto* panel = this;
            m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
                "Anchor Point",
                [apply, newX, newY, panel]() {
                    apply(newX, newY);
                    if (panel->m_effectControlsPanel) panel->m_effectControlsPanel->syncValuesFromClip();
                    if (panel->m_GraphicsEditorPanel) panel->m_GraphicsEditorPanel->refresh();
                    panel->invalidateCompositeCache();
                    panel->scheduleOverlayRefresh();
                    if (panel->m_programMonitor) panel->m_programMonitor->requestRefresh();
                },
                [apply, oldX, oldY, panel]() {
                    apply(oldX, oldY);
                    if (panel->m_effectControlsPanel) panel->m_effectControlsPanel->syncValuesFromClip();
                    if (panel->m_GraphicsEditorPanel) panel->m_GraphicsEditorPanel->refresh();
                    panel->invalidateCompositeCache();
                    panel->scheduleOverlayRefresh();
                    if (panel->m_programMonitor) panel->m_programMonitor->requestRefresh();
                }));
        });

        connect(ov, &TransformOverlayWidget::transformDragFinished,
                this, [this](float oldPosX, float oldPosY, float oldScX, float oldScY, float oldRot,
                             float newPosX, float newPosY, float newScX, float newScY, float newRot) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Capture pre-drag static state before resetting
            bool sxWasStatic = m_scaleXWasStaticAtDragStart;
            bool syWasStatic = m_scaleYWasStaticAtDragStart;
            const bool wasScaleDrag = m_scaleDragActive;
            m_scaleDragActive = false;
            // Snapshot the group-move state BEFORE resetting it. The
            // sibling start positions and the focus-layer start position
            // are baked into the undo / redo lambdas below so a single
            // Ctrl-Z unwinds every layer that travelled in the drag.
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

        // -- Mask live update: refresh composite during drag --
        connect(ov, &TransformOverlayWidget::maskLiveUpdate,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        // -- Motion-path handle drag: refresh composite + monitor while dragging --
        connect(ov, &TransformOverlayWidget::motionPathLiveUpdate,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });

        // -- Mask drag finished: undo support for mask manipulation in Program Monitor --
        connect(ov, &TransformOverlayWidget::maskDragFinished,
                this, [this](int maskIndex, OpacityMask oldMask, OpacityMask newMask) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_selectedClip || !m_commandStack) return;
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
            if (m_effectControlsPanel) {
                auto* track = m_timeline ? m_timeline->track(m_selectedTrackIdx) : nullptr;
                m_effectControlsPanel->setClip(m_selectedClip, track);
            }
            Clip* clip = m_selectedClip;
            int mi = maskIndex;
            auto cmd = std::make_unique<LambdaCommand>(
                "Move Mask",
                [clip, mi, newMask]() {
                    if (mi >= 0 && static_cast<size_t>(mi) < clip->masks().size())
                        clip->masks()[static_cast<size_t>(mi)] = newMask;
                },
                [clip, mi, oldMask]() {
                    if (mi >= 0 && static_cast<size_t>(mi) < clip->masks().size())
                        clip->masks()[static_cast<size_t>(mi)] = oldMask;
                });
            m_commandStack->pushWithoutExecute(std::move(cmd));
        });

        // -- Click on empty area: layer selection (Selection tool) or text creation (Text tool) --
        connect(ov, &TransformOverlayWidget::emptyAreaClicked,
                this, [this](float frameX, float frameY, Qt::KeyboardModifiers mods) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timelinePanel || !m_timeline) return;
            const bool addToSelection =
                (mods & (Qt::ShiftModifier | Qt::ControlModifier)) != 0;

            // -- Selection/Text tool: hit-test layers across all GraphicClips at playhead --
            if (m_timelinePanel->activeTool() == EditTool::Selection ||
                m_timelinePanel->activeTool() == EditTool::Text)
            {
                if (!m_GraphicsEditorPanel) goto skipHitTest;

                {
                // Layer posX/posY live in PROJECT-resolution space (what
                // renderGraphicClip composites at), which can differ from
                // the monitor preview resolution. Measure/hit-test in that
                // same space so clicks select the layer where it's drawn.
                uint32_t outW = 0, outH = 0;
                graphicCanvasRes(outW, outH);

                // frameX/frameY are in viewport source (composite) resolution.
                // Scale to project resolution to match text layout space.
                float hitX = frameX;
                float hitY = frameY;
                if (m_programMonitor) {
                    uint32_t cW = m_programMonitor->compositeWidth();
                    uint32_t cH = m_programMonitor->compositeHeight();
                    if (cW > 0 && cH > 0) {
                        hitX = frameX * static_cast<float>(outW) / static_cast<float>(cW);
                        hitY = frameY * static_cast<float>(outH) / static_cast<float>(cH);
                    }
                }

                int64_t playheadTick = m_playbackController
                    ? m_playbackController->currentTick() : 0;

                // Lambda: hit-test all layers in a GraphicClip, returns layer index or -1
                // skipIdx: if >= 0, skip that layer (used for cycling)
                auto hitTestGraphicClip = [&](GraphicClip* gc, int skipIdx = -1) -> int {
                    for (int i = static_cast<int>(gc->layerCount()) - 1; i >= 0; --i) {
                        if (i == skipIdx) continue;
                        auto* layer = gc->layer(static_cast<size_t>(i));
                        if (!layer || !layer->isVisible()) continue;

                        int64_t localTick = playheadTick - gc->timelineIn();
                        const auto& xf = layer->transform();
                        float posX   = xf.posX.evaluate(localTick);
                        float posY   = xf.posY.evaluate(localTick);
                        float scaleX = xf.scaleX.evaluate(localTick);
                        float scaleY = xf.scaleY.evaluate(localTick);

                        float cL = 0, cT = 0, cR = 0, cB = 0;
                        if (layer->layerType() == GraphicLayerType::Text) {
                            auto* tl = static_cast<TextLayer*>(layer);
                            QFont font(QString::fromStdString(tl->fontFamily()),
                                       static_cast<int>(tl->fontSize()));
                            font.setWeight(static_cast<QFont::Weight>(tl->fontWeight()));
                            font.setItalic(tl->isItalic());
                            float tracking = tl->tracking().evaluate(localTick);
                            font.setLetterSpacing(QFont::AbsoluteSpacing, static_cast<qreal>(tracking));
                            QString txt = QString::fromStdString(tl->text());
                            if (tl->allCaps()) txt = txt.toUpper();
                            double bigW = static_cast<double>(outW) * 10.0;
                            double bigH = static_cast<double>(outH) * 10.0;
                            QRectF textRect(-bigW * 0.5 + static_cast<double>(outW) * 0.5,
                                            -bigH * 0.5 + static_cast<double>(outH) * 0.5,
                                            bigW, bigH);
                            int hAlign = Qt::AlignHCenter;
                            switch (tl->alignment()) {
                                case GTextAlign::Left:    hAlign = Qt::AlignLeft;    break;
                                case GTextAlign::Center:  hAlign = Qt::AlignHCenter; break;
                                case GTextAlign::Right:   hAlign = Qt::AlignRight;   break;
                                case GTextAlign::Justify: hAlign = Qt::AlignJustify; break;
                            }
                            int vAlign = Qt::AlignVCenter;
                            switch (tl->vAlignment()) {
                                case GTextVAlign::Top:    vAlign = Qt::AlignTop;     break;
                                case GTextVAlign::Middle: vAlign = Qt::AlignVCenter; break;
                                case GTextVAlign::Bottom: vAlign = Qt::AlignBottom;  break;
                            }
                            // Use QPainter::drawText to measure � matches renderer
                            QImage metricsImg(static_cast<int>(outW), static_cast<int>(outH), QImage::Format_ARGB32_Premultiplied);
                            QPainter mp(&metricsImg);
                            mp.setFont(font);
                            QRectF tb;
                            mp.drawText(textRect,
                                hAlign | vAlign | Qt::TextWordWrap, txt, &tb);
                            mp.end();
                            float pad = tl->fontSize() * 0.3f;
                            cL = static_cast<float>(tb.left())   - pad;
                            cT = static_cast<float>(tb.top())    - pad;
                            cR = static_cast<float>(tb.right())  + pad;
                            cB = static_cast<float>(tb.bottom()) + pad;
                        } else {
                            auto* sl = static_cast<ShapeLayer*>(layer);
                            float sw = sl->shapeWidth();
                            float sh = sl->shapeHeight();
                            float cx = static_cast<float>(outW) * 0.5f;
                            float cy = static_cast<float>(outH) * 0.5f;
                            cL = cx - sw * 0.5f;
                            cT = cy - sh * 0.5f;
                            cR = cx + sw * 0.5f;
                            cB = cy + sh * 0.5f;
                        }

                        // Match renderer pivot transform:
                        // final = (pt - canvas_center) * scale + canvas_center + pos
                        float canvasCX = static_cast<float>(outW) * 0.5f;
                        float canvasCY = static_cast<float>(outH) * 0.5f;
                        float tL = (cL - canvasCX) * scaleX + canvasCX + posX;
                        float tR = (cR - canvasCX) * scaleX + canvasCX + posX;
                        float tT = (cT - canvasCY) * scaleY + canvasCY + posY;
                        float tB = (cB - canvasCY) * scaleY + canvasCY + posY;

                        if (hitX >= tL && hitX <= tR &&
                            hitY >= tT && hitY <= tB)
                            return i;
                    }
                    return -1;
                };

                // 1) First try the already-selected clip (lowest latency)
                if (m_selectedClip && m_selectedClip->clipType() == ClipType::Graphic) {
                    auto* gc = static_cast<GraphicClip*>(m_selectedClip);
                    int hitIdx = hitTestGraphicClip(gc);
                    if (hitIdx >= 0) {
                        if (addToSelection) {
                            // Shift/Ctrl-click on a layer in the program
                            // monitor: toggle its membership in the
                            // multi-selection so a subsequent body drag
                            // moves every selected layer in lockstep.
                            m_GraphicsEditorPanel->toggleLayerInSelection(hitIdx);
                        } else if (hitIdx != m_selectedGraphicLayerIdx) {
                            m_GraphicsEditorPanel->selectLayerByStackIndex(hitIdx);
                        } else {
                            // Already-selected layer was hit cycle to next
                            // layer underneath by skipping the current one
                            int cycleIdx = hitTestGraphicClip(gc, hitIdx);
                            if (cycleIdx >= 0)
                                m_GraphicsEditorPanel->selectLayerByStackIndex(cycleIdx);
                        }
                        return;
                    }
                }

                // 2) Search all other GraphicClips at the playhead (top tracks first)
                for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                    auto* trk = m_timeline->track(ti);
                    if (!trk || trk->type() != TrackType::Video) continue;
                    for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                        auto* clip = trk->clip(ci);
                        if (!clip || clip->clipType() != ClipType::Graphic) continue;
                        if (clip == m_selectedClip) continue;
                        if (playheadTick < clip->timelineIn() || playheadTick >= clip->timelineOut()) continue;

                        auto* gc = static_cast<GraphicClip*>(clip);
                        int hitIdx = hitTestGraphicClip(gc);
                        if (hitIdx >= 0) {
                            // Switch to this clip and select the hit layer
                            m_selectedClip = clip;
                            m_selectedTrackIdx = ti;
                            m_selectedClipIdx = ci;
                            m_selectedGraphicLayerIdx = -1;
                            if (m_effectControlsPanel) m_effectControlsPanel->setClip(clip, trk);
                            if (m_GraphicsEditorPanel) {
                                m_GraphicsEditorPanel->setClip(clip, trk);
                                m_GraphicsEditorPanel->selectLayerByStackIndex(hitIdx);
                            }
                            if (m_ColorGradingPanel) m_ColorGradingPanel->setClip(clip, trk);
                            if (m_propertiesPanel) m_propertiesPanel->setClip(clip, trk);
                            if (m_timelinePanel) m_timelinePanel->selection().selectClip(ClipRef{ti, clip->id()}, false);
                            scheduleOverlayRefresh();
                            return;
                        }
                    }
                }
                } // end scope for outW/outH/playheadTick
            }
            skipHitTest:
            if (m_timelinePanel->activeTool() != EditTool::Text) return;

            // Re-entrancy guard: if the overlay click signal fires twice
            // (seen on first interaction after project open), only process
            // the first one to avoid duplicate clip creation.
            if (m_textToolBusy.exchange(true, std::memory_order_acquire)) return;
            // Ensure the guard is cleared when this scope exits, including
            // early returns below.
            struct BusyGuard { std::atomic<bool>& flag; ~BusyGuard() { flag.store(false, std::memory_order_release); } };
            BusyGuard guard{m_textToolBusy};

            // Find the topmost video track.
            Track* targetTrack = nullptr;
            size_t targetTrackIdx = 0;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* trk = m_timeline->track(ti);
                if (trk && trk->type() == TrackType::Video) {
                    targetTrack = trk;
                    targetTrackIdx = ti;
                    break;
                }
            }

            // Get playhead position
            int64_t tick = 0;
            if (m_playbackController)
                tick = m_playbackController->currentTick();

            // Check if the topmost track is occupied at this position.
            bool topmostOccupied = false;
            if (targetTrack) {
                for (size_t ci = 0; ci < targetTrack->clipCount(); ++ci) {
                    const Clip* c = targetTrack->clip(ci);
                    if (tick >= c->timelineIn() && tick < c->timelineOut()) {
                        topmostOccupied = true;
                        break;
                    }
                }
            }

            // If no video track exists yet or the topmost one is occupied,
            // create a new video track so the text click always succeeds
            // (Premiere Pro behaviour).  The new track goes straight to
            // index 0 so text layers are always the topmost element.
            std::unique_ptr<CompoundCommand> textCompound;
            bool createdNewTrack = false;
            if (!targetTrack || topmostOccupied) {
                // Build a name that matches addVideoTrack() numbering.
                int videoCount = 0;
                for (size_t ti2 = 0; ti2 < m_timeline->trackCount(); ++ti2) {
                    auto* t = m_timeline->track(ti2);
                    if (t && t->type() == TrackType::Video && !t->isDivider()
                        && !t->isCaptionTrack())
                        ++videoCount;
                }
                std::string trackName = "V" + std::to_string(videoCount + 1);

                auto newTrack = std::make_unique<Track>(TrackType::Video, trackName);
                targetTrack = newTrack.get();
                m_timeline->insertTrack(0, std::move(newTrack));
                targetTrackIdx = 0;
                createdNewTrack = true;

                if (m_commandStack) {
                    textCompound = std::make_unique<CompoundCommand>("Add Text Layer");
                    // Undo: remove the track we just inserted at index 0.
                    Timeline* tl = m_timeline;
                    textCompound->addExecuted(std::make_unique<LambdaCommand>(
                        "Add Track",
                        [](){},  // already executed above
                        [tl](){
                            if (tl && tl->trackCount() > 0)
                                tl->removeTrack(0);
                        }));
                }

                if (!targetTrack) return;

                spdlog::info("[TextTool] created new top track '{}'", trackName);
            }

            // Create a GraphicClip with a TextLayer.  Duration is 5 seconds
            // by default, but capped so the clip never spills into the next
            // clip on the same track (both clips occupying the same time
            // range causes compositing errors).
            int64_t duration = kTicksPerSecond * 5;
            for (size_t ci = 0; ci < targetTrack->clipCount(); ++ci) {
                const Clip* c = targetTrack->clip(ci);
                if (c->timelineIn() > tick) {
                    int64_t gap = c->timelineIn() - tick;
                    if (gap < duration) duration = gap;
                    break; // clips are ordered, first after tick is the nearest
                }
            }
            auto gc = std::make_unique<GraphicClip>();
            gc->setTimelineIn(tick);
            gc->setDuration(duration);
            gc->setSourceIn(0);
            gc->setLabel("Text");

            // Position the text layer at the click position.
            //
            // frameX/frameY arrive in composite (preview) px — the click
            // handler scales by m_vulkanVp->srcWidth(), which tracks
            // ProgramMonitor::compositeWidth(). A GraphicClip layer's
            // posX/posY are raw offsets from canvas center in
            // PROJECT-resolution px: renderGraphicClip() adds them
            // directly to renderW/2 where renderW == the project/sequence
            // resolution (it renders full-res then downscales). The
            // preview/output resolution can be lower (e.g. 1920 preview of
            // a 4K project), so converting the click via outputWidth made
            // the text/box drift by a factor of projectRes/previewRes,
            // worsening with distance from center. Use the same project
            // resolution the compositor does.
            auto* tl = gc->addTextLayer("Text");
            uint32_t compW = m_programMonitor->compositeWidth();
            uint32_t compH = m_programMonitor->compositeHeight();
            uint32_t canvasW = 0, canvasH = 0;
            graphicCanvasRes(canvasW, canvasH);
            // Scale the default font size to project resolution. The TextLayer
            // default (72 pt) is sized for a 1080-tall canvas; renderGraphicClip
            // rasterises into the project canvas without rescaling by resolution,
            // so 72 pt in a 2160 canvas covers half the relative height of 1080,
            // making the default look tiny on 4K projects. Anchor at 1080.
            if (canvasH > 0) {
                const float scaledFontSize =
                    tl->fontSize() * (static_cast<float>(canvasH) / 1080.0f);
                tl->setFontSize(scaledFontSize);
            }
            if (compW == 0) compW = canvasW;
            if (compH == 0) compH = canvasH;
            const float nx = static_cast<float>(frameX) / static_cast<float>(compW);
            const float ny = static_cast<float>(frameY) / static_cast<float>(compH);
            const float newPosX = (nx - 0.5f) * static_cast<float>(canvasW);
            const float newPosY = (ny - 0.5f) * static_cast<float>(canvasH);
            tl->transform().posX = KeyframeTrack<float>(newPosX);
            tl->transform().posY = KeyframeTrack<float>(newPosY);

            spdlog::info("[TextTool] click frame=({:.1f},{:.1f}) compW={} compH={} "
                         "canvas={}x{} -> posX={:.1f} posY={:.1f}",
                         frameX, frameY, compW, compH, canvasW, canvasH,
                         newPosX, newPosY);

            // Route through the command stack so Ctrl+Z undoes the new
            // text layer (previously addClip() was called directly, leaving
            // nothing on the undo stack).
            const uint64_t newClipId = gc->id();
            if (textCompound) {
                auto clipCmd = std::make_unique<AddClipCommand>(targetTrack, std::move(gc));
                clipCmd->execute();
                textCompound->addExecuted(std::move(clipCmd));
                m_commandStack->pushWithoutExecute(std::move(textCompound));
            } else if (m_commandStack) {
                m_commandStack->execute(
                    std::make_unique<AddClipCommand>(targetTrack, std::move(gc)));
            } else {
                targetTrack->addClip(std::move(gc));
            }

            // Re-fetch the clip from the track — AddClipCommand owns the
            // unique_ptr until execute(), so the raw pointer must come from
            // the track to stay valid across undo/redo.
            size_t clipIdx = targetTrack->findClipIndexById(newClipId);
            if (clipIdx >= targetTrack->clipCount()) return;
            Clip* ptr = targetTrack->clip(clipIdx);
            if (!ptr) return;
            m_selectedClip = ptr;
            m_selectedTrackIdx = targetTrackIdx;
            m_selectedClipIdx = clipIdx;
            m_selectedGraphicLayerIdx = -1;

            // Refresh everything — rebuildTracks if the track set changed,
            // otherwise just refresh the contents of existing widgets.
            if (m_timelinePanel) {
                if (createdNewTrack)
                    m_timelinePanel->rebuildTracks();
                else
                    m_timelinePanel->refreshTrackContents();
            }
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();

            // Select in panels â€” setClip triggers layerSelected which sets
            // m_selectedGraphicLayerIdx and calls updateTransformOverlay().
            // We must NOT call updateTransformOverlay() before setClip,
            // otherwise it runs with layerIdx == -1 â†’ full-canvas overlay.
            if (m_effectControlsPanel) m_effectControlsPanel->setClip(ptr, targetTrack);
            if (m_GraphicsEditorPanel) m_GraphicsEditorPanel->setClip(ptr, targetTrack);
            if (m_ColorGradingPanel) m_ColorGradingPanel->setClip(ptr, targetTrack);
            if (m_propertiesPanel) m_propertiesPanel->setClip(ptr, targetTrack);
            scheduleOverlayRefresh();

            // Raise Essential Graphics for the new graphic clip
            if (auto* dock = dockForPanel(QStringLiteral("Graphics Editor"))) {
                dock->setVisible(true);
                dock->raise();
            }

            spdlog::info("Text tool: created GraphicClip at frame ({}, {}), track {}", frameX, frameY, targetTrackIdx);
        });
    }
}

} // namespace rt
