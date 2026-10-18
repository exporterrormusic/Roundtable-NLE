/*
 * VideoPathResolve.cpp — see VideoPathResolve.h.
 */

#include "project/VideoPathResolve.h"

#include "PathUtils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace rt {

// When loading a project, the stored video path may reference a .mov (ProRes)
// file that has since been re-rendered as .mp4 (HEVC), or vice versa.
// Try the alternate extension so format migrations don't break projects.
//
// 2026-05-28 Wells migration: remap all old Wells paths to HEVC
// stacked-alpha (NVDEC hardware decode, no chroma key needed).
std::string resolveVideoPath(const std::string& path)
{
    if (path.empty()) return path;
    namespace fs = std::filesystem;
    // mediaPath strings are UTF-8 (the project file stores UTF-8).  The
    // fs::path(std::string) ctor decodes via the ANSI codepage and silently
    // mojibakes any non-ANSI character (U+FF5C etc.) — must use utf8ToPath.
    fs::path p = utf8ToPath(path);

    // Wells path remap: point any old Wells path at the lightweight HEVC
    // packed-alpha source (fast NVDEC scrub/playback).  This source is used
    // for preview AND export — the old export-time ProRes-master
    // substitution (wellsExportSource) was removed with FrameRenderer.
    {
        std::string filename = pathToUtf8(p.filename());
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // Map each Wells costume's old/master path to its lightweight stacked
        // HEVC proxy (skip paths already pointing at a _HEVC proxy).
        const char* hevcName = nullptr;
        if (lower.find("_hevc") == std::string::npos) {
            if      (lower.find("wells-chrono-mute") != std::string::npos) hevcName = "WELLS-CHRONO-MUTE_HEVC.mp4";
            else if (lower.find("wells-chrono-talk") != std::string::npos) hevcName = "WELLS-CHRONO-TALK_HEVC.mp4";
            else if (lower.find("wells-dress-mute")  != std::string::npos) hevcName = "WELLS-DRESS-MUTE_HEVC.mp4";
            else if (lower.find("wells-dress-talk")  != std::string::npos) hevcName = "WELLS-DRESS-TALK_HEVC.mp4";
        }
        if (hevcName) {
            fs::path hevcPath = p;
            hevcPath.replace_filename(hevcName);
            if (fs::exists(hevcPath)) {
                spdlog::info("ClipSerialization: Wells migration '{}' -> '{}'",
                             path, pathToUtf8(hevcPath));
                return pathToUtf8(hevcPath);
            }
        }
    }

    if (fs::exists(p)) return path;

    auto ext = pathToUtf8(p.extension());
    fs::path alt;
    if (ext == ".mov")       alt = p.replace_extension(".mp4");
    else if (ext == ".mp4")  alt = p.replace_extension(".mov");
    else if (ext == ".webm") alt = p.replace_extension(".mp4");
    else return path;

    if (fs::exists(alt)) {
        spdlog::info("ClipSerialization: re-resolved missing '{}' → '{}'",
                     path, pathToUtf8(alt));
        return pathToUtf8(alt);
    }
    return path;
}

} // namespace rt
