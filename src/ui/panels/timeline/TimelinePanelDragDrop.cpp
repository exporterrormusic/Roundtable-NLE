/*
 * TimelinePanelDragDrop.cpp - drag-preview phase (enter/move/leave).
 * Drop handling --> TimelinePanelDrop.cpp
 */

#include "panels/timeline/TimelinePanel.h"
#include "panels/timeline/TimelinePanelInternal.h"
#include "widgets/TimelineTrackWidget.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/Transition.h"
#include "command/CommandStack.h"
#include "command/commands/ClipCommands.h"
#include "command/commands/TransitionCmds.h"
#include "effects/Effect.h"
#include "playback/MediaPool.h"
#include "PathUtils.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QTreeWidget>

#include <spdlog/spdlog.h>

namespace rt {
namespace {
// Still-image extensions — FFmpeg's image2 demuxer reports JPGs as a tiny
// 0.04s "video", which would shrink the drag-preview ghost to a single
// frame.  Mirror the still-image guard the drop-side code uses so the
// ghost matches the 5-second default the actual drop will create.
bool isStillImagePathDrag(const QString& path)
{
    QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList kExts = {
        "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "tga", "dds"
    };
    return kExts.contains(ext);
}
}

// ═════════════════════════════════════════════════════════════════════════════
//  Drag-and-drop from Project Bin → Timeline
// ═════════════════════════════════════════════════════════════════════════════

void TimelinePanel::dragEnterEvent(QDragEnterEvent* event)
{
    // Accept effect drags, transition drags, media drags, sequence drags, or external file drops
    if (event->mimeData()->hasFormat("application/x-roundtable-effect") ||
        event->mimeData()->hasFormat("application/x-roundtable-glitch-preset") ||
        event->mimeData()->hasFormat("application/x-roundtable-audiofx") ||
        event->mimeData()->hasFormat(kTransitionMimeType) ||
        event->mimeData()->hasFormat("application/x-roundtable-media") ||
        event->mimeData()->hasFormat("application/x-roundtable-sequence") ||
        event->mimeData()->hasFormat("application/x-roundtable-adjustment") ||
        qobject_cast<QTreeWidget*>(event->source()) ||
        event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();

        // Build snap targets for the incoming drag operation
        if (m_timeline) {
            m_snapEngine.setPixelsPerSecond(m_layoutEngine.pixelsPerSecond());
            m_snapEngine.buildTargets(*m_timeline, m_playheadTick, 0.0, {});
        }
    }
    else
        event->ignore();
}

void TimelinePanel::dragMoveEvent(QDragMoveEvent* event)
{
    // ── Transition drag (custom MIME) ───────────────────────────────────
    if (event->mimeData()->hasFormat(kTransitionMimeType)) {
        QPointF pos = event->position();
        size_t trackIdx = hitTestTrack(pos.y());

        // Adjust X for track header column (same as hitTestClip)
        double cursorPx = pos.x() - headerWidth();

        TransitionDropTarget bestTarget;

        if (m_timeline && trackIdx < m_timeline->trackCount()) {
            const Track* track = m_timeline->track(trackIdx);

            // Find the closest clip edge to the cursor
            double bestDist = kEdgeSnapPixels + 1;

            for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                const Clip* clip = track->clip(ci);
                if (!clip) continue;

                // Check tail edge (end of this clip)
                double tailX = m_layoutEngine.timeToPixelX(clip->timelineOut());
                double tailDist = std::abs(cursorPx - tailX);
                if (tailDist < bestDist) {
                    bestDist = tailDist;
                    bestTarget.trackIndex = trackIdx;
                    bestTarget.editPointTick = clip->timelineOut();
                    bestTarget.leftClipId = clip->id();
                    bestTarget.rightClipId = 0;

                    // Check if there's an adjacent clip starting at or near this edge
                    for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                        if (cj == ci) continue;
                        const Clip* adjClip = track->clip(cj);
                        if (!adjClip) continue;
                        int64_t gap = std::abs(adjClip->timelineIn() - clip->timelineOut());
                        if (gap <= 1600) { // ~33ms at 48kHz
                            bestTarget.rightClipId = adjClip->id();
                            break;
                        }
                    }
                }

                // Check head edge (start of this clip)
                double headX = m_layoutEngine.timeToPixelX(clip->timelineIn());
                double headDist = std::abs(cursorPx - headX);
                if (headDist < bestDist) {
                    bestDist = headDist;
                    bestTarget.trackIndex = trackIdx;
                    bestTarget.editPointTick = clip->timelineIn();
                    bestTarget.rightClipId = clip->id();
                    bestTarget.leftClipId = 0;

                    // Check if there's an adjacent clip ending at or near this edge
                    for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                        if (cj == ci) continue;
                        const Clip* adjClip = track->clip(cj);
                        if (!adjClip) continue;
                        int64_t gap = std::abs(adjClip->timelineOut() - clip->timelineIn());
                        if (gap <= 1600) {
                            bestTarget.leftClipId = adjClip->id();
                            break;
                        }
                    }
                }
            }

