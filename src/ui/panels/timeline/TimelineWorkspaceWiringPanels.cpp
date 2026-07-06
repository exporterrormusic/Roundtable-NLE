// TimelineWorkspaceWiringPanels.cpp
// Properties / Effect Controls / Graphics / Color panel feedback wiring.
// Extracted from TimelineWorkspaceWiringClipSelection.cpp for file size.

#include <volk.h>

#include <cmath>
#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

// ShotPanel removed � character/shot controls merged into PropertiesPanel
#include "panels/effects/EffectsPanel.h"
#include "panels/effects/KeyframeEditor.h"
#include "panels/monitors/ProgramMonitor.h"
#include "panels/project/ProjectBin.h"
#include "panels/properties/PropertiesPanel.h"
#include "panels/effects/EffectControlsPanel.h"
#include "panels/effects/GraphicsEditorPanel.h"
#include "panels/effects/ColorGradingPanel.h"
#include "panels/captions/CaptionsPanel.h"
#include "panels/tierlist/TierListPanel.h"
#include "panels/monitors/SourceMonitor.h"
#include "panels/timeline/TimelinePanel.h"

#include "widgets/MiniTimeline.h"
#include "widgets/DockTitleBar.h"
#include "widgets/VUMeter.h"
#include "viewport/Viewport.h"
#include "viewport/TransformOverlayWidget.h"
#include "panels/timeline/OverlayController.h"

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
#include "audio/AudioEngine.h"
#include "playback/MediaPool.h"
#include "playback/PlaybackController.h"
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
#include "cache/FrameCache.h"
#include "audio/AudioPlaybackService.h"

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

