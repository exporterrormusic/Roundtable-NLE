// DropControllerNest.cpp - Nest/sequence signal wiring.
// Extracted from TimelineWorkspaceWiring.cpp for maintainability.

#include <volk.h>

#include <algorithm>
#include <set>
#include <unordered_map>

#include "panels/timeline/DropController.h"
#include "panels/timeline/TimelineWorkspace.h"
#include "ClipRenderers.h"  // src/core/ClipRenderers.h — shared with the gpu module
#include "CompositeService.h"
#include "spine/AnimationVideoCache.h"
#include "Theme.h"

#include "timeline/AudioClip.h"
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
#include "timeline/NestTransitionTransfer.h"
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
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

void DropController::wireNestSignals()
{
    // =====================================================================
    //  NEST SELECTED CLIPS -> CREATE NESTED SEQUENCE
    // =====================================================================
    if (m_ws->timelinePanel() && m_ws->timeline()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::nestSelectedClips,
                this, [this](const std::vector<ClipRef>& clips, const QString& nestName) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline() || !m_ws->project() || !m_ws->commandStack() || clips.empty()) return;

            // Find the time range spanned by selected clips
            int64_t minTick = std::numeric_limits<int64_t>::max();
            int64_t maxTick = std::numeric_limits<int64_t>::min();
            size_t targetTrackIdx = SIZE_MAX;

            for (const auto& cr : clips) {
                auto* trk = m_ws->timeline()->track(cr.trackIndex);
                if (!trk) continue;
                size_t ci = trk->findClipIndexById(cr.clipId);
                if (ci >= trk->clipCount()) continue;
                auto* c = trk->clip(ci);
                if (!c) continue;
                minTick = std::min(minTick, c->timelineIn());
                maxTick = std::max(maxTick, c->timelineOut());
                if (targetTrackIdx == SIZE_MAX ||
                    cr.trackIndex < targetTrackIdx)
                    targetTrackIdx = cr.trackIndex;
            }
            if (minTick >= maxTick || targetTrackIdx == SIZE_MAX) return;

            // Save clones of the original clips BEFORE the operation so
            // undo can restore them precisely (including original IDs).
            struct SavedClip {
                size_t trackIndex;
                uint64_t clipId;           // original ID (used on first execute)
                uint64_t restoredId{0};    // ID after undo-restore (used on redo)
                std::shared_ptr<Clip> clonedClip;  // shared for lambda capture
            };
            auto savedClips = std::make_shared<std::vector<SavedClip>>();
            auto selectedClipIds =
                std::make_shared<std::unordered_set<uint64_t>>();
            for (const auto& cr : clips) {
                auto* trk = m_ws->timeline()->track(cr.trackIndex);
                if (!trk) continue;
                size_t ci = trk->findClipIndexById(cr.clipId);
                if (ci >= trk->clipCount()) continue;
                auto* srcClip = trk->clip(ci);
                if (!srcClip) continue;
                SavedClip sc;
                sc.trackIndex = cr.trackIndex;
                sc.clipId     = cr.clipId;
                sc.clonedClip = std::shared_ptr<Clip>(srcClip->clone().release());
                savedClips->push_back(std::move(sc));
                selectedClipIds->insert(cr.clipId);
            }
            // Transitions are track-owned, not clip-owned. Snapshot every
            // transition touching the selection before removeClipById()
            // deletes it, so execute can transfer it and undo can restore it.
            auto savedTransitions =
                std::make_shared<std::vector<NestTransitionSnapshot>>(
                    captureTransitionsForNest(*m_ws->timeline(),
                                              *selectedClipIds));

            // Shared state for execute / undo
            auto seqIdx     = std::make_shared<size_t>(SIZE_MAX);
            auto seqClipId  = std::make_shared<uint64_t>(0);
            auto targetTk   = std::make_shared<size_t>(targetTrackIdx);
            auto savedMin   = minTick;
            auto savedMax   = maxTick;
            auto name       = nestName.toStdString();

            auto refreshAfter = [this]() {
                if (m_ws->isDestroying()) return;
                m_ws->selection().clip = nullptr;
                m_ws->selection().graphicLayerIdx = -1;
                m_ws->timelinePanel()->selection().clear();
                if (m_ws->effectControlsPanel()) m_ws->effectControlsPanel()->clearClip();
                if (m_ws->graphicsEditorPanel()) m_ws->graphicsEditorPanel()->clearClip();
                if (m_ws->colorGradingPanel()) m_ws->colorGradingPanel()->clearClip();
                if (m_ws->propertiesPanel()) m_ws->propertiesPanel()->clearClip();
                if (m_ws->programMonitor() && m_ws->programMonitor()->viewport())
                    m_ws->programMonitor()->viewport()->clearTransformOverlay();
                if (m_ws->programMonitor() && m_ws->programMonitor()->transformOverlay())
                    m_ws->programMonitor()->transformOverlay()->clearTransformOverlay();
                m_ws->timelinePanel()->refreshTrackContents();
                emit m_ws->timelinePanel()->selectionChanged();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                if (m_ws->projectBin()) {
                    m_ws->projectBin()->refreshSequences();
                    emit m_ws->projectBin()->sequencesChanged();
                }
            };

            m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                "Nest Selected Clips",
                /* execute / redo */
                [this, savedClips, selectedClipIds, savedTransitions,
                 seqIdx, seqClipId, targetTk, savedMin, savedMax, name,
                 refreshAfter]() {
                    // Create a new sequence for the nested content
                    *seqIdx = m_ws->project()->sequenceCount();
                    auto* nestedTimeline = m_ws->project()->addSequence(name);
                    if (!nestedTimeline) return;

                    // Strip the default V1+A1 tracks
                    while (nestedTimeline->trackCount() > 0)
                        nestedTimeline->removeTrack(0);

                    // Collect source track indices
                    std::set<size_t> usedTrackIndices;
                    for (const auto& sc : *savedClips)
                        usedTrackIndices.insert(sc.trackIndex);

                    // Mirror tracks: video first, then audio
                    std::unordered_map<size_t, size_t> trackMap;
                    for (size_t si : usedTrackIndices) {
                        auto* srcTrack = m_ws->timeline()->track(si);
                        if (!srcTrack || srcTrack->type() != TrackType::Video) continue;
                        size_t ni = nestedTimeline->trackCount();
                        nestedTimeline->addVideoTrack(srcTrack->name());
                        trackMap[si] = ni;
                    }
                    for (size_t si : usedTrackIndices) {
                        auto* srcTrack = m_ws->timeline()->track(si);
                        if (!srcTrack || srcTrack->type() != TrackType::Audio) continue;
                        size_t ni = nestedTimeline->trackCount();
                        nestedTimeline->addAudioTrack(srcTrack->name());
                        trackMap[si] = ni;
                    }

                    // Clone saved clips into the nested timeline
                    std::unordered_map<uint64_t, uint64_t> nestedClipIds;
                    for (const auto& sc : *savedClips) {
                        auto mapIt = trackMap.find(sc.trackIndex);
                        if (mapIt == trackMap.end()) continue;
                        auto* dstTrack = nestedTimeline->track(mapIt->second);
                        if (!dstTrack) continue;
                        auto cloned = sc.clonedClip->clone();
                        cloned->setTimelineIn(sc.clonedClip->timelineIn() - savedMin);
                        cloned->setDuration(sc.clonedClip->duration());
                        cloned->setSourceIn(sc.clonedClip->sourceIn());
                        nestedClipIds[sc.clipId] = cloned->id();
                        dstTrack->addClip(std::move(cloned));
                    }

                    // Recreate internal dissolves and one-sided fades on the
                    // mirrored nested tracks. IDs and edit points must both be
                    // remapped because clone() assigns fresh IDs and the nest
                    // starts at timeline tick zero.
                    addTransitionsInsideNest(
                        *nestedTimeline, *savedTransitions, *selectedClipIds,
                        trackMap, nestedClipIds, savedMin);

                    // Remove the original clips from the current timeline.
                    // On first execute, clips are removed by their original ID.
                    // On redo after undo, undo restored clips with new IDs, so
                    // we use restoredId (which undo captured).
                    for (auto& sc : *savedClips) {
                        auto* trk = m_ws->timeline()->track(sc.trackIndex);
                        if (!trk) continue;
                        uint64_t removeId = (sc.restoredId != 0) ? sc.restoredId : sc.clipId;
                        trk->removeClipById(removeId);
                        sc.restoredId = 0;  // reset for next undo/redo cycle
                    }

                    // Insert a SequenceClip in their place
                    auto* targetTrack = m_ws->timeline()->track(*targetTk);
                    if (targetTrack && targetTrack->type() == TrackType::Video) {
                        auto seqClip = std::make_unique<SequenceClip>();
                        seqClip->setSequenceIndex(*seqIdx);
                        seqClip->setSequenceName(name);
                        seqClip->setLabel(name);
                        seqClip->setTimelineIn(savedMin);
                        seqClip->setDuration(savedMax - savedMin);
                        *seqClipId = seqClip->id();
                        targetTrack->addClip(std::move(seqClip));

                        // A dissolve between a selected edge clip and an
                        // unselected neighbour remains in the parent, now
                        // attached to the SequenceClip endpoint.
                        addBoundaryTransitionsToNestClip(
                            *m_ws->timeline(), *savedTransitions,
                            *selectedClipIds, *targetTk, *seqClipId);
                    }

                    refreshAfter();
                },
                /* undo */
                [this, savedClips, savedTransitions, seqIdx, seqClipId,
                 targetTk, refreshAfter]() {
                    // Remove the SequenceClip from the target track
                    if (*targetTk < m_ws->timeline()->trackCount()) {
                        auto* trk = m_ws->timeline()->track(*targetTk);
                        if (trk) trk->removeClipById(*seqClipId);
                    }

                    // Restore the original clips.  Because clone() assigns
                    // fresh IDs, we capture the new ID on each SavedClip so
                    // that redo can find and remove them again.
                    std::unordered_map<uint64_t, uint64_t> restoredClipIds;
                    for (auto& sc : *savedClips) {
                        auto* trk = m_ws->timeline()->track(sc.trackIndex);
                        if (!trk) continue;
                        auto restored = sc.clonedClip->clone();
                        sc.restoredId = restored->id();
                        restoredClipIds[sc.clipId] = sc.restoredId;
                        trk->addClip(std::move(restored));
                    }

                    restoreTransitionsAfterNestUndo(
                        *m_ws->timeline(), *savedTransitions,
                        restoredClipIds);

                    // Remove the created nested sequence
                    if (*seqIdx < m_ws->project()->sequenceCount())
                        m_ws->project()->extractSequence(*seqIdx);

                    refreshAfter();
                }));

            spdlog::info("Nested {} clips into sequence '{}' (index {})",
                         clips.size(), name, *seqIdx);
        });

        // -- Sequence dropped from project bin or Source Monitor --------
        connect(m_ws->timelinePanel(), &TimelinePanel::sequenceDropped,
                this, [this](size_t sequenceIndex, int64_t atTick, size_t trackIndex,
                             int64_t sourceIn, int64_t sourceOut, int dragMode) {
            if (m_ws->isDestroying()) return;
            const bool dropVideo = (dragMode != TimelinePanel::DragAudioOnly);
            const bool dropAudio = (dragMode != TimelinePanel::DragVideoOnly);
            if (!m_ws->timeline() || !m_ws->project() || !m_ws->commandStack()) return;
            if (sequenceIndex >= m_ws->project()->sequenceCount()) return;

            auto* nestedTimeline = m_ws->project()->sequence(sequenceIndex);
            if (!nestedTimeline) return;

            // Prevent dropping a sequence into itself (infinite recursion)
            if (nestedTimeline == m_ws->timeline()) {
                spdlog::warn("Cannot nest a sequence into itself");
                return;
            }

            // Compute nested sequence duration from its content
            int64_t dur = 0;
            for (size_t ti = 0; ti < nestedTimeline->trackCount(); ++ti) {
                auto* trk = nestedTimeline->track(ti);
                if (!trk) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    auto* c = trk->clip(ci);
                    if (c) dur = std::max(dur, c->timelineOut());
                }
            }
            if (dur <= 0) dur = static_cast<int64_t>(5.0 * 48000.0); // fallback 5s

            // Find a video track to place the SequenceClip
            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            if (forceGhostVideoTrack)
                needsNewTrack = true;
            if (trackIndex < m_ws->timeline()->trackCount() &&
                m_ws->timeline()->track(trackIndex)->type() == TrackType::Video)
                targetTrackIdx = trackIndex;
            if (targetTrackIdx == SIZE_MAX) {
                for (size_t i = m_ws->timeline()->trackCount(); i > 0; --i) {
                    if (m_ws->timeline()->track(i - 1)->type() == TrackType::Video) {
                        targetTrackIdx = i - 1;
                        break;
                    }
                }
            }
            if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;

            auto clipId     = std::make_shared<uint64_t>(0);
            auto createdTk  = std::make_shared<bool>(false);
            auto tkIdx      = std::make_shared<size_t>(targetTrackIdx);
            auto overlapCmd2 = std::make_shared<std::unique_ptr<Command>>(nullptr);
            std::string seqName = nestedTimeline->name();

            // ── Audio nest: mirror the video SequenceClip onto an audio
            //    track so the parent timeline plays the inner sequence's
            //    audio (Premiere-style nesting). Determined here only if
            //    the inner sequence actually has any audio content.
            bool innerHasAudio = false;
            for (size_t ti = 0; ti < nestedTimeline->trackCount() && !innerHasAudio; ++ti) {
                auto* trk = nestedTimeline->track(ti);
                if (!trk || trk->type() != TrackType::Audio) continue;
                for (size_t ci = 0; ci < trk->clipCount(); ++ci) {
                    auto* c = trk->clip(ci);
                    if (c && dynamic_cast<AudioClip*>(c) && c->isEnabled()) {
                        innerHasAudio = true; break;
                    }
                }
            }
            // Honour the drop target: if the cursor was over an audio
            // track, the audio nest clip goes THERE (not always the top
            // audio track). Captured by Track* so it survives any video
            // track insertion that shifts indices before audio placement.
            Track* preferredAudioTrack = nullptr;
            if (trackIndex < m_ws->timeline()->trackCount() &&
                m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio)
                preferredAudioTrack = m_ws->timeline()->track(trackIndex);

            Track* videoTargetTrack = targetTrackIdx < m_ws->timeline()->trackCount()
                ? m_ws->timeline()->track(targetTrackIdx) : nullptr;
            Track* audioTargetTrack = preferredAudioTrack;
            if (!audioTargetTrack && dropAudio && innerHasAudio) {
                for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                    Track* candidate = m_ws->timeline()->track(i);
                    if (candidate && candidate->type() == TrackType::Audio &&
                        !candidate->isDivider()) {
                        audioTargetTrack = candidate;
                        break;
                    }
                }
            }
            // A sequence drop is one compound edit. Reject it before the
            // command runs if either existing destination is locked, so the
            // unlocked half can never be left behind on the timeline.
            if ((dropVideo && videoTargetTrack && videoTargetTrack->isLocked()) ||
                (dropAudio && innerHasAudio && audioTargetTrack &&
                 audioTargetTrack->isLocked())) {
                spdlog::warn("Sequence drop rejected: a destination track is locked");
                return;
            }

            auto audioClipId      = std::make_shared<uint64_t>(0);
            auto audioTkIdx       = std::make_shared<size_t>(SIZE_MAX);
            auto createdAudioTk   = std::make_shared<bool>(false);
            auto audioOverlapCmd  = std::make_shared<std::unique_ptr<Command>>(nullptr);

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (m_ws->isDestroying()) return;
                if (trackStructureChanged)
                    m_ws->timelinePanel()->rebuildTracks();
                else
                    m_ws->timelinePanel()->refreshTrackContents();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                // Audio topology changed — reload sources so the next play
                // pulls audio from the newly-nested sequence.
                m_ws->invalidateAudioSources();
            };

            m_ws->commandStack()->execute(std::make_unique<LambdaCommand>(
                "Add Sequence to Timeline",
                /* execute / redo */
                [this, sequenceIndex, atTick, sourceIn, sourceOut, dur,
                 needsNewTrack, forceGhostVideoTrack, innerHasAudio,
                 dropVideo, dropAudio,
                 clipId, createdTk, tkIdx, overlapCmd2, seqName,
                 audioTargetTrack,
                 audioClipId, audioTkIdx, createdAudioTk, audioOverlapCmd,
                 refreshAfter]() {
                    if (dropVideo) {
                        if (needsNewTrack && *tkIdx == SIZE_MAX) {
                            Track* t = nullptr;
                            if (forceGhostVideoTrack) {
                                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                                t = m_ws->timeline()->insertTrack(0, std::move(newTrack));
                            } else {
                                t = m_ws->timeline()->addVideoTrack("V1");
                            }
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i) == t) {
                                    *tkIdx = i; break;
                                }
                            }
                            *createdTk = true;
                        }
                        auto* track = m_ws->timeline()->track(*tkIdx);
                        if (track) {
                            auto seqClip = std::make_unique<SequenceClip>();
                            seqClip->setSequenceIndex(sequenceIndex);
                            seqClip->setSequenceName(seqName);
                            seqClip->setLabel(seqName);
                            seqClip->setTimelineIn(atTick);

                            if (sourceIn >= 0 && sourceOut > sourceIn) {
                                seqClip->setSourceIn(sourceIn);
                                seqClip->setDuration(sourceOut - sourceIn);
                            } else {
                                seqClip->setDuration(dur);
                            }
                            // Link the video sequence clip to its audio
                            // mirror via groupId so clicking either selects
                            // BOTH (Premiere-style linked A/V) and dragging
                            // moves them together. Without this, the audio
                            // mirror sits at the original tick as a phantom
                            // snap target -- the snap engine pulls the
                            // dragged video clip back to its starting
                            // position because its edge "snaps" to the
                            // audio mirror's edge at the original spot.
                            // The clip's own id is unique and non-zero, so
                            // it's a safe group key.
                            *clipId = seqClip->id();
                            seqClip->setGroupId(*clipId);
                            track->addClip(std::move(seqClip));

                            *overlapCmd2 = EditOperations::resolveOverlaps(
                                *m_ws->timeline(), *tkIdx, *clipId);
                            if (*overlapCmd2) (*overlapCmd2)->execute();
                        }
                    }

                    // ── Audio nest: place a single SequenceClip on a
                    //    parent audio track. AudioPlaybackService recurses
                    //    into the inner sequence at playback time so
                    //    edits to the inner propagate live.
                    *audioTkIdx     = SIZE_MAX;
                    *createdAudioTk = false;
                    *audioClipId    = 0;
                    audioOverlapCmd->reset();
                    if (dropAudio && innerHasAudio) {
                        size_t aIdx = SIZE_MAX;
                        // 1) Drop target audio track (resolved by identity so
                        //    a just-inserted video track doesn't misalign it).
                        if (audioTargetTrack) {
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i) == audioTargetTrack) {
                                    aIdx = i; break;
                                }
                            }
                        }
                        // 2) Otherwise the first existing audio track.
                        if (aIdx == SIZE_MAX) {
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i)->type() == TrackType::Audio) {
                                    aIdx = i; break;
                                }
                            }
                        }
                        // 3) None exist — create one.
                        if (aIdx == SIZE_MAX) {
                            Track* nt = m_ws->timeline()->addAudioTrack("A1");
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i) == nt) {
                                    aIdx = i; break;
                                }
                            }
                            *createdAudioTk = true;
                        }
                        if (aIdx != SIZE_MAX) {
                            auto* aTrack = m_ws->timeline()->track(aIdx);
                            if (aTrack) {
                                auto aClip = std::make_unique<SequenceClip>();
                                aClip->setSequenceIndex(sequenceIndex);
                                aClip->setSequenceName(seqName);
                                aClip->setLabel(seqName);
                                aClip->setTimelineIn(atTick);
                                if (sourceIn >= 0 && sourceOut > sourceIn) {
                                    aClip->setSourceIn(sourceIn);
                                    aClip->setDuration(sourceOut - sourceIn);
                                } else {
                                    aClip->setDuration(dur);
                                }
                                // Link the audio mirror to its video sibling
                                // via the shared groupId (see comment on the
                                // video clip above). If the video drop was
                                // skipped (audio-only drag), fall back to
                                // this clip's own id so it's still well-
                                // formed even without a sibling.
                                *audioClipId = aClip->id();
                                aClip->setGroupId(*clipId != 0 ? *clipId : *audioClipId);
                                aTrack->addClip(std::move(aClip));
                                *audioTkIdx = aIdx;

                                *audioOverlapCmd = EditOperations::resolveOverlaps(
                                    *m_ws->timeline(), aIdx, *audioClipId);
                                if (*audioOverlapCmd) (*audioOverlapCmd)->execute();
                            }
                        }
                    }

                    refreshAfter(*createdTk || *createdAudioTk);
                },
                /* undo */
                [this, clipId, createdTk, tkIdx, overlapCmd2,
                 audioClipId, audioTkIdx, createdAudioTk, audioOverlapCmd,
                 refreshAfter]() {
                    const bool trackStructureChanged = *createdTk || *createdAudioTk;

                    if (*audioOverlapCmd) (*audioOverlapCmd)->undo();
                    if (*audioTkIdx < m_ws->timeline()->trackCount() && *audioClipId != 0) {
                        auto* aTrack = m_ws->timeline()->track(*audioTkIdx);
                        if (aTrack) aTrack->removeClipById(*audioClipId);
                    }

                    if (*overlapCmd2) (*overlapCmd2)->undo();

                    if (*tkIdx < m_ws->timeline()->trackCount()) {
                        auto* track = m_ws->timeline()->track(*tkIdx);
                        if (track) track->removeClipById(*clipId);
                    }
                    if (*createdTk) {
                        m_ws->timeline()->removeTrack(*tkIdx);
                        *tkIdx = SIZE_MAX;
                        *createdTk = false;
                    }
                    if (*createdAudioTk && *audioTkIdx != SIZE_MAX &&
                        *audioTkIdx < m_ws->timeline()->trackCount()) {
                        m_ws->timeline()->removeTrack(*audioTkIdx);
                        *audioTkIdx = SIZE_MAX;
                        *createdAudioTk = false;
                    }
                    refreshAfter(trackStructureChanged);
                }));

            spdlog::info("Sequence '{}' (index {}) dropped on timeline at tick {}",
                         seqName, sequenceIndex, atTick);
        });

        // -- Open nested sequence (from context menu) --------------------
        connect(m_ws->timelinePanel(), &TimelinePanel::openNestedSequence,
                this, [this](size_t sequenceIndex) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->project() || sequenceIndex >= m_ws->project()->sequenceCount()) return;
            auto* mw = qobject_cast<MainWindow*>(m_ws->window());
            if (mw) mw->switchSequence(sequenceIndex);
        });

        // -- Reveal in Project Bin (from clip context menu) --------------
        connect(m_ws->timelinePanel(), &TimelinePanel::revealInProjectBin,
                this, [this](const QString& filePath) {
            if (m_ws->isDestroying()) return;
            if (m_ws->projectBin()) {
                m_ws->projectBin()->revealByPath(filePath);
                // Raise the Project Bin dock so the user sees the selection
                if (auto* dock = m_ws->dockForPanel(QStringLiteral("Project")))  {
                    dock->setVisible(true);
                    dock->raise();
                }
            }
        });
    }
}

} // namespace rt
