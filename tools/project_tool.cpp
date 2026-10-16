#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "project/ClipSerialization.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "Constants.h"
#include "PathUtils.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <fstream>
#include <map>

// Equal-length media-path replacement reaches timeline, bin, and the opaque
// AudioSync selected-render records without rewriting their unrelated data.
static size_t replacePaths(std::vector<uint8_t>& bytes, const std::map<std::string, std::string>& paths)
{
    size_t count = 0;
    for (const auto& [oldPath, newPath] : paths) {
        if (oldPath.size() != newPath.size()) throw std::runtime_error("Replacement path length differs");
        rt::BinaryWriter oldRecord, newRecord;
        oldRecord.writeString(oldPath); newRecord.writeString(newPath);
        auto start = bytes.begin();
        while ((start = std::search(start, bytes.end(), oldRecord.data().begin(), oldRecord.data().end())) != bytes.end()) {
            std::copy(newRecord.data().begin(), newRecord.data().end(), start);
            start += newRecord.size();
            ++count;
        }
    }
    return count;
}

static int replaceAlpha(rt::Project& project, rt::ProjectSerializer& serializer,
                        const char* output, const char* mapping)
{
    if (std::filesystem::exists(rt::utf8ToPath(output))) throw std::runtime_error("Output already exists");
    std::ifstream file(rt::utf8ToPath(mapping));
    std::map<std::string, std::string> paths, reverse;
    std::string oldPath, newPath;
    while (file >> std::quoted(oldPath) >> std::quoted(newPath)) {
        if (!oldPath.ends_with(".mp4") || !newPath.ends_with(".mov") ||
            oldPath.substr(0, oldPath.size() - 4) != newPath.substr(0, newPath.size() - 4))
            throw std::runtime_error("Expected same-basename MP4 to MOV replacement");
        if (!std::filesystem::exists(rt::utf8ToPath(newPath))) throw std::runtime_error("Replacement media missing");
        paths[oldPath] = newPath; reverse[newPath] = oldPath;
    }
    if (paths.empty() || !file.eof()) throw std::runtime_error("Invalid replacement mapping");
    const auto before = serializer.serialize(project);
    struct Removed { rt::Clip* clip; size_t index; std::unique_ptr<rt::Effect> effect; };
    std::vector<Removed> removed;
    std::map<std::string, size_t> hits;
    for (size_t s = 0; s < project.sequenceCount(); ++s)
        for (size_t t = 0; t < project.sequence(s)->trackCount(); ++t) {
            auto* track = project.sequence(s)->track(t);
            for (size_t c = 0; c < track->clipCount(); ++c) {
                auto* clip = dynamic_cast<rt::VideoClip*>(track->clip(c));
                if (!clip || !paths.contains(clip->mediaPath())) continue;
                ++hits[clip->mediaPath()];
                for (size_t e = clip->effects().effectCount(); e-- > 0;)
                    if (clip->effects().effect(e).effectType() == rt::EffectType::ChromaKey)
                        removed.push_back({clip, e, clip->effects().removeEffect(e)});
            }
        }
    if (hits.size() != paths.size()) throw std::runtime_error("A selected render is no longer in the timeline");
    auto after = serializer.serialize(project);
    const auto replacements = replacePaths(after, paths);
    auto check = after;
    if (replacePaths(check, reverse) != replacements) throw std::runtime_error("Path reversal count differs");
    if (check != serializer.serialize(project)) throw std::runtime_error("Unexpected path mutation");
    auto candidate = serializer.deserialize(after);
    if (!candidate || serializer.serialize(*candidate) != after) throw std::runtime_error("Candidate round-trip failed");
    if (!serializer.save(*candidate, rt::utf8ToPath(output))) throw std::runtime_error("Candidate save failed");
    auto saved = serializer.load(rt::utf8ToPath(output));
    if (!saved || serializer.serialize(*saved) != after) throw std::runtime_error("Saved project round-trip failed");
    const auto effectCount = removed.size();
    for (auto it = removed.rbegin(); it != removed.rend(); ++it)
        it->clip->effects().insertEffect(it->index, std::move(it->effect));
    if (serializer.serialize(project) != before) throw std::runtime_error("Unrelated project data changed");
    std::cout << "REPLACED " << paths.size() << " selected media files in " << replacements
              << " project references; removed " << effectCount << " Ultra Key effects.\n"
              << "Verified exact reversal and saved-project round-trip; all other edits preserved.\n";
    return 0;
}