void TimelineWorkspace::wirePanelFeedbackSignals()
{
    // Refresh Program Monitor when clip properties change
    if (m_propertiesPanel && m_programMonitor) {
        connect(m_propertiesPanel, &PropertiesPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
#ifdef ROUNDTABLE_HAS_SPINE
            // Refresh timeline track widgets so clip label changes are visible
            if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
            // Spine character/outfit changes may require reloading the engine
            if (m_compositeService && m_selection.clip && m_selection.clip->clipType() == ClipType::Spine) {
                auto* spineClip = dynamic_cast<SpineClip*>(m_selection.clip);
                if (spineClip) {
                    const uint64_t clipId = spineClip->id();
                    m_compositeService->evictSpineState(clipId);

                    const std::string key = m_compositeService->spineCharKey(*spineClip);
                    auto shared = m_compositeService->findSpineSharedData(key);
                    if (shared && !shared->skelBytes.empty()) {
                        m_compositeService->getOrCreateSpineState(spineClip);
                    } else {
                        std::string assetsDir = "assets";
                        if (m_modelManager) assetsDir = m_modelManager->assetsDir();
                        scheduleSpineSharedLoad(
                            spineClip->characterName(), spineClip->outfit(),
                            static_cast<int>(spineClip->stance()), assetsDir);
                    }
                }
            }
#endif
            // Audio-FX (EQ/dynamics) edits change the PROCESSED window
            // samples, which are baked into the source buffers at load time
            // (AudioPlaybackServiceLoad) — rebuild the audio sources so the
            // preview reflects the new chain immediately instead of after
            // the next natural window swap.  Non-blocking variant: same
            // call the playback-refresh path uses mid-play.
            if (m_audioPlayback && m_selection.clip &&
                m_selection.clip->isAudio()) {
                m_audioPlayback->invalidateSources();
                m_audioPlayback->loadSources(/*allowBlockingMisses=*/false);
            }
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        connect(m_propertiesPanel, &PropertiesPanel::shotSwitchRequested,
                this, [this](uint64_t groupId, const std::string& newShotName) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            applyShotSwitch(groupId, newShotName);
        });
        // An audio-stream change re-decodes that clip's waveform so the drawn
        // shape follows the audible stream. (propertyChanged above already
        // rebuilt the audio sources; it must NOT also re-decode the waveform
        // on every volume/pan nudge, hence this dedicated signal.)
        connect(m_propertiesPanel, &PropertiesPanel::audioStreamChanged,
                this, [this](quint64 clipId) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_timelinePanel) m_timelinePanel->invalidateClipWaveform(clipId);
        });
    }

    // Refresh Program Monitor when Effect Controls change
    if (m_effectControlsPanel && m_programMonitor) {
        connect(m_effectControlsPanel, &EffectControlsPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            scheduleOverlayRefresh();
        });
        connect(m_effectControlsPanel, &EffectControlsPanel::maskChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            scheduleOverlayRefresh();
        });
        connect(m_effectControlsPanel, &EffectControlsPanel::maskSelected,
                this, [this](int maskIndex, quint64 effectId) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Route the Program Monitor at the selected mask list first
            // (clip opacity masks when effectId==0, else that effect's
            // masks), then focus the clicked mask within it.
            if (m_overlay)
                m_overlay->setActiveMaskContext(effectId);
            if (m_programMonitor) {
                auto* ov = m_programMonitor->transformOverlay();
                if (ov) ov->setActiveMaskIndex(maskIndex);
            }
        });
    }

    // Push live volume/pan changes from Effect Controls straight to the
    // AudioEngine mixer so playback reflects edits without rebuilding the
    // source list (which would glitch during scrubbing).
    if (m_effectControlsPanel) {
        connect(m_effectControlsPanel, &EffectControlsPanel::audioLevelsChanged,
                this, [this](uint64_t clipId, float vol, float pan) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            // Resolve track mute state so the mixer sees the effective
            // muted flag (otherwise updateSourceLevels would re-enable
            // a muted clip).
            bool muted = false;
            for (size_t ti = 0; ti < m_timeline->trackCount(); ++ti) {
                auto* tr = m_timeline->track(ti);
                if (!tr) continue;
                for (size_t ci = 0; ci < tr->clipCount(); ++ci) {
                    if (tr->clip(ci) && tr->clip(ci)->id() == clipId) {
                        muted = tr->isMuted();
                        break;
                    }
                }
            }
            if (m_audioPlayback)
                m_audioPlayback->updateClipLevels(clipId, vol, pan, muted);
        });
    }

    // Re-sync transform overlay whenever a new frame is displayed.
    // The overlay must track the current tick (properties may be keyframed)
    // and the current compositor resolution (scrub vs settled).
    if (m_programMonitor) {
        connect(m_programMonitor, &ProgramMonitor::frameDisplayed,
                this, [this](int64_t) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            // Only the focused-layer case re-measures per displayed frame (it
            // already did pre-change). The whole-clip text box is sized by
            // updateTransformOverlay on selection/property/commit — it does NOT
            // need a per-frame full-res QImage re-measure during playback, which
            // would be a needless allocation every frame.
            if (m_selection.clip && m_selection.graphicLayerIdx >= 0)
                updateTransformOverlay();
        });
    }

    // Effect Controls seek (mini-timeline scrub and keyframe navigation).
    // Mirrors the main timeline-ruler scrub path so the Program Monitor
    // actually renders the new frame instead of staying on the old one.
    if (m_effectControlsPanel && m_playbackController) {
        connect(m_effectControlsPanel, &EffectControlsPanel::seekRequested,
                this, [this](int64_t tick) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_playbackController->isPlaying())
                m_playbackController->pause();
            m_playbackController->seekTo(tick);
            ensureAudioSourcesLoaded();
            if (m_audioEngine && !m_playbackController->isPlaying()) {
                const uint32_t sr = m_audioEngine->sampleRate();
                const int64_t frame = static_cast<int64_t>(
                    static_cast<double>(tick) / 48000.0 * sr);
                m_audioEngine->scrub(frame);
            }
            // Force the Program Monitor to repaint with the frame at the new
            // playhead — without this it stays on whatever was last rendered.
            if (m_programMonitor) {
                m_programMonitor->notifyScrub();
                m_programMonitor->requestRefresh();
            }
        });
    }

    // -- Eyedropper: switch to pick mode when UI requests it -------------
    if (m_effectControlsPanel && m_programMonitor) {
        auto* ov = m_programMonitor->transformOverlay();
        if (ov) {
            connect(m_effectControlsPanel, &EffectControlsPanel::eyedropperRequested,
                    this, [this, ov](size_t effectIdx) {
                m_eyedropperEffectIdx = effectIdx;
                m_savedEditToolBeforeEyedropper = static_cast<uint8_t>(
                    m_timelinePanel ? m_timelinePanel->activeTool() : EditTool::Selection);
                ov->setEditTool(8); // Eyedropper
            });

            connect(ov, &TransformOverlayWidget::colorPicked,
                    this, [this, ov](float frameX, float frameY) {
                // Restore previous tool
                ov->setEditTool(m_savedEditToolBeforeEyedropper);

                auto frame = m_programMonitor->lastDisplayedFrame();
                if (!frame || !frame->ensurePixels() || frame->pixels.empty())
                    return;

                int px = std::clamp(static_cast<int>(frameX), 0,
                                    static_cast<int>(frame->width) - 1);
                int py = std::clamp(static_cast<int>(frameY), 0,
                                    static_cast<int>(frame->height) - 1);
                const uint8_t* p = frame->pixels.data()
                                   + py * frame->stride + px * 4;
                float r = p[2] / 255.0f;  // BGRA ? R at offset 2
                float g = p[1] / 255.0f;
                float b = p[0] / 255.0f;

                // Write into the Ultra Key effect's KeyColor params
                if (m_selection.clip) {
                    auto& st = m_selection.clip->effects();
                    if (m_eyedropperEffectIdx < st.effectCount()) {
                        auto& fx = st.effect(m_eyedropperEffectIdx);
                        int64_t t = m_playbackController
                            ? m_playbackController->currentTick() - m_selection.clip->timelineIn()
                            : 0;
                        fx.param(ChromaKey::KeyColorR).track.writeValue(t, r);
                        fx.param(ChromaKey::KeyColorG).track.writeValue(t, g);
                        fx.param(ChromaKey::KeyColorB).track.writeValue(t, b);
                        invalidateCompositeCache();
                        m_programMonitor->requestRefresh();
                        m_effectControlsPanel->refresh();
                    }
                }
            });
        }
    }

    // Refresh Program Monitor when Essential Graphics change
    if (m_GraphicsEditorPanel && m_programMonitor) {
        connect(m_GraphicsEditorPanel, &GraphicsEditorPanel::propertyChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            scheduleOverlayRefresh();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        // Track which graphic layer is selected for per-layer transform overlay
        connect(m_GraphicsEditorPanel, &GraphicsEditorPanel::layerSelected,
                this, [this](GraphicLayer* layer, int layerIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            m_selection.graphicLayerIdx = layerIdx;
            // Premiere-style per-layer Motion: route the Effect Controls
            // panel's Position/Scale/Rotation/Opacity rows through the
            // selected layer's transform. Passing nullptr falls back to
            // the clip's transform (default for non-graphic clips).
            if (m_effectControlsPanel)
                m_effectControlsPanel->setSelectedGraphicLayer(layer);
            scheduleOverlayRefresh();
        });
        // Track the full multi-selection set from the Essential Graphics
        // layer list so a single body drag in the program monitor applies
        // the same delta to every selected layer.
        connect(m_GraphicsEditorPanel,
                &GraphicsEditorPanel::layerSelectionSetChanged,
                this, [this](const std::vector<int>& stackIdxs) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            m_selection.graphicLayerIdxs = stackIdxs;
            // Repopulate the overlay so the sibling outline boxes
            // (drawn for non-focused multi-selected layers) appear /
            // disappear in sync with the selection set.
            scheduleOverlayRefresh();
        });
        // (Double-clicking a layer row in Essential Graphics now focuses
        // the in-panel text box directly — handled inside the panel — so
        // it no longer swaps to the Properties panel.)
    }

    // -- Wire ColorGradingPanel property changes to refresh the monitor ----
    if (m_ColorGradingPanel && m_programMonitor) {
        connect(m_ColorGradingPanel, &ColorGradingPanel::propertyChanged,
                this, [this]() {
            invalidateCompositeCache();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // -- Wire CaptionsPanel edits to refresh timeline + monitor ------------
    if (m_captionsPanel) {
        connect(m_captionsPanel, &CaptionsPanel::captionEdited,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_timelinePanel) {
                // A new/removed caption track changes the track count and
                // needs a full rebuild to add/remove its row; clip-only edits
                // just repaint.
                if (m_timeline &&
                    m_timelinePanel->laidOutTrackCount() != m_timeline->trackCount())
                    m_timelinePanel->rebuildTracks();
                else
                    m_timelinePanel->refreshTrackContents();
            }
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
        // Jump the playhead to a caption when selected in the list.
        connect(m_captionsPanel, &CaptionsPanel::captionSelected,
                this, [this](int64_t timelineIn) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (m_playbackController) {
                if (m_playbackController->isPlaying())
                    m_playbackController->pause();
                m_playbackController->seekTo(timelineIn);
            } else if (m_timeline) {
                m_timeline->setPlayheadPosition(timelineIn);
            }
            if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }

    // -- Wire TierListPanel edits to refresh timeline + monitor -----------
    if (m_tierListPanel) {
        connect(m_tierListPanel, &TierListPanel::tierListEdited,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            invalidateCompositeCache();
            if (m_timelinePanel) m_timelinePanel->refreshTrackContents();
            if (m_programMonitor) m_programMonitor->requestRefresh();
        });
    }
}

} // namespace rt
