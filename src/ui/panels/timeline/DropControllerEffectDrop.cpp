// DropControllerEffectDrop.cpp - Effect/transition drop signal wiring.
// Extracted from TimelineWorkspaceWiring.cpp for maintainability.

#include <volk.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

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

namespace {

struct EffectDropTarget
{
    size_t trackIdx{SIZE_MAX};
    size_t clipIdx{SIZE_MAX};
    Track* track{nullptr};
    Clip* clip{nullptr};
};

// A drop broadcasts to the current multi-selection only when the actual drop
// target belongs to that selection. Dropping on an unselected clip remains a
// single-clip operation. Incompatible selected clips are skipped.
std::vector<EffectDropTarget> resolveEffectDropTargets(
    Timeline* timeline, TimelinePanel* panel,
    size_t anchorTrackIdx, uint64_t anchorClipId,
    bool audioOnly)
{
    std::vector<EffectDropTarget> result;
    if (!timeline || !panel) return result;

    const ClipRef anchor{anchorTrackIdx, anchorClipId};
    std::vector<ClipRef> refs;
    const auto& selection = panel->selection();
    if (selection.count() > 1 && selection.isSelected(anchor))
        refs = selection.clips();
    else
        refs.push_back(anchor);

    for (const auto& ref : refs) {
        auto* track = timeline->track(ref.trackIndex);
        if (!track) continue;
        const size_t clipIdx = track->findClipIndexById(ref.clipId);
        if (clipIdx == SIZE_MAX) continue;
        auto* clip = track->clip(clipIdx);
        if (!clip) continue;
        if (audioOnly ? !clip->isAudio() : !clip->isVisual()) continue;
        // Preflight the complete compatible selection. Never apply an effect
        // to only the unlocked subset of a mixed locked/unlocked selection.
        if (track->isLocked()) return {};
        result.push_back({ref.trackIndex, clipIdx, track, clip});
    }
    return result;
}

} // namespace

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
            auto type = static_cast<EffectType>(effectType);
            auto targets = resolveEffectDropTargets(
                m_ws->timeline(), m_ws->timelinePanel(),
                trackIdx, clipId, /*audioOnly=*/false);
            if (targets.empty()) return;

            if (m_ws->commandStack()) {
                auto* commands = m_ws->commandStack();
                const bool grouped = targets.size() > 1;
                if (grouped)
                    commands->beginMacro(std::string("Add ") + effectTypeName(type) +
                                         " to selected clips");
                for (auto& target : targets) {
                    commands->execute(std::make_unique<AddEffectCommand>(
                        &target.clip->effects(), type));
                }
                if (grouped) commands->endMacro();
            } else {
                for (auto& target : targets)
                    target.clip->effects().addEffect(createEffect(type));
            }

            // Keep the dropped-on clip as the primary inspector target when
            // compatible; otherwise inspect the first clip that received it.
            auto primaryIt = std::find_if(
                targets.begin(), targets.end(),
                [clipId](const EffectDropTarget& target) {
                    return target.clip && target.clip->id() == clipId;
                });
            auto& primary = primaryIt != targets.end() ? *primaryIt : targets.front();
            auto* clip = primary.clip;
            auto* track = primary.track;
            const size_t clipIdx = primary.clipIdx;
            trackIdx = primary.trackIdx;

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

            spdlog::info("Effect '{}' added to {} selected-compatible clip(s) via drag-drop",
                         effectTypeName(type), targets.size());
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
            auto preset = static_cast<GlitchPreset>(presetId);
            auto targets = resolveEffectDropTargets(
                m_ws->timeline(), m_ws->timelinePanel(),
                trackIdx, clipId, /*audioOnly=*/false);
            if (targets.empty()) return;

            if (m_ws->commandStack()) {
                auto* commands = m_ws->commandStack();
                const bool grouped = targets.size() > 1;
                if (grouped)
                    commands->beginMacro(std::string("Add ") + glitchPresetName(preset) +
                                         " to selected clips");
                for (auto& target : targets) {
                    if (auto cmd = makeAddGlitchPresetCommand(
                            &target.clip->effects(), preset)) {
                        commands->execute(std::move(cmd));
                    }
                }
                if (grouped) commands->endMacro();
            } else {
                for (auto& target : targets) {
                    for (auto& fx : buildGlitchPreset(preset))
                        target.clip->effects().addEffect(std::move(fx));
                }
            }

            auto primaryIt = std::find_if(
                targets.begin(), targets.end(),
                [clipId](const EffectDropTarget& target) {
                    return target.clip && target.clip->id() == clipId;
                });
            auto& primary = primaryIt != targets.end() ? *primaryIt : targets.front();
            auto* clip = primary.clip;
            auto* track = primary.track;
            const size_t clipIdx = primary.clipIdx;
            trackIdx = primary.trackIdx;

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

            spdlog::info("Glitch preset '{}' added to {} selected-compatible clip(s) via drag-drop",
                         glitchPresetName(preset), targets.size());
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
            const auto kind = static_cast<audiofx::ProcessorKind>(kindInt);
            auto targets = resolveEffectDropTargets(
                m_ws->timeline(), m_ws->timelinePanel(),
                trackIdx, clipId, /*audioOnly=*/true);
            if (targets.empty()) return;

            struct AudioFxChange {
                AudioClip* clip{nullptr};
                audiofx::FxChain before;
                audiofx::FxChain after;
            };
            auto changes = std::make_shared<std::vector<AudioFxChange>>();
            changes->reserve(targets.size());
            for (auto& target : targets) {
                auto* audioClip = static_cast<AudioClip*>(target.clip);
                AudioFxChange change{
                    audioClip, audioClip->audioFx().clone(), audioClip->audioFx().clone()};
                auto* proc = change.after.add(kind);
                if (kind == audiofx::ProcessorKind::Dynamics)
                    static_cast<audiofx::Dynamics*>(proc)->loadVoicePreset();
                changes->push_back(std::move(change));
            }

            auto primaryIt = std::find_if(
                targets.begin(), targets.end(),
                [clipId](const EffectDropTarget& target) {
                    return target.clip && target.clip->id() == clipId;
                });
            const auto& primary = primaryIt != targets.end() ? *primaryIt : targets.front();
            const size_t primaryTrackIdx = primary.trackIdx;
            const uint64_t primaryClipId = primary.clip->id();
            auto refresh = [this, primaryTrackIdx, primaryClipId]() {
                if (m_ws->isDestroying() || !m_ws->timeline()) return;
                auto* tr = m_ws->timeline()->track(primaryTrackIdx);
                if (!tr) return;
                size_t ci = tr->findClipIndexById(primaryClipId);
                if (ci == SIZE_MAX) return;
                auto* c = tr->clip(ci);
                if (m_ws->propertiesPanel()) m_ws->propertiesPanel()->setClip(c, tr);
                m_ws->selection().clip = c;
                m_ws->selection().trackIdx = primaryTrackIdx;
                m_ws->selection().clipIdx = ci;
                m_ws->selection().graphicLayerIdx = -1;
            };
            auto redo = [changes, refresh]() {
                for (auto& change : *changes)
                    change.clip->audioFx() = change.after.clone();
                refresh();
            };
            auto undo = [changes, refresh]() {
                for (auto& change : *changes)
                    change.clip->audioFx() = change.before.clone();
                refresh();
            };

            if (m_ws->commandStack())
                m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                    std::string("Add ") + audiofx::processorKindName(kind), redo, undo));
            else
                redo();

            spdlog::info("Audio FX '{}' added to {} selected-compatible clip(s) via drag-drop",
                         audiofx::processorKindName(kind), targets.size());
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
