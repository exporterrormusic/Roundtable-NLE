#include "panels/comfy/ComfyTimelineTemplate.h"
#include "Constants.h"
#include "PathUtils.h"
#include "audio/AudioFile.h"
#include "decode/VideoDecoder.h"
#include "playback/PlaybackController.h"
#include "project/Project.h"
#include "project/ProjectSerializer.h"
#include "timeline/AudioClip.h"
#include "timeline/ImageClip.h"
#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/VideoClip.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace rt {
namespace {
int64_t ticks(double sec) { return std::llround(sec * kTicksPerSecond); }
QString sectionKey(const std::string& name)
{
    auto key = QString::fromStdString(name).toUpper().trimmed();
    key.remove('-'); key.remove(' '); key.remove('_');
    return key;
}
std::string mediaPath(const Clip& clip)
{
    if (auto* v = dynamic_cast<const VideoClip*>(&clip)) return v->mediaPath();
    if (auto* a = dynamic_cast<const AudioClip*>(&clip)) return a->mediaPath();
    if (auto* i = dynamic_cast<const ImageClip*>(&clip)) return i->mediaPath();
    return {};
}
void setMediaPath(Clip& clip, const std::string& path)
{
    if (auto* v = dynamic_cast<VideoClip*>(&clip)) v->setMediaPath(path);
    else if (auto* a = dynamic_cast<AudioClip*>(&clip)) a->setMediaPath(path);
    else if (auto* i = dynamic_cast<ImageClip*>(&clip)) i->setMediaPath(path);
}
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int64_t sourceTimecodeTicks(const QString& text, double fps)
{
    auto normalized = text;
    normalized.replace(';', ':');
    const auto values = normalized.split(':');
    require(values.size() == 4, "Invalid source timecode");
    int fields[4]{};
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        fields[i] = values[i].toInt(&ok);
        require(ok && fields[i] >= 0, "Invalid source timecode field");
    }
    // Use the Source Monitor's own conversion, including normalization of
    // an entered frame count that exceeds the nominal rate.
    return timecodeToTick(Timecode{fields[0], fields[1], fields[2], fields[3]}, fps);
}
}

struct ComfyTimelineTemplate::Data
{
    std::unique_ptr<Project> project;
    Timeline* source{};
    QJsonObject recipe;
    QDir folder;
    std::unordered_map<uint64_t, Clip*> clips;
    std::unordered_map<uint64_t, Track*> tracks;

    Clip& clip(const QJsonObject& object, const char* role) const
    {
        const auto id = static_cast<uint64_t>(object.value(QLatin1String(role)).toDouble());
        auto found = clips.find(id);
        if (found == clips.end()) throw std::runtime_error(std::string("Missing template clip: ") + role);
        return *found->second;
    }
    std::unique_ptr<Clip> copy(const Clip& prototype) const
    {
        auto result = prototype.clone();
        result->setGroupId(0);
        result->setLinkId(0);
        const QString replacement = recipe.value("mediaOverrides").toObject()
            .value(QString::number(prototype.id())).toString();
        if (!replacement.isEmpty()) setMediaPath(*result, folder.absoluteFilePath(replacement).toStdString());
        const auto path = mediaPath(*result);
        if (!path.empty() && !QFileInfo::exists(QString::fromStdString(path)))
            throw std::runtime_error("Missing template media: " + path);
        return result;
    }
    void addFlash(Track& target, const Clip& prototype, int64_t cut, int64_t originalCut, int64_t assemblyStart) const
    {
        auto flash = copy(prototype);
        flash->setTimelineIn(cut + prototype.timelineIn() - originalCut);
        require(flash->timelineIn() >= assemblyStart, "Stinger is too short for its white transition");
        const auto shift = flash->timelineIn() - prototype.timelineIn();
        const auto id = flash->id();
        target.addClip(std::move(flash));
        for (auto tr : tracks.at(prototype.id())->transitions()) {
            if (tr.leftClipId != prototype.id() && tr.rightClipId != prototype.id()) continue;
            require(tr.leftClipId == 0 || tr.rightClipId == 0, "White flash must have independent fades");
            if (tr.leftClipId) tr.leftClipId = id;
            if (tr.rightClipId) tr.rightClipId = id;
            tr.editPointTick += shift;
            target.addTransition(tr);
        }
    }
};

ComfyTimelineTemplate::ComfyTimelineTemplate() : d(std::make_unique<Data>()) {}
ComfyTimelineTemplate::~ComfyTimelineTemplate() = default;

