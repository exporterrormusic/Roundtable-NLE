// DropControllerMediaDrop.cpp - Media drag-drop signal wiring.
// Extracted from TimelineWorkspaceWiring.cpp for maintainability.

#include <volk.h>

#include <map>
#include <memory>
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
#include "timeline/AdjustmentClip.h"
#include "timeline/AudioClip.h"
#include "timeline/EditOperations.h"
#include "audio/AudioFile.h"
#include "PathUtils.h"
#include "timeline/ImageClip.h"
#include "timeline/OpacityMask.h"
#include "timeline/SequenceClip.h"
#include "timeline/SpineClip.h"
#include "timeline/PngPuppetClip.h"
#include "panels/characters/PuppetLibrary.h"
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
#include <QGuiApplication>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <spdlog/spdlog.h>

namespace rt {

// Returns true for still-image media. Such files have no real "source duration",
// so trim operations should treat them as unbounded (Premiere-style infinite extension).
static bool isStillImagePath(const std::string& path)
{
    QString ext = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    static const QStringList kImageExts = {
        "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff", "webp", "tga", "dds"
    };
    return kImageExts.contains(ext);
}

// ── Multi-stream audio drop (Premiere-style) ────────────────────────────────
// A multicam/broadcast file (e.g. an 8-track MXF) carries several discrete
// audio streams.  On drop we lay EACH stream onto its own audio track as one
// linked group — the caller's existing companion clip is stream 0, and these
// helpers add streams 1..N-1 on the consecutive audio tracks BELOW it, reusing
// existing audio tracks and creating new ones only when there aren't enough.

namespace {

struct AudioStreamSibling {
    uint64_t                 clipId{0};
    Track*                   track{nullptr};
    bool                     trackCreated{false};
    std::unique_ptr<Command> overlapCmd;
};

// Returns how many audio streams `path` has (0/1 = nothing to explode).
int probeAudioStreamCount(const std::string& path)
{
    return static_cast<int>(
        AudioFile::enumerateAudioStreams(utf8ToPath(path)).size());
}

// Create one AudioClip per stream 1..streamCount-1 on the audio tracks below
// baseAudioTkIdx.  Records each into `out` for undo.  Stream 0 is the caller's
// own companion clip (already placed) — set its ordinal to 0 separately.
void createAudioStreamSiblings(Timeline* tl, const std::string& path,
                               int64_t in, int64_t dur, int64_t srcDur,
                               int64_t sourceIn, const std::string& label,
                               uint64_t linkId, size_t baseAudioTkIdx,
                               int streamCount,
                               std::vector<AudioStreamSibling>& out)
{
    if (!tl) return;
    // Match a created track's height to the existing tracks so the new ones
    // don't tower over their neighbours (Track's default is 80px).
    float refTrackHeight = 0.0f;
    for (size_t ri = 0; ri < tl->trackCount(); ++ri) {
        Track* tr = tl->track(ri);
        if (!tr || tr->isDivider()) continue;
        if (tr->height() >= 1.0f) { refTrackHeight = tr->height(); break; }
    }
    for (int i = 1; i < streamCount; ++i) {
        const size_t targetIdx = baseAudioTkIdx + static_cast<size_t>(i);
        Track* t = nullptr;
        bool   created = false;
        if (targetIdx < tl->trackCount() && tl->track(targetIdx) &&
            tl->track(targetIdx)->type() == TrackType::Audio &&
            !tl->track(targetIdx)->isDivider()) {
            t = tl->track(targetIdx);                       // reuse existing track
        } else {
            // Empty name → Timeline auto-numbers it (A<next>), and the
            // rebuildTracks renumber pass corrects it to its final position —
            // i.e. normal A1/A2/A3… naming, never a hardcoded guess.
            t = tl->addAudioTrack("");
            if (t && refTrackHeight >= 1.0f) t->setHeight(refTrackHeight);
            created = true;
        }
        if (!t) continue;

        auto ac = std::make_unique<AudioClip>(path);
        ac->setTimelineIn(in);
        ac->setDuration(dur);
        ac->setSourceDuration(srcDur);
        ac->setSourceIn(sourceIn);
        ac->setLabel(label);
        ac->setAudioStreamIndex(i);     // this clip decodes stream i
        ac->setLinkId(linkId);          // same group as the video + siblings
        const uint64_t cid = ac->id();
        t->addClip(std::move(ac));

        size_t tIdx = SIZE_MAX;
        for (size_t k = 0; k < tl->trackCount(); ++k)
            if (tl->track(k) == t) { tIdx = k; break; }
        std::unique_ptr<Command> ov;
        if (tIdx != SIZE_MAX) {
            ov = EditOperations::resolveOverlaps(*tl, tIdx, cid);
            if (ov) ov->execute();
        }
        out.push_back({cid, t, created, std::move(ov)});
    }
}

// Reverse of the above (called from the drop command's undo).
void undoAudioStreamSiblings(Timeline* tl, std::vector<AudioStreamSibling>& sibs)
{
    if (!tl) { sibs.clear(); return; }
    for (auto it = sibs.rbegin(); it != sibs.rend(); ++it) {
        if (it->overlapCmd) it->overlapCmd->undo();
        size_t tIdx = SIZE_MAX;
        for (size_t k = 0; k < tl->trackCount(); ++k)
            if (tl->track(k) == it->track) { tIdx = k; break; }
        if (tIdx != SIZE_MAX) {
            tl->track(tIdx)->removeClipById(it->clipId);
            if (it->trackCreated) tl->removeTrack(tIdx);
        }
    }
    sibs.clear();
}

} // namespace

void DropController::wireMediaDropSignals()
{
    // =====================================================================
    //  MEDIA DRAG-DROP -> CREATE CLIP ON TIMELINE
    // =====================================================================
    if (m_ws->timelinePanel() && m_ws->timeline()) {
        connect(m_ws->timelinePanel(), &TimelinePanel::mediaDropped,
                this, [this](const QString& filePath, uint64_t /*mediaHandle*/,
                             int64_t atTick, size_t trackIndex,
                             int dragMode) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            const bool forceAudioOnly = (dragMode == TimelinePanel::DragAudioOnly);
            const bool forceVideoOnly = (dragMode == TimelinePanel::DragVideoOnly);

            // If no project or no sequences exist, prompt to create one
            if (!m_ws->project() || m_ws->project()->sequenceCount() == 0) {
                // For Spine animation drops, just create a default sequence
                if (filePath.startsWith(QStringLiteral("spine:"))) {
                    emit m_ws->requestNewProjectForMedia(QString(), atTick, trackIndex);
                    return;
                }
                uint32_t fileW = 0, fileH = 0;
                double fileFps = 30.0;
                if (m_ws->mediaPool()) {
                    uint64_t h = m_ws->mediaPool()->open(filePath.toStdString());
                    if (h != 0) {
                        const auto* info = m_ws->mediaPool()->getInfo(h);
                        if (info) {
                            fileW = info->width;
                            fileH = info->height;
                            if (info->fps > 0.0) fileFps = info->fps;
                        }
                    }
                }
                QString resolutionStr = (fileW > 0 && fileH > 0)
                    ? QString("%1 x %2").arg(fileW).arg(fileH)
                    : QString("Unknown");
                QString fpsStr = QString::number(fileFps, 'f', 2);
                auto result = QMessageBox::question(
                    m_ws->timelinePanel(), "Create Sequence",
                    QString("No sequence is open.\n\n"
                            "Do you want to create a new sequence with this media?\n\n"
                            "File: %1\n"
                            "Resolution: %2\n"
                            "Frame rate: %3 fps\n\n"
                            "A new project will be created automatically.")
                        .arg(QFileInfo(filePath).fileName())
                        .arg(resolutionStr).arg(fpsStr),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
                if (result == QMessageBox::Yes)
                    emit m_ws->requestNewProjectForMedia(filePath, atTick, trackIndex);
                return;
            }

            // ── PNG puppet drops (from the Library puppet sub-tab) ───────────
            // Format: "puppet:<folderName>|<variant>".  Builds a PngPuppetClip
            // resolved from the puppet's manifest and returns early — puppets
            // never touch MediaPool (they own their own 4-image pipeline).
            if (filePath.startsWith(QStringLiteral("puppet:"))) {
                const QString payload = filePath.mid(7);  // strip "puppet:"
                const int bar = payload.indexOf(QLatin1Char('|'));
                const QString folder  = bar >= 0 ? payload.left(bar) : payload;
                QString variant       = bar >= 0 ? payload.mid(bar + 1) : QStringLiteral("default");

                PuppetManifest manifest;
                if (!puppetlib::load(folder, manifest)) {
                    spdlog::warn("puppet drop: cannot load manifest for '{}'",
                                 folder.toStdString());
                    return;
                }
                if (!manifest.variants.contains(variant) && !manifest.variantOrder.isEmpty())
                    variant = manifest.variantOrder.first();
                const PuppetVariant var = manifest.variants.value(variant);

                // Resolve a video track (use the drop target if it's a video
                // track, else the first existing video track, else create one).
                Track* track = (trackIndex < m_ws->timeline()->trackCount())
                    ? m_ws->timeline()->track(trackIndex) : nullptr;
                bool createdTrack = false;
                if (!track || track->type() != TrackType::Video || track->isDivider()) {
                    track = nullptr;
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        Track* t = m_ws->timeline()->track(i);
                        if (t && t->type() == TrackType::Video && !t->isDivider()) {
                            track = t;
                            break;
                        }
                    }
                    if (!track) {
                        track = m_ws->timeline()->addVideoTrack("V1");
                        createdTrack = true;
                    }
                }
                if (!track) return;

                auto pc = std::make_unique<PngPuppetClip>(
                    manifest.displayName.toStdString(), variant.toStdString());
                for (int f = 0; f < puppetlib::kFaceCount; ++f)
                    pc->setFacePath(f, var.faces[static_cast<size_t>(f)].toStdString());
                pc->setTimelineIn(atTick);
                pc->setDuration(secondsToTicks(5.0));   // stretchable like any character
                pc->setLabel(manifest.displayName.toStdString());
                // Seed blink/talk jitter from the CHARACTER (folder), not the
                // clip id, so two clips of the same character cut together in
                // phase (motion is evaluated in global time — see the renderer).
                pc->setSeed(static_cast<uint32_t>(
                    std::hash<std::string>{}(folder.toStdString()) & 0xFFFFFFFFu));

                if (m_ws->commandStack())
                    m_ws->commandStack()->execute(std::make_unique<AddClipCommand>(track, std::move(pc)));
                else
                    track->addClip(std::move(pc));

                if (createdTrack) m_ws->timelinePanel()->rebuildTracks();
                else              m_ws->timelinePanel()->refreshTrackContents();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
                spdlog::info("Puppet dropped: '{}' variant '{}' at tick {}",
                             manifest.displayName.toStdString(), variant.toStdString(), atTick);
                return;
            }

            // ── Detect Spine animation drops (from Library CharactersPanel) ──
            // Format: "spine:charName|outfit|stanceInt|animName"
            bool isSpineAnimDrop = filePath.startsWith(QStringLiteral("spine:"));
            std::string spineCharName, spineOutfit, spineAnimName;
            int spineStanceInt = 0;
            if (isSpineAnimDrop) {
                QString payload = filePath.mid(6); // strip "spine:"
                QStringList parts = payload.split('|');
                if (parts.size() >= 4) {
                    spineCharName = parts[0].toStdString();
                    spineOutfit   = parts[1].toStdString();
                    spineStanceInt = parts[2].toInt();
                    spineAnimName = parts[3].toStdString();
                }
                spdlog::info("DIAG-DROP detected Spine animation: {} / {} / stance={} / {}",
                             spineCharName, spineOutfit, spineStanceInt, spineAnimName);
            }

            // Determine if this is an audio file by extension
            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            bool isAudio = audioExts.contains(ext);
            // Source-monitor "drag audio only": treat (possibly video) media
            // as audio → a single AudioClip, no video, no companion
            // (mediaHasAudio stays false because !isAudio is false below).
            if (forceAudioOnly) isAudio = true;

            // Pre-compute clip properties before the lambda captures
            std::string label = isSpineAnimDrop
                ? spineCharName + " - " + spineAnimName
                : QFileInfo(filePath).baseName().toStdString();
            std::string path  = filePath.toStdString();

            // Detect video character files so the clip gets full metadata
            std::string vcCharName, vcMutePath, vcTalkPath;
            std::string vcOutfit, vcAnimName;
            float vcPosX = 0.0f, vcPosY = 0.0f, vcScale = 1.0f, vcOpacity = 1.0f;
            bool vcIsTalking = false;
            bool isCharacterClip = false;
            {
                std::string lowerName = QFileInfo(filePath).fileName().toLower().toStdString();
                const auto& vcTable = videoCharacterFiles();
                auto vcIt = vcTable.find(lowerName);
                if (vcIt != vcTable.end()) {
                    vcCharName = vcIt->second.charName;
                    vcMutePath = vcIt->second.mutePath;
                    vcTalkPath = vcIt->second.talkPath;
                    // For character/animation clips, label = "CHARACTER - ANIMATION"
                    label = vcCharName;
                    isCharacterClip = true;
                }

                // Fallback: detect character from AnimationVideoCache path.
                // Cache files live under assets/converted/{format}/{charName}/{outfit}/{anim}.ext
                if (vcCharName.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/converted/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        // rest = "{format}/{charName}/{outfit}/{anim}.ext"
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            std::string fmtName = rest.substr(0, slash1);
                            std::string afterFmt = rest.substr(slash1 + 1);
                            auto slash2 = afterFmt.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcCharName = afterFmt.substr(0, slash2);
                                std::string afterChar = afterFmt.substr(slash2 + 1);
                                auto slash3 = afterChar.find('/');
                                if (slash3 != std::string::npos && slash3 > 0) {
                                    vcOutfit = afterChar.substr(0, slash3);
                                    std::string fileName = afterChar.substr(slash3 + 1);
                                    // Remove extension to get animation name
                                auto dotPos = fileName.rfind('.');
                                std::string stem = (dotPos != std::string::npos)
                                    ? fileName.substr(0, dotPos) : fileName;
                                std::string extStr = (dotPos != std::string::npos)
                                    ? fileName.substr(dotPos) : "";
                                // Detect _talk suffix
                                const std::string talkSuffix = "_talk";
                                if (stem.size() > talkSuffix.size() &&
                                    stem.compare(stem.size() - talkSuffix.size(),
                                                 talkSuffix.size(), talkSuffix) == 0) {
                                    // Dragged file is the talk variant
                                    std::string baseStem = stem.substr(0, stem.size() - talkSuffix.size());
                                    vcAnimName = baseStem;
                                    vcIsTalking = true;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName + "/" + vcOutfit + "/";
                                    vcMutePath = prefix + baseStem + extStr;
                                    vcTalkPath = prefix + stem + extStr;
                                } else {
                                    vcAnimName = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName + "/" + vcOutfit + "/";
                                    vcMutePath = prefix + stem + extStr;
                                    vcTalkPath = prefix + stem + talkSuffix + extStr;
                                }
                                // For character/animation clips, label = "CHARACTER - ANIMATION"
                                label = vcCharName + " - " + vcAnimName;
                                isCharacterClip = true;
                                }
                            }
                        }
                    }
                }

                // Load the default shot preset to get character transform
                if (!vcCharName.empty() && m_ws->shotPresetManager()) {
                    std::string presetName = vcCharName + " (Default)";
                    auto preset = m_ws->shotPresetManager()->load(presetName);
                    if (preset) {
                        for (int ci2 = 0; ci2 < preset->characterCount(); ++ci2) {
                            auto* ch = preset->character(ci2);
                            if (!ch || ch->characterName != vcCharName || !ch->isVideoCharacter())
                                continue;
                            constexpr float cW = 1920.0f, cH = 1080.0f;
                            vcPosX    = (ch->posX - 0.5f) * cW;
                            vcPosY    = (ch->posY - 0.5f) * cH;
                            vcScale   = ch->scale;
                            vcOpacity = ch->opacity;
                            vcIsTalking = ch->isTalking;
                            break;
                        }
                    }
                }
            }

