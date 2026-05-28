/*
 * AudioSyncExport.cpp - Timeline export logic for AudioSync.
 * Split from AudioSyncData.cpp for maintainability.
 */
#include "panels/audio/AudioSync.h"
#include "ai/ScriptMatcher.h"
#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/AudioClip.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"
#include "Theme.h"

#include <spdlog/spdlog.h>

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>

namespace rt {

// File basename helper — returns just the filename portion of a path-like
// string. Used to label exported timeline clips with their source filename
// instead of shot/character names.
static std::string sourceBasename(const std::string& path)
{
    if (path.empty()) return {};
    try {
        return std::filesystem::path(path).filename().string();
    } catch (...) {
        auto p = path.find_last_of("/\\");
        return p == std::string::npos ? path : path.substr(p + 1);
    }
}

// True when the outfit name represents the character's "Default" outfit.
// Case-insensitive and treats empty as Default so labels like
// "Chime - default - idle" collapse to "Chime - Idle".
static bool isDefaultOutfit(const std::string& outfit)
{
    if (outfit.empty()) return true;
    if (outfit.size() != 7) return false;
    static constexpr char kDefault[] = "default";
    for (size_t i = 0; i < 7; ++i) {
        char c = outfit[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kDefault[i]) return false;
    }
    return true;
}

// Creates AudioClips AND visual clips (SpineClip/VideoClip) from default shots
// for each character, matching the old Python behavior.

int AudioSync::exportToTimeline(Timeline* timeline)
{
    if (!timeline) return 0;

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Gather confirmed clips, sorted by scriptLineNumber ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    std::vector<const SyncClip*> confirmed;
    for (const auto& c : m_clips) {
        if (c.scriptLineNumber >= 0 && c.matchState == 2) // 2 = confirmed
            confirmed.push_back(&c);
    }
    std::sort(confirmed.begin(), confirmed.end(),
              [](const SyncClip* a, const SyncClip* b) {
                  return a->scriptLineNumber < b->scriptLineNumber;
              });

    if (confirmed.empty()) {
        spdlog::warn("AudioSync::exportToTimeline ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â no confirmed clips");
        return 0;
    }

    spdlog::info("AudioSync::exportToTimeline ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â exporting {} confirmed clips",
                 confirmed.size());

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Build a lookup from lineNumber ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ ScriptLine character ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    std::unordered_map<int, std::string> lineCharacterMap;
    if (m_script) {
        for (const auto& line : m_script->lines)
            lineCharacterMap[line.lineNumber] = line.character;
    }

    // Remove old audio clips and tracks from previous exports (clean re-export)
    {
        std::vector<size_t> audioTrackIndices;
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            if (timeline->track(i)->type() == TrackType::Audio)
                audioTrackIndices.push_back(i);
        }
        // Remove from end to preserve indices
        for (auto it = audioTrackIndices.rbegin(); it != audioTrackIndices.rend(); ++it)
            timeline->removeTrack(*it);
        spdlog::info("AudioSync: cleared {} old audio tracks", audioTrackIndices.size());
    }

    // Per-speaker audio tracks: one track per character, named after the
    // character (e.g. "Chime", "Wells").  Tracks are created on demand the
    // first time a clip for that speaker is placed and reused thereafter.
    std::unordered_map<std::string, Track*> speakerTracks;
    auto trackForSpeaker = [&](const std::string& character) -> Track* {
        std::string name = character.empty() ? std::string("VO") : character;
        auto it = speakerTracks.find(name);
        if (it != speakerTracks.end()) return it->second;
        Track* t = timeline->addAudioTrack(name);
        // Set new track to minimum height so audio tracks don't "expand"
        // to the default 80px after export — they stay compact.
        t->setHeight(30.0f);
        speakerTracks.emplace(name, t);
        spdlog::info("AudioSync: created audio track '{}'", name);
        return t;
    };

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Group confirmed clips by scriptLineNumber (for shot grouping) ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    // Each group of segments from the same clip_group gets one shot placement
    struct ExportGroup {
        std::string character;
        int64_t     timelineStart{0};
        int64_t     totalDuration{0};
        std::vector<const SyncClip*> clips;
    };

    // ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ Place audio clips back-to-back and build groups ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬ÃƒÂ¢Ã¢â‚¬ÂÃ¢â€šÂ¬
    int64_t currentPos = 0; // ticks
    int exportCount = 0;
    static uint64_t s_nextGroupId = 1;