bool ComfyTimelineTemplate::load(const QString& path, QString* error)
{
    try {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "Cannot open timeline template recipe");
        QJsonParseError parse;
        auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        require(parse.error == QJsonParseError::NoError && doc.isObject(), "Invalid timeline template JSON");
        d->recipe = doc.object();
        require(d->recipe.value("version").toInt() == 1, "Unsupported timeline template version");
        d->folder = QFileInfo(path).absoluteDir();
        const auto projectPath = d->folder.absoluteFilePath(d->recipe.value("project").toString());
        d->project = ProjectSerializer{}.load(utf8ToPath(projectPath.toStdString()));
        require(bool(d->project), "Cannot read timeline template project");
        const auto name = d->recipe.value("sequence").toString().toStdString();
        for (size_t i = 0; i < d->project->sequenceCount(); ++i)
            if (d->project->sequence(i)->name() == name) d->source = d->project->sequence(i);
        require(d->source != nullptr, "Template sequence not found");
        for (size_t t = 0; t < d->source->trackCount(); ++t)
            for (size_t c = 0; c < d->source->track(t)->clipCount(); ++c) {
                auto* clip = d->source->track(t)->clip(c);
                d->clips[clip->id()] = clip;
                d->tracks[clip->id()] = d->source->track(t);
            }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QStringLiteral("Timeline template: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
}

void ComfyTimelineTemplate::applyCharacterStyle(VideoClip& target, bool closeUp, bool centered) const
{
    auto& source = d->clip(d->recipe, closeUp ? "closeUpCharacter" : centered ? "centeredCharacter" : "wideCharacter");
    target.positionX() = source.positionX(); target.positionY() = source.positionY();
    target.scaleX() = source.scaleX(); target.scaleY() = source.scaleY();
    target.rotation() = source.rotation(); target.opacity() = source.opacity();
    target.anchorX() = source.anchorX(); target.anchorY() = source.anchorY();
    if (const auto* video = dynamic_cast<const VideoClip*>(&source)) {
        const auto res = d->source->settings().resolution();
        auto cover = [&](uint32_t w, uint32_t h) {
            return std::max(double(res.width) / std::max(1u, w), double(res.height) / std::max(1u, h));
        };
        const auto ratio = cover(video->sourceWidth(), video->sourceHeight()) / cover(target.sourceWidth(), target.sourceHeight());
        target.scaleX().setDefaultValue(static_cast<float>(source.scaleX().defaultValue() * ratio));
        target.scaleY().setDefaultValue(static_cast<float>(source.scaleY().defaultValue() * ratio));
    }
    if (auto effects = source.effects().clone()) target.effects() = std::move(*effects);
}

bool ComfyTimelineTemplate::build(const std::vector<AssemblyShot>& shots, int64_t start, double backgroundSec,
                                Timeline& timeline, AssemblyPlan& plan, QString* error)
{
    try {
        require(d->source && !shots.empty(), "No template or rendered shots");
        if (start == 0) timeline.setSettings(d->source->settings());
        require(timeline.settings().resolution() == d->source->settings().resolution(),
                "Template and destination sequence resolutions must match");
        // Validate character roles before staging any production clips.
        for (const auto* role : {"wideCharacter", "centeredCharacter", "closeUpCharacter"})
            require(dynamic_cast<VideoClip*>(&d->clip(d->recipe, role)) != nullptr, "Invalid character template");
        auto videoTracks = [&]() {
            std::vector<Track*> result;
            for (size_t i = 0; i < timeline.trackCount(); ++i) {
                auto* t = timeline.track(i);
                if (t->type() == TrackType::Video && !t->isLocked() && !t->isDivider() && !t->isCaptionTrack()) result.push_back(t);
            }
            return result;
        };
        auto video = videoTracks();
        while (video.size() < 6) { timeline.addVideoTrack(""); video = videoTracks(); }
        const auto base = video.size() - 6;
        Track& bar = *video[base]; Track& flashes = *video[base + 1];
        Track& adjustment = *video[base + 2]; Track& endFlashes = *video[base + 3];
        Track& background = *video[base + 5];
        std::vector<Track*> audio;
        for (size_t i = 0; i < timeline.trackCount(); ++i) {
            auto* t = timeline.track(i);
            if (t->type() == TrackType::Audio && !t->isLocked() && !t->isDivider()) audio.push_back(t);
        }
        while (audio.size() < 2) audio.push_back(timeline.addAudioTrack(audio.empty() ? "Dialogue" : "Music"));
        auto& music = *audio[1];
        const auto sections = d->recipe.value("sections").toObject();
        plan = {};
        int64_t cursor = start;
        for (size_t first = 0; first < shots.size();) {
            size_t end = first + 1;
            while (end < shots.size() && sectionKey(shots[end].segment) == sectionKey(shots[first].segment)) ++end;
            const auto key = sectionKey(shots[first].segment);
            const auto spec = sections.value(key).toObject();
            if (spec.isEmpty()) throw std::runtime_error("No timeline template for section: " + shots[first].segment);
            auto& videoProto = d->clip(spec, "video");
            auto intro = d->copy(videoProto);
            auto lead = d->copy(d->clip(spec, "leadMusic"));
            auto bed = d->copy(d->clip(spec, "bedMusic"));
            auto* introVideo = dynamic_cast<VideoClip*>(intro.get());
            auto* leadAudio = dynamic_cast<AudioClip*>(lead.get());
            auto* bedAudio = dynamic_cast<AudioClip*>(bed.get());
            require(introVideo && leadAudio && bedAudio, "Invalid section video/music roles");
            if (spec.contains("videoSourceTimecode")) {
                VideoDecoder dec;
                require(dec.open(utf8ToPath(introVideo->mediaPath()), true), "Cannot probe section stinger");
                const auto fps = dec.info().fps > 0 ? dec.info().fps : 24.0;
                introVideo->setSourceIn(sourceTimecodeTicks(spec.value("videoSourceTimecode").toString(), fps));
                introVideo->setSourceDuration(ticks(dec.info().duration));
                introVideo->setDuration(introVideo->sourceDuration() - introVideo->sourceIn());
            }
            require(intro->duration() > 0, "Stinger in point is past its end");
            const auto leadLength = intro->duration();
            if (spec.contains("musicFile")) {
                const auto path = d->folder.absoluteFilePath(spec.value("musicFile").toString()).toStdString();
                AudioFile file;
                require(file.open(utf8ToPath(path)), "Cannot read section music");
                // WAV has no video rate. SourceMonitor::loadClip uses 24 fps
                // for this case; do not substitute the destination's 60 fps.
                const auto sourceIn = sourceTimecodeTicks(spec.value("musicSourceTimecode").toString(), 24.0);
                leadAudio->setMediaPath(path); bedAudio->setMediaPath(path);
                leadAudio->setSourceDuration(ticks(file.info().duration));
                bedAudio->setSourceDuration(ticks(file.info().duration));
                leadAudio->setSourceIn(sourceIn); leadAudio->setDuration(leadLength);
                bedAudio->setSourceIn(sourceIn + leadLength);
            }
            const auto leadMusicLength = lead->duration();
            require(leadMusicLength > 0 && leadMusicLength <= leadLength, "Invalid lead-in music duration");
            std::vector<AssemblyShot> group(shots.begin() + first, shots.begin() + end);
            auto section = planAssembly(group, cursor + leadLength, 0.0, backgroundSec);
            const auto sectionEnd = section.end;
            for (auto ps : section.shots) { ps.index += first; plan.shots.push_back(ps); }
            for (auto bg : section.backgrounds) { bg.firstShot += first; plan.backgrounds.push_back(bg); }
            plan.markers.push_back({cursor + leadLength, shots[first].segment});
            intro->setTimelineIn(cursor);
            background.addClip(std::move(intro));
            if (spec.contains("leadFlash"))
                d->addFlash(flashes, d->clip(spec, "leadFlash"), cursor, videoProto.timelineIn(), start);
            if (spec.contains("tailFlash"))
                d->addFlash(flashes, d->clip(spec, "tailFlash"), cursor + leadLength, videoProto.timelineOut(), start);
            lead->setTimelineIn(cursor);
            const auto leadId = lead->id();
            require(leadAudio->sourceIn() + lead->duration() <= leadAudio->sourceDuration(), "Lead-in music exceeds its source");
            music.addClip(std::move(lead));
            const auto available = bedAudio->sourceDuration() - bedAudio->sourceIn();
            auto loopLength = static_cast<int64_t>(spec.value("musicLoopTicks").toDouble(double(available)));
            require(loopLength > 0 && loopLength <= available, "Invalid music loop bounds");
            uint64_t firstBedId = 0;
            for (int64_t at = cursor + leadMusicLength; at < sectionEnd;) {
                auto part = bed->clone();
                part->setTimelineIn(at);
                part->setDuration(std::min(loopLength, sectionEnd - at));
                if (!firstBedId) firstBedId = part->id();
                at += part->duration();
                music.addClip(std::move(part));
            }
            // Preserve the authored loud-to-quiet music crossfade, with new IDs.
            const auto& leadProto = d->clip(spec, "leadMusic");
            const auto& bedProto = d->clip(spec, "bedMusic");
            for (auto tr : d->tracks.at(leadProto.id())->transitions()) {
                if (tr.leftClipId != leadProto.id() || tr.rightClipId != bedProto.id()) continue;
                tr.leftClipId = leadId; tr.rightClipId = firstBedId;
                tr.editPointTick = cursor + leadMusicLength;
                tr.duration = std::min(tr.duration, 2 * std::min(leadMusicLength, sectionEnd - tr.editPointTick));
                if (firstBedId && tr.duration > 0) music.addTransition(tr);
            }
            cursor = sectionEnd;
            first = end;
        }
        if (d->recipe.contains("endingVideo")) {
            auto& prototype = d->clip(d->recipe, "endingVideo");
            auto ending = d->copy(prototype);
            ending->setTimelineIn(cursor);
            if (d->recipe.contains("endingFlash"))
                d->addFlash(endFlashes, d->clip(d->recipe, "endingFlash"), cursor, prototype.timelineIn(), start);
            cursor += ending->duration();
            background.addClip(std::move(ending));
        }
        for (auto [role, track] : {std::pair{"bar", &bar}, std::pair{"adjustment", &adjustment}}) {
            auto overlay = d->copy(d->clip(d->recipe, role));
            overlay->setTimelineIn(start); overlay->setSourceIn(0); overlay->setDuration(cursor - start);
            track->addClip(std::move(overlay));
        }
        plan.end = cursor;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = QStringLiteral("Timeline template: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
}
} // namespace rt
