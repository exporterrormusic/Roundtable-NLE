/*
 * TimelinePanelDrop.cpp - dropEvent: commit a Project-Bin drag onto the timeline.
 * Extracted from TimelinePanelDragDrop.cpp (behavior-preserving).
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
#include "media/MediaPool.h"

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
constexpr size_t kGhostDropTrackVideoAbove = SIZE_MAX - 1;
constexpr size_t kGhostDropTrackAudioBelow = SIZE_MAX - 2;

// Drop of a video+audio file in the audio-below zone: the video lands on
// its existing target track, but the audio COMPANION needs a brand-new
// audio track at the bottom. Distinct from kGhostDropTrackAudioBelow which
// targets the whole drop at a new audio track (used for audio-only files).
constexpr size_t kGhostDropTrackAudioCompanionBelow = SIZE_MAX - 3;
}

void TimelinePanel::dropEvent(QDropEvent* event)
{
    const bool ghostWasVisible = m_ghostTrackVisible;
    const bool ghostWasAbove = m_ghostTrackIsAbove;
    const bool ghostWasOnExisting = m_ghostTrackOnExisting;

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

    // Clear all drag previews
    for (auto tw : m_trackWidgets) tw->clearMediaDragPreview();

    // Compute drop-time ghost zones directly from cursor Y and current track geometry,
    // so routing does not depend on whether a prior dragMove state was preserved.
    auto computeGhostDropZones = [this](const QPointF& pos, bool& aboveTopVideo, bool& belowBottomAudio) {
        aboveTopVideo = false;
        belowBottomAudio = false;
        if (!m_timeline || m_trackWidgets.empty()) return;

        size_t firstVideoIdx = SIZE_MAX;
        size_t lastAudioIdx = SIZE_MAX;
        for (size_t i = 0; i < m_timeline->trackCount(); ++i) {
            auto* tr = m_timeline->track(i);
            if (!tr || tr->isDivider()) continue;  // skip dividers
            if (tr->type() == TrackType::Video) {
                if (firstVideoIdx == SIZE_MAX) firstVideoIdx = i;
            } else {
                lastAudioIdx = i;
            }
        }

        if (firstVideoIdx < m_trackWidgets.size()) {
            auto w = m_trackWidgets[firstVideoIdx];
            QPoint top = w->mapTo(this, QPoint(0, 0));
            aboveTopVideo = (pos.y() < top.y());
        }
        if (lastAudioIdx < m_trackWidgets.size()) {
            auto w = m_trackWidgets[lastAudioIdx];
            QPoint bot = w->mapTo(this, QPoint(0, w->height()));
            belowBottomAudio = (pos.y() > bot.y());
        }
    };

    // ── Transition drop (custom MIME type) ──────────────────────────────
    if (event->mimeData()->hasFormat(kTransitionMimeType)) {
        // Clear highlights
        for (auto tw : m_trackWidgets) tw->clearTransitionDropEdge();

        if (!m_transitionDropTarget || !m_timeline) {
            m_transitionDropTarget.reset();
            event->ignore();
            return;
        }

        QByteArray transData = event->mimeData()->data(kTransitionMimeType);
        bool ok = false;
        int transType = transData.toInt(&ok);
        if (!ok) { m_transitionDropTarget.reset(); event->ignore(); return; }

        auto target = *m_transitionDropTarget;
        m_transitionDropTarget.reset();

        // Must have at least one clip
        if (target.leftClipId == 0 && target.rightClipId == 0) {
            event->ignore();
            return;
        }

        emit transitionDroppedAtEdge(target.trackIndex, target.leftClipId,
                                     target.rightClipId, target.editPointTick,
                                     transType);
        event->acceptProposedAction();
        return;
    }

    // ── Effect drop (custom MIME type) ──────────────────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-effect")) {
        m_effectDropTarget.reset();
        for (auto tw : m_trackWidgets) tw->clearEffectHighlight();

        QByteArray effectData = event->mimeData()->data("application/x-roundtable-effect");
        bool ok = false;
        int effectType = effectData.toInt(&ok);
        if (!ok) { event->ignore(); return; }

        // Hit-test to find which clip was dropped on
        QPointF pos = event->position();
        auto hitRef = hitTestClip(pos);
        if (!hitRef || !m_timeline) { event->ignore(); return; }

        auto* track = m_timeline->track(hitRef->trackIndex);
        if (!track) { event->ignore(); return; }

        size_t clipIdx = track->findClipIndexById(hitRef->clipId);
        if (clipIdx == SIZE_MAX) { event->ignore(); return; }

        emit effectDroppedOnClip(hitRef->trackIndex, hitRef->clipId, effectType);
        event->acceptProposedAction();
        return;
    }

    // ── Audio FX drop (EQ / Dynamics → clip FxChain) ────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-audiofx")) {
        QByteArray fxData = event->mimeData()->data("application/x-roundtable-audiofx");
        bool ok = false;
        int kind = fxData.toInt(&ok);
        if (!ok) { event->ignore(); return; }

        QPointF pos = event->position();
        auto hitRef = hitTestClip(pos);
        if (!hitRef || !m_timeline) { event->ignore(); return; }

        auto* track = m_timeline->track(hitRef->trackIndex);
        if (!track) { event->ignore(); return; }
        size_t clipIdx = track->findClipIndexById(hitRef->clipId);
        if (clipIdx == SIZE_MAX) { event->ignore(); return; }

        // Audio DSP only applies to audio clips.
        const Clip* clip = track->clip(clipIdx);
        if (!clip || clip->clipType() != ClipType::Audio) { event->ignore(); return; }

        emit audioFxDroppedOnClip(hitRef->trackIndex, hitRef->clipId, kind);
        event->acceptProposedAction();
        return;
    }

    // ── Adjustment-layer drop (from project bin) ───────────────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-adjustment")) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;

        int64_t dur = static_cast<int64_t>(5.0 * 48000.0);
        auto snapRes = m_snapEngine.snapPair(tick, tick + dur);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        QString name = QString::fromUtf8(
            event->mimeData()->data("application/x-roundtable-adjustment"));
        if (name.isEmpty()) name = QStringLiteral("Adjustment Layer");

        emit adjustmentDropped(name, tick, trackIdx);
        event->acceptProposedAction();
        return;
    }

    // ── Sequence drop (from project bin or Source Monitor) ─────────────
    if (event->mimeData()->hasFormat("application/x-roundtable-sequence")) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());
        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;

        bool ok = false;
        size_t seqIndex = event->mimeData()->data("application/x-roundtable-sequence")
                              .toULongLong(&ok);

        if (ok) {
            // Read source in/out if present (from Source Monitor drag-out)
            int64_t sourceIn  = -1;
            int64_t sourceOut = -1;
            if (event->mimeData()->hasFormat("application/x-roundtable-source-in"))
                sourceIn = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
            if (event->mimeData()->hasFormat("application/x-roundtable-source-out"))
                sourceOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();

            // Snap
            int64_t dropDur = 0;
            if (sourceIn >= 0 && sourceOut > sourceIn) {
                dropDur = sourceOut - sourceIn;
            } else if (event->mimeData()->hasFormat("application/x-roundtable-sequence-duration")) {
                bool durOk = false;
                dropDur = event->mimeData()->data("application/x-roundtable-sequence-duration")
                              .toLongLong(&durOk);
                if (!durOk) dropDur = 0;
            }
            if (dropDur > 0) {
                auto snapRes = m_snapEngine.snapPair(tick, tick + dropDur);
                if (snapRes.didSnap) tick = snapRes.snappedTick;
            } else {
                auto snapRes = m_snapEngine.snap(tick);
                if (snapRes.didSnap) tick = snapRes.snappedTick;
            }

            int dragMode = TimelinePanel::DragBoth;
            if (event->mimeData()->hasFormat("application/x-roundtable-drag-mode")) {
                const QByteArray m = event->mimeData()->data(
                    "application/x-roundtable-drag-mode");
                if (m == "video") dragMode = TimelinePanel::DragVideoOnly;
                else if (m == "audio") dragMode = TimelinePanel::DragAudioOnly;
            }

            emit sequenceDropped(seqIndex, tick, trackIdx, sourceIn, sourceOut,
                                 dragMode);
        }
        event->acceptProposedAction();
        return;
    }

    // ── Media drop (custom MIME from MediaDragTreeWidget / ThumbnailGrid) ──
    if (event->mimeData()->hasFormat("application/x-roundtable-media")) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        // Parse all media handles (comma-separated for multi-item drag)
        QByteArray mediaData = event->mimeData()->data("application/x-roundtable-media");
        QList<QByteArray> handleTokens = mediaData.split(',');
        QList<QUrl> urls = event->mimeData()->urls();

        // Determine audio/video type from first valid handle for ghost-zone routing
        bool isAudioDrop = false;
        bool dropMediaHasAudio = false;
        if (!handleTokens.isEmpty()) {
            bool firstOk = false;
            uint64_t firstHandle = handleTokens.first().toULongLong(&firstOk);
            QString firstPath;
            if (!urls.isEmpty())
                firstPath = urls.first().toLocalFile();
            if (firstOk && firstHandle != 0 && m_mediaPool) {
                if (const auto* info = m_mediaPool->getInfo(firstHandle)) {
                    isAudioDrop = (info->videoStreamIndex < 0);
                    dropMediaHasAudio = info->hasAudio;
                }
            }
            if (!isAudioDrop && !firstPath.isEmpty()) {
                QString ext = QFileInfo(firstPath).suffix().toLower();
                static const QStringList audioExts = {
                    "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
                };
                isAudioDrop = audioExts.contains(ext);
            }
        }
        // Source-monitor "drag audio only" → route to an audio track even
        // for video media (the handler creates just an AudioClip).
        if (event->mimeData()->hasFormat("application/x-roundtable-drag-mode")
            && event->mimeData()->data("application/x-roundtable-drag-mode") == "audio")
            isAudioDrop = true;
        // Video-only drag forces no companion audio.
        const bool forceVideoOnly =
            event->mimeData()->hasFormat("application/x-roundtable-drag-mode")
            && event->mimeData()->data("application/x-roundtable-drag-mode") == "video";
        if (forceVideoOnly) dropMediaHasAudio = false;

        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;
        else if (isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
            trackIdx = kGhostDropTrackAudioBelow;
        // Video+audio file dropped in the audio-below ghost zone: the video
        // takes its normal target (bottom existing video), but the audio
        // companion needs a brand-new audio track. Distinct sentinel so the
        // handler can route the two halves to different destinations.
        else if (!isAudioDrop && dropMediaHasAudio && !ghostWasOnExisting
                 && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
            trackIdx = kGhostDropTrackAudioCompanionBelow;

        // Check for source in/out points (from Source Monitor drag)
        int64_t sourceIn = -1;
        int64_t sourceOut = -1;
        if (event->mimeData()->hasFormat("application/x-roundtable-source-in"))
            sourceIn = event->mimeData()->data("application/x-roundtable-source-in").toLongLong();
        if (event->mimeData()->hasFormat("application/x-roundtable-source-out"))
            sourceOut = event->mimeData()->data("application/x-roundtable-source-out").toLongLong();

        int mediaDragMode = TimelinePanel::DragBoth;
        if (event->mimeData()->hasFormat("application/x-roundtable-drag-mode")) {
            const QByteArray m = event->mimeData()->data(
                "application/x-roundtable-drag-mode");
            if (m == "video") mediaDragMode = TimelinePanel::DragVideoOnly;
            else if (m == "audio") mediaDragMode = TimelinePanel::DragAudioOnly;
        }

        // Emit mediaDropped for each handle, placing clips sequentially.
        // Wrap in a macro so Ctrl+Z undoes the whole multi-drop as one action.
        if (m_commandStack && handleTokens.size() > 1)
            m_commandStack->beginMacro("Import Files");

        int64_t currentTick = tick;
        for (int i = 0; i < handleTokens.size(); ++i) {
            bool ok = false;
            uint64_t handle = handleTokens[i].toULongLong(&ok);
            if (!ok) continue;

            // Get file path from URLs (one per item, in order)
            QString filePath;
            if (i < urls.size())
                filePath = urls[i].toLocalFile();
            else if (!urls.isEmpty())
                filePath = urls.first().toLocalFile();

            // Skip only if we have neither a valid handle nor a file path.
            // Images may have handle==0 but a valid URL — allow them through
            // so they resolve via the file-path fallback below.
            if (handle == 0 && filePath.isEmpty()) continue;

            // Compute duration for this clip
            int64_t clipDur = 0;
            if (sourceIn >= 0 && sourceOut > sourceIn) {
                clipDur = sourceOut - sourceIn;
            } else if (m_mediaPool && handle != 0) {
                const auto* info = m_mediaPool->getInfo(handle);
                if (info && info->duration > 0.0)
                    clipDur = static_cast<int64_t>(info->duration * 48000.0);
            }
            // Resolve via file path if handle had no info (covers handle==0)
            if (clipDur <= 0 && m_mediaPool && !filePath.isEmpty()) {
                auto h = m_mediaPool->open(filePath.toStdString());
                if (h != 0) {
                    const auto* info = m_mediaPool->getInfo(h);
                    if (info && info->duration > 0.0)
                        clipDur = static_cast<int64_t>(info->duration * 48000.0);
                    // Promote handle from 0 to the newly opened handle so the
                    // downstream handler can use it for metadata lookup.
                    if (handle == 0) handle = h;
                }
            }

            // Snap this clip's position
            int64_t snapTick = currentTick;
            if (clipDur > 0) {
                auto snapRes = m_snapEngine.snapPair(snapTick, snapTick + clipDur);
                if (snapRes.didSnap) snapTick = snapRes.snappedTick;
            } else {
                auto snapRes = m_snapEngine.snap(snapTick);
                if (snapRes.didSnap) snapTick = snapRes.snappedTick;
            }

            if (!filePath.isEmpty()) {
                if (sourceIn >= 0 && sourceOut > sourceIn)
                    emit mediaDroppedWithRegion(filePath, handle, snapTick, trackIdx,
                                                sourceIn, sourceOut, mediaDragMode);
                else
                    emit mediaDropped(filePath, handle, snapTick, trackIdx,
                                      mediaDragMode);
            }

            // Ghost-sentinel rewrite: the first emit triggered the handler
            // to CREATE a new track. Subsequent emits must target THAT same
            // new track by index, otherwise each handle would spawn its own
            // fresh track (Premiere drops the whole group onto one new row).
            // The handler creates video-above at idx 0 and audio at the end.
            if (m_timeline) {
                if (trackIdx == kGhostDropTrackVideoAbove) {
                    trackIdx = 0;
                } else if (trackIdx == kGhostDropTrackAudioBelow ||
                           trackIdx == kGhostDropTrackAudioCompanionBelow) {
                    trackIdx = m_timeline->trackCount() - 1;
                }
            }

            // Advance tick for next clip (sequential placement like Premiere).
            // Use 5-second default for stills/images/unknown media with 0 duration.
            if (clipDur > 0)
                currentTick = snapTick + clipDur;
            else
                currentTick = snapTick + static_cast<int64_t>(5.0 * 48000.0);
        }

        if (m_commandStack && handleTokens.size() > 1)
            m_commandStack->endMacro();

        event->acceptProposedAction();
        return;
    }

    // ── External file drop (from Windows Explorer) ───────────────────────
    if (event->mimeData()->hasUrls() &&
        !event->mimeData()->hasFormat("application/x-roundtable-media") &&
        !qobject_cast<QTreeWidget*>(event->source())) {
        QPointF pos = event->position();
        double px = pos.x() - headerWidth();
        int64_t tick = m_layoutEngine.pixelXToTime(px);
        if (tick < 0) tick = 0;
        size_t trackIdx = hitTestTrack(pos.y());

        bool isAudioDrop = false;
        bool isDirectoryDrop = false;
        QString firstPath;
        if (!event->mimeData()->urls().isEmpty()) {
            firstPath = event->mimeData()->urls().first().toLocalFile();
            QFileInfo fi(firstPath);
            isDirectoryDrop = fi.isDir();
            if (!isDirectoryDrop) {
                QString ext = fi.suffix().toLower();
                static const QStringList audioExts = {
                    "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
                };
                isAudioDrop = audioExts.contains(ext);
            }
        }

        // ── Character folder drop (from Explorer) ────────────────────
        // If a folder is dropped, check if it looks like a character
        // outfit folder (contains .skel files) and create a Spine clip
        // defaulting to "default-idle".
        static const QStringList kStanceSubdirs =
            {"Default", "default", "aim", "cover"};

        if (isDirectoryDrop && !firstPath.isEmpty()) {
            QString skelFile;
            QString outfitFolder = firstPath;

            // Step 1: look for .skel directly in the dropped folder
            {
                QDir dir(firstPath);
                for (const QString& f : dir.entryList({"*.skel"}, QDir::Files)) {
                    skelFile = dir.absoluteFilePath(f);
                    break;
                }
            }

            // Step 2: look in known stance subdirectories
            if (skelFile.isEmpty()) {
                for (const QString& sd : kStanceSubdirs) {
                    QDir subDir(firstPath + "/" + sd);
                    for (const QString& f : subDir.entryList({"*.skel"}, QDir::Files)) {
                        skelFile = subDir.absoluteFilePath(f);
                        break;
                    }
                    if (!skelFile.isEmpty()) {
                        outfitFolder = firstPath;
                        break;
                    }
                }
            }

            // Step 3: check all child directories (user may have dragged
            // the character root folder, e.g. assets/characters/2B/)
            if (skelFile.isEmpty()) {
                QDir parentDir(firstPath);
                for (const QString& childDir : parentDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    QString childPath = firstPath + "/" + childDir;
                    for (const QString& sd : kStanceSubdirs) {
                        QDir subDir(childPath + "/" + sd);
                        for (const QString& f : subDir.entryList({"*.skel"}, QDir::Files)) {
                            skelFile = subDir.absoluteFilePath(f);
                            break;
                        }
                        if (!skelFile.isEmpty()) {
                            outfitFolder = childPath;
                            break;
                        }
                    }
                    if (!skelFile.isEmpty()) break;
                    // Also check directly
                    QDir child(childPath);
                    for (const QString& f : child.entryList({"*.skel"}, QDir::Files)) {
                        skelFile = child.absoluteFilePath(f);
                        outfitFolder = childPath;
                        break;
                    }
                    if (!skelFile.isEmpty()) break;
                }
            }

            if (!skelFile.isEmpty()) {
                // Extract character and outfit names from the folder path.
                // Expected structure: .../assets/characters/<charName>/<outfit>/
                // or .../assets/characters/<charName>/<outfit>/aim|cover
                QFileInfo fi(outfitFolder);
                QString folderName = fi.fileName();                 // outfit name (e.g. "default")
                QString parentPath = fi.absolutePath();
                QFileInfo parentFi(parentPath);
                QString grandparentName = parentFi.fileName();      // should be "characters"
                QString charFolderName = parentFi.absolutePath();
                QFileInfo charFi(charFolderName);
                QString charName = charFi.fileName();               // character name (e.g. "2B")

                // Sanity check: folder should be under "characters"
                if (grandparentName.toLower() == "characters" && !charFolderName.isEmpty()) {
                    QString spineUri = QStringLiteral("spine:") + charFolderName
                        + "|" + folderName + "|0|idle";
                    spdlog::info("Character folder drop: {} -> {}",
                                 firstPath.toStdString(), spineUri.toStdString());
                    emit mediaDropped(spineUri, 0, tick, trackIdx);
                    event->acceptProposedAction();
                    return;
                }
            }

            // If we couldn't parse it as a character folder, fall through
            // to the normal external file handler which will just add it
            // to the project bin (doing nothing for a directory).
        }

        bool aboveTopVideo = false;
        bool belowBottomAudio = false;
        computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);
        if (!isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
            trackIdx = kGhostDropTrackVideoAbove;
        else if (isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
            trackIdx = kGhostDropTrackAudioBelow;

        auto snapRes = m_snapEngine.snap(tick);
        if (snapRes.didSnap) tick = snapRes.snappedTick;

        // Place files sequentially (Premiere Pro-style), advancing tick
        // by each file's duration so they don't stack on top of each other.
        // Wrap in a macro so Ctrl+Z undoes the whole multi-drop as one action.
        const auto& dropUrls = event->mimeData()->urls();
        if (m_commandStack && dropUrls.size() > 1)
            m_commandStack->beginMacro("Import Files");

        int64_t currentTick = tick;
        for (const QUrl& url : dropUrls) {
            QString localPath = url.toLocalFile();
            if (localPath.isEmpty()) continue;

            // Compute duration for sequential advance
            int64_t advance = static_cast<int64_t>(5.0 * 48000.0); // 5-second default
            if (m_mediaPool) {
                auto h = m_mediaPool->open(localPath.toStdString());
                if (h != 0) {
                    const auto* info = m_mediaPool->getInfo(h);
                    if (info && info->duration > 0.0)
                        advance = static_cast<int64_t>(info->duration * 48000.0);
                }
            }

            emit externalFileDropped(localPath, currentTick, trackIdx);
            // Rewrite the sentinel to the just-created track's real index
            // so subsequent files in the same drop go into the SAME new
            // track instead of each spawning their own.
            if (m_timeline) {
                if (trackIdx == kGhostDropTrackVideoAbove) {
                    trackIdx = 0;
                } else if (trackIdx == kGhostDropTrackAudioBelow ||
                           trackIdx == kGhostDropTrackAudioCompanionBelow) {
                    trackIdx = m_timeline->trackCount() - 1;
                }
            }
            currentTick += advance;
        }

        if (m_commandStack && dropUrls.size() > 1)
            m_commandStack->endMacro();

        event->acceptProposedAction();
        return;
    }

    // ── Media drop fallback (QTreeWidget default drag) ──────────────────
    auto* srcTree = qobject_cast<QTreeWidget*>(event->source());
    if (!srcTree) { event->ignore(); return; }

    auto selected = srcTree->selectedItems();
    if (selected.isEmpty()) { event->ignore(); return; }

    // Map drop position to timeline tick and track
    QPointF pos = event->position();
    double px = pos.x() - headerWidth();
    int64_t tick = m_layoutEngine.pixelXToTime(px);
    if (tick < 0) tick = 0;
    size_t trackIdx = hitTestTrack(pos.y());
    bool aboveTopVideo = false;
    bool belowBottomAudio = false;
    computeGhostDropZones(pos, aboveTopVideo, belowBottomAudio);

    // Snap the drop tick to clip edges
    {
        auto snapRes = m_snapEngine.snap(tick);
        if (snapRes.didSnap) tick = snapRes.snappedTick;
    }

    // Wrap in a macro so Ctrl+Z undoes the whole multi-drop as one action.
    if (m_commandStack && selected.size() > 1)
        m_commandStack->beginMacro("Import Files");

    int64_t currentTick = tick;
    for (auto* item : selected) {
        // Skip bin (folder) items
        if (item->data(0, Qt::UserRole + 2).toBool()) continue;

        // Handle sequence items
        if (item->data(0, Qt::UserRole + 3).toBool()) {
            size_t seqIndex = item->data(0, Qt::UserRole + 4).toULongLong();
            size_t seqTrack = trackIdx;
            if (!ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
                seqTrack = kGhostDropTrackVideoAbove;
            emit sequenceDropped(seqIndex, currentTick, seqTrack);
            // Advance by 5-second default for sequences (duration unknown here)
            currentTick += static_cast<int64_t>(5.0 * 48000.0);
            continue;
        }

        QString filePath = item->data(0, Qt::UserRole).toString();
        uint64_t handle  = item->data(0, Qt::UserRole + 1).toULongLong();
        if ((ghostWasVisible || aboveTopVideo || belowBottomAudio) && !filePath.isEmpty()) {
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            const bool isAudioDrop = audioExts.contains(ext);
            if (!isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && ghostWasAbove) || aboveTopVideo))
                trackIdx = kGhostDropTrackVideoAbove;
            else if (isAudioDrop && !ghostWasOnExisting && ((ghostWasVisible && !ghostWasAbove) || belowBottomAudio))
                trackIdx = kGhostDropTrackAudioBelow;
        }
        if (!filePath.isEmpty()) {
            emit mediaDropped(filePath, handle, currentTick, trackIdx);

            // Advance tick for sequential placement
            int64_t advance = static_cast<int64_t>(5.0 * 48000.0);
            if (handle != 0 && m_mediaPool) {
                const auto* info = m_mediaPool->getInfo(handle);
                if (info && info->duration > 0.0)
                    advance = static_cast<int64_t>(info->duration * 48000.0);
            } else if (m_mediaPool) {
                auto h = m_mediaPool->open(filePath.toStdString());
                if (h != 0) {
                    const auto* info = m_mediaPool->getInfo(h);
                    if (info && info->duration > 0.0)
                        advance = static_cast<int64_t>(info->duration * 48000.0);
                }
            }
            currentTick += advance;
        }
    }

    if (m_commandStack && selected.size() > 1)
        m_commandStack->endMacro();

    event->acceptProposedAction();
}

} // namespace rt