    // Build groups: consecutive clips for the same character form a group
    std::vector<ExportGroup> groups;
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
            groups.push_back({character, currentPos, 0, {}});
            lastCharacter = character;
        }
        groups.back().clips.push_back(sc);

        for (size_t ri = 0; ri < effectiveRanges.size(); ++ri) {
            const auto& [rangeStart, rangeEnd] = effectiveRanges[ri];
            double rangeDuration = rangeEnd - rangeStart;
            if (rangeDuration <= 0.0) continue;

            // Build the AudioClip
            auto audioClip = std::make_unique<AudioClip>(sc->sourceFile);
            audioClip->setTimelineIn(currentPos);
            audioClip->setDuration(secondsToTicks(rangeDuration));
            audioClip->setSourceIn(secondsToTicks(rangeStart));
            audioClip->setSourceDuration(secondsToTicks(sc->end));

            // Label: use the source audio file's basename so the timeline
            // shows the actual asset name instead of the character/shot.
            std::string label = sourceBasename(sc->sourceFile);
            if (label.empty()) label = character;
            audioClip->setLabel(label);

            // Route the clip to the per-speaker track for this character
            trackForSpeaker(character)->addClip(std::move(audioClip));

            groups.back().totalDuration += secondsToTicks(rangeDuration);
            currentPos += secondsToTicks(rangeDuration);
            ++exportCount;
        }
    }

    spdlog::info("AudioSync::exportToTimeline placed {} audio clips across {} speaker tracks",
                 exportCount, static_cast<int>(speakerTracks.size()));


    // -- DEFAULT SHOT PLACEMENT -- create SpineClip/VideoClip layers ----
    // For each character group, look up its "(Default)" shot preset and
    // place all visible layers (backgrounds + characters) on video tracks
    // covering the group's timeline span.  Layer order: layerOrder[0] is
    // FRONT (top V#), layerOrder[last] is BACK (V1).
    if (m_shotPresetManager) {
        // Clear existing visual clips from video tracks (clean re-export)
        for (size_t i = 0; i < timeline->trackCount(); ++i) {
            Track* t = timeline->track(i);
            if (!t || t->type() != TrackType::Video) continue;
            for (size_t ci = t->clipCount(); ci > 0; --ci)
                t->removeClip(ci - 1);
        }
        spdlog::info("AudioSync::exportToTimeline cleared old visual clips from video tracks");

        for (auto& group : groups) {
            if (group.character.empty() || group.totalDuration <= 0) continue;

            std::string shotName;
            auto preset = m_shotPresetManager->resolveDefaultShot(group.character);
            if (preset)
                shotName = preset->name();

            if (!preset) {
                spdlog::debug("  No default shot for '{}', skipping visual clips",
                              group.character);
                continue;
            }

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
            // but reject clips; counting them would mis-size the array and
            // (worse) target the divider as a clip destination — Track::addClip
            // returns nullptr for dividers, silently dropping the placed clip.
            std::vector<size_t> videoIndices;
            for (size_t vi = 0; vi < timeline->trackCount(); ++vi) {
                Track* tk = timeline->track(vi);
                if (tk && tk->type() == TrackType::Video && !tk->isDivider())
                    videoIndices.push_back(vi);
            }

            while (static_cast<int>(videoIndices.size()) < visibleCount) {
                Track* t = timeline->addVideoTrack("");
                (void)t;
                size_t newIdx = videoIndices.empty() ? 0 : videoIndices.back() + 1;
                videoIndices.push_back(newIdx);
            }

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
                    std::string bgLabel = std::filesystem::path(bg->path).stem().string();
                    vClip->setLabel(bgLabel);
                    vClip->setShotName(shotName);
                    vClip->setGroupId(groupId);
                    vClip->setLayerId(layerIdStr);
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

                } else if (layerRef.type == LayerType::Character) {
                    layerIdStr = "char_" + std::to_string(layerRef.index);

                    const CharacterState* ch = preset->character(layerRef.index);
                    if (!ch || !ch->visible) continue;

                    if (ch->isVideoCharacter()) {
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
                    }
                }
                ++placedIdx;
            }

            spdlog::info("  Created {} visual layers for '{}' (shot '{}')",
                         layerCount, group.character, preset->name());
        }
    } else {
        spdlog::warn("AudioSync::exportToTimeline: no ShotPresetManager set, "
                     "skipping default shot placement");
    }

    return exportCount;
}

} // namespace rt