            // For all other (non-character) clips, label is always the original filename (no extension)
            if (!isCharacterClip) {
                label = QFileInfo(QString::fromStdString(path)).baseName().toStdString();
            }
            int64_t dur = secondsToTicks(5.0);
            double sourceFps = 0.0;
            bool mediaHasAudio = false;
            uint32_t srcW = 0, srcH = 0;
            // Spine animation drops use a virtual "spine:" URI scheme.
            // Never feed that to MediaPool::open — it tries to decode it as
            // a video file, fails, logs an error, and caches the URI as a
            // failed media handle.  SpineClip uses its own pipeline.
            if (m_ws->mediaPool() && !isSpineAnimDrop) {
                auto handle = m_ws->mediaPool()->open(path);
                if (handle != InvalidMedia) {
                    const auto* info = m_ws->mediaPool()->getInfo(handle);
                    if (info) {
                        spdlog::info("DIAG-DROP mediaDropped '{}': info->duration={:.3f}s, "
                                     "frameCount={}, fps={:.2f}, hasAudio={}, videoIdx={}, audioIdx={}",
                                     path, info->duration, info->frameCount, info->fps,
                                     info->hasAudio, info->videoStreamIndex, info->audioStreamIndex);
                        sourceFps = info->fps;
                        srcW = info->width;
                        srcH = info->height;
                    }
                    // Keep the 5-second default for character animation clips so
                    // the animation loops for a full 5 seconds (Premiere Pro behavior).
                    // Still images (JPG/PNG/...) also stay at the 5-second
                    // default — FFmpeg's image2 demuxer reports JPG as a tiny
                    // 1-frame "video" (e.g. 0.04s @ 25fps), which would
                    // otherwise shrink the dropped clip to a single frame.
                    if (info && info->duration > 0 && !isCharacterClip
                        && !isStillImagePath(path))
                        dur = secondsToTicks(info->duration);
                    if (info && info->hasAudio && !isAudio)
                        mediaHasAudio = true;
                }
            }
            // Source-monitor "drag video only": never spawn the companion
            // audio clip even though the media has an audio stream.
            if (forceVideoOnly) mediaHasAudio = false;
            spdlog::info("DIAG-DROP mediaDropped '{}': dur={} ticks ({:.3f}s), mediaHasAudio={}",
                         path, dur, ticksToSeconds(dur), mediaHasAudio);