static int restartShotBackgrounds(rt::Project& project, rt::ProjectSerializer& serializer,
                                 const char* output, const char* sequence, const char* background)
{
    if (std::filesystem::exists(rt::utf8ToPath(output))) throw std::runtime_error("Output already exists");
    rt::Timeline* timeline = nullptr;
    for (size_t i = 0; i < project.sequenceCount(); ++i)
        if (project.sequence(i)->name() == sequence) {
            if (timeline) throw std::runtime_error("Ambiguous sequence");
            timeline = project.sequence(i);
        }
    if (!timeline) throw std::runtime_error("Sequence not found");
    rt::Track* bgTrack = nullptr;
    std::vector<rt::VideoClip*> backgrounds, shots;
    for (size_t t = 0; t < timeline->trackCount(); ++t) {
        auto* track = timeline->track(t);
        for (size_t c = 0; c < track->clipCount(); ++c) {
            auto* clip = dynamic_cast<rt::VideoClip*>(track->clip(c));
            if (!clip) continue;
            if (std::filesystem::equivalent(rt::utf8ToPath(clip->mediaPath()), rt::utf8ToPath(background))) {
                if (bgTrack && bgTrack != track) throw std::runtime_error("Multiple background tracks");
                bgTrack = track;
                backgrounds.push_back(clip);
            } else if (clip->label().ends_with("WIDE") || clip->label().ends_with("CU")) shots.push_back(clip);
        }
    }
    if (!bgTrack || shots.empty() || bgTrack->isLocked())
        throw std::runtime_error("Expected unlocked background track");
    for (const auto& transition : bgTrack->transitions())
        for (auto* bg : backgrounds)
            if (transition.leftClipId == bg->id() || transition.rightClipId == bg->id())
                throw std::runtime_error("A background transition needs manual review");
    std::sort(shots.begin(), shots.end(), [](auto* a, auto* b) { return a->timelineIn() < b->timelineIn(); });
    // All pieces being joined must agree on everything except identity and
    // timing. This preserves framing/effects and rejects ambiguous edits.
    auto appearance = [](const rt::Clip& clip) {
        auto copy = clip.clone();
        copy->setId(0);
        copy->setTimelineIn(0);
        copy->setDuration(1);
        copy->setSourceIn(0);
        rt::BinaryWriter writer;
        rt::writeClip(writer, *copy);
        return writer.data();
    };
    std::vector<std::unique_ptr<rt::Clip>> replacements;
    int64_t totalCoverage = 0;
    for (auto* shot : shots) {
        rt::VideoClip* source = nullptr;
        int64_t covered = 0;
        for (auto* bg : backgrounds) {
            const auto overlap = std::min(bg->timelineOut(), shot->timelineOut()) -
                                 std::max(bg->timelineIn(), shot->timelineIn());
            if (overlap <= 0) continue;
            if (!source) source = bg;
            else if (appearance(*source) != appearance(*bg))
                throw std::runtime_error("Background appearance changes within a shot");
            covered += overlap;
        }
        if (!source || covered != shot->duration()) throw std::runtime_error("Shot background coverage is ambiguous");
        if (!source->positionX().keyframes().empty() || !source->positionY().keyframes().empty() ||
            !source->scaleX().keyframes().empty() || !source->scaleY().keyframes().empty() || source->speed() != 1.0)
            throw std::runtime_error("Animated or retimed background needs manual review");
        totalCoverage += covered;
        for (int64_t from = shot->timelineIn(); from < shot->timelineOut();) {
            auto copy = source->clone();
            const auto remaining = shot->timelineOut() - from;
            const auto length = source->sourceDuration() > 0 ? std::min(remaining, source->sourceDuration()) : remaining;
            copy->setTimelineIn(from);
            copy->setDuration(length);
            copy->setSourceIn(0);
            replacements.push_back(std::move(copy));
            from += length;
        }
    }
    int64_t bgCoverage = 0;
    for (auto* bg : backgrounds) bgCoverage += bg->duration();
    if (bgCoverage != totalCoverage) throw std::runtime_error("Unmatched background material");
    const auto before = serializer.serialize(project);
    std::vector<std::unique_ptr<rt::Clip>> originals;
    // Intro/stinger clips share this track; replace only the loop backgrounds.
    for (auto* bg : backgrounds) originals.push_back(bgTrack->removeClipById(bg->id()));
    const auto count = replacements.size();
    std::vector<uint64_t> replacementIds;
    for (auto& clip : replacements) {
        replacementIds.push_back(clip->id());
        bgTrack->addClip(std::move(clip));
    }
    const auto after = serializer.serialize(project);
    if (!serializer.save(project, rt::utf8ToPath(output))) throw std::runtime_error("Save failed");
    for (auto id : replacementIds) (void)bgTrack->removeClipById(id);
    for (auto& clip : originals) bgTrack->addClip(std::move(clip));
    if (serializer.serialize(project) != before) throw std::runtime_error("Unexpected unrelated project mutation");
    auto reloaded = serializer.load(rt::utf8ToPath(output));
    if (!reloaded || serializer.serialize(*reloaded) != after) throw std::runtime_error("Round-trip failed");
    std::cout << "UPDATED " << shots.size() << " shots with " << count
              << " fresh background clips starting at source zero. All other project data preserved.\n";
    return 0;
}

