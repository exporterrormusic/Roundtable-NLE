// TimelineWorkspaceWiringClipSelection.cpp - Clip-selection signal wiring.
// Split from TimelineWorkspacePanels.cpp for maintainability.

#include <volk.h>

#include <cmath>
#include <map>
#include <set>

#include "panels/timeline/TimelineWorkspace.h"
#include "panels/timeline/ClipBindingController.h"
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
#include "panels/captions/CaptionsPanel.h"
#include "panels/tierlist/TierListPanel.h"
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

void TimelineWorkspace::wireClipSelectionSignals() {
    //  CLIP SELECTION -> EFFECT CONTROLS / PROPERTIES PANEL
    // =====================================================================
    if (m_timelinePanel) {
        connect(m_timelinePanel, &TimelinePanel::clipSelected,
                this, [this](size_t trackIdx, size_t clipIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            // NOTE: Do NOT early-return when selection count > 1.
            // Linked clips (groupId) cause multiple clips to be selected,
            // but we still want to populate panels with the clicked clip.
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;
            m_clipBinding->selectClip(track->clip(clipIdx), track, trackIdx, clipIdx,
                                      ClipBindingController::Bind::OpenDocks);
        });
        connect(m_timelinePanel, &TimelinePanel::clipDoubleClicked,
                this, [this](size_t trackIdx, size_t clipIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline || !m_sourceMonitor || !m_mediaPool) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track) return;
            auto* clip = track->clip(clipIdx);
            if (!clip) return;

            // CaptionClip: focus the Captions panel and select this cue.
            if (clip->isCaption()) {
                if (m_captionsPanel) {
                    if (auto* dock = qobject_cast<QDockWidget*>(
                            m_captionsPanel->parentWidget())) {
                        dock->show();
                        dock->raise();
                    }
                    m_captionsPanel->selectCaption(trackIdx, clipIdx);
                }
                return;
            }

            // SequenceClip: open the nested sequence
            if (clip->clipType() == ClipType::Sequence) {
                auto* seqClip = static_cast<SequenceClip*>(clip);
                if (m_project && seqClip->sequenceIndex() < m_project->sequenceCount()) {
                    // Premiere-style: if the playhead sits over this clip, open
                    // the inner sequence at the matching position. Map the
                    // parent-local offset through the clip's source-in + speed
                    // into the nested sequence's own timebase.
                    int64_t seekTick = -1;
                    const int64_t ph = m_timeline->playheadPosition();
                    if (ph >= seqClip->timelineIn() && ph < seqClip->timelineOut()) {
                        const int64_t localOffset = ph - seqClip->timelineIn();
                        seekTick = seqClip->sourceIn() +
                                   static_cast<int64_t>(std::llround(
                                       localOffset * seqClip->speed()));
                        if (seekTick < 0) seekTick = 0;
                    }
                    auto* mw = qobject_cast<MainWindow*>(window());
                    if (mw) mw->switchSequence(seqClip->sequenceIndex(), seekTick);
                }
                return;
            }

            // SpineClip: try to open the pre-rendered video from AnimationVideoCache.
            // If no cached video exists, fall back to live Spine rendering in the Source Monitor.
            if (clip->clipType() == ClipType::Spine) {
                auto* spineClip = static_cast<SpineClip*>(clip);
                if (m_compositeService) {
                    if (m_compositeService->animVideoCache()) {
                        uint64_t handle = m_compositeService->animVideoCache()->getMediaHandle(
                            spineClip->characterName(),
                            spineClip->outfit(),
                            spineClip->animationName());
                        if (handle != 0) {
                            m_sourceMonitor->loadClip(handle, m_mediaPool);
                            return;
                        }
                    }
                    // No cached video — render live from the Spine skeleton
                    m_sourceMonitor->loadSpineClip(spineClip, m_compositeService.get());
                }
                return;
            }

            std::string mediaPath;
            if (clip->clipType() == ClipType::Video) {
                mediaPath = static_cast<VideoClip*>(clip)->mediaPath();
            } else if (clip->isAudio()) {
                mediaPath = static_cast<AudioClip*>(clip)->mediaPath();
            } else if (clip->clipType() == ClipType::Image) {
                mediaPath = static_cast<ImageClip*>(clip)->mediaPath();
            }
            if (mediaPath.empty()) return;

            uint64_t handle = 0;
            if (m_compositeService)
                handle = m_compositeService->findMediaHandle(mediaPath);
            if (handle == 0) {
                handle = m_mediaPool->open(mediaPath);
                if (handle != 0 && m_compositeService)
                    m_compositeService->registerMediaHandle(mediaPath, handle);
            }
            if (handle != 0)
                m_sourceMonitor->loadClip(handle, m_mediaPool);
        });
        connect(m_timelinePanel, &TimelinePanel::transitionSelected,
                this, [this](size_t trackIdx, size_t transIdx) {
            if (m_destroying.load(std::memory_order_acquire)) return;
            if (!m_timeline) return;
            auto* track = m_timeline->track(trackIdx);
            if (!track || transIdx >= track->transitionCount()) return;
            m_selection.clip = nullptr;
            if (m_propertiesPanel) {
                m_propertiesPanel->setTransition(track, transIdx);
                m_clipBinding->raiseDock(QStringLiteral("Properties"));
            }
        });
        connect(m_timelinePanel, &TimelinePanel::selectionChanged,
                this, [this]() {
            if (m_destroying.load(std::memory_order_acquire)) return;
            const auto& sel = m_timelinePanel->selection();
            if (sel.empty()) {
                m_clipBinding->clearSelection();
            } else if (sel.count() == 1) {
                // Single clip selected (click, drag, marquee).
                const auto& ref = sel.clips().front();
                auto* trk = m_timeline->track(ref.trackIndex);
                if (trk) {
                    size_t idx = trk->findClipIndexById(ref.clipId);
                    if (idx < trk->clipCount())
                        m_clipBinding->selectClip(trk->clip(idx), trk, ref.trackIndex, idx,
                                                  ClipBindingController::Bind::ClipPanels);
                }
            } else if (sel.count() > 1) {
                std::vector<Clip*> clips;
                clips.reserve(sel.count());
                bool editable = true;
                for (const auto& ref : sel.clips()) {
                    if (auto* trk = m_timeline->track(ref.trackIndex)) {
                        if (trk->isLocked()) editable = false;
                        size_t idx = trk->findClipIndexById(ref.clipId);
                        if (idx < trk->clipCount())
                            clips.push_back(trk->clip(idx));
                    }
                }
                m_clipBinding->selectClips(clips, editable);
            }
        });
    }

    // Delegated wiring groups (extracted to sibling files for size):
    wireViewportTransformSignals();
    wireTransformOverlaySignals();
    wireOverlayToolSignals();
    wireTimelineContentSignals();
    wirePanelFeedbackSignals();
}

} // namespace rt