            // Find the target track (don't create one yet — that happens inside the command)
            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            const bool forceGhostAudioTrack = (trackIndex == (SIZE_MAX - 2));
            // Audio-companion sentinel: video lands on its normal target
            // (bottom existing video), but the audio companion needs a fresh
            // audio track at the bottom. Video routing falls through to the
            // !isAudio branch below; the audio-companion branch later in
            // this lambda picks up the "force new audio track" flag.
            const bool forceGhostAudioCompanion = (trackIndex == (SIZE_MAX - 3));

            if (isAudio && forceGhostAudioTrack) {
                needsNewTrack = true;
            } else if (!isAudio && forceGhostVideoTrack) {
                needsNewTrack = true;
            } else if (isAudio) {
                if (trackIndex < m_ws->timeline()->trackCount() &&
                    m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio &&
                    !m_ws->timeline()->track(trackIndex)->isDivider())
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        Track* tr = m_ws->timeline()->track(i);
                        if (tr->type() == TrackType::Audio && !tr->isDivider()) {
                            targetTrackIdx = i;
                            break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            } else {
                // Video routing — for forceGhostAudioCompanion the cursor is
                // in the audio-below zone, so trackIndex is the sentinel
                // (not a real index); fall through to the bottom-video
                // fallback below.
                if (!forceGhostAudioCompanion &&
                    trackIndex < m_ws->timeline()->trackCount() &&
                    m_ws->timeline()->track(trackIndex)->type() == TrackType::Video &&
                    !m_ws->timeline()->track(trackIndex)->isDivider())
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    // Drop-on-audio fallback: land on the BOTTOM video layer
                    // (the one closest to the V/A divider — that's where the
                    // user is dropping, and matches Premiere's behaviour).
                    // Scan BACKWARD for the highest-index video, SKIPPING the
                    // divider itself (it's TrackType::Video but rejects clips).
                    for (size_t i = m_ws->timeline()->trackCount(); i > 0; --i) {
                        Track* tr = m_ws->timeline()->track(i - 1);
                        if (tr->type() == TrackType::Video && !tr->isDivider()) {
                            targetTrackIdx = i - 1;
                            break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            }

            spdlog::info("DIAG-DROP mediaDropped routing: dropTrackIdx={} isAudio={} forceGhostVideo={} forceGhostAudio={} targetTrackIdx={} needsNewTrack={}",
                         trackIndex, isAudio, forceGhostVideoTrack, forceGhostAudioTrack,
                         targetTrackIdx, needsNewTrack);

            // Shared state for undo/redo: track the clip ID and whether we created a track
            auto clipId    = std::make_shared<uint64_t>(0);
            auto createdTk = std::make_shared<bool>(false);
            auto tkIdx     = std::make_shared<size_t>(targetTrackIdx);
            // Track overlap resolution state for undo
            auto overlapCmd = std::make_shared<std::unique_ptr<Command>>(nullptr);

            // Audio-companion state for video files that contain audio.
            // If the user dropped directly on an audio track, route the audio
            // companion there (mirrors the "drop video higher to pick upper
            // video track" behavior).  Otherwise fall back to the first audio
            // track.
            // Multi-stream (multicam/broadcast MXF): how many discrete audio
            // streams the source has.  Probed first because it decides the
            // companion's target track below.
            const int audioStreamCount =
                mediaHasAudio ? probeAudioStreamCount(path) : 0;

            size_t audioTargetIdx = SIZE_MAX;
            bool needsNewAudioTrack = false;
            if (mediaHasAudio) {
                if (audioStreamCount > 1) {
                    // Multi-stream: START on the audio track under the cursor
                    // (just like a normal file), or the FIRST audio track when
                    // the cursor is on a video track.  When the cursor is in the
                    // audio-BELOW ghost zone, create a fresh block starting just
                    // below the existing tracks — every stream is a new track
                    // (matches the ghost dragged below the stack).
                    if (forceGhostAudioCompanion) {
                        needsNewAudioTrack = true;
                    } else if (trackIndex < m_ws->timeline()->trackCount() &&
                        m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio &&
                        !m_ws->timeline()->track(trackIndex)->isDivider()) {
                        audioTargetIdx = trackIndex;
                    }
                    if (!needsNewAudioTrack && audioTargetIdx == SIZE_MAX) {
                        for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                            Track* tr = m_ws->timeline()->track(i);
                            if (tr && tr->type() == TrackType::Audio && !tr->isDivider()) {
                                audioTargetIdx = i;
                                break;
                            }
                        }
                    }
                    if (audioTargetIdx == SIZE_MAX) needsNewAudioTrack = true;
                } else if (forceGhostAudioCompanion) {
                    // Force a fresh audio track at the bottom for the
                    // companion when the user dropped in the audio-below
                    // ghost zone with a video+audio file.
                    needsNewAudioTrack = true;
                } else {
                    if (trackIndex < m_ws->timeline()->trackCount() &&
                        m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio &&
                        !m_ws->timeline()->track(trackIndex)->isDivider()) {
                        audioTargetIdx = trackIndex;
                    }
                    if (audioTargetIdx == SIZE_MAX) {
                        for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                            Track* tr = m_ws->timeline()->track(i);
                            if (tr->type() == TrackType::Audio && !tr->isDivider()) {
                                audioTargetIdx = i;
                                break;
                            }
                        }
                    }
                    if (audioTargetIdx == SIZE_MAX) needsNewAudioTrack = true;
                }
            }
            auto audioClipId      = std::make_shared<uint64_t>(0);
            auto audioCreatedTk   = std::make_shared<bool>(false);
            auto audioTkIdx       = std::make_shared<size_t>(audioTargetIdx);
            auto audioOverlapCmd  = std::make_shared<std::unique_ptr<Command>>(nullptr);
            auto audioSiblings =
                std::make_shared<std::vector<AudioStreamSibling>>();

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (m_ws->isDestroying()) return;
                if (trackStructureChanged)
                    m_ws->timelinePanel()->rebuildTracks();
                else
                    m_ws->timelinePanel()->refreshTrackContents();
                m_ws->invalidateAudioSources();
                m_ws->invalidateCompositeCache();
                m_ws->warmAudioCacheAsync();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
            };

