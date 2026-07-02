/*
 * AudioSyncExport.cpp - Timeline export logic for AudioSync.
 * Split from AudioSyncData.cpp for maintainability.
 *
 * Two application modes share one deterministic plan (audio clips laid
 * back-to-back from tick 0 on per-speaker tracks + one default-shot visual
 * group per consecutive same-character run):
 *
 *  - FULL: first export into a sequence (no clip carries a syncLine
 *    back-link).  Clears previous export-owned content and rebuilds.
 *  - INCREMENTAL: re-export into a previously exported sequence.  Clips
 *    tagged with a syncLine are diffed against the plan and updated in
 *    place (AudioSync wins on position/range), preserving clip identity —
 *    so user shot swaps, transforms, effects and transitions survive.
 *    Clips without a back-link (user media, captions, music tracks) are
 *    never touched.
 */
#include "panels/audio/AudioSync.h"
#include "PathUtils.h"
#include "ai/ScriptMatcher.h"
#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "timeline/PngPuppetClip.h"
#include "panels/characters/PuppetLibrary.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <functional>

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <limits>
#include <filesystem>

namespace rt {

// File basename helper — returns just the filename portion of a path-like
// string. Used to label exported timeline clips with their source filename
// instead of shot/character names.
static std::string sourceBasename(const std::string& path)
{
    if (path.empty()) return {};
    try {
        return pathToUtf8(utf8ToPath(path).filename());
    } catch (...) {
        auto p = path.find_last_of("/\\");
        return p == std::string::npos ? path : path.substr(p + 1);
    }
}

namespace {

// One audio clip the plan wants on the timeline.
struct PlannedAudio {
    int         line{-1};        // script line (back-link value)
    std::string character;       // speaker (audio track name)
    std::string sourceFile;
    std::string label;
    int64_t     timelineIn{0};
    int64_t     duration{0};
    int64_t     sourceIn{0};
    int64_t     sourceDuration{0};
};

// One visual shot group (consecutive same-character run of lines).
struct PlannedGroup {
    std::string character;
    int         firstLine{-1};   // back-link value for the group's visuals
    int64_t     timelineStart{0};
    int64_t     totalDuration{0};
};

using TrackForSpeakerFn  = std::function<Track*(const std::string&)>;
using MakeAudioClipFn    = std::function<std::unique_ptr<AudioClip>(const PlannedAudio&)>;
using PlaceGroupVisualsFn = std::function<int(const PlannedGroup&)>;

int applyFull(Timeline* timeline,
              const std::vector<PlannedAudio>& plannedAudio,
              const std::vector<PlannedGroup>& groups,
              bool hasShotMgr,
              const TrackForSpeakerFn& trackForSpeaker,
              const MakeAudioClipFn& makeAudioClip,
              const PlaceGroupVisualsFn& placeGroupVisuals);

int applyIncremental(Timeline* timeline,
                     const std::vector<PlannedAudio>& plannedAudio,
                     const std::vector<PlannedGroup>& groups,
                     const TrackForSpeakerFn& trackForSpeaker,
                     const MakeAudioClipFn& makeAudioClip,
                     const PlaceGroupVisualsFn& placeGroupVisuals);

} // namespace

// Creates AudioClips AND visual clips (SpineClip/VideoClip) from default shots
// for each character, matching the old Python behavior.

int AudioSync::exportToTimeline(Timeline* timeline)
{
    if (!timeline) return 0;

    // ── EXPORT == MATCH tab, exactly ────────────────────────────────────
    // The MATCH tab shows (and plays) exactly ONE clip per script line: its
    // "primary" — the best CONFIRMED clip on that line. The export mirrors
    // that precisely by walking the script lines IN ORDER and placing each
    // line's primary confirmed clip. This guarantees the timeline matches the
    // MATCH tab in both order AND content — no duplicates, no cross-character
    // strays, no reordering — and needs no re-sync.
    //
    // Primary selection: same rule as populateCards() — highest confidence
    // among confirmed clips (all matchState==2 here, so confidence is the
    // sole differentiator).  Character-name matching is deliberately not
    // used because the MATCH tab doesn't either; manual confirmation
    // already guarantees the right clip is assigned to each line.

    // lineNumber → its position in m_script->lines (script reading order) — for
    // the diagnostic below.
    std::unordered_map<int, int> linePos;
    if (m_script) {
        int p = 0;
        for (const auto& ln : m_script->lines) linePos[ln.lineNumber] = p++;
    }
    auto scriptPosOf = [&](const SyncClip* c) -> int {
        auto it = linePos.find(c->scriptLineNumber);
        return (it != linePos.end()) ? it->second
                                     : std::numeric_limits<int>::max();
    };

    std::vector<const SyncClip*> confirmed;
    if (m_script) {
        // Confirmed clips per line, in m_clips order.
        std::unordered_map<int, std::vector<size_t>> confirmedByLine;
        for (size_t i = 0; i < m_clips.size(); ++i) {
            const auto& c = m_clips[i];
            if (c.scriptLineNumber >= 0 && c.matchState == 2)
                confirmedByLine[c.scriptLineNumber].push_back(i);
        }
        for (const auto& line : m_script->lines) {
            auto it = confirmedByLine.find(line.lineNumber);
            if (it == confirmedByLine.end()) continue;
            // Primary: pick the clip with the highest confidence, matching
            // the MATCH tab's populateCards primary-selection rule exactly.
            const SyncClip* best = nullptr;
            float bestConf = -1.0f;
            for (size_t ci : it->second) {
                const auto& c = m_clips[ci];
                if (!best || c.confidence > bestConf) {
                    best = &c;
                    bestConf = c.confidence;
                }
            }
            if (best) confirmed.push_back(best);   // already in script order
        }
    } else {
        // No script loaded — fall back to every confirmed clip in m_clips order.
        for (const auto& c : m_clips)
            if (c.scriptLineNumber >= 0 && c.matchState == 2)
                confirmed.push_back(&c);
    }

    // DIAG-EXPORT-ORDER: dump the resolved export order with each clip's
    // line#, its position in the script (= MATCH order), character and
    // source, so a misplaced (e.g. manually-matched) clip is traceable.
    {
        std::string seq;
        for (const SyncClip* c : confirmed) {
            const int pos = scriptPosOf(c);
            seq += "\n    scriptPos=" + (pos == std::numeric_limits<int>::max()
                                         ? std::string("MISSING")
                                         : std::to_string(pos))
                 + " line#=" + std::to_string(c->scriptLineNumber)
                 + " char='" + c->character + "'"
                 + " src='" + sourceBasename(c->sourceFile) + "'"
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

    // ── Build a lookup from lineNumber → ScriptLine character ───────────
    std::unordered_map<int, std::string> lineCharacterMap;
    if (m_script) {
        for (const auto& line : m_script->lines)
            lineCharacterMap[line.lineNumber] = line.character;
    }

    // ── Build the plan (pure — no timeline mutation) ────────────────────
    // Audio clips back-to-back from tick 0; consecutive same-character
    // lines form one visual group spanning their combined range.
    std::vector<PlannedAudio> plannedAudio;
    std::vector<PlannedGroup> groups;
    {
        int64_t currentPos = 0; // ticks
        std::string lastCharacter;
        for (const SyncClip* sc : confirmed) {
            auto effectiveRanges = getEffectiveRanges(*sc);

            // Determine character
            std::string character;
            auto it = lineCharacterMap.find(sc->scriptLineNumber);
            if (it != lineCharacterMap.end() && !it->second.empty())
                character = it->second;
            else
                character = sc->character;

            // Start a new group if character changes or first clip
            if (character != lastCharacter || groups.empty()) {
                PlannedGroup g;
                g.character     = character;
                g.firstLine     = sc->scriptLineNumber;
                g.timelineStart = currentPos;
                groups.push_back(std::move(g));
                lastCharacter = character;
            }

            // Label: use the source audio file's basename so the timeline
            // shows the actual asset name instead of the character/shot.
            std::string label = sourceBasename(sc->sourceFile);
            if (label.empty()) label = character;

            for (const auto& [rangeStart, rangeEnd] : effectiveRanges) {
                double rangeDuration = rangeEnd - rangeStart;
                if (rangeDuration <= 0.0) continue;

                PlannedAudio pa;
                pa.line           = sc->scriptLineNumber;
                pa.character      = character;
                pa.sourceFile     = sc->sourceFile;
                pa.label          = label;
                pa.timelineIn     = currentPos;
                pa.duration       = secondsToTicks(rangeDuration);
                pa.sourceIn       = secondsToTicks(rangeStart);
                pa.sourceDuration = secondsToTicks(sc->end);
                plannedAudio.push_back(std::move(pa));

                groups.back().totalDuration += secondsToTicks(rangeDuration);
                currentPos += secondsToTicks(rangeDuration);
            }
        }
    }

    // ── Shared helpers ──────────────────────────────────────────────────

    // Find-or-create the per-speaker audio track for a character.  The
    // full path wipes previous speaker tracks first, so "find" only kicks
    // in for the incremental path (and for repeated speakers either way).
    std::unordered_map<std::string, Track*> speakerTracks;
    auto trackForSpeaker = [&](const std::string& character) -> Track* {
        std::string name = character.empty() ? std::string("VO") : character;
        auto it = speakerTracks.find(name);
        if (it != speakerTracks.end()) return it->second;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (t && t->type() == TrackType::Audio && !t->isDivider()
                && t->name() == name) {
                speakerTracks.emplace(name, t);
                return t;
            }
        }
        Track* t = timeline->addAudioTrack(name);
        // Set new track to minimum height so audio tracks don't "expand"
        // to the default 80px after export — they stay compact.
        t->setHeight(30.0f);
        speakerTracks.emplace(name, t);
        spdlog::info("AudioSync: created audio track '{}'", name);
        return t;
    };

    // Build a fresh AudioClip from a planned entry (tagged with its line).
    auto makeAudioClip = [](const PlannedAudio& pa) -> std::unique_ptr<AudioClip> {
        auto audioClip = std::make_unique<AudioClip>(pa.sourceFile);
        audioClip->setTimelineIn(pa.timelineIn);
        audioClip->setDuration(pa.duration);
        audioClip->setSourceIn(pa.sourceIn);
        audioClip->setSourceDuration(pa.sourceDuration);
        audioClip->setLabel(pa.label);
        audioClip->setSyncLine(pa.line);
        return audioClip;
    };

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
    auto placeGroupVisuals = [&](const PlannedGroup& group) -> int {
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
                && !tk->isCaptionTrack())
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

    // ── Mode detection ──────────────────────────────────────────────────
    // Any clip carrying a syncLine back-link means this sequence was
    // exported by a v29+ build before → update it in place instead of
    // wiping.  Legacy timelines (pre-v29 exports) have no back-links and
    // take the full path once; from then on every clip is tagged.
    bool incremental = false;
    for (size_t ti = 0; ti < timeline->trackCount() && !incremental; ++ti) {
        Track* t = timeline->track(ti);
        if (!t) continue;
        for (size_t ci = 0; ci < t->clipCount(); ++ci) {
            const Clip* c = t->clip(ci);
            if (c && c->syncLine() >= 0) { incremental = true; break; }
        }
    }

    if (!incremental) {
        return applyFull(timeline, plannedAudio, groups,
                         m_shotPresetManager != nullptr,
                         trackForSpeaker, makeAudioClip, placeGroupVisuals);
    }
    return applyIncremental(timeline, plannedAudio, groups,
                            trackForSpeaker, makeAudioClip, placeGroupVisuals);
}

namespace {

// ── FULL export: clear previous export-owned content, rebuild from plan ────

int applyFull(Timeline* timeline,
              const std::vector<PlannedAudio>& plannedAudio,
              const std::vector<PlannedGroup>& groups,
              bool hasShotMgr,
              const TrackForSpeakerFn& trackForSpeaker,
              const MakeAudioClipFn& makeAudioClip,
              const PlaceGroupVisualsFn& placeGroupVisuals)
{
    // Remove old speaker audio tracks from previous exports (clean
    // re-export).  A track is removed when it is empty or when its name
    // matches a speaker in this export (legacy exports only ever created
    // speaker-named tracks) — user-added audio tracks with their own
    // content (music, SFX) survive.
    {
        std::unordered_set<std::string> speakerNames;
        for (const auto& pa : plannedAudio)
            speakerNames.insert(pa.character.empty() ? std::string("VO")
                                                     : pa.character);
        std::vector<size_t> removeIndices;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (!t || t->type() != TrackType::Audio || t->isDivider()) continue;
            if (t->clipCount() == 0 || speakerNames.count(t->name()))
                removeIndices.push_back(i);
            else
                spdlog::info("AudioSync: keeping non-speaker audio track '{}' "
                             "({} clips)", t->name(), t->clipCount());
        }
        // Remove from end to preserve indices
        for (auto it = removeIndices.rbegin(); it != removeIndices.rend(); ++it)
            timeline->removeTrack(*it);
        spdlog::info("AudioSync: cleared {} old audio tracks", removeIndices.size());
    }

    // Place audio clips
    int exportCount = 0;
    for (const auto& pa : plannedAudio) {
        trackForSpeaker(pa.character)->addClip(makeAudioClip(pa));
        ++exportCount;
    }

    spdlog::info("AudioSync::exportToTimeline placed {} audio clips", exportCount);

    // -- DEFAULT SHOT PLACEMENT -- create SpineClip/VideoClip layers ----
    if (hasShotMgr) {
        // Clear existing export-owned visual clips (clean re-export).
        // Guards: the caption track is NEVER touched (a previous version
        // wiped subtitles here), and standalone user clips (no shot group,
        // no back-link) survive — only shot-group content is rebuilt.
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (!t || t->type() != TrackType::Video) continue;
            if (t->isCaptionTrack()) continue;
            for (size_t ci = t->clipCount(); ci > 0; --ci) {
                const Clip* c = t->clip(ci - 1);
                if (!c) continue;
                const bool exportOwned =
                    c->syncLine() >= 0
                    || (c->groupId() != 0 && !c->shotName().empty());
                if (exportOwned)
                    t->removeClip(ci - 1);
            }
        }
        spdlog::info("AudioSync::exportToTimeline cleared old visual clips from video tracks");

        for (const auto& group : groups)
            placeGroupVisuals(group);
    } else {
        spdlog::warn("AudioSync::exportToTimeline: no ShotPresetManager set, "
                     "skipping default shot placement");
    }

    return exportCount;
}

// ── INCREMENTAL export: diff plan vs back-linked clips, update in place ────

int applyIncremental(Timeline* timeline,
                     const std::vector<PlannedAudio>& plannedAudio,
                     const std::vector<PlannedGroup>& groups,
                     const TrackForSpeakerFn& trackForSpeaker,
                     const MakeAudioClipFn& makeAudioClip,
                     const PlaceGroupVisualsFn& placeGroupVisuals)
{
    struct ExistingRef {
        Track* track;
        Clip*  clip;
    };

    // ── Index existing export-owned clips by back-link ──────────────────
    std::map<int, std::vector<ExistingRef>> exAudio;   // line → audio clips
    std::map<int, std::vector<ExistingRef>> exVisual;  // group firstLine → visuals
    // Export-time speaker of each line = name of the audio track its clip
    // sits on.  Captured BEFORE the audio pass so the visual pass can tell
    // whether a group's character changed (→ rebuild from default shot).
    std::unordered_map<int, std::string> exLineSpeaker;

    for (size_t ti = 0; ti < timeline->trackCount(); ++ti) {
        Track* t = timeline->track(ti);
        if (!t || t->isDivider() || t->isCaptionTrack()) continue;
        for (size_t ci = 0; ci < t->clipCount(); ++ci) {
            Clip* c = t->clip(ci);
            if (!c || c->syncLine() < 0) continue;
            if (t->type() == TrackType::Audio
                && c->clipType() == ClipType::Audio) {
                exAudio[c->syncLine()].push_back({t, c});
                exLineSpeaker.emplace(c->syncLine(), t->name());
            } else if (t->type() == TrackType::Video && c->isVisual()
                       && !c->isCaption()) {
                exVisual[c->syncLine()].push_back({t, c});
            }
        }
    }

    // Audio tracks that lose sync clips may end up empty → removed at the
    // end (user tracks never appear here: their clips carry no back-link).
    std::unordered_set<Track*> touchedAudioTracks;

    auto removeExisting = [&](const ExistingRef& ref) {
        touchedAudioTracks.insert(ref.track);
        ref.track->removeClipById(ref.clip->id());  // frees the clip
    };

    // ── Audio pass ──────────────────────────────────────────────────────
    std::map<int, std::vector<const PlannedAudio*>> planByLine;
    for (const auto& pa : plannedAudio)
        planByLine[pa.line].push_back(&pa);

    int updated = 0, unchanged = 0, created = 0, removed = 0;

    for (const auto& [line, planned] : planByLine) {
        auto exIt = exAudio.find(line);
        std::vector<ExistingRef> existing =
            (exIt != exAudio.end()) ? exIt->second : std::vector<ExistingRef>{};
        std::sort(existing.begin(), existing.end(),
                  [](const ExistingRef& a, const ExistingRef& b) {
                      return a.clip->timelineIn() < b.clip->timelineIn();
                  });

        if (existing.size() == planned.size()) {
            // Update each clip in place — identity (id, fades, audio FX,
            // volume keyframes) survives; AudioSync wins on track, range
            // and position.
            for (size_t k = 0; k < planned.size(); ++k) {
                const PlannedAudio& pa = *planned[k];
                auto* ac = static_cast<AudioClip*>(existing[k].clip);
                Track* cur    = existing[k].track;
                Track* wanted = trackForSpeaker(pa.character);

                const bool fieldsChanged =
                       ac->mediaPath()      != pa.sourceFile
                    || ac->sourceIn()       != pa.sourceIn
                    || ac->duration()       != pa.duration
                    || ac->sourceDuration() != pa.sourceDuration
                    || ac->timelineIn()     != pa.timelineIn
                    || cur != wanted;
                if (!fieldsChanged) { ++unchanged; continue; }

                ac->setMediaPath(pa.sourceFile);
                ac->setLabel(pa.label);
                ac->setSourceIn(pa.sourceIn);
                ac->setSourceDuration(pa.sourceDuration);
                ac->setDuration(pa.duration);
                if (cur != wanted) {
                    touchedAudioTracks.insert(cur);
                    auto owned = cur->removeClipById(ac->id());
                    owned->setTimelineIn(pa.timelineIn);
                    wanted->addClip(std::move(owned));
                } else if (ac->timelineIn() != pa.timelineIn) {
                    cur->moveClip(cur->findClipIndexById(ac->id()),
                                  pa.timelineIn);
                }
                ++updated;
            }
        } else {
            // Range count changed (re-segmented) — recreate this line.
            for (const auto& ref : existing) { removeExisting(ref); ++removed; }
            for (const PlannedAudio* pa : planned) {
                trackForSpeaker(pa->character)->addClip(makeAudioClip(*pa));
                ++created;
            }
        }
    }

    // Lines that vanished from the plan (unconfirmed / removed from script)
    for (const auto& [line, refs] : exAudio) {
        if (planByLine.count(line)) continue;
        for (const auto& ref : refs) { removeExisting(ref); ++removed; }
    }

    // ── Visual pass ─────────────────────────────────────────────────────
    int vReused = 0, vRebuilt = 0, vRemoved = 0;
    std::unordered_set<int> plannedFirstLines;

    for (const auto& group : groups) {
        plannedFirstLines.insert(group.firstLine);

        auto exIt = exVisual.find(group.firstLine);
        std::vector<ExistingRef> existing =
            (exIt != exVisual.end()) ? exIt->second : std::vector<ExistingRef>{};

        // Same group (same first line) AND same speaker as at export time →
        // keep the clips (shot swaps, transforms, effects, transitions all
        // survive) and just re-span them.  Speaker unknown (no audio clip
        // was found for the line) counts as unchanged.
        auto spIt = exLineSpeaker.find(group.firstLine);
        const bool sameCharacter =
            (spIt == exLineSpeaker.end()) || (spIt->second == group.character);

        if (!existing.empty() && sameCharacter) {
            for (const auto& ref : existing) {
                if (ref.clip->duration() != group.totalDuration)
                    ref.clip->setDuration(group.totalDuration);
                if (ref.clip->timelineIn() != group.timelineStart)
                    ref.track->moveClip(
                        ref.track->findClipIndexById(ref.clip->id()),
                        group.timelineStart);
            }
            ++vReused;
        } else {
            for (const auto& ref : existing)
                ref.track->removeClipById(ref.clip->id());
            if (!existing.empty()) ++vRemoved;
            if (placeGroupVisuals(group) > 0) ++vRebuilt;
        }
    }

    // Visual groups that vanished from the plan
    for (const auto& [firstLine, refs] : exVisual) {
        if (plannedFirstLines.count(firstLine)) continue;
        for (const auto& ref : refs)
            ref.track->removeClipById(ref.clip->id());
        ++vRemoved;
    }

    // ── Drop speaker tracks left empty by removals ──────────────────────
    for (size_t i = timeline->trackCount(); i > 0; --i) {
        Track* t = timeline->track(i - 1);
        if (t && t->type() == TrackType::Audio && !t->isDivider()
            && t->clipCount() == 0 && touchedAudioTracks.count(t))
            timeline->removeTrack(i - 1);
    }

    spdlog::info("AudioSync incremental re-export: audio {} updated / {} "
                 "unchanged / {} created / {} removed; visuals {} groups "
                 "kept / {} rebuilt / {} removed",
                 updated, unchanged, created, removed,
                 vReused, vRebuilt, vRemoved);

    return updated + unchanged + created;
}

} // namespace

} // namespace rt
