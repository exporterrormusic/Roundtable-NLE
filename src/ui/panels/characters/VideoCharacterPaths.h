#pragma once

// File locations for video characters, in one place. Paths are relative to
// the application root (see AppPaths.h); projects and presets store them in
// this relative form.

#include "PathUtils.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace rt {

// ── Hand-made video characters ──────────────────────────────────────────────
// Stacked-alpha HEVC proxies (top RGB / bottom alpha, 1080x3776, NVDEC).
namespace wells_videos {
inline constexpr const char* kChronoMute = "assets/videos/WELLS-CHRONO-MUTE_HEVC.mp4";
inline constexpr const char* kChronoTalk = "assets/videos/WELLS-CHRONO-TALK_HEVC.mp4";
inline constexpr const char* kDressMute  = "assets/videos/WELLS-DRESS-MUTE_HEVC.mp4";
inline constexpr const char* kDressTalk  = "assets/videos/WELLS-DRESS-TALK_HEVC.mp4";
} // namespace wells_videos

// A video character can have multiple outfits, each swapping the mute/talk
// video pair (mirrors Spine outfit switching).  The first entry is the
// default outfit used when the character is first added to a shot.
struct VCOutfit { std::string name; std::string mutePath; std::string talkPath; };

inline const std::vector<VCOutfit>& videoCharacterOutfitsFor(const std::string& charName)
{
    static const std::unordered_map<std::string, std::vector<VCOutfit>> table {
        { "Wells", {
            { "CHRONO", wells_videos::kChronoMute, wells_videos::kChronoTalk },
            { "DRESS",  wells_videos::kDressMute,  wells_videos::kDressTalk },
        }},
    };
    static const std::vector<VCOutfit> kEmpty;
    auto it = table.find(charName);
    return it != table.end() ? it->second : kEmpty;
}

// ── Converted (pre-rendered Spine) video characters ─────────────────────────
// Layout: assets/converted/{format}/{character}/{outfit}/{animation}[_talk]{ext}

struct ConvertedVideoPaths { std::string mute; std::string talk; };

/// Mute/talk paths for another outfit or animation of the converted clip at
/// `currentMediaPath`, keeping its format folder and file extension.
inline ConvertedVideoPaths convertedVideoPaths(const std::string& currentMediaPath,
                                               const std::string& character,
                                               const std::string& outfit,
                                               const std::string& animation)
{
    const auto current = utf8ToPath(currentMediaPath);
    std::string extension = pathToUtf8(current.extension());
    if (extension.empty()) extension = ".mov";

    // current = .../converted/{format}/{character}/{outfit}/{file}
    std::string format = pathToUtf8(current.parent_path().parent_path().parent_path().filename());
    if (format != "H264_Green" && format != "H264_Blue"
        && format != "H264_Custom" && format != "ProRes")
        format = "H264_Green";

    const std::string base = "assets/converted/" + format + "/" + character + "/"
                           + outfit + "/" + animation;
    return {base + extension, base + "_talk" + extension};
}

} // namespace rt