            if (m_ws->commandStack()) {
                auto cmd = std::make_unique<LambdaCommand>(
                    "Add Media to Timeline",
                    /* execute / redo */
                    [this, isAudio, isSpineAnimDrop, mediaHasAudio, path, label, atTick, dur, sourceFps,
                     srcW, srcH,
                     needsNewTrack, needsNewAudioTrack, forceGhostVideoTrack, forceGhostAudioTrack,
                     clipId, createdTk, tkIdx, overlapCmd,
                     audioClipId, audioCreatedTk, audioTkIdx, audioOverlapCmd,
                     audioStreamCount, audioSiblings,
                     refreshAfter,
                     vcCharName, vcMutePath, vcTalkPath, vcOutfit, vcAnimName,
                     vcPosX, vcPosY, vcScale, vcOpacity, vcIsTalking,
                     spineCharName, spineOutfit, spineStanceInt, spineAnimName]() {
                        // Create track if needed
                        if (needsNewTrack && *tkIdx == SIZE_MAX) {
                            // Snapshot the current "standard" track height
                            // BEFORE creating the new track so we can match
                            // it. Otherwise the new track keeps Track's
                            // default 80px, which (a) towers over collapsed
                            // existing tracks, and (b) becomes the first
                            // non-divider track when inserted at index 0,
                            // making rebuildTracks recompute dividerHeight
                            // from it and scale every divider with it.
                            float refTrackHeight = 0.0f;
                            for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                                Track* tr = m_ws->timeline()->track(ri);
                                if (!tr || tr->isDivider()) continue;
                                float h = tr->height();
                                if (h >= 1.0f) { refTrackHeight = h; break; }
                            }

                            Track* t = nullptr;
                            if (!isAudio && forceGhostVideoTrack) {
                                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                                if (refTrackHeight >= 1.0f)
                                    newTrack->setHeight(refTrackHeight);
                                t = m_ws->timeline()->insertTrack(0, std::move(newTrack));
                            } else if (isAudio && forceGhostAudioTrack) {
                                t = m_ws->timeline()->addAudioTrack("A1");
                                if (t && refTrackHeight >= 1.0f)
                                    t->setHeight(refTrackHeight);
                            } else {
                                t = isAudio ? m_ws->timeline()->addAudioTrack("A1")
                                            : m_ws->timeline()->addVideoTrack("V1");
                                if (t && refTrackHeight >= 1.0f)
                                    t->setHeight(refTrackHeight);
                            }
                            // Find the index of the newly added track
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i) == t) { *tkIdx = i; break; }
                            }
                            *createdTk = true;
                        }
                        Track* track = m_ws->timeline()->track(*tkIdx);
                        if (!track) return;
                        spdlog::info("DIAG-DROP mediaDropped execute: resolved track idx={} name='{}' type={}",
                                     *tkIdx, track->name(),
                                     track->type() == TrackType::Video ? "video" : "audio");

                        std::unique_ptr<Clip> clip;
                        if (isSpineAnimDrop) {
                            // Create a SpineClip for animation drops from the Library panel
                            auto sc = std::make_unique<SpineClip>(spineCharName, spineOutfit);
                            CharacterStance stance = CharacterStance::Default;
                            if (spineStanceInt == 1) stance = CharacterStance::Aim;
                            else if (spineStanceInt == 2) stance = CharacterStance::Cover;
                            sc->setStance(stance);
                            sc->setAnimationName(spineAnimName);
                            sc->setLooping(true);
                            sc->setTimelineIn(atTick);
                            sc->setDuration(dur);
                            sc->setLabel(label);
                            clip = std::move(sc);
                        } else if (isAudio) {
                            auto ac = std::make_unique<AudioClip>(path);
                            ac->setTimelineIn(atTick);
                            ac->setDuration(dur);
                            ac->setSourceDuration(dur);
                            ac->setLabel(label);
                            clip = std::move(ac);
                        } else {
                            auto vc = std::make_unique<VideoClip>(path);
                            vc->setTimelineIn(atTick);
                            vc->setDuration(dur);
                            // Still images have no real source duration — treat as unbounded.
                            vc->setSourceDuration(isStillImagePath(path) ? 0 : dur);
                            vc->setSourceFps(sourceFps);
                            // Record native source resolution so Effect Controls
                            // can show Premiere-style native-pixel Scale %.
                            if (srcW > 0 && srcH > 0)
                                vc->setSourceResolution(srcW, srcH);
                            vc->setLabel(label);
                            if (!vcCharName.empty()) {
                                vc->setCharacterName(vcCharName);
                                vc->setVideoMutePath(vcMutePath);
                                vc->setVideoTalkPath(vcTalkPath);
                                vc->setOutfit(vcOutfit);
                                vc->setAnimationName(vcAnimName);
                                vc->setTalking(vcIsTalking);
                                vc->positionX().setDefaultValue(vcPosX);
                                vc->positionY().setDefaultValue(vcPosY);
                                vc->scaleX().setDefaultValue(vcScale);
                                vc->scaleY().setDefaultValue(vcScale);
                                vc->opacity().setDefaultValue(vcOpacity);
                            }
                            clip = std::move(vc);
                        }
                        *clipId = clip->id();
                        // Link the video to its audio companion (Premiere-
                        // style A/V link). Both clips share the video's id
                        // as their linkId; the selection logic auto-includes
                        // partners on click unless Alt is held.
                        if (mediaHasAudio)
                            clip->setLinkId(*clipId);
                        spdlog::info("DIAG-DROP mediaDropped clip id={} type={} "
                                     "timelineIn={} dur={} ({:.3f}s) sourceIn={} srcDur={}",
                                     *clipId, isAudio ? "audio" : "video",
                                     atTick, dur, dur/48000.0, 0, dur);
                        // Ctrl held at drop = Premiere-style INSERT: ripple-push
                        // existing clips on this track (and sync-locked tracks)
                        // right by `dur` so the new clip slots in instead of
                        // overwriting. resolveOverlaps below is a no-op if the
                        // ripple was complete; we keep it as a safety net for
                        // straddle cases that openGap intentionally skips.
                        const bool insertMode =
                            (QGuiApplication::keyboardModifiers() & Qt::ControlModifier);
                        if (insertMode && *tkIdx < m_ws->timeline()->trackCount()) {
                            auto openCmd = EditOperations::openGap(
                                *m_ws->timeline(), *tkIdx, atTick, dur);
                            if (openCmd) openCmd->execute();
                        }
                        track->addClip(std::move(clip));

                        // Resolve overlaps (overwrite like Premiere Pro)
                        *overlapCmd = EditOperations::resolveOverlaps(
                            *m_ws->timeline(), *tkIdx, *clipId);
                        if (*overlapCmd) (*overlapCmd)->execute();

                        // Verify clip state after overlap resolution
                        {
                            size_t vi = track->findClipIndexById(*clipId);
                            if (vi < track->clipCount()) {
                                const auto* c = track->clip(vi);
                                spdlog::info("DIAG-DROP mediaDropped VERIFY clip id={} "
                                             "in={} dur={} ({:.3f}s) srcIn={} out={}",
                                             c->id(), c->timelineIn(), c->duration(),
                                             c->duration()/48000.0, c->sourceIn(),
                                             c->timelineOut());
                            }
                        }

                        // -- Create companion AudioClip for video+audio media --
                        if (mediaHasAudio) {
                            if (needsNewAudioTrack && *audioTkIdx == SIZE_MAX) {
                                // Snapshot existing track height before adding,
                                // so the companion audio track doesn't tower
                                // over its neighbours.
                                float refTrackHeight = 0.0f;
                                for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                                    Track* tr = m_ws->timeline()->track(ri);
                                    if (!tr || tr->isDivider()) continue;
                                    float h = tr->height();
                                    if (h >= 1.0f) { refTrackHeight = h; break; }
                                }
                                Track* at = m_ws->timeline()->addAudioTrack("A1");
                                if (at && refTrackHeight >= 1.0f)
                                    at->setHeight(refTrackHeight);
                                for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                    if (m_ws->timeline()->track(i) == at) { *audioTkIdx = i; break; }
                                }
                                *audioCreatedTk = true;
                            } else if (*audioTkIdx != SIZE_MAX) {
                                // Re-validate audio track index — a new video track may have been
                                // inserted at index 0 above, shifting all existing track indices.
                                if (*audioTkIdx >= m_ws->timeline()->trackCount() ||
                                    m_ws->timeline()->track(*audioTkIdx)->type() != TrackType::Audio) {
                                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                        if (m_ws->timeline()->track(i)->type() == TrackType::Audio) {
                                            *audioTkIdx = i;
                                            break;
                                        }
                                    }
                                }
                            }
                            Track* audioTrack = m_ws->timeline()->track(*audioTkIdx);
                            if (audioTrack) {
                                auto ac = std::make_unique<AudioClip>(path);
                                ac->setTimelineIn(atTick);
                                ac->setDuration(dur);
                                ac->setSourceDuration(dur);
                                ac->setLabel(label);
                                ac->setLinkId(*clipId);  // pair with companion video
                                // Multicam/broadcast source: this companion plays
                                // stream 0; the other streams get their own clips
                                // on the tracks below (Premiere-style).
                                if (audioStreamCount > 1)
                                    ac->setAudioStreamIndex(0);
                                *audioClipId = ac->id();
                                spdlog::info("DIAG-DROP mediaDropped audioCompanion id={} "
                                             "in={} dur={} ({:.3f}s) streams={}",
                                             *audioClipId, atTick, dur, dur/48000.0,
                                             audioStreamCount);
                                // Mirror the insert on the audio track when Ctrl
                                // was held — the companion needs the same room
                                // as the video so the pair stays in sync.
                                if (insertMode && *audioTkIdx < m_ws->timeline()->trackCount()) {
                                    auto openAudio = EditOperations::openGap(
                                        *m_ws->timeline(), *audioTkIdx, atTick, dur);
                                    if (openAudio) openAudio->execute();
                                }
                                audioTrack->addClip(std::move(ac));
                                *audioOverlapCmd = EditOperations::resolveOverlaps(
                                    *m_ws->timeline(), *audioTkIdx, *audioClipId);
                                if (*audioOverlapCmd) (*audioOverlapCmd)->execute();

                                // Lay every remaining audio stream onto its own
                                // track below, as one linked group.
                                if (audioStreamCount > 1) {
                                    createAudioStreamSiblings(
                                        m_ws->timeline(), path, atTick, dur, dur,
                                        /*sourceIn=*/0, label, *clipId, *audioTkIdx,
                                        audioStreamCount, *audioSiblings);
                                }
                            }
                        }

                        const bool trackStructureChanged =
                            (*createdTk || *audioCreatedTk || !audioSiblings->empty());
                        refreshAfter(trackStructureChanged);
                    },
                    /* undo */
                    [this, clipId, createdTk, tkIdx, overlapCmd,
                     mediaHasAudio, audioClipId, audioCreatedTk, audioTkIdx, audioOverlapCmd,
                     audioSiblings, refreshAfter]() {
                        const bool trackStructureChanged =
                            (*createdTk || *audioCreatedTk || !audioSiblings->empty());
                        // Undo audio companion first
                        if (mediaHasAudio) {
                            // Remove the per-stream siblings (+ any tracks they
                            // created) before the companion, since they sit below.
                            undoAudioStreamSiblings(m_ws->timeline(), *audioSiblings);
                            if (*audioOverlapCmd) (*audioOverlapCmd)->undo();
                            if (*audioTkIdx < m_ws->timeline()->trackCount()) {
                                Track* at = m_ws->timeline()->track(*audioTkIdx);
                                if (at) at->removeClipById(*audioClipId);
                            }
                            if (*audioCreatedTk) {
                                m_ws->timeline()->removeTrack(*audioTkIdx);
                                *audioTkIdx = SIZE_MAX;
                                *audioCreatedTk = false;
                            }
                        }

                        // Undo overlap resolution first
                        if (*overlapCmd) (*overlapCmd)->undo();

                        if (*tkIdx < m_ws->timeline()->trackCount()) {
                            Track* track = m_ws->timeline()->track(*tkIdx);
                            if (track) track->removeClipById(*clipId);
                        }
                        if (*createdTk) {
                            m_ws->timeline()->removeTrack(*tkIdx);
                            *tkIdx = SIZE_MAX;
                            *createdTk = false;
                        }
                        refreshAfter(trackStructureChanged);
                    }
                );
                m_ws->commandStack()->execute(std::move(cmd));
            } else {
                // No command stack — fall back to direct add
                Track* track = nullptr;
                bool trackStructureChanged = false;
                if (needsNewTrack) {
                    // Snapshot height of an existing real track so the new
                    // one doesn't tower over its neighbours and drag the
                    // divider heights up with it via rebuildTracks'
                    // dividerHeight = refTrackHeight * 0.25 rule.
                    float refTrackHeight = 0.0f;
                    for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                        Track* tr = m_ws->timeline()->track(ri);
                        if (!tr || tr->isDivider()) continue;
                        float h = tr->height();
                        if (h >= 1.0f) { refTrackHeight = h; break; }
                    }
                    if (!isAudio && forceGhostVideoTrack) {
                        auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                        if (refTrackHeight >= 1.0f)
                            newTrack->setHeight(refTrackHeight);
                        track = m_ws->timeline()->insertTrack(0, std::move(newTrack));
                        trackStructureChanged = true;
                    } else if (isAudio && forceGhostAudioTrack) {
                        track = m_ws->timeline()->addAudioTrack("A1");
                        if (track && refTrackHeight >= 1.0f)
                            track->setHeight(refTrackHeight);
                        trackStructureChanged = true;
                    } else {
                        track = isAudio ? m_ws->timeline()->addAudioTrack("A1")
                                        : m_ws->timeline()->addVideoTrack("V1");
                        if (track && refTrackHeight >= 1.0f)
                            track->setHeight(refTrackHeight);
                        trackStructureChanged = true;
                    }
                }
                else
                    track = m_ws->timeline()->track(targetTrackIdx);
                if (!track) return;

                std::unique_ptr<Clip> clip;
                if (isAudio) {
                    auto ac = std::make_unique<AudioClip>(path);
                    ac->setTimelineIn(atTick);
                    ac->setDuration(dur);
                    ac->setSourceDuration(dur);
                    ac->setLabel(label);
                    clip = std::move(ac);
                } else {
                    auto vc = std::make_unique<VideoClip>(path);
                    vc->setTimelineIn(atTick);
                    vc->setDuration(dur);
                    // Still images have no real source duration — treat as unbounded.
                    vc->setSourceDuration(isStillImagePath(path) ? 0 : dur);
                    vc->setSourceFps(sourceFps);
                    if (srcW > 0 && srcH > 0)
                        vc->setSourceResolution(srcW, srcH);
                    vc->setLabel(label);
                    if (!vcCharName.empty()) {
                        vc->setCharacterName(vcCharName);
                        vc->setVideoMutePath(vcMutePath);
                        vc->setVideoTalkPath(vcTalkPath);
                        vc->setOutfit(vcOutfit);
                        vc->setAnimationName(vcAnimName);
                        vc->setTalking(vcIsTalking);
                        vc->positionX().setDefaultValue(vcPosX);
                        vc->positionY().setDefaultValue(vcPosY);
                        vc->scaleX().setDefaultValue(vcScale);
                        vc->scaleY().setDefaultValue(vcScale);
                        vc->opacity().setDefaultValue(vcOpacity);
                    }
                    clip = std::move(vc);
                }
                uint64_t cid = clip->id();
                if (mediaHasAudio)
                    clip->setLinkId(cid);  // pair with companion audio
                track->addClip(std::move(clip));

                // Resolve overlaps in fallback path
                size_t fbTkIdx = SIZE_MAX;
                for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                    if (m_ws->timeline()->track(i) == track) { fbTkIdx = i; break; }
                }
                if (fbTkIdx != SIZE_MAX) {
                    auto cmd = EditOperations::resolveOverlaps(*m_ws->timeline(), fbTkIdx, cid);
                    if (cmd) cmd->execute();
                }

                // -- Create companion AudioClip for video+audio media (fallback path) --
                if (mediaHasAudio) {
                    Track* audioTrack = nullptr;
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        if (m_ws->timeline()->track(i)->type() == TrackType::Audio) {
                            audioTrack = m_ws->timeline()->track(i);
                            break;
                        }
                    }
                    if (!audioTrack) {
                        // Snapshot height before creating, so the new audio
                        // track matches the existing standard track height
                        // instead of using Track's default 80px.
                        float refTrackHeight = 0.0f;
                        for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                            Track* tr = m_ws->timeline()->track(ri);
                            if (!tr || tr->isDivider()) continue;
                            float h = tr->height();
                            if (h >= 1.0f) { refTrackHeight = h; break; }
                        }
                        audioTrack = m_ws->timeline()->addAudioTrack("A1");
                        if (audioTrack && refTrackHeight >= 1.0f)
                            audioTrack->setHeight(refTrackHeight);
                        trackStructureChanged = true;
                    }
                    if (audioTrack) {
                        auto ac = std::make_unique<AudioClip>(path);
                        ac->setTimelineIn(atTick);
                        ac->setDuration(dur);
                        ac->setSourceDuration(dur);
                        ac->setLabel(label);
                        ac->setLinkId(cid);  // pair with companion video
                        uint64_t acid = ac->id();
                        audioTrack->addClip(std::move(ac));
                        size_t fbAudioIdx = SIZE_MAX;
                        for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                            if (m_ws->timeline()->track(i) == audioTrack) { fbAudioIdx = i; break; }
                        }
                        if (fbAudioIdx != SIZE_MAX) {
                            auto acmd = EditOperations::resolveOverlaps(*m_ws->timeline(), fbAudioIdx, acid);
                            if (acmd) acmd->execute();
                        }
                    }
                }

                refreshAfter(trackStructureChanged);
            }

            spdlog::info("Media dropped on timeline: '{}' at tick {}",
                         path, atTick);
        });

        // Source Monitor drag with in/out region
        connect(m_ws->timelinePanel(), &TimelinePanel::mediaDroppedWithRegion,
                this, [this](const QString& filePath, uint64_t /*mediaHandle*/,
                             int64_t atTick, size_t trackIndex,
                             int64_t sourceIn, int64_t sourceOut,
                             int dragMode) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;
            const bool forceAudioOnly = (dragMode == TimelinePanel::DragAudioOnly);
            const bool forceVideoOnly = (dragMode == TimelinePanel::DragVideoOnly);

            QString ext = QFileInfo(filePath).suffix().toLower();
            static const QStringList audioExts = {
                "wav", "mp3", "ogg", "flac", "aac", "m4a", "wma", "aiff", "opus"
            };
            bool isAudio = audioExts.contains(ext);
            if (forceAudioOnly) isAudio = true;

            std::string label = QFileInfo(filePath).baseName().toStdString();
            std::string path  = filePath.toStdString();
            int64_t dur = sourceOut - sourceIn;
            if (dur <= 0) dur = secondsToTicks(5.0);

            spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': sourceIn={} ({:.3f}s) "
                         "sourceOut={} ({:.3f}s) dur={} ({:.3f}s)",
                         path, sourceIn, sourceIn/48000.0,
                         sourceOut, sourceOut/48000.0, dur, dur/48000.0);

            // Query full media duration for sourceDuration (extent limit)
            int64_t sourceDur = dur;  // fallback: region length
            double sourceFps = 0.0;
            bool mediaHasAudio = false;
            if (m_ws->mediaPool()) {
                auto handle = m_ws->mediaPool()->open(path);
                if (handle != InvalidMedia) {
                    const auto* info = m_ws->mediaPool()->getInfo(handle);
                    if (info) {
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': info->duration={:.3f}s, "
                                     "frameCount={}, fps={:.2f}, hasAudio={}",
                                     path, info->duration, info->frameCount, info->fps, info->hasAudio);
                        sourceFps = info->fps;
                    }
                    if (info && info->duration > 0)
                        sourceDur = secondsToTicks(info->duration);
                    if (info && info->hasAudio && !isAudio)
                        mediaHasAudio = true;
                }
            }
            // Source-monitor "drag video only": suppress the companion audio.
            if (forceVideoOnly) mediaHasAudio = false;
            spdlog::info("DIAG-DROP mediaDroppedWithRegion '{}': final dur={} ({:.3f}s) "
                         "sourceDur={} ({:.3f}s) mediaHasAudio={}",
                         path, dur, dur/48000.0, sourceDur, sourceDur/48000.0, mediaHasAudio);

            // Detect video character files
            std::string vcCharName2, vcMutePath2, vcTalkPath2;
            std::string vcOutfit2, vcAnimName2;
            float vcPosX2 = 0.0f, vcPosY2 = 0.0f, vcScale2 = 1.0f, vcOpacity2 = 1.0f;
            bool vcIsTalking2 = false;
            {
                std::string lowerName = QFileInfo(filePath).fileName().toLower().toStdString();
                const auto& vcTable = videoCharacterFiles();
                auto vcIt = vcTable.find(lowerName);
                if (vcIt != vcTable.end()) {
                    vcCharName2 = vcIt->second.charName;
                    vcMutePath2 = vcIt->second.mutePath;
                    vcTalkPath2 = vcIt->second.talkPath;
                    label = vcCharName2;
                }

                // Fallback: detect character from AnimationVideoCache path.
                // Cache files live under assets/converted/{format}/{charName}/{outfit}/{anim}.ext
                if (vcCharName2.empty()) {
                    std::string generic = std::filesystem::path(path).generic_string();
                    const std::string marker = "assets/converted/";
                    auto pos = generic.find(marker);
                    if (pos != std::string::npos) {
                        std::string rest = generic.substr(pos + marker.size());
                        // rest = "{format}/{charName}/{outfit}/{anim}.ext"
                        auto slash1 = rest.find('/');
                        if (slash1 != std::string::npos && slash1 > 0) {
                            std::string fmtName = rest.substr(0, slash1);
                            std::string afterFmt = rest.substr(slash1 + 1);
                            auto slash2 = afterFmt.find('/');
                            if (slash2 != std::string::npos && slash2 > 0) {
                                vcCharName2 = afterFmt.substr(0, slash2);
                                label = vcCharName2;
                                std::string afterChar = afterFmt.substr(slash2 + 1);
                                auto slash3 = afterChar.find('/');
                                if (slash3 != std::string::npos && slash3 > 0) {
                                    vcOutfit2 = afterChar.substr(0, slash3);
                                    std::string fileName = afterChar.substr(slash3 + 1);
                                auto dotPos = fileName.rfind('.');
                                std::string stem = (dotPos != std::string::npos)
                                    ? fileName.substr(0, dotPos) : fileName;
                                std::string extStr = (dotPos != std::string::npos)
                                    ? fileName.substr(dotPos) : "";
                                const std::string talkSuffix = "_talk";
                                if (stem.size() > talkSuffix.size() &&
                                    stem.compare(stem.size() - talkSuffix.size(),
                                                 talkSuffix.size(), talkSuffix) == 0) {
                                    std::string baseStem = stem.substr(0, stem.size() - talkSuffix.size());
                                    vcAnimName2 = baseStem;
                                    vcIsTalking2 = true;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + baseStem + extStr;
                                    vcTalkPath2 = prefix + stem + extStr;
                                } else {
                                    vcAnimName2 = stem;
                                    std::string prefix = generic.substr(0, pos) + marker
                                        + fmtName + "/" + vcCharName2 + "/" + vcOutfit2 + "/";
                                    vcMutePath2 = prefix + stem + extStr;
                                    vcTalkPath2 = prefix + stem + talkSuffix + extStr;
                                }
                                }
                            }
                        }
                    }
                }

                if (!vcCharName2.empty() && m_ws->shotPresetManager()) {
                    std::string presetName = vcCharName2 + " (Default)";
                    auto preset = m_ws->shotPresetManager()->load(presetName);
                    if (preset) {
                        for (int ci2 = 0; ci2 < preset->characterCount(); ++ci2) {
                            auto* ch = preset->character(ci2);
                            if (!ch || ch->characterName != vcCharName2 || !ch->isVideoCharacter())
                                continue;
                            constexpr float cW = 1920.0f, cH = 1080.0f;
                            vcPosX2    = (ch->posX - 0.5f) * cW;
                            vcPosY2    = (ch->posY - 0.5f) * cH;
                            vcScale2   = ch->scale;
                            vcOpacity2 = ch->opacity;
                            vcIsTalking2 = ch->isTalking;
                            break;
                        }
                    }
                }
            }

            size_t targetTrackIdx = SIZE_MAX;
            bool needsNewTrack = false;
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));
            const bool forceGhostAudioTrack = (trackIndex == (SIZE_MAX - 2));

            if (isAudio && forceGhostAudioTrack) {
                needsNewTrack = true;
            } else if (!isAudio && forceGhostVideoTrack) {
                needsNewTrack = true;
            } else if (isAudio) {
                if (trackIndex < m_ws->timeline()->trackCount() &&
                    m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio &&
                    !m_ws->timeline()->track(trackIndex)->isDivider())
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        Track* tr = m_ws->timeline()->track(i);
                        if (tr->type() == TrackType::Audio && !tr->isDivider()) {
                            targetTrackIdx = i; break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            } else {
                if (trackIndex < m_ws->timeline()->trackCount() &&
                    m_ws->timeline()->track(trackIndex)->type() == TrackType::Video &&
                    !m_ws->timeline()->track(trackIndex)->isDivider())
                    targetTrackIdx = trackIndex;
                if (targetTrackIdx == SIZE_MAX) {
                    // Bottom video layer (highest video index, skipping the
                    // divider). See the mediaDropped variant for rationale.
                    for (size_t i = m_ws->timeline()->trackCount(); i > 0; --i) {
                        Track* tr = m_ws->timeline()->track(i - 1);
                        if (tr->type() == TrackType::Video && !tr->isDivider()) {
                            targetTrackIdx = i - 1; break;
                        }
                    }
                }
                if (targetTrackIdx == SIZE_MAX) needsNewTrack = true;
            }

            spdlog::info("DIAG-DROP mediaDroppedWithRegion routing: dropTrackIdx={} isAudio={} forceGhostVideo={} forceGhostAudio={} targetTrackIdx={} needsNewTrack={}",
                         trackIndex, isAudio, forceGhostVideoTrack, forceGhostAudioTrack,
                         targetTrackIdx, needsNewTrack);

            auto clipId    = std::make_shared<uint64_t>(0);
            auto createdTk = std::make_shared<bool>(false);
            auto tkIdx     = std::make_shared<size_t>(targetTrackIdx);
            auto overlapCmd2 = std::make_shared<std::unique_ptr<Command>>(nullptr);

            // Audio-companion state for video+audio media
            // Audio-below ghost zone sentinel (same value the drop emits) — the
            // user dropped a video+audio file below the bottom audio track.
            const bool forceGhostAudioCompanion2 = (trackIndex == (SIZE_MAX - 3));
            const int audioStreamCount2 =
                mediaHasAudio ? probeAudioStreamCount(path) : 0;
            size_t audioTargetIdx2 = SIZE_MAX;
            bool needsNewAudioTrack2 = false;
            if (mediaHasAudio) {
                // Follow the cursor like the bin drag: start on the audio track
                // under the cursor, the first audio track when on a video track,
                // or a brand-new block below when dropped in the audio-below zone.
                if (forceGhostAudioCompanion2) {
                    needsNewAudioTrack2 = true;
                } else if (trackIndex < m_ws->timeline()->trackCount() &&
                    m_ws->timeline()->track(trackIndex)->type() == TrackType::Audio &&
                    !m_ws->timeline()->track(trackIndex)->isDivider()) {
                    audioTargetIdx2 = trackIndex;
                }
                if (!needsNewAudioTrack2 && audioTargetIdx2 == SIZE_MAX) {
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        Track* tr = m_ws->timeline()->track(i);
                        if (tr->type() == TrackType::Audio && !tr->isDivider()) {
                            audioTargetIdx2 = i; break;
                        }
                    }
                }
                if (audioTargetIdx2 == SIZE_MAX) needsNewAudioTrack2 = true;
            }
            auto audioClipId2      = std::make_shared<uint64_t>(0);
            auto audioCreatedTk2   = std::make_shared<bool>(false);
            auto audioTkIdx2       = std::make_shared<size_t>(audioTargetIdx2);
            auto audioOverlapCmd2  = std::make_shared<std::unique_ptr<Command>>(nullptr);
            auto audioSiblings2 =
                std::make_shared<std::vector<AudioStreamSibling>>();

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (m_ws->isDestroying()) return;
                if (trackStructureChanged)
                    m_ws->timelinePanel()->rebuildTracks();
                else
                    m_ws->timelinePanel()->refreshTrackContents();
                m_ws->invalidateAudioSources();
                m_ws->invalidateCompositeCache();
                m_ws->warmAudioCacheAsync();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
            };

            if (m_ws->commandStack()) {
                auto cmd = std::make_unique<LambdaCommand>(
                    "Add Source Region to Timeline",
                    /* execute / redo */
                    [this, isAudio, mediaHasAudio, path, label, atTick, dur, sourceDur, sourceIn, sourceFps,
                     needsNewTrack, needsNewAudioTrack2, forceGhostVideoTrack, forceGhostAudioTrack,
                     clipId, createdTk, tkIdx, overlapCmd2,
                     audioClipId2, audioCreatedTk2, audioTkIdx2, audioOverlapCmd2,
                     audioStreamCount2, audioSiblings2,
                     refreshAfter,
                     vcCharName2, vcMutePath2, vcTalkPath2, vcOutfit2, vcAnimName2,
                     vcPosX2, vcPosY2, vcScale2, vcOpacity2, vcIsTalking2]() {
                        if (needsNewTrack && *tkIdx == SIZE_MAX) {
                            // Match the existing standard track height —
                            // see comment in the mediaDropped variant.
                            float refTrackHeight = 0.0f;
                            for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                                Track* tr = m_ws->timeline()->track(ri);
                                if (!tr || tr->isDivider()) continue;
                                float h = tr->height();
                                if (h >= 1.0f) { refTrackHeight = h; break; }
                            }
                            Track* t = nullptr;
                            if (!isAudio && forceGhostVideoTrack) {
                                auto newTrack = std::make_unique<Track>(TrackType::Video, "");
                                if (refTrackHeight >= 1.0f)
                                    newTrack->setHeight(refTrackHeight);
                                t = m_ws->timeline()->insertTrack(0, std::move(newTrack));
                            } else if (isAudio && forceGhostAudioTrack) {
                                t = m_ws->timeline()->addAudioTrack("A1");
                                if (t && refTrackHeight >= 1.0f)
                                    t->setHeight(refTrackHeight);
                            } else {
                                t = isAudio ? m_ws->timeline()->addAudioTrack("A1")
                                            : m_ws->timeline()->addVideoTrack("V1");
                                if (t && refTrackHeight >= 1.0f)
                                    t->setHeight(refTrackHeight);
                            }
                            for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                if (m_ws->timeline()->track(i) == t) { *tkIdx = i; break; }
                            }
                            *createdTk = true;
                        }
                        Track* track = m_ws->timeline()->track(*tkIdx);
                        if (!track) return;
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion execute: resolved track idx={} name='{}' type={}",
                                     *tkIdx, track->name(),
                                     track->type() == TrackType::Video ? "video" : "audio");

                        std::unique_ptr<Clip> clip;
                        if (isAudio) {
                            auto ac = std::make_unique<AudioClip>(path);
                            ac->setTimelineIn(atTick);
                            ac->setDuration(dur);
                            ac->setSourceDuration(sourceDur);
                            ac->setSourceIn(sourceIn);
                            ac->setLabel(label);
                            clip = std::move(ac);
                        } else {
                            auto vc = std::make_unique<VideoClip>(path);
                            vc->setTimelineIn(atTick);
                            vc->setDuration(dur);
                            // Still images have no real source duration — treat as unbounded.
                            vc->setSourceDuration(isStillImagePath(path) ? 0 : sourceDur);
                            vc->setSourceIn(sourceIn);
                            vc->setSourceFps(sourceFps);
                            vc->setLabel(label);
                            if (!vcCharName2.empty()) {
                                vc->setCharacterName(vcCharName2);
                                vc->setVideoMutePath(vcMutePath2);
                                vc->setVideoTalkPath(vcTalkPath2);
                                vc->setOutfit(vcOutfit2);
                                vc->setAnimationName(vcAnimName2);
                                vc->setTalking(vcIsTalking2);
                                vc->positionX().setDefaultValue(vcPosX2);
                                vc->positionY().setDefaultValue(vcPosY2);
                                vc->scaleX().setDefaultValue(vcScale2);
                                vc->scaleY().setDefaultValue(vcScale2);
                                vc->opacity().setDefaultValue(vcOpacity2);
                            }
                            clip = std::move(vc);
                        }
                        *clipId = clip->id();
                        if (mediaHasAudio)
                            clip->setLinkId(*clipId);  // pair with companion audio
                        spdlog::info("DIAG-DROP mediaDroppedWithRegion clip id={} type={} "
                                     "timelineIn={} dur={} ({:.3f}s) sourceIn={} srcDur={} ({:.3f}s)",
                                     *clipId, isAudio ? "audio" : "video",
                                     atTick, dur, dur/48000.0, sourceIn, sourceDur, sourceDur/48000.0);
                        track->addClip(std::move(clip));

                        // Resolve overlaps (overwrite like Premiere Pro)
                        *overlapCmd2 = EditOperations::resolveOverlaps(
                            *m_ws->timeline(), *tkIdx, *clipId);
                        if (*overlapCmd2) (*overlapCmd2)->execute();

                        // Verify clip state after overlap resolution
                        {
                            size_t vi = track->findClipIndexById(*clipId);
                            if (vi < track->clipCount()) {
                                const auto* c = track->clip(vi);
                                spdlog::info("DIAG-DROP mediaDroppedWithRegion VERIFY clip id={} "
                                             "in={} dur={} ({:.3f}s) srcIn={} out={}",
                                             c->id(), c->timelineIn(), c->duration(),
                                             c->duration()/48000.0, c->sourceIn(),
                                             c->timelineOut());
                            }
                        }

                        // -- Create companion AudioClip for video+audio media --
                        if (mediaHasAudio) {
                            if (needsNewAudioTrack2 && *audioTkIdx2 == SIZE_MAX) {
                                // Match existing track height — see comment
                                // in the mediaDropped variant.
                                float refTrackHeight = 0.0f;
                                for (size_t ri = 0; ri < m_ws->timeline()->trackCount(); ++ri) {
                                    Track* tr = m_ws->timeline()->track(ri);
                                    if (!tr || tr->isDivider()) continue;
                                    float h = tr->height();
                                    if (h >= 1.0f) { refTrackHeight = h; break; }
                                }
                                Track* at = m_ws->timeline()->addAudioTrack("A1");
                                if (at && refTrackHeight >= 1.0f)
                                    at->setHeight(refTrackHeight);
                                for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                    if (m_ws->timeline()->track(i) == at) { *audioTkIdx2 = i; break; }
                                }
                                *audioCreatedTk2 = true;
                            } else if (*audioTkIdx2 != SIZE_MAX) {
                                // Re-validate audio track index — a new video track may have been
                                // inserted at index 0 above, shifting all existing track indices.
                                if (*audioTkIdx2 >= m_ws->timeline()->trackCount() ||
                                    m_ws->timeline()->track(*audioTkIdx2)->type() != TrackType::Audio) {
                                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                                        if (m_ws->timeline()->track(i)->type() == TrackType::Audio) {
                                            *audioTkIdx2 = i;
                                            break;
                                        }
                                    }
                                }
                            }
                            Track* audioTrack = m_ws->timeline()->track(*audioTkIdx2);
                            if (audioTrack) {
                                auto ac = std::make_unique<AudioClip>(path);
                                ac->setTimelineIn(atTick);
                                ac->setDuration(dur);
                                ac->setSourceDuration(sourceDur);
                                ac->setSourceIn(sourceIn);
                                ac->setLabel(label);
                                ac->setLinkId(*clipId);  // pair with companion video
                                if (audioStreamCount2 > 1)
                                    ac->setAudioStreamIndex(0);  // companion = stream 0
                                *audioClipId2 = ac->id();
                                audioTrack->addClip(std::move(ac));
                                *audioOverlapCmd2 = EditOperations::resolveOverlaps(
                                    *m_ws->timeline(), *audioTkIdx2, *audioClipId2);
                                if (*audioOverlapCmd2) (*audioOverlapCmd2)->execute();

                                // Lay every remaining audio stream onto its own
                                // track below, as one linked group.
                                if (audioStreamCount2 > 1) {
                                    createAudioStreamSiblings(
                                        m_ws->timeline(), path, atTick, dur, sourceDur,
                                        sourceIn, label, *clipId, *audioTkIdx2,
                                        audioStreamCount2, *audioSiblings2);
                                }
                            }
                        }

                        const bool trackStructureChanged =
                            (*createdTk || *audioCreatedTk2 || !audioSiblings2->empty());
                        refreshAfter(trackStructureChanged);
                    },
                    /* undo */
                    [this, clipId, createdTk, tkIdx, overlapCmd2,
                     mediaHasAudio, audioClipId2, audioCreatedTk2, audioTkIdx2, audioOverlapCmd2,
                     audioSiblings2, refreshAfter]() {
                        const bool trackStructureChanged =
                            (*createdTk || *audioCreatedTk2 || !audioSiblings2->empty());
                        // Undo audio companion first
                        if (mediaHasAudio) {
                            undoAudioStreamSiblings(m_ws->timeline(), *audioSiblings2);
                            if (*audioOverlapCmd2) (*audioOverlapCmd2)->undo();
                            if (*audioTkIdx2 < m_ws->timeline()->trackCount()) {
                                Track* at = m_ws->timeline()->track(*audioTkIdx2);
                                if (at) at->removeClipById(*audioClipId2);
                            }
                            if (*audioCreatedTk2) {
                                m_ws->timeline()->removeTrack(*audioTkIdx2);
                                *audioTkIdx2 = SIZE_MAX;
                                *audioCreatedTk2 = false;
                            }
                        }

                        // Undo overlap resolution
                        if (*overlapCmd2) (*overlapCmd2)->undo();

                        if (*tkIdx < m_ws->timeline()->trackCount()) {
                            Track* track = m_ws->timeline()->track(*tkIdx);
                            if (track) track->removeClipById(*clipId);
                        }
                        if (*createdTk) {
                            m_ws->timeline()->removeTrack(*tkIdx);
                            *tkIdx = SIZE_MAX;
                            *createdTk = false;
                        }
                        refreshAfter(trackStructureChanged);
                    }
                );
                m_ws->commandStack()->execute(std::move(cmd));
            }

            spdlog::info("Source region dropped on timeline: '{}' at tick {}, sourceIn={} dur={}",
                         path, atTick, sourceIn, dur);
        });

        // External file drop from Windows Explorer — add to bin + timeline
        connect(m_ws->timelinePanel(), &TimelinePanel::externalFileDropped,
                this, [this](const QString& filePath,
                             int64_t atTick, size_t trackIndex) {
            if (!m_ws->timeline()) return;

            // If no project or no sequences exist, prompt to create one
            if (!m_ws->project() || m_ws->project()->sequenceCount() == 0) {
                uint32_t fileW = 0, fileH = 0;
                double fileFps = 30.0;
                if (m_ws->mediaPool()) {
                    uint64_t h = m_ws->mediaPool()->open(filePath.toStdString());
                    if (h != 0) {
                        const auto* info = m_ws->mediaPool()->getInfo(h);
                        if (info) {
                            fileW = info->width;
                            fileH = info->height;
                            if (info->fps > 0.0) fileFps = info->fps;
                        }
                    }
                }
                QString resolutionStr = (fileW > 0 && fileH > 0)
                    ? QString("%1 x %2").arg(fileW).arg(fileH)
                    : QString("Unknown");
                QString fpsStr = QString::number(fileFps, 'f', 2);
                auto result = QMessageBox::question(
                    m_ws->timelinePanel(), "Create Sequence",
                    QString("No sequence is open.\n\n"
                            "Do you want to create a new sequence with this media?\n\n"
                            "File: %1\n"
                            "Resolution: %2\n"
                            "Frame rate: %3 fps\n\n"
                            "A new project will be created automatically.")
                        .arg(QFileInfo(filePath).fileName())
                        .arg(resolutionStr).arg(fpsStr),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
                if (result == QMessageBox::Yes)
                    emit m_ws->requestNewProjectForMedia(filePath, atTick, trackIndex);
                return;
            }

            // Auto-add to the Project Bin (skip if already present)
            if (m_ws->projectBin()) {
                namespace fs = std::filesystem;
                m_ws->projectBin()->addFiles({ fs::path(filePath.toStdWString()) });
            }

            // Open in MediaPool and forward to the normal media-drop path
            uint64_t handle = 0;
            if (m_ws->mediaPool())
                handle = m_ws->mediaPool()->open(filePath.toStdString());

            emit m_ws->timelinePanel()->mediaDropped(filePath, handle, atTick, trackIndex);
        });

        // =================================================================
        //  ADJUSTMENT-LAYER DRAG-DROP -> CREATE AdjustmentClip ON TIMELINE
        // =================================================================
        connect(m_ws->timelinePanel(), &TimelinePanel::adjustmentDropped,
                this, [this](const QString& name, int64_t atTick,
                             size_t trackIndex) {
            if (m_ws->isDestroying()) return;
            if (!m_ws->timeline()) return;

            const int64_t dur = secondsToTicks(5.0);
            const bool forceGhostVideoTrack = (trackIndex == (SIZE_MAX - 1));

            // Resolve the destination video track. If the drop landed above
            // the top video (kGhostDropTrackVideoAbove sentinel), or on an
            // audio track / empty area, fall back to the top-most video
            // track. Create one if the sequence has none.
            auto resolveVideoTrack = [&]() -> Track* {
                if (forceGhostVideoTrack || trackIndex >= m_ws->timeline()->trackCount() ||
                    m_ws->timeline()->track(trackIndex)->type() != TrackType::Video ||
                    m_ws->timeline()->track(trackIndex)->isDivider()) {
                    for (size_t i = 0; i < m_ws->timeline()->trackCount(); ++i) {
                        Track* tr = m_ws->timeline()->track(i);
                        if (tr && tr->type() == TrackType::Video && !tr->isDivider())
                            return tr;
                    }
                    return nullptr;
                }
                return m_ws->timeline()->track(trackIndex);
            };

            auto refreshAfter = [this](bool trackStructureChanged = false) {
                if (m_ws->isDestroying()) return;
                if (trackStructureChanged)
                    m_ws->timelinePanel()->rebuildTracks();
                else
                    m_ws->timelinePanel()->refreshTrackContents();
                m_ws->invalidateCompositeCache();
                if (m_ws->programMonitor()) m_ws->programMonitor()->requestRefresh();
            };

            // If no video track exists, create one (matches the media-drop
            // "new track on empty sequence" behaviour).
            bool createdTrack = false;
            Track* track = resolveVideoTrack();
            if (!track) {
                track = m_ws->timeline()->addVideoTrack("Video 1");
                createdTrack = true;
                if (!track) return;
            }

            auto clip = std::make_unique<AdjustmentClip>();
            clip->setTimelineIn(atTick);
            clip->setDuration(dur);
            clip->setLabel(name.toStdString());

            if (m_ws->commandStack()) {
                m_ws->commandStack()->execute(std::make_unique<AddClipCommand>(
                    track, std::move(clip)));
            } else {
                track->addClip(std::move(clip));
            }

            refreshAfter(createdTrack);
            spdlog::info("Adjustment Layer dropped on timeline: '{}' at tick {}",
                         name.toStdString(), atTick);
        });
    }
}

} // namespace rt