// Writes a separate candidate. The caller backs up and installs it only after
// reviewing the report. Refuse ambiguous overlaps and animated transforms.
static int centerWideBackgrounds(rt::Project& project, rt::ProjectSerializer& serializer,
                                  const char* output, const char* sequence, const char* background)
{
    if (std::filesystem::exists(rt::utf8ToPath(output)))
        throw std::runtime_error("Output already exists");
    rt::Timeline* timeline = nullptr;
    for (size_t i = 0; i < project.sequenceCount(); ++i)
        if (project.sequence(i)->name() == sequence) {
            if (timeline) throw std::runtime_error("Sequence name is ambiguous");
            timeline = project.sequence(i);
        }
    if (!timeline || timeline->settings().resolution().width != 1920 ||
        timeline->settings().resolution().height != 1080)
        throw std::runtime_error("Expected named 1920x1080 sequence");
    std::vector<rt::VideoClip*> shots, backgrounds;
    for (size_t t = 0; t < timeline->trackCount(); ++t)
        for (size_t c = 0; c < timeline->track(t)->clipCount(); ++c)
            if (auto* clip = dynamic_cast<rt::VideoClip*>(timeline->track(t)->clip(c))) {
                if (std::filesystem::equivalent(rt::utf8ToPath(clip->mediaPath()), rt::utf8ToPath(background)))
                    backgrounds.push_back(clip);
                else if (clip->label().ends_with("WIDE") || clip->label().ends_with("CU"))
                    shots.push_back(clip);
            }
    std::sort(shots.begin(), shots.end(), [](auto* a, auto* b) { return a->timelineIn() < b->timelineIn(); });
    auto first = std::find_if(shots.begin(), shots.end(), [](auto* s) { return s->label().ends_with("WIDE"); });
    if (first == shots.end() || backgrounds.empty()) throw std::runtime_error("No shots/backgrounds found");
    struct Change { rt::Clip* clip; std::array<float, 4> before; };
    std::vector<Change> changes;
    size_t wideShots = 0;
    for (auto* shot : shots) if (shot->label().ends_with("WIDE")) ++wideShots;
    for (auto* bg : backgrounds) {
        bool wide = false, other = false;
        int64_t covered = 0;
        for (auto* shot : shots) {
            const auto overlap = std::min(bg->timelineOut(), shot->timelineOut()) -
                                 std::max(bg->timelineIn(), shot->timelineIn());
            if (overlap <= 0) continue;
            if (shot->label().ends_with("WIDE")) {
                wide = true;
                covered += overlap;
            } else other = true;
        }
        if (!wide) continue;
        if (other || covered != bg->duration()) throw std::runtime_error("Ambiguous background coverage");
        if (!bg->positionX().keyframes().empty() || !bg->positionY().keyframes().empty() ||
            !bg->scaleX().keyframes().empty() || !bg->scaleY().keyframes().empty())
            throw std::runtime_error("Background has animated transforms");
        changes.push_back({bg, {bg->positionX().defaultValue(), bg->positionY().defaultValue(),
                               bg->scaleX().defaultValue(), bg->scaleY().defaultValue()}});
    }
    if (changes.empty()) throw std::runtime_error("No backgrounds to update");
    const auto before = serializer.serialize(project);
    for (auto& change : changes) {
        change.clip->positionX().setDefaultValue(0.0f);
        change.clip->positionY().setDefaultValue(0.0f);
        change.clip->scaleX().setDefaultValue(1.0f);
        change.clip->scaleY().setDefaultValue(1.0f);
    }
    const auto after = serializer.serialize(project);
    if (!serializer.save(project, rt::utf8ToPath(output))) throw std::runtime_error("Save failed");
    // Prove that reversing just these four values restores the entire project.
    for (auto& change : changes) {
        change.clip->positionX().setDefaultValue(change.before[0]);
        change.clip->positionY().setDefaultValue(change.before[1]);
        change.clip->scaleX().setDefaultValue(change.before[2]);
        change.clip->scaleY().setDefaultValue(change.before[3]);
    }
    if (serializer.serialize(project) != before) throw std::runtime_error("Unexpected project mutation");
    auto reloaded = serializer.load(rt::utf8ToPath(output));
    if (!reloaded || serializer.serialize(*reloaded) != after)
        throw std::runtime_error("Saved project did not round-trip exactly");
    std::cout << "UPDATED " << changes.size() << " background pieces under " << wideShots
              << " wide shots centred at 100%. All other data preserved.\n";
    return 0;
}

