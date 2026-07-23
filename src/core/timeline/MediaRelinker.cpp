#include "timeline/MediaRelinker.h"

#include "timeline/Timeline.h"
#include "timeline/Track.h"
#include "timeline/Clip.h"
#include "timeline/AudioClip.h"
#include "timeline/VideoClip.h"
#include "timeline/ImageClip.h"
#include "project/Project.h"
#include "spine/ShotPreset.h"

#include <spdlog/spdlog.h>

namespace rt::MediaRelinker {

int relinkPath(Timeline* timeline,
               const std::string& oldPath,
               const std::string& newPath)
{
    if (!timeline || oldPath.empty() || newPath.empty() || oldPath == newPath)
        return 0;

    int updated = 0;
    const size_t nTracks = timeline->trackCount();
    for (size_t ti = 0; ti < nTracks; ++ti) {
        Track* track = timeline->track(ti);
        if (!track) continue;
        const size_t nClips = track->clipCount();
        for (size_t ci = 0; ci < nClips; ++ci) {
            Clip* c = track->clip(ci);
            if (!c) continue;
            if (auto* vc = dynamic_cast<VideoClip*>(c)) {
                if (vc->mediaPath() == oldPath) { vc->setMediaPath(newPath); ++updated; }
            } else if (auto* ac = dynamic_cast<AudioClip*>(c)) {
                if (ac->mediaPath() == oldPath) { ac->setMediaPath(newPath); ++updated; }
            } else if (auto* ic = dynamic_cast<ImageClip*>(c)) {
                if (ic->mediaPath() == oldPath) { ic->setMediaPath(newPath); ++updated; }
            }
        }
    }
    spdlog::info("MediaRelinker: replaced {} reference(s): '{}' -> '{}'",
                 updated, oldPath, newPath);
    return updated;
}

int relinkPath(Project* project,
               const std::string& oldPath,
               const std::string& newPath)
{
    if (!project || oldPath.empty() || newPath.empty() || oldPath == newPath)
        return 0;

    int updated = 0;
    for (size_t i = 0; i < project->sequenceCount(); ++i)
        updated += relinkPath(project->sequence(i), oldPath, newPath);

    spdlog::info("MediaRelinker: replaced {} project reference(s): '{}' -> '{}'",
                 updated, oldPath, newPath);
    return updated;
}

int relinkPresetBackground(ShotPresetManager* mgr,
                           const std::string& oldPath,
                           const std::string& newPath)
{
    if (!mgr || oldPath.empty() || newPath.empty() || oldPath == newPath)
        return 0;
    int updated = 0;
    const auto names = mgr->presetNames();
    for (const auto& name : names) {
        auto opt = mgr->load(name);
        if (!opt) continue;
        ShotPreset preset = std::move(*opt);
        bool dirty = false;
        for (int i = 0; i < preset.backgroundCount(); ++i) {
            BackgroundState* bg = preset.background(i);
            if (bg && bg->path == oldPath) {
                bg->path = newPath;
                ++updated;
                dirty = true;
            }
        }
        if (dirty) {
            mgr->save(preset);
        }
    }
    spdlog::info("MediaRelinker: updated {} preset BG reference(s)", updated);
    return updated;
}

} // namespace rt::MediaRelinker