            // If no edge within snap distance, snap to nearest edge of
            // the clip under cursor (Premiere Pro: drop anywhere on clip)
            if (bestTarget.trackIndex == SIZE_MAX) {
                int64_t cursorTick = m_layoutEngine.pixelXToTime(cursorPx);
                for (size_t ci = 0; ci < track->clipCount(); ++ci) {
                    const Clip* clip = track->clip(ci);
                    if (!clip) continue;
                    if (cursorTick >= clip->timelineIn() && cursorTick < clip->timelineOut()) {
                        // Cursor is over this clip — use nearest edge
                        int64_t headDist = std::abs(cursorTick - clip->timelineIn());
                        int64_t tailDist = std::abs(cursorTick - clip->timelineOut());
                        if (headDist <= tailDist) {
                            bestTarget.trackIndex = trackIdx;
                            bestTarget.editPointTick = clip->timelineIn();
                            bestTarget.rightClipId = clip->id();
                            bestTarget.leftClipId = 0;
                            for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                                if (cj == ci) continue;
                                const Clip* adj = track->clip(cj);
                                if (adj && std::abs(adj->timelineOut() - clip->timelineIn()) <= 1600) {
                                    bestTarget.leftClipId = adj->id();
                                    break;
                                }
                            }
                        } else {
                            bestTarget.trackIndex = trackIdx;
                            bestTarget.editPointTick = clip->timelineOut();
                            bestTarget.leftClipId = clip->id();
                            bestTarget.rightClipId = 0;
                            for (size_t cj = 0; cj < track->clipCount(); ++cj) {
                                if (cj == ci) continue;
                                const Clip* adj = track->clip(cj);
                                if (adj && std::abs(adj->timelineIn() - clip->timelineOut()) <= 1600) {
                                    bestTarget.rightClipId = adj->id();
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }

        // Update highlight on track widgets
        bool hasTarget = bestTarget.trackIndex != SIZE_MAX;
        if (hasTarget) {
            m_transitionDropTarget = bestTarget;
        } else {
            m_transitionDropTarget.reset();
        }
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (hasTarget && i == bestTarget.trackIndex)
                m_trackWidgets[i]->setTransitionDropEdgeTick(bestTarget.editPointTick);
            else
                m_trackWidgets[i]->clearTransitionDropEdge();
        }

        event->acceptProposedAction();
        return;
    }

    // ── Effect / audio-FX drag (custom MIME) — same clip highlight ──────
    if (event->mimeData()->hasFormat("application/x-roundtable-effect") ||
        event->mimeData()->hasFormat("application/x-roundtable-glitch-preset") ||
        event->mimeData()->hasFormat("application/x-roundtable-audiofx")) {
        QPointF pos = event->position();
        auto hitRef = hitTestClip(pos);
        if (hitRef != m_effectDropTarget) {
            m_effectDropTarget = hitRef;
            // Forward highlight to the correct TimelineTrackWidget
            for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
                if (hitRef && i == hitRef->trackIndex)
                    m_trackWidgets[i]->setEffectHighlightClipId(hitRef->clipId);
                else
                    m_trackWidgets[i]->clearEffectHighlight();
            }
        }
        event->acceptProposedAction();
        return;
    }

    // ── Adjustment-layer drag (from project bin) ────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-adjustment")) {
        m_effectDropTarget.reset();
        for (auto tw : m_trackWidgets) tw->clearEffectHighlight();

        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        // 5-second default duration matches Premiere Pro.
        int64_t previewDur = static_cast<int64_t>(5.0 * 48000.0);
        auto snapRes = m_snapEngine.snapPair(tick, tick + previewDur);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        // Adjustment layers always go on a VIDEO track. Find the target.
        size_t videoTrackIdx = SIZE_MAX;
        if (m_timeline && trackIdx < m_timeline->trackCount() &&
            m_timeline->track(trackIdx)->type() == TrackType::Video) {
            videoTrackIdx = trackIdx;
        } else if (m_timeline) {
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                if (m_timeline->track(i)->type() == TrackType::Video) {
                    videoTrackIdx = i; break;
                }
            }
        }
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (i == videoTrackIdx)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
            else
                m_trackWidgets[i]->clearMediaDragPreview();
        }
        event->acceptProposedAction();
        return;
    }

    // ── Sequence drag (from project bin) ────────────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        m_effectDropTarget.reset();
        for (auto tw : m_trackWidgets) tw->clearEffectHighlight();

        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        // Read actual duration from MIME, fall back to 5 seconds
        int64_t previewDur = static_cast<int64_t>(5.0 * 48000.0);

        // Source in/out from Source Monitor drag take priority
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in")
            && event->mimeData()->hasFormat("application/x-roundtable-source-out")) {
            int64_t srcIn  = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            int64_t srcOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();
            if (srcOut > srcIn) previewDur = srcOut - srcIn;
        } else if (event->mimeData()->hasFormat("application/x-roundtable-sequence-duration")) {
            bool durOk = false;
            int64_t d = event->mimeData()->data("application/x-roundtable-sequence-duration")
                            .toLongLong(&durOk);
            if (durOk && d > 0) previewDur = d;
        }

        auto snapRes = m_snapEngine.snapPair(tick, tick + previewDur);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        // Source-monitor video-only / audio-only drag restricts the ghost
        // to just that lane.
        bool seqShowVideo = true, seqShowAudio = true;
        if (event->mimeData()->hasFormat("application/x-roundtable-drag-mode")) {
            const QByteArray m = event->mimeData()->data(
                "application/x-roundtable-drag-mode");
            if (m == "video") seqShowAudio = false;
            else if (m == "audio") seqShowVideo = false;
        }
        // Honour the sequence's actual audio content: if the sequence has
        // no audio clips, don't show an audio ghost track.
        if (seqShowAudio && event->mimeData()->hasFormat(
                "application/x-roundtable-sequence-has-audio")) {
            const QByteArray a = event->mimeData()->data(
                "application/x-roundtable-sequence-has-audio");
            if (a == "0") seqShowAudio = false;
        }

        // A nested sequence drops as a video nest clip AND an audio nest
        // clip (Premiere-style). Show ghosts on BOTH a video track and an
        // audio track so the user sees where audio will land, mirroring
        // the behaviour of dragging an A/V media file.
        size_t videoTrackIdx = SIZE_MAX;
        if (m_timeline && trackIdx < m_timeline->trackCount() &&
            m_timeline->track(trackIdx)->type() == TrackType::Video) {
            videoTrackIdx = trackIdx;
        } else if (m_timeline) {
            // Cursor not over a video track — preview on the bottom video
            // track (same fallback the drop handler uses).
            for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                if (m_timeline->track(i - 1)->type() == TrackType::Video) {
                    videoTrackIdx = i - 1; break;
                }
            }
        }
        size_t audioTrackIdx = SIZE_MAX;
        if (m_timeline) {
            if (trackIdx < m_timeline->trackCount() &&
                m_timeline->track(trackIdx)->type() == TrackType::Audio) {
                audioTrackIdx = trackIdx;
            } else {
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    if (m_timeline->track(i)->type() == TrackType::Audio) {
                        audioTrackIdx = i; break;
                    }
                }
            }
        }
        if (!seqShowVideo) videoTrackIdx = SIZE_MAX;
        if (!seqShowAudio) audioTrackIdx = SIZE_MAX;
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (i == videoTrackIdx)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
            else if (i == audioTrackIdx)
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, true);
            else
                m_trackWidgets[i]->clearMediaDragPreview();
        }
        event->acceptProposedAction();
        return;
    }

    // ── Media drag (custom MIME, QTreeWidget fallback, or external file URLs) ──
    if (event->mimeData()->hasFormat("application/x-roundtable-media")
        || qobject_cast<QTreeWidget*>(event->source())
        || event->mimeData()->hasUrls()) {
        m_effectDropTarget.reset();
        for (auto tw : m_trackWidgets) tw->clearEffectHighlight();
        // Compute drag preview position and duration
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        spdlog::debug("GHOST-CLIP: dragMove media pos=({:.0f},{:.0f}) px={:.0f} tick={} trackIdx={} widgets={}",
                      pos.x(), pos.y(), px, tick, trackIdx, m_trackWidgets.size());

        // Parse all media handles (comma-separated for multi-item drag)
        QByteArray mediaData = event->mimeData()->data("application/x-roundtable-media");
        QList<QByteArray> handleTokens;
        if (!mediaData.isEmpty())
            handleTokens = mediaData.split(',');
        QList<QUrl> urls = event->mimeData()->urls();

        // Determine clip duration for the preview
        int64_t previewDur = 0;
        bool isAudio = false;
        // Per-clip durations for ghost overlay multi-preview
        std::vector<int64_t> clipDurations;

        // Check source in/out (from Source Monitor drag)
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in")
            && event->mimeData()->hasFormat("application/x-roundtable-source-out")) {
            int64_t srcIn  = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            int64_t srcOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();
            if (srcOut > srcIn) {
                previewDur = srcOut - srcIn;
                clipDurations.push_back(previewDur);
            }
        }

        // Compute duration for each handle and sum for combined preview
        if (previewDur <= 0 && m_mediaPool && !handleTokens.isEmpty()) {
            for (int i = 0; i < handleTokens.size(); ++i) {
                bool ok = false;
                uint64_t handle = handleTokens[i].toULongLong(&ok);
                int64_t clipDur = 0;
                QString resolvedPath;
                if (ok && handle != 0) {
                    const auto* info = m_mediaPool->getInfo(handle);
                    resolvedPath = QString::fromStdString(
                        pathToUtf8(m_mediaPool->getPath(handle)));
                    if (info && info->duration > 0.0 &&
                        !isStillImagePathDrag(resolvedPath))
                        clipDur = static_cast<int64_t>(info->duration * 48000.0);
                    // Use first clip to determine audio/video type
                    if (i == 0 && info)
                        isAudio = (info->videoStreamIndex < 0);
                }
                // Resolve via file path if handle had no info
                if (clipDur <= 0 && i < urls.size()) {
                    QString path = urls[i].toLocalFile();
                    if (resolvedPath.isEmpty()) resolvedPath = path;
                    if (!path.isEmpty()) {
                        auto h = m_mediaPool->open(path.toStdString());
                        if (h != 0) {
                            const auto* info = m_mediaPool->getInfo(h);
                            if (info && info->duration > 0.0 &&
                                !isStillImagePathDrag(path))
                                clipDur = static_cast<int64_t>(info->duration * 48000.0);
                            if (i == 0 && info)
                                isAudio = (info->videoStreamIndex < 0);
                        }
                    }
                }
                // Fallback per clip (also covers stills — 5s default matches
                // what TimelineWorkspaceWiringMediaDrop.cpp creates).
                if (clipDur <= 0)
                    clipDur = static_cast<int64_t>(5.0 * 48000.0);
                clipDurations.push_back(clipDur);
                previewDur += clipDur;
            }
        }

        // Fallback: iterate all URLs for external file / no-handle multi-drag
        if (previewDur <= 0 && m_mediaPool
            && event->mimeData()->hasUrls()
            && !event->mimeData()->urls().isEmpty()) {
            const auto& allUrls = event->mimeData()->urls();
            for (int i = 0; i < allUrls.size(); ++i) {
                QString path = allUrls[i].toLocalFile();
                int64_t clipDur = 0;
                if (!path.isEmpty()) {
                    auto h = m_mediaPool->open(path.toStdString());
                    if (h != 0) {
                        const auto* info = m_mediaPool->getInfo(h);
                        if (info && info->duration > 0.0 &&
                            !isStillImagePathDrag(path))
                            clipDur = static_cast<int64_t>(info->duration * 48000.0);
                        if (i == 0 && info)
                            isAudio = (info->videoStreamIndex < 0);
                    }
                }
                if (clipDur <= 0)
                    clipDur = static_cast<int64_t>(5.0 * 48000.0);
                clipDurations.push_back(clipDur);
                previewDur += clipDur;
            }
        }

        // Fallback: 5-second default if all else fails
        if (previewDur <= 0) {
            previewDur = static_cast<int64_t>(5.0 * 48000.0);
        }
        // Ensure clipDurations has at least one entry for ghost overlay
        if (clipDurations.empty())
            clipDurations.push_back(previewDur);
        if (!isAudio && event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()) {
            QString ext = QFileInfo(event->mimeData()->urls().first().toLocalFile())
                              .suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            if (audioExts.contains(ext)) isAudio = true;
        }

        // Source-monitor video-only / audio-only drag: restrict the ghost
        // to a single lane (matches what the drop will actually create).
        bool mediaForceVideoOnly = false;
        if (event->mimeData()->hasFormat("application/x-roundtable-drag-mode")) {
            const QByteArray m = event->mimeData()->data(
                "application/x-roundtable-drag-mode");
            if (m == "audio")      isAudio = true;
            else if (m == "video") mediaForceVideoOnly = true;
        }

        // Snap both head and tail (Premiere Pro-style edge snapping)
        bool didSnap = false;
        if (previewDur > 0) {
            auto snapRes = m_snapEngine.snapPair(tick, tick + previewDur);
            if (snapRes.didSnap) { tick = snapRes.snappedTick; didSnap = true; }
        } else {
            auto snapRes = m_snapEngine.snap(tick);
            if (snapRes.didSnap) { tick = snapRes.snappedTick; didSnap = true; }
        }

        // Show snap indicator if snapping occurred; clear it otherwise.
        for (auto tw : m_trackWidgets) tw->setSnapIndicatorTick(didSnap ? tick : -1);

        // Update only the target track's preview; clear all others.
        // Skip preview if media type doesn't match the track type
        // (e.g. audio file over a video track, or video over audio track).
        // Dividers are TrackType::Video but reject clips — reject them as
        // drop targets here too so the ghost doesn't land on a divider row.
        bool trackCompatible = false;
        if (m_timeline && trackIdx < m_timeline->trackCount()) {
            const Track* targetTr = m_timeline->track(trackIdx);
            TrackType tt = targetTr->type();
            const bool isRealVideo = (tt == TrackType::Video) && !targetTr->isDivider();
            const bool isRealAudio = (tt == TrackType::Audio) && !targetTr->isDivider();
            trackCompatible = isAudio ? isRealAudio : isRealVideo;
        }
        // mediaHasAudio is a property of the MEDIA, not the cursor position
        // — compute it unconditionally so the audio-companion preview still
        // shows when the cursor leaves the existing tracks (e.g. dragging
        // above the topmost video to create a new track).
        bool mediaHasAudio = false;
        if (!isAudio && m_mediaPool && !handleTokens.isEmpty()) {
            bool firstOk = false;
            uint64_t firstH = handleTokens.first().toULongLong(&firstOk);
            if (firstOk && firstH != 0) {
                const auto* info = m_mediaPool->getInfo(firstH);
                if (info && info->hasAudio)
                    mediaHasAudio = true;
            }
        }
        // Video-only drag never spawns the companion audio clip.
        if (mediaForceVideoOnly) mediaHasAudio = false;

        // Find target audio track index for the audio preview. Fires for
        // BOTH audio-only files (isAudio=true) and video+audio companion
        // (mediaHasAudio=true). If the cursor hovers an audio track, the
        // preview follows it there; otherwise fall back to the first
        // (real) audio track. Skip divider rows — they're not real tracks
        // and can't host clips.
        size_t audioTrackIdx = SIZE_MAX;
        if ((isAudio || mediaHasAudio) && m_timeline) {
            if (trackIdx < m_timeline->trackCount() &&
                m_timeline->track(trackIdx)->type() == TrackType::Audio &&
                !m_timeline->track(trackIdx)->isDivider()) {
                audioTrackIdx = trackIdx;
            } else {
                for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                    Track* tr = m_timeline->track(i);
                    if (tr->type() == TrackType::Audio && !tr->isDivider()) {
                        audioTrackIdx = i; break;
                    }
                }
            }
        }
        // Find target video track index for companion preview whenever
        // the cursor isn't over a real video track (over audio, below
        // the audio area, or anywhere else). Premiere routes the video
        // to the BOTTOM video layer (closest to the V/A boundary). Scan
        // BACKWARD for the highest video index, skipping the V/A divider
        // (it's TrackType::Video but rejects clips). Without this, the
        // video preview vanishes the moment the cursor leaves the video
        // section — even though that's exactly where the user is going
        // when they want the audio companion to land on a new track.
        size_t videoTrackIdx = SIZE_MAX;
        const bool cursorOnRealVideo = !isAudio
            && m_timeline && trackIdx < m_timeline->trackCount()
            && m_timeline->track(trackIdx)->type() == TrackType::Video
            && !m_timeline->track(trackIdx)->isDivider();
        if (!isAudio && m_timeline) {
            if (cursorOnRealVideo) {
                // Cursor is over a real video track — that's where the video
                // lands. Setting videoTrackIdx here lets the multi-clip
                // primary overlay anchor on the cursor's track instead of
                // collapsing to SIZE_MAX (which hides the overlay).
                videoTrackIdx = trackIdx;
            } else {
                // Fallback to bottom video (closest to V/A boundary).
                for (size_t i = m_timeline->trackCount(); i > 0; --i) {
                    Track* tr = m_timeline->track(i - 1);
                    if (tr->type() == TrackType::Video && !tr->isDivider()) {
                        videoTrackIdx = i - 1; break;
                    }
                }
            }
        }
        spdlog::debug("GHOST-CLIP: dragMove media isAudio={} previewDur={} trackCompatible={} m_engine={} trackCount={}",
                      isAudio, previewDur, trackCompatible,
                      (m_timeline && trackIdx < m_timeline->trackCount() ? "yes" : "N/A"),
                      m_timeline ? m_timeline->trackCount() : 0);

        // When dragging multiple clips, the GhostTrackOverlay shows
        // individual clip outlines — but only on ONE row (above/below/on-
        // existing). The OPPOSITE side (video for an audio-row ghost, etc.)
        // and the case where no ghost is showing still need a per-track
        // preview so the user can see where the clips will land.
        const bool multiClipDrag = (clipDurations.size() > 1);
        for (size_t i = 0; i < m_trackWidgets.size(); ++i) {
            if (multiClipDrag) {
                // Audio companion: combined per-track ghost so the audio
                // drop zone is still visually marked.
                if (mediaHasAudio && i == audioTrackIdx)
                    m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, true);
                // Video side: combined per-track ghost on the existing video
                // track. Without this, dragging multi-clip video files with
                // audio shows nothing on the video track when the cursor is
                // anywhere other than directly over it.
                else if (!isAudio && i == videoTrackIdx)
                    m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
                else
                    m_trackWidgets[i]->clearMediaDragPreview();
            } else if (i == trackIdx && trackCompatible) {
                spdlog::debug("GHOST-CLIP: >> setting preview on trackIdx={}", i);
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, isAudio);
            } else if (mediaHasAudio && i == audioTrackIdx) {
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, true);
            } else if (!isAudio && i == videoTrackIdx) {
                m_trackWidgets[i]->setMediaDragPreview(tick, previewDur, false);
            } else {
                m_trackWidgets[i]->clearMediaDragPreview();
            }
        }

        // ── Ghost track overlay for bin/external drags ──
        if (!m_trackWidgets.empty()) {
            // Find first/last track indices of each type. Skip dividers —
            // they're TrackType::Video but not real tracks, and treating
            // them as bottom-of-video / top-of-audio would put the ghost
            // zones in the wrong place.
            size_t firstVideoIdx = SIZE_MAX, lastVideoIdx = SIZE_MAX;
            size_t firstAudioIdx = SIZE_MAX, lastAudioIdx = SIZE_MAX;
            for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
                Track* t = m_timeline->track(i);
                if (!t || t->isDivider()) continue;
                if (t->type() == TrackType::Video) {
                    if (firstVideoIdx == SIZE_MAX) firstVideoIdx = i;
                    lastVideoIdx = i;
                } else {
                    if (firstAudioIdx == SIZE_MAX) firstAudioIdx = i;
                    lastAudioIdx = i;
                }
            }

            // Anchor the ghost zones to the FIRST real video and LAST real
            // audio widget specifically, not just the front/back of the
            // m_trackWidgets vector — if any trailing widget exists (an
            // extra divider, or any non-standard layout), the back() would
            // be off and the cursor would have to go past it to trigger
            // the "below bottom audio" zone.
            QWidget* firstVideoTw = (firstVideoIdx < m_trackWidgets.size())
                                    ? m_trackWidgets[firstVideoIdx].data()
                                    : nullptr;
            QWidget* lastAudioTw  = (lastAudioIdx < m_trackWidgets.size())
                                    ? m_trackWidgets[lastAudioIdx].data()
                                    : nullptr;
            QPoint firstTop = firstVideoTw
                              ? firstVideoTw->mapTo(this, QPoint(0, 0))
                              : QPoint(0, 0);
            QPoint lastBot  = lastAudioTw
                              ? lastAudioTw->mapTo(this, QPoint(0, lastAudioTw->height()))
                              : QPoint(0, 0);

            // Scroll area X position (tracks start after the header column)
            QPoint scrollOrig = m_verticalScroll->mapTo(this, QPoint(0, 0));
            int ghostX = scrollOrig.x();
            int ghostW = m_verticalScroll->width();

            if (!isAudio && firstVideoTw && pos.y() < firstTop.y()) {
                // Above topmost video track → ghost new video track above
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = true;
                m_ghostTrackOnExisting = false;
                m_ghostTrackHeight = std::max(firstVideoTw->height(), 30);
                m_ghostTrackY = firstTop.y() - m_ghostTrackHeight;
            } else if ((isAudio || mediaHasAudio) && lastAudioTw && pos.y() > lastBot.y()) {
                // Below bottommost audio track → ghost new audio track below.
                // Also fires for video+audio files (mediaHasAudio): the video
                // stays on its existing track, the audio companion lands on
                // a new audio track below.
                m_ghostTrackVisible = true;
                m_ghostTrackIsAbove = false;
                m_ghostTrackOnExisting = false;
                m_ghostTrackHeight = std::max(lastAudioTw->height(), 30);
                m_ghostTrackY = lastBot.y();
            } else if (multiClipDrag) {
                // Multi-clip drag — always show the primary ghost overlay on
                // the VIDEO destination (or audio destination for audio-only
                // files), even when the cursor is over a different-type
                // track. Without this, the overlay only fires when cursor is
                // over a compatible existing track, and the V_dest collapses
                // to a single combined block during all other times.
                const size_t primary = isAudio ? audioTrackIdx : videoTrackIdx;
                if (primary < m_trackWidgets.size()) {
                    m_ghostTrackVisible = true;
                    m_ghostTrackIsAbove = !isAudio; // visual color
                    m_ghostTrackOnExisting = true;  // existing-track preview
                    m_ghostTrackHeight = m_trackWidgets[primary]->height();
                    QPoint trackTop = m_trackWidgets[primary]->mapTo(this, QPoint(0, 0));
                    m_ghostTrackY = trackTop.y();
                } else {
                    m_ghostTrackVisible = false;
                    m_ghostTrackOnExisting = false;
                    if (m_ghostOverlay) {
                        m_ghostOverlay->setClipPreviews({});
                        m_ghostOverlay->hide();
                    }
                }
            } else {
                m_ghostTrackVisible = false;
                m_ghostTrackOnExisting = false;
                if (m_ghostOverlay) {
                    m_ghostOverlay->setClipPreviews({});
                    m_ghostOverlay->hide();
                }
            }
            if (m_ghostTrackVisible && m_ghostOverlay) {
                // Populate ghost track with clip previews for bin/external drags.
                // The clip color reflects which SIDE the ghost is on, not the
                // source media's primary type: an audio-below ghost shows the
                // companion AUDIO clip (green) even if the source file is
                // video+audio. Without this, the bottom ghost would render a
                // blue video block in the new audio row.
                const bool ghostShowsAudio = !m_ghostTrackIsAbove;
                {
                    std::vector<GhostTrackOverlay::GhostClipPreview> previews;
                    int64_t cursor = tick;
                    for (size_t ci = 0; ci < clipDurations.size(); ++ci) {
                        int64_t cd = clipDurations[ci];
                        if (cd <= 0) continue;
                        double clipPx = m_layoutEngine.timeToPixelX(cursor);
                        double clipPw = m_layoutEngine.timeToPixelX(cursor + cd) - clipPx;
                        if (clipPw > 0) {
                            GhostTrackOverlay::GhostClipPreview gp;
                            gp.x = static_cast<int>(clipPx);
                            gp.width = static_cast<int>(clipPw);
                            gp.color = ghostShowsAudio ? 0x3CA05AFF : 0x4A90D9FF;
                            // Label from corresponding URL or placeholder
                            if (ci < urls.size())
                                gp.label = QFileInfo(urls[ci].toLocalFile()).fileName();
                            else if (!urls.isEmpty())
                                gp.label = QFileInfo(urls.first().toLocalFile()).fileName();
                            else if (event->mimeData()->hasFormat("application/x-roundtable-media"))
                                gp.label = QStringLiteral("Media");
                            else
                                gp.label = QStringLiteral("Clip");
                            previews.push_back(gp);
                        }
                        cursor += cd;
                    }
                    m_ghostOverlay->setClipPreviews(previews);
                }

                m_ghostOverlay->isAbove = m_ghostTrackIsAbove;
                m_ghostOverlay->onExistingTrack = m_ghostTrackOnExisting;
                m_ghostOverlay->setGeometry(ghostX, m_ghostTrackY, ghostW, m_ghostTrackHeight);
                m_ghostOverlay->raise();
                m_ghostOverlay->show();
                m_ghostOverlay->update();

                // Suppress the duplicate per-track preview that's now being
                // rendered inside the ghost overlay. For a video-above ghost
                // the video lands in the NEW track above, not on the existing
                // bottom video — so the V_bottom preview must clear. Same
                // mirror for audio-below. The OTHER side's preview stays
                // (e.g. for video+audio + audio-below ghost, the video still
                // previews on its existing track).
                if (!m_ghostTrackOnExisting) {
                    if (m_ghostTrackIsAbove) {
                        if (videoTrackIdx < m_trackWidgets.size())
                            m_trackWidgets[videoTrackIdx]->clearMediaDragPreview();
                        if (trackIdx < m_trackWidgets.size() && trackCompatible && !isAudio)
                            m_trackWidgets[trackIdx]->clearMediaDragPreview();
                    } else {
                        if (audioTrackIdx < m_trackWidgets.size())
                            m_trackWidgets[audioTrackIdx]->clearMediaDragPreview();
                        if (trackIdx < m_trackWidgets.size() && trackCompatible && isAudio)
                            m_trackWidgets[trackIdx]->clearMediaDragPreview();
                    }
                } else {
                    // Multi-clip existing-track ghost — clear the combined
                    // per-track preview on the same row so it doesn't
                    // double-render under the individual clip outlines.
                    const size_t primary = isAudio ? audioTrackIdx : videoTrackIdx;
                    if (primary < m_trackWidgets.size())
                        m_trackWidgets[primary]->clearMediaDragPreview();
                }
            }

            // === Secondary overlay (multi-clip video+audio drags) ===
            // The primary m_ghostOverlay only covers ONE row. For multi-clip
            // drags of video+audio media we need a second overlay for the
            // OTHER side so both video and audio destinations show individual
            // clip outlines. Which side it covers depends on what the primary
            // is doing:
            //   - audio-below ghost active → primary = new audio track below,
            //                                secondary = VIDEO on V_existing
            //   - otherwise                → primary = video destination,
            //                                secondary = AUDIO on A_existing
            if (m_ghostOverlayAudio) {
                const bool audioBelowGhostActive =
                    m_ghostTrackVisible && !m_ghostTrackIsAbove && !m_ghostTrackOnExisting;
                size_t secondaryIdx = SIZE_MAX;
                uint32_t secondaryColor = 0;
                bool secondaryIsAudio = false;
                if (multiClipDrag && !isAudio && mediaHasAudio) {
                    if (audioBelowGhostActive) {
                        // Primary handles audio. Secondary = video on existing.
                        secondaryIdx = videoTrackIdx;
                        secondaryColor = 0x4A90D9FF;  // video blue
                        secondaryIsAudio = false;
                    } else {
                        // Primary handles video. Secondary = audio on A_existing.
                        secondaryIdx = audioTrackIdx;
                        secondaryColor = 0x3CA05AFF;  // audio green
                        secondaryIsAudio = true;
                    }
                }

                if (secondaryIdx < m_trackWidgets.size()) {
                    auto* w = m_trackWidgets[secondaryIdx].data();
                    if (w) {
                        std::vector<GhostTrackOverlay::GhostClipPreview> previews;
                        int64_t cur = tick;
                        for (size_t ci = 0; ci < clipDurations.size(); ++ci) {
                            int64_t cd = clipDurations[ci];
                            if (cd <= 0) continue;
                            double cpx = m_layoutEngine.timeToPixelX(cur);
                            double cpw = m_layoutEngine.timeToPixelX(cur + cd) - cpx;
                            if (cpw > 0) {
                                GhostTrackOverlay::GhostClipPreview gp;
                                gp.x = static_cast<int>(cpx);
                                gp.width = static_cast<int>(cpw);
                                gp.color = secondaryColor;
                                if (ci < urls.size())
                                    gp.label = QFileInfo(urls[ci].toLocalFile()).fileName();
                                else if (!urls.isEmpty())
                                    gp.label = QFileInfo(urls.first().toLocalFile()).fileName();
                                else
                                    gp.label = QStringLiteral("Clip");
                                previews.push_back(gp);
                            }
                            cur += cd;
                        }
                        m_ghostOverlayAudio->setClipPreviews(previews);
                        m_ghostOverlayAudio->isAbove = !secondaryIsAudio;
                        m_ghostOverlayAudio->onExistingTrack = true;
                        QPoint topPt = w->mapTo(this, QPoint(0, 0));
                        m_ghostOverlayAudio->setGeometry(ghostX, topPt.y(), ghostW, w->height());
                        m_ghostOverlayAudio->raise();
                        m_ghostOverlayAudio->show();
                        m_ghostOverlayAudio->update();
                        // Clear the combined block underneath so it doesn't
                        // bleed through the individual outlines.
                        w->clearMediaDragPreview();
                    }
                } else {
                    m_ghostOverlayAudio->setClipPreviews({});
                    m_ghostOverlayAudio->hide();
                }
            }
        }

        event->acceptProposedAction();
    } else {
        // Hide ghost overlay if not dragging media
        m_ghostTrackVisible = false;
        m_ghostTrackOnExisting = false;
        if (m_ghostOverlayAudio) {
            m_ghostOverlayAudio->setClipPreviews({});
            m_ghostOverlayAudio->hide();
        }
        if (m_ghostOverlay) {
            m_ghostOverlay->setClipPreviews({});
            m_ghostOverlay->hide();
        }
        event->ignore();
    }
}

void TimelinePanel::dragLeaveEvent(QDragLeaveEvent* event)
{
    m_effectDropTarget.reset();
    m_transitionDropTarget.reset();
    m_ghostTrackVisible = false;
    m_ghostTrackOnExisting = false;
    if (m_ghostOverlayAudio) {
        m_ghostOverlayAudio->setClipPreviews({});
        m_ghostOverlayAudio->hide();
    }
    if (m_ghostOverlay) {
        m_ghostOverlay->setClipPreviews({});
        m_ghostOverlay->hide();
    }
    for (auto tw : m_trackWidgets) {
        tw->clearEffectHighlight();
        tw->clearTransitionDropEdge();
        tw->clearMediaDragPreview();
    }
    QWidget::dragLeaveEvent(event);
}

} // namespace rt
