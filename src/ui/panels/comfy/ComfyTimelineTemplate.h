#pragma once

#include "ai/ShotPipeline.h"
#include <QString>
#include <memory>
#include <vector>

namespace rt {
class Timeline;
class Track;
class VideoClip;

// A recipe selects reusable clips from a frozen native project snapshot.
// Clone only the named roles, never the episode's custom graphics or renders.
class ComfyTimelineTemplate
{
public:
    ComfyTimelineTemplate();
    ~ComfyTimelineTemplate();
    bool load(const QString& recipe, QString* error);
    bool build(const std::vector<AssemblyShot>& shots, int64_t start, double backgroundSec,
               Timeline& timeline, AssemblyPlan& plan, QString* error);
    void applyCharacterStyle(VideoClip& clip, bool closeUp, bool centered) const;
private:
    struct Data;
    std::unique_ptr<Data> d;
};
} // namespace rt
