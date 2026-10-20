/*
 * AudioSyncExport.cpp - Timeline export for AudioSync: one undoable step
 * around core audiosync/ExportPlan (what goes where, full export vs
 * in-place re-export), plus the default-shot layers of each visual group.
 */
#include "panels/audio/AudioSync.h"
#include "audiosync/ExportPlan.h"
#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "spine/ShotPreset.h"
#include "command/CommandStack.h"
#include "edit/EditSession.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/PngPuppetClip.h"
#include "panels/characters/PuppetLibrary.h"
#include "Theme.h"
#include "command/CommandStack.h"
#include "command/LambdaCommand.h"

#include <spdlog/spdlog.h>

#include <functional>

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <limits>
#include <filesystem>

namespace rt {

// Creates AudioClips AND visual clips (SpineClip/VideoClip) from default shots
// for each character, matching the old Python behavior.

int AudioSync::exportToTimeline(Timeline* timeline)
{
    if (!timeline) return 0;
    m_lastExportError.clear();

    // Build and apply against a detached deep clone first. Nothing visible is
    // mutated until every validation and placement step has succeeded. The
    // before/after snapshots then form one history entry.
    if (m_commandStack && !m_exportTransactionActive) {
        // Exact copies (cloneForSnapshot): the staged edit must keep every
        // existing clip id, or links and transitions break when applied.
        auto before = timeline->cloneForSnapshot();
        auto staged = timeline->cloneForSnapshot();
        m_exportTransactionActive = true;
        const int result = exportToTimeline(staged.get());
        m_exportTransactionActive = false;
        if (result <= 0) return result;

        // Apply in place as one recorded step: existing clips keep their
        // objects, removed ones are kept alive by the step.
        if (EditSession* edits = m_commandStack->editSession()) {
            edits->assignSequence("Audio Sync Export", *timeline, *staged);
            return result;
        }

        auto after = staged->cloneForSnapshot();
        timeline->restoreFrom(*staged);
        auto beforeShared = std::shared_ptr<Timeline>(std::move(before));
        auto afterShared = std::shared_ptr<Timeline>(std::move(after));
        m_commandStack->pushWithoutExecute(std::make_unique<LambdaCommand>(
            "Audio Sync Export",
            [timeline, afterShared]() { timeline->restoreFrom(*afterShared); },
            [timeline, beforeShared]() { timeline->restoreFrom(*beforeShared); }));
        return result;
    }

    // The MATCH tab's clip for each script line, in script order.
    const std::vector<const SyncClip*> confirmed =
        audiosync::exportedClips(session().script.get(), session().clips);

    // DIAG-EXPORT-ORDER: dump the resolved export order with each clip's
    // line#, its position in the script (= MATCH order), character and
    // source, so a misplaced (e.g. manually-matched) clip is traceable.
    {
        std::unordered_map<int, int> linePos;
        if (session().script) {
            int p = 0;
            for (const auto& ln : session().script->lines) linePos[ln.lineNumber] = p++;
        }
        auto scriptPosOf = [&](const SyncClip* c) -> int {
            auto it = linePos.find(c->scriptLineNumber);
            return (it != linePos.end()) ? it->second
                                         : std::numeric_limits<int>::max();
        };
        std::string seq;
        for (const SyncClip* c : confirmed) {
            const int pos = scriptPosOf(c);
            seq += "\n    scriptPos=" + (pos == std::numeric_limits<int>::max()
                                         ? std::string("MISSING")
                                         : std::to_string(pos))
                 + " line#=" + std::to_string(c->scriptLineNumber)
                 + " char='" + c->character + "'"
                 + " src='" + audiosync::fileNameOf(c->sourceFile) + "'"
                 + " start=" + std::to_string(c->start);
        }
        spdlog::warn("DIAG-EXPORT-ORDER: {} confirmed clips (export order):{}",
                     confirmed.size(), seq);
    }

    if (confirmed.empty()) {
        spdlog::warn("AudioSync::exportToTimeline — no confirmed clips");
        return 0;
    }

    spdlog::info("AudioSync::exportToTimeline — exporting {} confirmed clips",
                 confirmed.size());

    const audiosync::ExportPlan plan = audiosync::planExport(session().script.get(), confirmed);

    // ── Default-shot layers of a visual group ───────────────────────────

    // New shot groups must not collide with groupIds already present in
    // the timeline (the incremental path KEEPS existing groups, and the
    // session-static counter resets every app run).
    static uint64_t s_nextGroupId = 1;
    {
        uint64_t maxExisting = 0;
        for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
            Track* t = timeline->track(ti);
            if (!t) continue;
            for (size_t ci = 0; ci < t->clipCount(); ++ci)
                if (const Clip* c = t->clip(ci))
                    maxExisting = std::max(maxExisting, c->groupId());
        }
        s_nextGroupId = std::max(s_nextGroupId, maxExisting + 1);
    }

