/*
 * AudioSyncExportVisuals.cpp - Default shot visual placement for AudioSync export.
 */

#include "panels/audio/AudioSync.h"

#include "spine/ShotPreset.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/SpineClip.h"
#include "timeline/VideoClip.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace rt {

void AudioSync::placeDefaultShotVisuals(Timeline* timeline,
                                        const std::vector<TimelineExportGroup>& groups)
{
    if (!timeline) return;

    if (!m_shotPresetManager) {
        spdlog::warn("AudioSync::exportToTimeline: no ShotPresetManager set, skipping default shot placement");
        return;
    }

    for (size_t i = 0; i < timeline->trackCount(); ++i) {
        Track* track = timeline->track(i);
        if (!track || track->type() != TrackType::Video) continue;
        for (size_t clipIndex = track->clipCount(); clipIndex > 0; --clipIndex)
            track->removeClip(clipIndex - 1);
    }
    spdlog::info("AudioSync::exportToTimeline cleared old visual clips from video tracks");

    static uint64_t s_nextGroupId = 1;

    for (const auto& group : groups) {
        if (group.character.empty() || group.totalDuration <= 0) continue;

        auto preset = m_shotPresetManager->resolveDefaultShot(group.character);

        if (!preset) {
            spdlog::debug("  No default shot for '{}', skipping visual clips", group.character);
            continue;
        }

        const uint64_t groupId = s_nextGroupId++;
        const auto& layerOrder = preset->layerOrder();
        const int layerCount = static_cast<int>(layerOrder.size());

        int visibleCount = 0;
        for (int clipIndex = 0; clipIndex < layerCount; ++clipIndex) {
            const auto& layerRef = layerOrder[clipIndex];
            bool visible = false;
            if (layerRef.type == LayerType::Background) {
                auto* background = preset->background(layerRef.index);
                visible = (background && background->visible);
            } else {
                auto* character = preset->character(layerRef.index);
                visible = (character && character->visible);
            }
            if (visible) ++visibleCount;
        }

        // Real video tracks only — dividers are TrackType::Video but cannot
        // hold clips, so counting them would target the divider for placement
        // and Track::addClip would silently drop the clip.
        std::vector<size_t> videoIndices;
        for (size_t trackIndex = 0; trackIndex < timeline->trackCount(); ++trackIndex) {
            Track* tk = timeline->track(trackIndex);
            if (tk && tk->type() == TrackType::Video && !tk->isDivider())
                videoIndices.push_back(trackIndex);
        }

        while (static_cast<int>(videoIndices.size()) < visibleCount) {
            Track* track = timeline->addVideoTrack("");
            (void)track;
            size_t newIndex = videoIndices.empty() ? 0 : videoIndices.back() + 1;
            videoIndices.push_back(newIndex);
        }

        int placedIndex = 0;
        for (int layerIndex = layerCount - 1; layerIndex >= 0; --layerIndex) {
            const auto& layerRef = layerOrder[layerIndex];
            std::string layerId;

            size_t trackPosition = videoIndices.size() - 1 - static_cast<size_t>(placedIndex);
            Track* videoTrack = (trackPosition < videoIndices.size())
                ? timeline->track(videoIndices[trackPosition])
                : timeline->track(videoIndices.back());

            if (layerRef.type == LayerType::Background) {
                layerId = "background_" + std::to_string(layerRef.index);

                const BackgroundState* background = preset->background(layerRef.index);
                if (!background || !background->visible) continue;

                auto clip = std::make_unique<VideoClip>(background->path);
                clip->setTimelineIn(group.timelineStart);
                clip->setDuration(group.totalDuration);
                clip->setLabel(std::filesystem::path(background->path).stem().string());
                clip->setShotName("");
                clip->setGroupId(groupId);
                clip->setLayerId(layerId);
                constexpr float outputWidth = 1920.0f;
                constexpr float outputHeight = 1080.0f;
                clip->positionX().setDefaultValue((background->posX - 0.5f) * outputWidth);
                clip->positionY().setDefaultValue((background->posY - 0.5f) * outputHeight);
                clip->scaleX().setDefaultValue(background->scale);
                clip->scaleY().setDefaultValue(background->scale);
                clip->opacity().setDefaultValue(background->opacity);
                if (background->cropLeft > 0 || background->cropRight > 0 ||
                    background->cropTop > 0 || background->cropBottom > 0)
                    clip->setCrop(background->cropLeft, background->cropRight,
                                  background->cropTop, background->cropBottom);

                videoTrack->addClip(std::move(clip));
            } else if (layerRef.type == LayerType::Character) {
                layerId = "char_" + std::to_string(layerRef.index);

                const CharacterState* character = preset->character(layerRef.index);
                if (!character || !character->visible) continue;

                if (character->isVideoCharacter()) {
                    const std::string& videoPath = character->activeVideoPath();
                    if (videoPath.empty()) continue;

                    auto clip = std::make_unique<VideoClip>(videoPath);
                    clip->setTimelineIn(group.timelineStart);
                    clip->setDuration(group.totalDuration);
                    std::string label = character->characterName;
                    if (!character->animation.empty())
                        label += " - " + character->animation;
                    clip->setLabel(label);
                    clip->setShotName("");
                    clip->setGroupId(groupId);
                    clip->setLayerId(layerId);
                    clip->setCharacterName(character->characterName);
                    clip->setTalking(character->isTalking);
                    clip->setVideoMutePath(character->videoMutePath);
                    clip->setVideoTalkPath(character->videoTalkPath);

                    constexpr float outputWidth = 1920.0f;
                    constexpr float outputHeight = 1080.0f;
                    clip->positionX().setDefaultValue((character->posX - 0.5f) * outputWidth);
                    clip->positionY().setDefaultValue((character->posY - 0.5f) * outputHeight);
                    // Raw preset scale — the compositor applies the 0.85×
                    // COMPOSE character-fit dynamically via hFit normalization.
                    const float chScaleX = character->flipX ? -character->scale : character->scale;
                    const float chScaleY = character->flipY ? -character->scale : character->scale;
                    clip->scaleX().setDefaultValue(chScaleX);
                    clip->scaleY().setDefaultValue(chScaleY);
                    clip->opacity().setDefaultValue(character->opacity);
                    if (character->cropLeft > 0 || character->cropRight > 0 ||
                        character->cropTop > 0 || character->cropBottom > 0)
                        clip->setCrop(character->cropLeft, character->cropRight,
                                      character->cropTop, character->cropBottom);

                    videoTrack->addClip(std::move(clip));
                } else {
                    auto clip = std::make_unique<SpineClip>(character->characterName, character->outfit);
                    clip->setTimelineIn(group.timelineStart);
                    clip->setDuration(group.totalDuration);
                    clip->setAnimationName(character->animation);
                    clip->setTalking(character->isTalking);
                    clip->setLooping(true);
                    std::string label = character->characterName;
                    if (!character->animation.empty())
                        label += " - " + character->animation;
                    clip->setLabel(label);
                    clip->setShotName("");
                    clip->setGroupId(groupId);
                    clip->setLayerId(layerId);

                    constexpr float outputWidth = 1920.0f;
                    constexpr float outputHeight = 1080.0f;
                    clip->positionX().setDefaultValue((character->posX - 0.5f) * outputWidth);
                    clip->positionY().setDefaultValue((character->posY - 0.5f) * outputHeight);
                    clip->scaleX().setDefaultValue(character->flipX ? -character->scale : character->scale);
                    clip->scaleY().setDefaultValue(character->flipY ? -character->scale : character->scale);
                    clip->opacity().setDefaultValue(character->opacity);
                    if (character->cropLeft > 0 || character->cropRight > 0 ||
                        character->cropTop > 0 || character->cropBottom > 0)
                        clip->setCrop(character->cropLeft, character->cropRight,
                                      character->cropTop, character->cropBottom);

                    videoTrack->addClip(std::move(clip));
                }
            }
            ++placedIndex;
        }

        spdlog::info("  Created {} visual layers for '{}' (shot '{}')",
                     layerCount, group.character, preset->name());
    }
}

} // namespace rt
