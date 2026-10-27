/*
 * CorridorKey — the offline neural keyer behind Ultra Key's "Render with
 * CorridorKey" button (CorridorKey by Corridor Digital, vendored unmodified in
 * third_party/corridorkey; licence there).
 *
 * Ultra Key stays the live key. When a shot is final, the clip's used source
 * range (plus handles) is extracted, keyed by tools/corridorkey/
 * roundtable_corridorkey.py into a ProRes 4444 cutout the size of the source
 * frame, and the clip is pointed at that file — Premiere's Render and Replace.
 * A record beside the render (<render>.corridorkey) remembers the source so
 * "Back to Ultra Key" restores it exactly.
 *
 * This header is the Qt-free part: install layout + versions, the GPU check,
 * render planning, command lines, the worker protocol, the record, and the
 * clip swap. The UI (src/ui/corridorkey) runs the processes.
 */

#pragma once

#include "decode/VideoDecoder.h"   // VideoStreamInfo, ColorMatrix

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rt {

class Clip;
class Track;
class VideoClip;

namespace corridorkey {

namespace fs = std::filesystem;

struct RenderRecord;

// ── Versions (bump kSetupVersion when anything below changes) ───────────────
inline constexpr std::string_view kSetupVersion  = "1";
/// Part of every render's key: bump when renders made earlier must not be
/// reused (frame extraction, worker output).
inline constexpr std::string_view kRenderFormat  = "2";
inline constexpr std::string_view kCodeCommit    = "485f5e9";
inline constexpr std::string_view kUvUrl =
    "https://github.com/astral-sh/uv/releases/download/0.12.24/uv-x86_64-pc-windows-msvc.zip";
inline constexpr std::string_view kUvSha256 =
    "7c38608c8a18ee137d748a1773053b07ec8f3a30fab49aebaa6f4e4efeceb019";
inline constexpr int64_t          kUvBytes       = 15685515;
inline constexpr std::string_view kPythonVersion = "3.12";
inline constexpr std::string_view kTorchIndex    = "https://download.pytorch.org/whl/cu128";
inline constexpr std::string_view kTorch         = "torch==2.9.1";
inline constexpr std::string_view kTorchVision   = "torchvision==0.24.1";

/// What the setup downloads (shown in the consent text and used for progress).
inline constexpr int64_t kLibrariesBytes = 3'200'000'000;   // PyTorch (CUDA) + libraries
inline constexpr int64_t kModelsBytes    = 4'857'000'000;   // CorridorKey + C-RADIO + MoGe + RVM
/// Free space the setup needs while it runs (downloads + unpacked copies).
inline constexpr int64_t kSetupDiskBytes = 14'000'000'000;
/// Space the finished install keeps.
inline constexpr int64_t kInstalledBytes = 9'500'000'000;

// ── Install layout ───────────────────────────────────────────────────────────
struct Layout
{
    fs::path codeDir;      ///< root/third_party/corridorkey — CorridorKey, read-only
    fs::path toolsDir;     ///< root/tools/corridorkey — worker + pinned requirements
    fs::path runtimeDir;   ///< root/runtimes/corridorkey — everything the setup writes

    [[nodiscard]] fs::path worker() const       { return toolsDir / "roundtable_corridorkey.py"; }
    [[nodiscard]] fs::path requirements() const { return toolsDir / "requirements.txt"; }
    [[nodiscard]] fs::path downloadScript() const { return codeDir / "scripts" / "download_weights.py"; }
    [[nodiscard]] fs::path uvDir() const        { return runtimeDir / "uv"; }
    [[nodiscard]] fs::path uvExe() const        { return uvDir() / "uv.exe"; }
    [[nodiscard]] fs::path pythonDir() const    { return runtimeDir / "python"; }
    [[nodiscard]] fs::path venvDir() const      { return runtimeDir / "venv"; }
    [[nodiscard]] fs::path venvPython() const   { return venvDir() / "Scripts" / "python.exe"; }
    [[nodiscard]] fs::path modelsDir() const    { return runtimeDir / "models"; }
    [[nodiscard]] fs::path cacheDir() const     { return runtimeDir / "cache"; }
    [[nodiscard]] fs::path hfHome() const       { return runtimeDir / "hf"; }
    [[nodiscard]] fs::path jobsDir() const      { return runtimeDir / "jobs"; }
    [[nodiscard]] fs::path stampFile() const    { return runtimeDir / "installed.txt"; }
    /// Written by the installer when "Download CorridorKey" was ticked; the
    /// app starts the setup on its next launch and deletes it.
    [[nodiscard]] fs::path setupRequestFile() const { return runtimeDir / "setup-requested"; }
};

[[nodiscard]] Layout layoutFor(const fs::path& appRoot);

/// The model files the worker loads (all must exist for isInstalled).
[[nodiscard]] std::vector<fs::path> modelFiles(const Layout& layout);

/// Setup finished for this kSetupVersion and every file it made is there.
[[nodiscard]] bool isInstalled(const Layout& layout);

/// Text for stampFile() after a successful setup.
[[nodiscard]] std::string stampText();

// ── GPU check ────────────────────────────────────────────────────────────────
struct GpuInfo
{
    std::string name;
    double      computeCapability{0.0};
    int         driverMajor{0};
    double      memoryGB{0.0};
};

/// One line of `nvidia-smi --query-gpu=name,compute_cap,driver_version,
/// memory.total --format=csv,noheader,nounits`.
[[nodiscard]] std::optional<GpuInfo> parseNvidiaSmiLine(std::string_view line);

/// Why this GPU cannot run CorridorKey, or empty when it can (RTX 30-series
/// or newer = compute 8.0+, and a driver new enough for CUDA 12.8).
[[nodiscard]] std::string gpuProblem(const GpuInfo& gpu);

/// How many frames CorridorKey keys together follows the GPU memory a render
/// may use: Standard keeps the rest of the system smooth, Steadier lets it
/// key more frames at once (steadier edges) at the cost of more memory.
enum class Steadiness { Standard, Steadier };

/// GPU memory one render may use. Standard: about a third of the card,
/// 4–8 GB (8-frame windows at 1080p). Steadier: about two thirds, 4–16 GB
/// (24-frame windows at 1080p).
[[nodiscard]] double vramCapGB(double totalGB, Steadiness steadiness = Steadiness::Standard);

// ── Render planning ──────────────────────────────────────────────────────────
struct FrameRate
{
    int64_t num{24};
    int64_t den{1};
    [[nodiscard]] double value() const noexcept { return double(num) / double(den); }
};

/// The exact rate behind a probed double (29.97… → 30000/1001).
[[nodiscard]] FrameRate exactFrameRate(double fps);

/// Smallest frame count that is a whole number of timeline ticks at `rate`
/// (30000/1001 → 5). Renders start on a multiple of it so the clip's source
/// in point can shift onto the render without rounding.
[[nodiscard]] int64_t tickAlignedFrameStep(FrameRate rate);

/// Ticks covered by `frames` frames at `rate` (exact for multiples of
/// tickAlignedFrameStep, rounded otherwise).
[[nodiscard]] int64_t framesToTicks(int64_t frames, FrameRate rate);

struct Rect
{
    int x{0}, y{0}, w{0}, h{0};
    [[nodiscard]] bool operator==(const Rect&) const = default;
};

struct RenderPlan
{
    std::string sourcePath;
    FrameRate   fps;
    int64_t     firstFrame{0};   ///< inclusive source frame index
    int64_t     lastFrame{0};    ///< inclusive
    int64_t     tickShift{0};    ///< framesToTicks(firstFrame): source time before the render's frame 0
    int         sourceWidth{0};
    int         sourceHeight{0};
    Rect        keyRect;         ///< part of the frame that is keyed (the rest stays transparent)
    ColorMatrix matrix{ColorMatrix::BT709};  ///< the app's YUV→RGB decision for this source
    bool        fullRange{false};
    Steadiness  steadiness{Steadiness::Standard};

    [[nodiscard]] int64_t frameCount() const noexcept { return lastFrame - firstFrame + 1; }
    [[nodiscard]] double  keyedMegapixels() const noexcept { return double(keyRect.w) * keyRect.h / 1e6; }
};

struct PlanResult
{
    std::optional<RenderPlan> plan;
    std::string               problem;   ///< why there is no plan
};

/// Timeline ticks a clip is shown before its in point (lead) and after its
/// out point (tail): two-clip transitions — dissolves, wipes, pushes — show
/// both clips across the cut. Single-sided fades stay inside the clip.
struct Coverage
{
    int64_t lead{0};
    int64_t tail{0};
};

/// Coverage of `clip` from the transitions on its `track`.
[[nodiscard]] Coverage transitionCoverage(const Track& track, const Clip& clip);

/// Plan a render for `clip` (using its current source): every source frame
/// it shows from timelineIn − lead to timelineOut + tail, sampled once per
/// sequence frame (`frameTicks`), so speed ramps are covered.
[[nodiscard]] PlanResult planRender(const VideoClip& clip, const VideoStreamInfo& info,
                                    int64_t frameTicks, Coverage coverage = {});

/// The keyed area for this crop: what the crop leaves visible plus a margin,
/// even edges. Empty (w == 0) when the crop leaves almost nothing.
[[nodiscard]] Rect keyRectFor(const VideoClip& clip, int sourceWidth, int sourceHeight);

/// A clip that plays `record`'s render still shows only frames and area the
/// render has (false after a transition was added or lengthened, the clip
/// was trimmed longer or slowed down, or its crop opened up).
[[nodiscard]] bool renderCovers(const VideoClip& clip, const RenderRecord& record,
                                Coverage coverage, int64_t frameTicks);

/// Stable name part for a render of `plan` from a source with this size/mtime.
[[nodiscard]] std::string renderKey(const RenderPlan& plan, uintmax_t sourceBytes,
                                    int64_t sourceModified);

/// "<source stem> CK <first>-<last> <key>.mov"
[[nodiscard]] std::string renderFileName(const RenderPlan& plan, std::string_view key);

/// ffmpeg arguments (no program) that write the plan's frames, keyed region
/// only, as 000000.png… into `framesDir` — converted to RGB exactly as the
/// app converts this source for display.
[[nodiscard]] std::vector<std::string> extractArgs(const RenderPlan& plan,
                                                   const fs::path& framesDir);

/// Arguments for the venv python that key `framesDir` into `output`.
[[nodiscard]] std::vector<std::string> workerArgs(const Layout& layout, const RenderPlan& plan,
                                                  const fs::path& framesDir, const fs::path& output,
                                                  const fs::path& tempDir, const fs::path& ffmpeg,
                                                  double vramGB);

/// Seconds a render takes, from measured seconds per keyed megapixel-frame
/// (0.65 on an RTX 4090) plus model load.
[[nodiscard]] double estimateSeconds(const RenderPlan& plan, double secondsPerMegapixelFrame = 0.65);

/// "about 4 min" / "about 40 s"
[[nodiscard]] std::string formatDuration(double seconds);

// ── Worker protocol ──────────────────────────────────────────────────────────
struct WorkerEvent
{
    enum class Kind { Log, Progress, Status, Done, Error };
    Kind        kind{Kind::Log};
    double      progress{0.0};
    std::string text;
};

[[nodiscard]] WorkerEvent parseWorkerLine(std::string_view line);

// ── The record beside a render ───────────────────────────────────────────────
struct RenderRecord
{
    std::string sourcePath;
    std::string sourceCodec;
    int64_t     sourceDuration{0};   ///< the clip's sourceDuration on the original media
    FrameRate   fps;
    int64_t     firstFrame{0};
    int64_t     lastFrame{0};
    int64_t     tickShift{0};
    Rect        keyRect;
    int         sourceWidth{0};
    int         sourceHeight{0};
    Steadiness  steadiness{Steadiness::Standard};
    std::string key;
    std::string codeCommit;
};

[[nodiscard]] fs::path recordPathFor(const fs::path& render);
bool writeRecord(const fs::path& recordFile, const RenderRecord& record, std::string* error = nullptr);
[[nodiscard]] std::optional<RenderRecord> readRecord(const fs::path& recordFile);

/// The record when `mediaPath` is a CorridorKey render, else nullopt.
[[nodiscard]] std::optional<RenderRecord> recordForMedia(const std::string& mediaPath);

[[nodiscard]] RenderRecord makeRecord(const RenderPlan& plan, const VideoClip& clip,
                                      std::string_view key);

// ── The clip swap (one undo step in the UI) ──────────────────────────────────
/// Point `clip` at `render` and turn off the Ultra Key `effectId` that keyed it.
void useRender(VideoClip& clip, const std::string& render, const RenderRecord& record,
               uint64_t effectId);
/// Point `clip` back at its source and turn the Ultra Key back on.
void useSource(VideoClip& clip, const RenderRecord& record, uint64_t effectId);

} // namespace corridorkey
} // namespace rt