    // Place the default-shot visual layers for one group.  Returns the
    // number of clips placed (0 when the character has no default shot).
    // Layer order: layerOrder[0] is FRONT (top V#), layerOrder[last] is
    // BACK (V1).
    auto placeGroupVisuals = [&](const audiosync::PlannedGroup& group) -> int {
        if (!m_shotPresetManager) return 0;
        if (group.character.empty() || group.totalDuration <= 0) return 0;

        auto preset = m_shotPresetManager->resolveDefaultShot(group.character, m_currentShow);
        if (!preset) {
            spdlog::debug("  No default shot for '{}', skipping visual clips",
                          group.character);
            return 0;
        }
        // Store the full (show/name) key so the clip re-resolves to the
        // correct per-show shot later (the Properties Shot dropdown).
        const std::string shotName =
            ShotPresetManager::makeKey(preset->show(), preset->name());

        const uint64_t groupId = s_nextGroupId++;

        const auto& layerOrder = preset->layerOrder();
        const int layerCount = static_cast<int>(layerOrder.size());

        // Count visible layers to ensure enough video tracks exist
        int visibleCount = 0;
        for (int cli = 0; cli < layerCount; ++cli) {
            const auto& lr = layerOrder[cli];
            bool vis = false;
            if (lr.type == LayerType::Background) {
                auto* bg = preset->background(lr.index);
                vis = (bg && bg->visible);
            } else {
                auto* ch = preset->character(lr.index);
                vis = (ch && ch->visible);
            }
            if (vis) ++visibleCount;
        }

        // Collect real video track indices. Dividers are TrackType::Video
        // but reject clips; the caption track is pinned/user-owned — both
        // would silently swallow or pollute placements.
        std::vector<size_t> videoIndices;
        for (size_t vi = 0; vi < timeline->trackCount(); ++vi) {
            Track* tk = timeline->track(vi);
            if (tk && tk->type() == TrackType::Video && !tk->isDivider()
                && !tk->isCaptionTrack() && !tk->isLocked())
                videoIndices.push_back(vi);
        }

        while (static_cast<int>(videoIndices.size()) < visibleCount) {
            Track* t = timeline->addVideoTrack("");
            (void)t;
            size_t newIdx = videoIndices.empty() ? 0 : videoIndices.back() + 1;
            videoIndices.push_back(newIdx);
        }

        int placedCount = 0;

        // Iterate back-to-front: layerOrder[last] = BACK -> highest video index (V1)
        int placedIdx = 0;
        for (int li = layerCount - 1; li >= 0; --li) {
            const auto& layerRef = layerOrder[li];
            std::string layerIdStr;

            size_t trackPos = videoIndices.size() - 1 - static_cast<size_t>(placedIdx);
            Track* vTrack = (trackPos < videoIndices.size())
                ? timeline->track(videoIndices[trackPos])
                : timeline->track(videoIndices.back());

            if (layerRef.type == LayerType::Background) {
                layerIdStr = "background_" + std::to_string(layerRef.index);

                const BackgroundState* bg = preset->background(layerRef.index);
                if (!bg || !bg->visible) continue;

                auto vClip = std::make_unique<VideoClip>(bg->path);
                vClip->setTimelineIn(group.timelineStart);
                vClip->setDuration(group.totalDuration);
                // Use original filename (no extension) as label
                std::string bgLabel = pathToUtf8(utf8ToPath(bg->path).stem());
                vClip->setLabel(bgLabel);
                vClip->setShotName(shotName);
                vClip->setGroupId(groupId);
                vClip->setLayerId(layerIdStr);
                vClip->setSyncLine(group.firstLine);
                constexpr float outW = 1920.0f;
                constexpr float outH = 1080.0f;
                vClip->positionX().setDefaultValue((bg->posX - 0.5f) * outW);
                vClip->positionY().setDefaultValue((bg->posY - 0.5f) * outH);
                vClip->scaleX().setDefaultValue(bg->scale);
                vClip->scaleY().setDefaultValue(bg->scale);
                vClip->opacity().setDefaultValue(bg->opacity);
                if (bg->cropLeft > 0 || bg->cropRight > 0 || bg->cropTop > 0 || bg->cropBottom > 0)
                    vClip->setCrop(bg->cropLeft, bg->cropRight, bg->cropTop, bg->cropBottom);

                vTrack->addClip(std::move(vClip));
                ++placedCount;

            } else if (layerRef.type == LayerType::Character) {
                layerIdStr = "char_" + std::to_string(layerRef.index);

                const CharacterState* ch = preset->character(layerRef.index);
                if (!ch || !ch->visible) continue;

                if (ch->isPuppet()) {
                    // ── PNG puppet → PngPuppetClip ──────────────────────
                    PuppetManifest man;
                    if (!puppetlib::load(
                            QString::fromStdString(ch->puppetFolder), man)) {
                        spdlog::warn("exportToTimeline: cannot load puppet "
                                     "manifest '{}'", ch->puppetFolder);
                        continue;
                    }
                    std::string variant = ch->puppetVariant.empty()
                        ? "default" : ch->puppetVariant;
                    auto vit = man.variants.find(QString::fromStdString(variant));
                    if (vit == man.variants.end() && !man.variantOrder.isEmpty())
                        vit = man.variants.find(man.variantOrder.first());
                    if (vit == man.variants.end()) continue;

                    auto pc = std::make_unique<PngPuppetClip>(
                        ch->characterName, variant);
                    for (int f = 0; f < puppetlib::kFaceCount; ++f)
                        pc->setFacePath(f, vit->faces[static_cast<size_t>(f)]
                                              .toStdString());
                    pc->setTimelineIn(group.timelineStart);
                    pc->setDuration(group.totalDuration);
                    pc->setTalking(ch->isTalking);
                    pc->setLabel(ch->characterName);
                    pc->setShotName(shotName);
                    pc->setGroupId(groupId);
                    pc->setLayerId(layerIdStr);
                    pc->setSyncLine(group.firstLine);
                    // Seed from the puppet folder (NOT the clip id) so two
                    // clips of the same character cut together in phase —
                    // matches the timeline drop handler.
                    pc->setSeed(static_cast<uint32_t>(
                        std::hash<std::string>{}(ch->puppetFolder) & 0xFFFFFFFFu));

                    constexpr float ppOutW = 1920.0f;
                    constexpr float ppOutH = 1080.0f;
                    pc->positionX().setDefaultValue((ch->posX - 0.5f) * ppOutW);
                    pc->positionY().setDefaultValue((ch->posY - 0.5f) * ppOutH);
                    pc->scaleX().setDefaultValue(ch->flipX ? -ch->scale : ch->scale);
                    pc->scaleY().setDefaultValue(ch->flipY ? -ch->scale : ch->scale);
                    pc->opacity().setDefaultValue(ch->opacity);
                    // (PngPuppetClip has no crop support — crop is a
                    //  Video/Spine-only clip feature.)

                    vTrack->addClip(std::move(pc));
                    ++placedCount;
                } else if (ch->isVideoCharacter()) {
                    const std::string& videoPath = ch->activeVideoPath();
                    if (videoPath.empty()) continue;

                    auto vClip = std::make_unique<VideoClip>(videoPath);
                    vClip->setTimelineIn(group.timelineStart);
                    vClip->setDuration(group.totalDuration);
                    // Use "CHARACTER - ANIMATION" as label
                    std::string animLabel = ch->characterName;
                    if (!ch->animation.empty())
                        animLabel += " - " + ch->animation;
                    vClip->setLabel(animLabel);
                    vClip->setShotName(shotName);
                    vClip->setGroupId(groupId);
                    vClip->setLayerId(layerIdStr);
                    vClip->setSyncLine(group.firstLine);

                    vClip->setCharacterName(ch->characterName);
                    vClip->setTalking(ch->isTalking);
                    vClip->setVideoMutePath(ch->videoMutePath);
                    vClip->setVideoTalkPath(ch->videoTalkPath);

                    constexpr float chOutW = 1920.0f;
                    constexpr float chOutH = 1080.0f;
                    // Raw preset scale — the compositor applies the 0.85×
                    // COMPOSE character-fit dynamically (containFit + kComposeFit).
                    // Do NOT bake 0.85 into the clip scale — that would
                    // double-compensate (0.85² = 0.7225) and cause the
                    // character to render ~15% undersized, forcing the user
                    // to zoom in and lose sharpness.
                    vClip->positionX().setDefaultValue((ch->posX - 0.5f) * chOutW);
                    vClip->positionY().setDefaultValue((ch->posY - 0.5f) * chOutH);
                    const float charScaleX = ch->flipX ? -ch->scale : ch->scale;
                    const float charScaleY = ch->flipY ? -ch->scale : ch->scale;
                    vClip->scaleX().setDefaultValue(charScaleX);
                    vClip->scaleY().setDefaultValue(charScaleY);
                    vClip->opacity().setDefaultValue(ch->opacity);
                    if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                        vClip->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);

                    vTrack->addClip(std::move(vClip));
                    ++placedCount;
                } else {
                    auto spClip = std::make_unique<SpineClip>(ch->characterName, ch->outfit);
                    spClip->setTimelineIn(group.timelineStart);
                    spClip->setDuration(group.totalDuration);
                    spClip->setAnimationName(ch->animation);
                    spClip->setTalking(ch->isTalking);
                    spClip->setLooping(true);
                    // Use "CHARACTER - ANIMATION" as label
                    std::string animLabel = ch->characterName;
                    if (!ch->animation.empty())
                        animLabel += " - " + ch->animation;
                    spClip->setLabel(animLabel);
                    spClip->setShotName(shotName);
                    spClip->setGroupId(groupId);
                    spClip->setLayerId(layerIdStr);
                    spClip->setSyncLine(group.firstLine);

                    constexpr float spOutW = 1920.0f;
                    constexpr float spOutH = 1080.0f;
                    spClip->positionX().setDefaultValue((ch->posX - 0.5f) * spOutW);
                    spClip->positionY().setDefaultValue((ch->posY - 0.5f) * spOutH);
                    spClip->scaleX().setDefaultValue(ch->flipX ? -ch->scale : ch->scale);
                    spClip->scaleY().setDefaultValue(ch->flipY ? -ch->scale : ch->scale);
                    spClip->opacity().setDefaultValue(ch->opacity);
                    if (ch->cropLeft > 0 || ch->cropRight > 0 || ch->cropTop > 0 || ch->cropBottom > 0)
                        spClip->setCrop(ch->cropLeft, ch->cropRight, ch->cropTop, ch->cropBottom);

                    vTrack->addClip(std::move(spClip));
                    ++placedCount;
                }
            }
            ++placedIdx;
        }

        spdlog::info("  Created {} visual layers for '{}' (shot '{}')",
                     layerCount, group.character, preset->name());
        return placedCount;
    };

    // A sequence exported to before is updated in place.
    const bool incremental = audiosync::isReexport(*timeline);

    // Nothing changes when a locked track is in the way.
    if (auto blocked = audiosync::lockedTrackInTheWay(*timeline, plan, incremental)) {
        if (incremental) {
            spdlog::warn("AudioSync: re-export blocked by locked track '{}'", blocked->track);
            m_lastExportError = tr("Track '%1' is locked; clip '%2' (line %3) cannot be updated.")
                .arg(QString::fromStdString(blocked->track),
                     QString::fromStdString(blocked->clipLabel))
                .arg(blocked->line);
        } else {
            spdlog::warn("AudioSync: export blocked by locked track '{}'", blocked->track);
            m_lastExportError = tr("Track '%1' is locked and is required by this export.")
                .arg(QString::fromStdString(blocked->track));
        }
        return kExportBlockedByLockedTrack;
    }

    return audiosync::applyExport(*timeline, plan, incremental, m_shotPresetManager != nullptr,
                                  placeGroupVisuals);
}

} // namespace rt
