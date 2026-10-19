/*
 * ShotSwitchController — undo-aware shot-preset switching.
 *
 * apply(groupId, shot) replaces every visual clip of a shot group with the
 * layers of another shot preset at the same time span (adding video tracks
 * when the preset needs more), renames the rest of the group (audio) to the
 * new shot, and records it as one undo step. Refuses a group that has a
 * member on a locked track.
 *
 * Knows nothing about panels: the workspace supplies the timeline, command
 * stack and presets, and reacts through three callbacks.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace rt {

class Clip;
class CommandStack;
class ShotPresetManager;
class Timeline;
class Track;

class ShotSwitchController {
public:
    struct Config {
        std::function<Timeline*()>          timeline;
        std::function<CommandStack*()>      commandStack;
        std::function<ShotPresetManager*()> presets;
        /// Nothing changed (missing preset, no timeline): repaint.
        std::function<void()>               refreshViews;
        /// Clips are about to be freed: panels must let go of them.
        std::function<void()>               releaseClipPanels;
        /// The switch (or its undo/redo) is done; `picked` is the group's
        /// first visual clip (may be null), to reselect.
        std::function<void(Clip* picked, Track* track)> shotSwitched;
    };

    explicit ShotSwitchController(Config cfg) : m_cfg(std::move(cfg)) {}

    void apply(uint64_t groupId, const std::string& newShotName);

private:
    /// Read at execute time: undo/redo act on the current timeline.
    [[nodiscard]] Timeline* timeline() const { return m_cfg.timeline(); }

    Config m_cfg;
};

} // namespace rt