// Offline project inspection. Uses the application's own reader so clip
// metadata is interpreted identically to the editor.
int main(int argc, char** argv)
{
    if (argc != 2 && argc != 5 && argc != 6) {
        std::cerr << "Usage: roundtable_project_tool input.rtp [output.rtp sequence background --center-wide-background|--restart-backgrounds]\n";
        return 2;
    }
    rt::ProjectSerializer serializer;
    auto project = serializer.load(rt::utf8ToPath(argv[1]));
    if (!project) return 1;
    if (argc == 5) {
        try {
            if (std::string(argv[4]) != "--replace-alpha") return 2;
            return replaceAlpha(*project, serializer, argv[2], argv[3]);
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    if (argc == 6) {
        try {
            if (std::string(argv[5]) == "--restart-backgrounds")
                return restartShotBackgrounds(*project, serializer, argv[2], argv[3], argv[4]);
            if (std::string(argv[5]) == "--center-wide-background")
                return centerWideBackgrounds(*project, serializer, argv[2], argv[3], argv[4]);
            return 2;
        }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    std::cout << std::setprecision(12);
    for (const auto& bin : project->binFiles()) std::cout << "BIN " << std::quoted(rt::pathToUtf8(bin)) << '\n';
    std::cout << "ACTIVE " << project->activeSequenceIndex() << '\n';
    for (size_t s = 0; s < project->sequenceCount(); ++s) {
        auto* timeline = project->sequence(s);
        const auto size = timeline->settings().resolution();
        std::cout << "SEQUENCE " << s << ' ' << std::quoted(timeline->name())
                  << ' ' << size.width << 'x' << size.height << " fps="
                  << timeline->settings().frameRate() << '\n';
        for (size_t t = 0; t < timeline->trackCount(); ++t) {
            auto* track = timeline->track(t);
            std::cout << "TRACK " << t << ' ' << std::quoted(track->name())
                      << " muted=" << track->isMuted() << " locked=" << track->isLocked() << '\n';
            for (const auto& tr : track->transitions())
                std::cout << "TRANSITION type=" << int(tr.type) << " duration=" << tr.duration
                          << " offset=" << tr.offset << " left=" << tr.leftClipId << " right=" << tr.rightClipId
                          << " edit=" << tr.editPointTick << " param=" << tr.param1 << ',' << tr.param2 << '\n';
            for (size_t c = 0; c < track->clipCount(); ++c) {
                auto* clip = track->clip(c);
                auto* video = dynamic_cast<rt::VideoClip*>(clip);
                std::cout << "CLIP " << clip->id() << ' ' << std::quoted(clip->label())
                          << " in=" << double(clip->timelineIn()) / rt::kTicksPerSecond
                          << " duration=" << double(clip->duration()) / rt::kTicksPerSecond
                          << " sourceIn=" << double(clip->sourceIn()) / rt::kTicksPerSecond
                          << " x=" << clip->positionX().defaultValue()
                          << " y=" << clip->positionY().defaultValue()
                          << " scale=" << clip->scaleX().defaultValue() << ',' << clip->scaleY().defaultValue();
                std::cout << " interpolation=" << int(clip->timeInterpolation());
                if (video) std::cout << " media=" << std::quoted(video->mediaPath())
                                    << " source=" << video->sourceWidth() << 'x' << video->sourceHeight()
                                    << " fps=" << video->sourceFps();
                std::cout << " effects=";
                for (size_t e = 0; e < clip->effects().effectCount(); ++e) {
                    const auto& effect = clip->effects().effect(e);
                    std::cout << int(effect.effectType()) << ':' << effect.isEnabled() << ',';
                }
                std::cout << '\n';
                auto dumpTrack = [](const char* name, const rt::KeyframeTrack<float>& k) {
                    std::cout << ' ' << name << '=' << k.defaultValue();
                    for (const auto& frame : k.keyframes())
                        std::cout << '[' << frame.time << ':' << frame.value << ':' << int(frame.interp) << ']';
                };
                std::cout << "DETAIL " << clip->id() << " ticks=" << clip->timelineIn() << ',' << clip->duration()
                          << ',' << clip->sourceIn();
                dumpTrack("opacity", clip->opacity());
                if (auto* a = dynamic_cast<rt::AudioClip*>(clip)) {
                    std::cout << " audio=" << std::quoted(a->mediaPath()) << " sourceDuration=" << a->sourceDuration()
                              << " fades=" << a->fadeInDuration() << ',' << a->fadeOutDuration();
                    dumpTrack("volume", a->volume());
                }
                if (auto* v = dynamic_cast<rt::VideoClip*>(clip)) std::cout << " sourceDuration=" << v->sourceDuration();
                if (auto* im = dynamic_cast<rt::ImageClip*>(clip)) std::cout << " image=" << std::quoted(im->mediaPath());
                std::cout << '\n';
                for (size_t e = 0; e < clip->effects().effectCount(); ++e) {
                    const auto& effect = clip->effects().effect(e);
                    std::cout << "FX " << clip->id() << ' ' << int(effect.effectType());
                    for (const auto& p : effect.params()) dumpTrack(p.name.c_str(), p.track);
                    std::cout << '\n';
                }
            }
        }
    }
}
