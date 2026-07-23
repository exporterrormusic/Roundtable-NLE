// DropControllerEffectDrop.cpp - Effect/transition drop signal wiring.
// Extracted from TimelineWorkspaceWiring.cpp for maintainability.

#include <volk.h>

#include <map>
#include <set>

#include "panels/timeline/DropController.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

// ShotPanel removed — character/shot controls merged into PropertiesPanel
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
#include "command/LambdaCommand.h"
#include "command/commands/ClipCommands.h"
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
#include "audiofx/FxChain.h"
#include "audiofx/Dynamics.h"
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
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void DropController::wireEffectDropSignals()
{
    // =====================================================================
    //  EFFECT DRAG-DROP -> ADD EFFECT TO CLIP
    // =====================================================================
    if (m_ws->timelinePanel()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::effectDroppedOnClip,
                this, [this](size_t trackIdx, uint64_t clipId, int effectType) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            auto* track = m_ws->timeline()->track(trackIdx);
            if (!track) return;
            size_t clipIdx = track->findClipIndexById(clipId);
            if (clipIdx == SIZE_MAX) return;
            auto* clip = track->clip(clipIdx);
            if (!clip) return;

            auto type = static_cast<EffectType>(effectType);
            auto& stack = clip->effects();

            if (m_ws->commandStack()) {
                m_ws->commandStack()->execute(
                    std::make_unique<AddEffectCommand>(&stack, type));
            } else {
                stack.addEffect(createEffect(type));
            }

            // Select the clip and refresh Effect Controls + Program Monitor
            if (m_ws->propertiesPanel()) {
                m_ws->propertiesPanel()->setClip(clip, track);
                m_ws->propertiesPanel()->refreshEffects();
            }
            if (m_ws->effectControlsPanel()) {
                m_ws->effectControlsPanel()->setClip(clip, track);
                m_ws->effectControlsPanel()->refresh();
            }
            m_ws->selection().clip = clip;
            m_ws->selection().trackIdx = trackIdx;
            m_ws->selection().clipIdx = clipIdx;
            m_ws->selection().graphicLayerIdx = -1;

            m_ws->invalidateCompositeCache();
            if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();

            spdlog::info("Effect '{}' added to clip '{}' via drag-drop",
                         effectTypeName(type), clip->label());
        });
    }

    // =====================================================================
    //  GLITCH-PRESET DRAG-DROP -> ADD CURATED EFFECT STACK TO CLIP
    // =====================================================================
    if (m_ws->timelinePanel()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::glitchPresetDroppedOnClip,
                this, [this](size_t trackIdx, uint64_t clipId, int presetId) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            auto* track = m_ws->timeline()->track(trackIdx);
            if (!track) return;
            size_t clipIdx = track->findClipIndexById(clipId);
            if (clipIdx == SIZE_MAX) return;
            auto* clip = track->clip(clipIdx);
            if (!clip) return;

            auto preset = static_cast<GlitchPreset>(presetId);
            auto& stack = clip->effects();

            if (m_ws->commandStack()) {
                if (auto cmd = makeAddGlitchPresetCommand(&stack, preset))
                    m_ws->commandStack()->execute(std::move(cmd));
            } else {
                for (auto& fx : buildGlitchPreset(preset))
                    stack.addEffect(std::move(fx));
            }

            if (m_ws->propertiesPanel()) {
                m_ws->propertiesPanel()->setClip(clip, track);
                m_ws->propertiesPanel()->refreshEffects();
            }
            if (m_ws->effectControlsPanel()) {
                m_ws->effectControlsPanel()->setClip(clip, track);
                m_ws->effectControlsPanel()->refresh();
            }
            m_ws->selection().clip = clip;
            m_ws->selection().trackIdx = trackIdx;
            m_ws->selection().clipIdx = clipIdx;
            m_ws->selection().graphicLayerIdx = -1;

            m_ws->invalidateCompositeCache();
            if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();

            spdlog::info("Glitch preset '{}' added to clip '{}' via drag-drop",
                         glitchPresetName(preset), clip->label());
        });
    }

    // =====================================================================
    //  AUDIO-FX DRAG-DROP -> ADD EQ/DYNAMICS TO CLIP'S FxChain
    // =====================================================================
    if (m_ws->timelinePanel()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::audioFxDroppedOnClip,
                this, [this](size_t trackIdx, uint64_t clipId, int kindInt) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            auto* track = m_ws->timeline()->track(trackIdx);
            if (!track) return;
            size_t clipIdx = track->findClipIndexById(clipId);
            if (clipIdx == SIZE_MAX) return;
            auto* clip = track->clip(clipIdx);
            if (!clip || !clip->isAudio()) return;
            auto* aclip = static_cast<AudioClip*>(clip);
            const auto kind = static_cast<audiofx::ProcessorKind>(kindInt);

            // Whole-chain snapshot undo, matching AudioFxSection::commitEdit so
            // both entry points behave identically.
            auto before = std::make_shared<audiofx::FxChain>(aclip->audioFx().clone());
            auto after  = std::make_shared<audiofx::FxChain>(aclip->audioFx().clone());
            auto* proc = after->add(kind);
            if (kind == audiofx::ProcessorKind::Dynamics)
                static_cast<audiofx::Dynamics*>(proc)->loadVoicePreset();

            auto refresh = [this, trackIdx, clipId]() {
                if (m_ws->isDestroying() || !m_ws->timeline()) return;
                auto* tr = m_ws->timeline()->track(trackIdx);
                if (!tr) return;
                size_t ci = tr->findClipIndexById(clipId);
                if (ci == SIZE_MAX) return;
                auto* c = tr->clip(ci);
                if (m_ws->propertiesPanel()) m_ws->propertiesPanel()->setClip(c, tr);
                m_ws->selection().clip = c;
                m_ws->selection().trackIdx = trackIdx;
                m_ws->selection().clipIdx = ci;
                m_ws->selection().graphicLayerIdx = -1;
            };
            auto redo = [aclip, after, refresh]() { aclip->audioFx() = after->clone(); refresh(); };
            auto undo = [aclip, before, refresh]() { aclip->audioFx() = before->clone(); refresh(); };

            if (m_ws->commandStack())
                m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                    std::string("Add ") + audiofx::processorKindName(kind), redo, undo));
            else
                redo();

            spdlog::info("Audio FX '{}' added to clip '{}' via drag-drop",
                         audiofx::processorKindName(kind), clip->label());
        });
    }

    // =====================================================================
    //  TRANSITION DRAG-DROP -> ADD TRANSITION AT CLIP EDGE
    // =====================================================================
    if (m_ws->timelinePanel()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::transitionDroppedAtEdge,
                this, [this](size_t trackIdx, uint64_t leftClipId,
                             uint64_t rightClipId, int64_t editPointTick,
                             int transitionType) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            auto* track = m_ws->timeline()->track(trackIdx);
            if (!track) return;

            // Find clip indices
            size_t clipIdxA = SIZE_MAX;
            size_t clipIdxB = SIZE_MAX;
            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* c = track->clip(ci);
                if (!c) continue;
                if (c->id() == leftClipId)  clipIdxA = ci;
                if (c->id() == rightClipId) clipIdxB = ci;
            }
            // Need at least one valid clip
            if (clipIdxA == SIZE_MAX && clipIdxB == SIZE_MAX) return;

            // Use whichever index is valid as both params if one is missing
            if (clipIdxA == SIZE_MAX) clipIdxA = clipIdxB;
            if (clipIdxB == SIZE_MAX) clipIdxB = clipIdxA;

            Transition trans;
            trans.type = static_cast<TransitionType>(transitionType);
            trans.duration = kDefaultTransitionDuration;
            trans.leftClipId = leftClipId;
            trans.rightClipId = rightClipId;
            trans.editPointTick = editPointTick;

            // Dragged transitions use the same fit-to-available behavior as
            // the default transition shortcut.
            if (!EditOperations::fitTransitionToAvailableDuration(*track, trans))
                return;

            if (m_ws->commandStack()) {
                m_ws->commandStack()->execute(
                    std::make_unique<AddTransitionCommand>(track, clipIdxA, clipIdxB, trans));
            } else {
                track->addTransition(trans);
            }

            m_ws->invalidateCompositeCache();
            // Rebuild audio sources too — a cross-dissolve on an audio track
            // bakes its crossfade into the mixed source, so without this the
            // transition has no audible effect until something else (e.g. a
            // duration tweak) triggers an audio rebuild.
            m_ws->invalidateAudioSources();
            if (m_ws->timelinePanel()) m_ws->timelinePanel()->rebuildTracks();
            if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();

            spdlog::info("Transition type {} added via drag-drop at edit point {}",
                         transitionType, editPointTick);
        });
    }
}

} // namespace rt
